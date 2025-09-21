#include "espn.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <stdarg.h>
#if __has_include(<esp_coexist.h>)
#include <esp_coexist.h>
#define HAVE_COEX 1
#endif
#include <string.h>

espn* espn::s_self = nullptr;

static const uint8_t ESPN_BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static inline uint32_t now_ms() { return millis(); }
static inline void short_jitter_us() { delayMicroseconds(2000 + (esp_random() % 10000)); }
inline uint8_t espn::make_gid() { return (uint8_t)esp_random(); }

// ---------- Util ----------
// Função auxiliar para log seguro com semáforo
// log não-bloqueante: se não conseguir o mutex, dropa o log
static void safeLog(const char* format, ...) {
    if (gSerialMutex && xSemaphoreTake(gSerialMutex, 0) == pdTRUE) {
        va_list args;
        va_start(args, format);
        char buf[256]; // Buffer seguro para log
        vsnprintf(buf, sizeof(buf), format, args);
        Serial.print(buf);
        va_end(args);
        xSemaphoreGive(gSerialMutex);
    }
}

// ---------- ctor ----------
espn::espn() {}

// ---------- WiFi + ESPNOW ----------
bool espn::initWifiEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);

  // força tipo de power-save (PS) para modem-sleep (não light-sleep)
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

#ifdef HAVE_COEX  
  // balanceia o tempo entre Wi-Fi e BLE (evita assert e starvation)
  esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
#endif

  // potência TX (padrão é 20.5dBm, máximo teórico)
  // WiFi.setTxPower(WIFI_POWER_19_5dBm); // ~máximo típico
   WiFi.setTxPower(WIFI_POWER_15dBm);
  // WiFi.setTxPower(WIFI_POWER_8_5dBm);
  // WiFi.setTxPower(WIFI_POWER_2dBm);

  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_start());
  // fixa canal conforme m_channel
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_channel(m_channel, WIFI_SECOND_CHAN_NONE));
  // liga LR
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR)); // 19.5dBm

  if (esp_now_init() != ESP_OK) return false;

  esp_now_register_recv_cb(&espn::onRecvStatic);
  // callback de envio → pra atualizar last_seen e futuras métricas
  esp_now_register_send_cb(&espn::onSendStatic);

  esp_now_peer_info_t p{};
  memcpy(p.peer_addr, ESPN_BCAST, 6);
  p.ifidx   = WIFI_IF_STA;
  p.channel = m_channel;
  p.encrypt = false;
  if (esp_now_add_peer(&p) != ESP_OK) return false;

  return true;
}

// ---------- API pública ----------
bool espn::init(const uint8_t net_id[NET_ID_LEN], const uint8_t mac[6],
                uint8_t channel, uint16_t link_mtu,
                int tx_core, int rx_core, bool balanced_split)
{
  if (s_self && s_self != this) return false; // 1 instância simples
  s_self = this;

  memcpy(m_net_id, net_id, NET_ID_LEN);
  if (mac) memcpy(m_my_mac, mac, 6);
  else WiFi.macAddress(m_my_mac);  // garante MAC local para ignorar meu HELLO

  // aplica canal/MTU
  m_channel  = channel ? channel : 6;
  if (link_mtu == 0 || link_mtu > MAX_LINK_MTU) link_mtu = MAX_LINK_MTU;
  m_link_mtu = link_mtu;
  m_balanced = balanced_split;

  if (!initWifiEspNow()) return false;

  m_txQ   = xQueueCreate(TX_QUEUE_LEN, sizeof(TxItem));
  m_rxQ   = xQueueCreate(RX_QUEUE_LEN, sizeof(RxItem));
  m_fragQ = xQueueCreate(FRAG_QUEUE_LEN, sizeof(FragItem));
  if (!m_txQ || !m_rxQ || !m_fragQ) return false;

  BaseType_t ok;
  ok = xTaskCreatePinnedToCore(txTaskThunk, "espnTx", 4096, this, 3,
                               &m_txTask, (tx_core == 0 || tx_core == 1) ? tx_core : tskNO_AFFINITY);
  if (ok != pdPASS) return false;

  ok = xTaskCreatePinnedToCore(rxTaskThunk, "espnRx", 4096, this, 4,
                               &m_rxTask, (rx_core == 0 || rx_core == 1) ? rx_core : tskNO_AFFINITY);
  if (ok != pdPASS) return false;

  // agenda primeiro ANNOUNCE com jitter inicial
  m_nextAnnounceMs = now_ms() + (1000 + (esp_random() % 4000));

  return true;
}

