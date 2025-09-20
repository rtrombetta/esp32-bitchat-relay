#include "espn.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_system.h>
#if __has_include(<esp_coexist.h>)
#include <esp_coexist.h>
#define HAVE_COEX 1
#endif
#include <string.h>

espn* espn::s_self = nullptr;

static const uint8_t ESPN_BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static inline uint32_t now_ms() { return millis(); }
static inline void short_jitter_us() { delayMicroseconds(1000 + (esp_random() % 5000)); }
inline uint8_t espn::make_gid() { return (uint8_t)esp_random(); }

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
  WiFi.setTxPower(WIFI_POWER_19_5dBm); // ~máximo típico
  // WiFi.setTxPower(WIFI_POWER_15dBm);
  // WiFi.setTxPower(WIFI_POWER_8_5dBm);
  // WiFi.setTxPower(WIFI_POWER_2dBm);

  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_start());
  // fixa canal conforme m_channel
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_channel(m_channel, WIFI_SECOND_CHAN_NONE));
  // liga LR
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR)); // 19.5dBm

  if (esp_now_init() != ESP_OK) return false;

  esp_now_register_recv_cb(&espn::onRecvStatic);

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

  return true;
}

bool espn::tx(const uint8_t* frame, size_t len, uint32_t timeout_ms) {
  if (!m_txQ || !frame || len == 0 || len > MAX_FRAME) return false;
  TxItem item{};
  item.len = (uint16_t)len;
  memcpy(item.buf, frame, len);
  TickType_t to = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
  return xQueueSend(m_txQ, &item, to) == pdTRUE;
}

bool espn::rx(uint8_t* out, size_t* inout_len, uint32_t timeout_ms) {
  if (!m_rxQ || !out || !inout_len || *inout_len == 0) return false;
  RxItem item{};
  TickType_t to = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
  if (xQueueReceive(m_rxQ, &item, to) == pdTRUE) {
    size_t n = item.len;
    if (n > *inout_len) n = *inout_len;
    memcpy(out, item.buf, n);
    *inout_len = n;
    return true;
  }
  return false;
}

size_t espn::rxAvailable() const {
  return m_rxQ ? uxQueueMessagesWaiting(m_rxQ) : 0;
}

// ---------- Callbacks ----------
void espn::onRecvStatic(const uint8_t* mac, const uint8_t* data, int len) {
  if (s_self) s_self->onRecv(mac, data, len);
}

void espn::onRecv(const uint8_t* mac, const uint8_t* data, int len) {
  if (!m_fragQ || len <= 0 || len > m_link_mtu || len > MAX_LINK_MTU) return;
  // Controle: ACK (LCTL_SIG/LCTL_ACK)
  if (len >= (int)(2 + NET_ID_LEN + 1) && data[0] == LCTL_SIG && data[1] == LCTL_ACK) {
    if (memcmp(&data[2], m_net_id, NET_ID_LEN) == 0) {
      uint8_t gid = data[2 + NET_ID_LEN];
      m_ack_seen[gid] = 1;
      //Serial.printf("espn: ACK gid=%u\r\n", gid);
    }
    return;
  }  
  FragItem fi{};
  fi.len = (uint8_t)len;
  memcpy(fi.buf, data, len);
  memcpy(fi.mac, mac, 6);
  // não bloqueia no callback
  xQueueSend(m_fragQ, &fi, 0);
}

// ---------- Tasks ----------
void espn::txTaskThunk(void* arg) { static_cast<espn*>(arg)->txTask(); }
void espn::rxTaskThunk(void* arg) { static_cast<espn*>(arg)->rxTask(); }

void espn::txTask() {
  TxItem it{};
  for (;;) {
    if (xQueueReceive(m_txQ, &it, portMAX_DELAY) == pdTRUE) {
      llf_sendFrame(it.buf, it.len);
    }
  }
}

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
      }
    }
    llf_gc();
  }
}

// ---------- Envio: LLF com split balanceado ----------
static bool espnow_send_raw(const uint8_t* buf, size_t len) {
  return esp_now_send(ESPN_BCAST, buf, len) == ESP_OK;
}

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
  m_ack_seen[gid] = 0;
  for (uint8_t attempt = 0; attempt <= (RELIABLE ? MAX_RETRIES : 0); ++attempt) {
    //Serial.printf("espn: TX gid=%u cnt=%u total=%u (attemp %u)\r\n",
    //              gid, cnt, (uint16_t)flen, (uint32_t)(attempt + 1));
    size_t off = 0;
    for (uint8_t i = 0; i < cnt; ++i) {
        size_t part = m_balanced ? (base + (i < rem ? 1 : 0))
                             : ( (i+1 < cnt) ? maxChunk : (flen - off) );
        if (part > maxChunk) part = maxChunk;

        uint8_t pkt[MAX_LINK_MTU];
        LLF h{};
        h.sig   = LLF_SIG;
        h.gid   = gid;
        h.idx   = i;
        h.cnt   = cnt;
        h.total = (uint16_t)flen;
        memcpy(h.net_id, m_net_id, NET_ID_LEN);

        memcpy(pkt, &h, hdr);
        memcpy(pkt + hdr, frame + off, part);

      short_jitter_us();
      espnow_send_raw(pkt, hdr + part);
      off += part;
    }
    if (!RELIABLE) return true;
    taskYIELD();
    // aguarda ACK do gid
    uint32_t t0 = now_ms();
    while ((now_ms() - t0) < ACK_TIMEOUT_MS) {
      if (m_ack_seen[gid]) { m_ack_seen[gid] = 0; return true; }
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
    if (RELIABLE) for (uint8_t i = 0; i < 3; i++) sendAck(h.gid, s->mac); vTaskDelay(pdMS_TO_TICKS(1));
    s->used = false;
    return true;
  }
  return false;
}

bool espn::ensurePeer(const uint8_t mac[6]) {
  if (!mac) return false;
  if (esp_now_is_peer_exist((uint8_t*)mac)) return true;
  esp_now_peer_info_t p{}; memcpy(p.peer_addr, mac, 6);
  p.ifidx = WIFI_IF_STA; p.channel = m_channel; p.encrypt = false;
  return esp_now_add_peer(&p) == ESP_OK;
}

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