// Enfileira um frame completo para enviar no backbone (fragmenta dentro)
bool espn::tx(const uint8_t* frame, size_t len, uint32_t timeout_ms) {
  if (!m_txQ || !frame || len == 0 || len > MAX_FRAME) return false;
  TxItem item{};
  item.len = (uint16_t)len;
  memcpy(item.buf, frame, len);
  TickType_t to = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
  //if (m_debug >= 3) safeLog("[ESPN-TXENQ] len=%u\r\n", (unsigned)len);
  return xQueueSend(m_txQ, &item, to) == pdTRUE;
}

// Se houver frame completo, copia pra 'out' (até *inout_len) e retorna true
bool espn::rx(uint8_t* out, size_t* inout_len, uint32_t timeout_ms) {
  if (!m_rxQ || !out || !inout_len || *inout_len == 0) return false;
  RxItem item{};
  TickType_t to = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
  if (xQueueReceive(m_rxQ, &item, to) == pdTRUE) {
    size_t n = item.len;
    if (n > *inout_len) n = *inout_len;
    memcpy(out, item.buf, n);
    *inout_len = n;
    //if (m_debug >= 3) safeLog("[ESPN-RXDEQ] len=%u\r\n", (unsigned)n);
    return true;
  }
  return false;
}

// Quantos frames completos aguardando leitura
size_t espn::rxAvailable() const {
  return m_rxQ ? uxQueueMessagesWaiting(m_rxQ) : 0;
}

// ---------- Callbacks ----------

// callback de recepção (chama método da instância)
void espn::onRecvStatic(const uint8_t* mac, const uint8_t* data, int len) {
  if (s_self) s_self->onRecv(mac, data, len);
}

// callback de envio (atualiza last_seen)
void espn::onSendStatic(const uint8_t* mac, esp_now_send_status_t s) {
  if (s_self) s_self->onSend(mac, s);
}

// método de instância chamado pelo callback estático
void espn::onRecv(const uint8_t* mac, const uint8_t* data, int len) {
  if (!m_fragQ || len <= 0 || len > m_link_mtu || len > MAX_LINK_MTU) return;
  // Controle (LCTL): ACK / HELLO
  if (len >= 2 && data[0] == LCTL_SIG) {
    // ACK de aplicação (só se RELIABLE ativo)
    if (RELIABLE && data[1] == LCTL_ACK &&
        len >= (int)(2 + NET_ID_LEN + 1) &&
        memcmp(&data[2], m_net_id, NET_ID_LEN) == 0) {
      uint8_t gid = data[2 + NET_ID_LEN];
      m_ack_seen[gid] = 1;
      if (m_debug >= 5) safeLog("[ESPN-ACK] gid=%u OK\r\n", gid);
      int idx = findPeerIdx(mac);
      if (idx >= 0) m_ack_mask_got |= (uint16_t)(1u << idx);
      return;
    }
    // Descoberta (HELLO/ANNOUNCE) — sem ACK
    if (data[1] == LCTL_HELLO &&
        len >= (int)(2 + NET_ID_LEN) &&
        memcmp(&data[2], m_net_id, NET_ID_LEN) == 0) {
      // ignora meu próprio HELLO
      if (memcmp(mac, m_my_mac, 6) != 0) {
        // add/refresh peer
        if (addPeer(mac)) {
          if (m_debug >= 3) safeLog("[ESPN-DISC] hello %02X:%02X:%02X:%02X:%02X:%02X peers=%d\r\n",
                  mac[0],mac[1],mac[2],mac[3],mac[4],mac[5], m_nPeers);
        }
      }
      return;
    }
  }  
  FragItem fi{};
  fi.len = (uint8_t)len;
  memcpy(fi.buf, data, len);
  memcpy(fi.mac, mac, 6);
  // não bloqueia no callback
  xQueueSend(m_fragQ, &fi, 0);
}

// método de instância chamado pelo callback estático
void espn::onSend(const uint8_t* mac, esp_now_send_status_t s) {
  if (!mac) return;
  if (s == ESP_NOW_SEND_SUCCESS) {
    int i = findPeerIdx(mac);
    if (i >= 0) m_peers[i].last_seen = now_ms();
  }
}

// ---------- Tasks ----------
// thunks para chamar métodos de instância
void espn::txTaskThunk(void* arg) { static_cast<espn*>(arg)->txTask(); }
void espn::rxTaskThunk(void* arg) { static_cast<espn*>(arg)->rxTask(); }


// Task de envio (fragmenta e envia)
void espn::txTask() {
  TxItem it{};
  for (;;) {
    if (xQueueReceive(m_txQ, &it, portMAX_DELAY) == pdTRUE) {
      if (m_debug >= 3) safeLog("[ESPN-TXDEQ] len=%u\r\n", (unsigned)it.len);
      llf_sendFrame(it.buf, it.len);
    }
  }
}

// Task de recepção (remonta e entrega)
void espn::rxTask() {
  FragItem fr{};
  for (;;) {
    if (xQueueReceive(m_fragQ, &fr, pdMS_TO_TICKS(50)) == pdTRUE) {
      const uint8_t* full = nullptr; size_t fullLen = 0;
      if (llf_feed(fr.buf, fr.len, &full, &fullLen, fr.mac)) {
        RxItem r{};
        r.len = (uint16_t)fullLen;
        if (r.len > MAX_FRAME) r.len = MAX_FRAME;
        memcpy(r.buf, full, r.len);
        xQueueSend(m_rxQ, &r, 0);
        if (m_debug >= 3) safeLog("[ESPN-RXENQ] len=%u\r\n", (unsigned)r.len);
      }
    }
    llf_gc();
    discoveryTick();
  }
}

// ---------- Envio (fragmentação + ARQ) ----------
bool espn::llf_sendFrame(const uint8_t* frame, size_t flen) {
  if (flen == 0 || flen > MAX_FRAME) return false;

  const size_t hdr = sizeof(LLF);
  if (m_link_mtu <= hdr) return false;

  const size_t maxChunk = m_link_mtu - hdr;
  // nº de frags mínimo para caber
  uint8_t cnt = (uint8_t)((flen + maxChunk - 1) / maxChunk);
  if (cnt == 0) cnt = 1;
  if (cnt > 16) cnt = 16; // cabe no bitmap

  // Calcula tamanhos balanceados (diferença <= 1)
  // base = floor, rem = resto -> primeiros 'rem' frags recebem +1
  size_t base = flen / cnt;
  size_t rem  = flen % cnt;

  const uint8_t gid = make_gid();
  m_ack_seen[gid]     = 0;
  m_ack_mask_got      = 0;
  m_ack_mask_expect   = (m_nPeers > 0) ? (uint16_t)((1u << m_nPeers) - 1u) : 0;

  for (uint8_t attempt = 0; attempt <= (RELIABLE ? MAX_RETRIES : 0); ++attempt) {
    if (m_debug >= 3) safeLog("[ESPN-TX] gid=%u cnt=%u total=%u (attemp %u)\r\n",
            gid, cnt, (uint16_t)flen, (uint32_t)(attempt + 1));

    // envia para cada peer que ainda não ACKou
    if (m_nPeers == 0) {
      // ninguém para enviar → falha silenciosa
      break;
    }
    for (int p = 0; p < m_nPeers; ++p) {
      if ( (m_ack_mask_got & (1u << p)) != 0 ) continue; // já OK
      if (!ensurePeer(m_peers[p].mac)) continue;
      size_t off = 0;
      for (uint8_t i = 0; i < cnt; ++i) {
        size_t part = m_balanced ? (base + (i < rem ? 1 : 0))
                                 : ((i+1 < cnt) ? maxChunk : (flen - off));
        if (part > maxChunk) part = maxChunk;
        uint8_t pkt[MAX_LINK_MTU];
        LLF h{};
        h.sig = LLF_SIG; h.gid = gid; h.idx = i; h.cnt = cnt; h.total = (uint16_t)flen;
        memcpy(h.net_id, m_net_id, NET_ID_LEN);
        memcpy(pkt, &h, hdr);
        memcpy(pkt + hdr, frame + off, part);
        short_jitter_us();
        esp_now_send((uint8_t*)m_peers[p].mac, pkt, hdr + part);
        off += part;
      }
    }
    if (!RELIABLE) return true;
    taskYIELD();
    // aguarda ACK do gid
    uint32_t t0 = now_ms();
    while ((now_ms() - t0) < ACK_TIMEOUT_MS) {
      // sucesso quando TODOS os peers esperados ackaram
      if (m_nPeers > 0 && m_ack_mask_expect != 0 && (m_ack_mask_got == m_ack_mask_expect)) {
        m_ack_seen[gid] = 0; return true;
      }
      vTaskDelay(pdMS_TO_TICKS(1));
    }
    // timeout → tenta de novo
  }
  m_ack_seen[gid] = 0;
  return false;    
}

// ---------- Remontagem ----------
bool espn::llf_feed(const uint8_t* pkt, size_t len, const uint8_t** out, size_t* outlen,
                    const uint8_t src_mac[6]) {
  *out = nullptr; *outlen = 0;
  if (len < sizeof(LLF)) return false;

  LLF h{}; memcpy(&h, pkt, sizeof(LLF));    
  if (h.sig != LLF_SIG || h.cnt == 0 || h.cnt > 16) return false;
  if (memcmp(h.net_id, m_net_id, NET_ID_LEN) != 0) return false;

  const size_t hdr = sizeof(LLF);
  const size_t frag_payload = len - hdr;
  const size_t maxChunk = m_link_mtu - hdr;

  // Calcula offset com a mesma regra de split
  size_t base = h.total / h.cnt;
  size_t rem  = h.total % h.cnt;

  size_t off = 0;
  if (m_balanced) {
    // soma dos tamanhos dos frags anteriores
    uint8_t prev = (h.idx < rem) ? h.idx : rem;
    off = (size_t)prev * (base + 1) + (size_t)(h.idx - prev) * base;
  } else {
    // modo “clássico”: idx*(maxChunk), com last menor
    off = (size_t)h.idx * maxChunk;
  }

  if (frag_payload > maxChunk) return false;
  if (off + frag_payload > MAX_FRAME) return false;

  // Seleciona/abre slot
  FragSlot* s = nullptr;
  uint32_t t = now_ms();

  for (auto &sl : m_slots) {
    if (sl.used && sl.gid == h.gid) { s = &sl; break; }
  }
  if (!s) {
    for (auto &sl : m_slots) {
      if (!sl.used || (t - sl.ts_start) > LLF_TIMEOUT_MS) { s = &sl; break; }
    }
    if (!s) return false;
    s->used = true; s->gid = h.gid; s->cnt = h.cnt; s->total = h.total;
    s->bitmap = 0; s->ts_start = t;
    if (src_mac) memcpy(s->mac, src_mac, 6);
  }
  uint16_t mask = (uint16_t)1u << h.idx;
  if (!(s->bitmap & mask)) {
    memcpy(s->buf + off, pkt + hdr, frag_payload);
    s->bitmap |= mask;
  }
  const uint16_t want = (uint16_t)((1u << s->cnt) - 1u);
  if (s->bitmap == want) {
    *out = s->buf;
    *outlen = s->total;

    if (RELIABLE) {
      // manda 2x com jitter mínimo pra robustez
      sendAck(h.gid, s->mac);
      vTaskDelay(pdMS_TO_TICKS(1));
      sendAck(h.gid, s->mac);
      if (m_debug >= 5) safeLog("[ESPN-ACK] ack sent gid=%u total=%u\r\n", s->gid, s->total);
    }    

    s->used = false;
    return true;
  }
  return false;
}

// ---------- Peers ESPNOW (driver) ----------
bool espn::ensurePeer(const uint8_t mac[6]) {
  if (!mac) return false;
  if (esp_now_is_peer_exist((uint8_t*)mac)) return true;
  esp_now_peer_info_t p{}; memcpy(p.peer_addr, mac, 6);
  p.ifidx = WIFI_IF_STA; p.channel = m_channel; p.encrypt = false;
  return esp_now_add_peer(&p) == ESP_OK;
}

// Envia ACK de aplicação (RELIABLE)
void espn::sendAck(uint8_t gid, const uint8_t dst_mac[6]) {
  if (!dst_mac) return;
  if (!ensurePeer(dst_mac)) return;
  uint8_t pkt[2 + NET_ID_LEN + 1]; // sig,type,net_id[8],gid
  pkt[0] = LCTL_SIG; pkt[1] = LCTL_ACK;
  memcpy(&pkt[2], m_net_id, NET_ID_LEN);
  pkt[2 + NET_ID_LEN] = gid;
  short_jitter_us();
  esp_now_send((uint8_t*)dst_mac, pkt, sizeof(pkt));
}

// ---------- GC de slots de remontagem ----------
// limpa slots com timeout
void espn::llf_gc() {
  uint32_t t = now_ms();
  for (auto &sl : m_slots) {
    if (sl.used && (t - sl.ts_start) > LLF_TIMEOUT_MS) {
      sl.used   = false;
      sl.bitmap = 0;
      sl.gid = 0; sl.cnt = 0; sl.total = 0;
      // (buf e mac ficam como lixo antigo; não faz diferença)
    }
  }
}

// ====================== Peers & Descoberta ======================

// Retorna índice do peer ou -1 se não existir
int espn::findPeerIdx(const uint8_t mac[6]) const {
  if (!mac) return -1;
  for (int i=0;i<m_nPeers;i++)
    if (memcmp(m_peers[i].mac, mac, 6)==0) return i;
  return -1;
}

// Adiciona peer (se não existir). Retorna false se não couber mais.
bool espn::addPeer(const uint8_t mac[6]) {
  if (!mac) return false;
  int idx = findPeerIdx(mac);
  if (idx >= 0) { m_peers[idx].last_seen = now_ms(); return true; }
  if (m_nPeers >= MAX_PEERS) return false;
  memcpy(m_peers[m_nPeers].mac, mac, 6);
  m_peers[m_nPeers].last_seen = now_ms();
  bool ok = ensurePeer(mac);
  if (ok) m_nPeers++;
  if (m_debug >= 3) safeLog("[ESPN-PEER] Peer added: %02X:%02X:%02X:%02X:%02X:%02X\r\n",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return ok;
}

// Remove peer (se existir)
bool espn::delPeer(const uint8_t mac[6]) {
  int idx = findPeerIdx(mac);
  if (idx < 0) return false;
  for (int i=idx+1;i<m_nPeers;i++) m_peers[i-1]=m_peers[i];
  m_nPeers--;
  if (m_debug >= 3) safeLog("[ESPN-PEER] Peer removed: %02X:%02X:%02X:%02X:%02X:%02X\r\n",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return true;
}

// Remove todos os peers
void espn::clearPeers(){ m_nPeers=0; }

// Envia broadcast ANNOUNCE (HELLO)
void espn::sendAnnounce() {
  // LCTL_SIG, LCTL_HELLO, NET_ID[8]
  uint8_t pkt[2 + NET_ID_LEN];
  pkt[0] = LCTL_SIG; pkt[1] = LCTL_HELLO;
  memcpy(&pkt[2], m_net_id, NET_ID_LEN);
  esp_now_send((uint8_t*)ESPN_BCAST, pkt, sizeof(pkt));
  if (m_debug >= 3) safeLog("[ESPN-DISC] Announce sent\r\n");
}

// Tick de descoberta (chamado periodicamente no rxTask)
void espn::discoveryTick() {
  uint32_t t = now_ms();
  if (t >= m_nextAnnounceMs) {
    sendAnnounce();
    // jitter 0–4 s pra evitar lock-step
    m_nextAnnounceMs = t + ANNOUNCE_IV_MS + (esp_random() % 4000);
  }
  // aging
  for (int i=0; i<m_nPeers; ) {
    if ((t - m_peers[i].last_seen) > PEER_TTL_MS) {
      delPeer(m_peers[i].mac);
    } else {
      ++i;
    }
  }
}  
