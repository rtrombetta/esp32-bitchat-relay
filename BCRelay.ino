/*
 * BitChat Relay – HUB (GATT Server only, NimBLE)
 * Objetivo: simples — recebe por WRITE/WRITE_NR e repassa para todos os centrals via NOTIFY.
 *
 * Porta para NimBLE-Arduino >= 2.3.6
 *  - Peripheral-only (sem scan); mantém advertising para multi-central
 *  - Notify-only (sem Indicate)
 *  - TTL--, dedup (FNV-1a), fragmentação/reassembly
 *  - Fila de TX + token bucket (rate limit)
 *  - ANNOUNCE periódico com jitter
 *  - CAP dinâmico (min MTU negociado); fallback seguro
 *  - Logs em inglês
 *
 * Extras desta versão:
 *  - Fan-out por conexão com notify(connHandle) e SEM eco para o remetente
 *  - Log [PKT] só após dedup (não polui com duplicados)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include <map>
#include <vector>
#include <queue>
#include <deque>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>

// ===== BitChat UUIDs (placeholders observados) =====
#define BITCHAT_SERVICE_UUID_MAINNET  "F47B5E2D-4A9E-4C5A-9B3F-8E1D2C3A4B5C"
#define BITCHAT_CHARACTERISTIC_UUID   "A1B2C3D4-E5F6-4A5B-8C9D-0E1F2A3B4C5D"

// ===== Tipos =====
#define ANNOUNCE_TYPE                 0x01
#define FRAGMENT_TYPE                 0x06
#define MIN_PKT_LEN                   21

// ===== Timers / tamanhos =====
#define ANNOUNCE_INTERVAL_MS          30000UL
#define REQUESTED_MTU                 517
#define DEFAULT_MTU                   23
#define FALLBACK_CAP                  20     // ATT 23 -> 20 payload
#define MIN_CHUNK_SIZE                64
#define FRAG_LIFETIME_MS              30000UL
#define MAX_INFLIGHT_FRAGS            10
#define MAX_INFLIGHT_BYTES            65536UL

// ---- Novos limites ----
// Limite global já existe (MAX_INFLIGHT_*). Agora cota por remetente:
#define MAX_INFLIGHT_BYTES_PER_SENDER 16384UL   // ~16 KB por sender
#define MAX_FRAGS_PER_SENDER          4         // no máx 4 assemblies por sender
// Limite de fila de TX (substitui literal "128"):
#define MAX_TX_QUEUE                  128

// ===== LED (opcional) =====
#ifndef LED_PIN
#define LED_PIN 2
#endif
#define LED_RX_ON_MS 30
#define LED_TX_ON_MS 30
#define LED_TX_GAP_MS 60

// ===== Debug level =====
static uint8_t debugLevel = 3;
#define DBG(lvl, msg) do{ if(debugLevel>=lvl){ Serial.print("[DBG");Serial.print(lvl);Serial.print("] ");Serial.println(msg);} }while(0)

// ===== Estado BLE =====
static NimBLEServer*         gServer = nullptr;
static NimBLECharacteristic* gChar   = nullptr;   // única char RX/TX (read/write/notify)
static std::map<uint16_t, uint16_t> gPeerMtus;    // connHandle -> MTU
static std::unordered_map<uint16_t, bool> gSubscribed; // connHandle -> quer notify
static uint32_t gLastAnnounce = 0;

// ===== Identidade do sender (8 bytes) =====
static uint8_t gSenderID[8];

// ===== Stats =====
static uint32_t gPktsIn=0, gPktsOut=0, gBytesIn=0, gBytesOut=0;
static uint32_t gWrites=0, gNotifies=0, gLastStats=0;
static uint32_t gDropsDedup=0, gDropsBackpressure=0;
static uint32_t gType1In=0, gType2In=0;

// ===== LED state =====
static bool ledState=false;
static uint32_t rxBlinkUntil=0;
static uint8_t txBlinkPhase=0;
static uint32_t txPhaseUntil=0;

// ===== Dedup (FNV-1a 64-bit) =====
static inline uint64_t fnv1a64(const uint8_t* d, size_t n){
  uint64_t h=1469598103934665603ULL;
  for(size_t i=0;i<n;i++){ h^=d[i]; h*=1099511628211ULL; }
  return h;
}
struct DedupEntry { uint64_t h; uint64_t tms; };
static std::deque<DedupEntry> gSeenDeque;
static std::unordered_set<uint64_t> gSeenSet;
static const size_t   DEDUP_MAX=1024;
static const uint32_t DEDUP_TTL_MS=60000;

// ===== Reassembly =====
struct FragmentKey {
  uint64_t sender;
  uint64_t fragID;
  bool operator<(const FragmentKey& o) const {
    if (sender!=o.sender) return sender<o.sender;
    return fragID<o.fragID;
  }
};
static std::map<FragmentKey, std::map<uint16_t, std::vector<uint8_t>>> incomingFragments;
static std::map<FragmentKey, std::pair<uint8_t, uint64_t>> fragmentMetadata;
static size_t gInflightBytes=0;

// Contadores por sender (para evitar monopolização de RAM)
static std::unordered_map<uint64_t, size_t>  gInflightBytesBySender;
static std::unordered_map<uint64_t, uint16_t> gInflightFragsBySender;

// ===== TX queue / rate limit =====
struct TxItem{ std::vector<uint8_t> pkt; int origin = -1; };
static std::queue<TxItem> gTxQ;
static SemaphoreHandle_t gTxMutex;
static uint32_t gNotifyBudgetPerSec = 150;   // orç. de notifies/seg (aprox.)
static uint32_t gNotifyTokens = 0;
static uint32_t gLastTokenRefill = 0;

// ===== Utils =====
static inline uint64_t nowUs(){ return (uint64_t)esp_timer_get_time(); }
static inline uint64_t nowMs(){ return nowUs()/1000ULL; }
static inline void writeU64BE(uint8_t* dst, uint64_t v){ for(int i=7;i>=0;--i) dst[7-i]=(v>>(i*8))&0xFF; }
static void hex8(const uint8_t* p, char* out17){
  static const char* H="0123456789abcdef";
  for(int i=0;i<8;i++){ out17[i*2]=H[(p[i]>>4)&0xF]; out17[i*2+1]=H[p[i]&0xF]; } out17[16]='\0';
}
static void makeSenderID(){
  uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_BT);
  for(int i=0;i<6;i++) gSenderID[i]=mac[i];
  uint16_t s=(uint16_t)(ESP.getEfuseMac()&0xFFFF);
  gSenderID[6]=(uint8_t)(s>>8); gSenderID[7]=(uint8_t)s;
}
static inline void ledOn(){ if(!ledState){ ledState=true; digitalWrite(LED_PIN, HIGH);} }
static inline void ledOff(){ if(ledState){ ledState=false; digitalWrite(LED_PIN, LOW);} }
static void kickRxBlink(){ ledOn(); rxBlinkUntil = millis()+LED_RX_ON_MS; }
static void kickTxBlink(){ txBlinkPhase=1; ledOn(); txPhaseUntil = millis()+LED_TX_ON_MS; }

// Dedup housekeeping
static void dedupCleanup(){
  uint64_t now = nowMs();
  while(!gSeenDeque.empty() && (now - gSeenDeque.front().tms) > DEDUP_TTL_MS){
    gSeenSet.erase(gSeenDeque.front().h);
    gSeenDeque.pop_front();
  }
}
static bool dedupSeen(uint64_t h){
  dedupCleanup();
  if(gSeenSet.find(h)!=gSeenSet.end()) return true;
  if(gSeenDeque.size()>=DEDUP_MAX){
    gSeenSet.erase(gSeenDeque.front().h);
    gSeenDeque.pop_front();
  }
  gSeenDeque.push_back({h, nowMs()});
  gSeenSet.insert(h);
  return false;
}

// CAP = min(MTU) - 3
static size_t capServer(){
  if (gPeerMtus.empty()) return FALLBACK_CAP; // 20 bytes
  uint16_t minMtu = 0xFFFF;
  for (auto &kv : gPeerMtus) if (kv.second < minMtu) minMtu = kv.second;
  if (minMtu < DEFAULT_MTU) minMtu = DEFAULT_MTU;
  size_t cap = (minMtu > 3) ? (minMtu - 3) : FALLBACK_CAP;
  if (cap < FALLBACK_CAP) cap = FALLBACK_CAP;
  return cap;
}

// Enfileirar pacote (sem bloquear)
static void txEnqueueFrom(const uint8_t* p, size_t n, int origin){
  if(!p || n==0) return;
  TxItem it; it.pkt.assign(p, p+n); it.origin = origin;
  xSemaphoreTake(gTxMutex, portMAX_DELAY);
  //if(gTxQ.size()<128) gTxQ.push(std::move(it)); else gDropsBackpressure++;
  if(gTxQ.size() < MAX_TX_QUEUE) {
    gTxQ.push(std::move(it));
  } else {
    // Descarta o MAIS ANTIGO e insere o novo (mantém fluxo atual)
    gTxQ.pop();
    gTxQ.push(std::move(it));
    gDropsBackpressure++;
  }
  xSemaphoreGive(gTxMutex);
}
static void txEnqueue(const uint8_t* p, size_t n){ txEnqueueFrom(p,n,-1); }

// Worker de TX: rate-limited; fan-out por conexão; sem eco
// Worker de TX: token-bucket global, fan-out por conexão, fragmentação por peer (sem eco)
static void txWorker(void*) {
  for (;;) {
    uint32_t ms = millis();
    if (ms - gLastTokenRefill >= 1000) {
      gNotifyTokens = gNotifyBudgetPerSec;
      gLastTokenRefill = ms;
    }

    // Pegar item da fila (se houver token disponível)
    xSemaphoreTake(gTxMutex, portMAX_DELAY);
    bool has = !gTxQ.empty() && gNotifyTokens > 0;
    TxItem it;
    if (has) {
      it = std::move(gTxQ.front());
      gTxQ.pop();
      gNotifyTokens--;
    }
    xSemaphoreGive(gTxMutex);

    if (!has || !gChar) {
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }

    // Fan-out para inscritos (sem eco), com MTU/capacidade por conexão
    bool anySent = false;
    for (auto &kv : gSubscribed) {
      uint16_t h = kv.first;
      if (!kv.second) continue;                    // não quer notify
      if (it.origin >= 0 && h == (uint16_t)it.origin) continue; // evita eco

      // MTU/cap por peer
      uint16_t mtu = DEFAULT_MTU;
      auto itM = gPeerMtus.find(h);
      if (itM != gPeerMtus.end()) mtu = itM->second;
      size_t capPeer = (mtu > 3) ? (mtu - 3) : FALLBACK_CAP;

      // Cabe direto neste peer?
      if (it.pkt.size() <= capPeer) {
        gChar->setValue(it.pkt.data(), it.pkt.size());
        if (gChar->notify(h)) {
          gNotifies++; gPktsOut++; gBytesOut += it.pkt.size();
          anySent = true;
        }
        continue;
      }

      // Precisa fragmentar para ESTE peer?
      if (capPeer < 32) {
        // inviável (overhead de 19+13 já passa do limite típico do ATT23)
        if (debugLevel >= 2) {
          char sid[17]; hex8(&it.pkt[11], sid);
          Serial.printf("[REL] drop (cap<32) len=%u cap=%u peer=%u sid=%s\r\n",
                        (unsigned)it.pkt.size(), (unsigned)capPeer, (unsigned)h, sid);
        }
        continue;
      }

      // Fragmentação por peer: FRAGMENT_TYPE com (fragID, idx, total, origType)
      const size_t overhead = 19 + 13;                 // header bitchat (19) + frag hdr (13)
      size_t maxChunk = (capPeer > overhead) ? (capPeer - overhead) : 0;
      if (maxChunk == 0) {
        if (debugLevel >= 2) {
          char sid[17]; hex8(&it.pkt[11], sid);
          Serial.printf("[REL] drop (cap<=overhead) len=%u cap=%u peer=%u sid=%s\r\n",
                        (unsigned)it.pkt.size(), (unsigned)capPeer, (unsigned)h, sid);
        }
        continue;
      }

      uint8_t origType = it.pkt[1];
      uint8_t fragID[8]; for (int i = 0; i < 8; i++) fragID[i] = (uint8_t)esp_random();
      size_t totalFrags = (it.pkt.size() + maxChunk - 1) / maxChunk;

      for (size_t off = 0, idx = 0; off < it.pkt.size(); off += maxChunk, idx++) {
        size_t clen = std::min(maxChunk, it.pkt.size() - off);
        // 19 bytes de header base
        //   [0]=0x01, [1]=FRAGMENT_TYPE, [2]=TTL (preserva do original)
        //   [3..10]=timestamp(8), [11..18]=senderID(8)
        // + 13 bytes de header de fragmento:
        //   [19..26]=fragID(8), [27..28]=idx(2), [29..30]=total(2), [31]=origType
        uint8_t pkt[600];
        pkt[0]  = 0x01;
        pkt[1]  = FRAGMENT_TYPE;
        pkt[2]  = it.pkt[2];                           // TTL preservado (mutável por hop)
        memcpy(pkt + 3,  it.pkt.data() + 3,  8);       // timestamp
        memcpy(pkt + 11, it.pkt.data() + 11, 8);       // senderID

        uint8_t* fp = pkt + 19;
        memcpy(fp, fragID, 8);
        uint16_t idx16  = (uint16_t)idx;
        uint16_t tot16  = (uint16_t)totalFrags;
        fp[8]  = (idx16 >> 8) & 0xFF; fp[9]  = idx16 & 0xFF;
        fp[10] = (tot16 >> 8) & 0xFF; fp[11] = tot16 & 0xFF;
        fp[12] = origType;

        memcpy(fp + 13, it.pkt.data() + off, clen);
        size_t outLen = 19 + 13 + clen;

        gChar->setValue(pkt, outLen);
        if (gChar->notify(h)) {
          gNotifies++; gPktsOut++; gBytesOut += outLen;
          anySent = true;
        }
        // pacing leve por peer para não engasgar
        //vTaskDelay(pdMS_TO_TICKS(8)); // ~8ms; pode aleatorizar se quiser
        // pacing leve por peer com jitter (evita lock-step entre conexões)
        uint32_t d = 4 + (esp_random() % 7); // 4..10 ms
        vTaskDelay(pdMS_TO_TICKS(d));
      }

      if (debugLevel >= 3) {
        char sid[17]; hex8(&it.pkt[11], sid);
        Serial.printf("[REL] fragmented len=%u cap=%u peers=%u peer=%u sid=%s\r\n",
                      (unsigned)it.pkt.size(), (unsigned)capPeer,
                      (unsigned)gPeerMtus.size(), (unsigned)h, sid);
      }
    } // for peers

    if (!anySent) {
      // ninguém recebeu (todos cap<32 ou sem inscritos): conta como backpressure “virtual”
      gDropsBackpressure++;
      vTaskDelay(pdMS_TO_TICKS(2));
    } else {
      // blink opcional (se você usa)
      kickTxBlink();
    }
  }
}


// ===== Announce =====
static uint32_t gAnnounceJitter=500;
static uint32_t nextAnnounceDue(){ return ANNOUNCE_INTERVAL_MS + gAnnounceJitter; }
static void refreshAnnounceJitter(){ gAnnounceJitter = 500 + (esp_random()%4500); }
static void sendAnnounce(){
  uint8_t buf[51];
  buf[0]=0x01; buf[1]=ANNOUNCE_TYPE; buf[2]=7;
  writeU64BE(&buf[3], nowMs());
  memcpy(&buf[11], gSenderID, 8);
  memset(&buf[19], 0, 32);
  txEnqueue(buf, sizeof(buf));
  gLastAnnounce = millis();
  refreshAnnounceJitter();
  DBG(3, "Announce enqueued");
}

// Limpeza de reassembly
static void cleanupFragments(){
  uint64_t now = nowMs();
  for(auto it=fragmentMetadata.begin(); it!=fragmentMetadata.end(); ){
    if(now - it->second.second > FRAG_LIFETIME_MS){
      auto mapIt = incomingFragments.find(it->first);
      if(mapIt!=incomingFragments.end()){
        //for(auto &kv: mapIt->second) gInflightBytes -= kv.second.size();

        uint64_t sender = it->first.sender;
        for(auto &kv: mapIt->second) {
          gInflightBytes -= kv.second.size();
          gInflightBytesBySender[sender] -= kv.second.size();
        }

        incomingFragments.erase(mapIt);
      }

      // um assembly deste sender expirou
      if (gInflightFragsBySender[it->first.sender] > 0) gInflightFragsBySender[it->first.sender]--;

      it = fragmentMetadata.erase(it);
    } else ++it;
  }
}

// Enfileira (direto ou fragmenta) — com origem
static void sendFragmentedNotifyFrom(const uint8_t* orig, size_t origLen, size_t cap, int origin){
  size_t maxChunk = cap>=32? cap-32 : MIN_CHUNK_SIZE;
  size_t nfr = (origLen + maxChunk - 1)/maxChunk;
  if(nfr==0 || nfr>65535 || cap<32){ DBG(2,"Invalid fragmentation, drop"); return; }
  uint8_t origType = orig[1];
  uint8_t fragID[8]; for(int i=0;i<8;i++) fragID[i]=(uint8_t)esp_random();
  for(size_t idx=0; idx<nfr; idx++){
    size_t off = idx*maxChunk, clen = std::min(maxChunk, origLen-off);
    uint8_t pkt[600];
    pkt[0]=0x01; pkt[1]=FRAGMENT_TYPE; pkt[2]=orig[2];
    memcpy(pkt+3,  orig+3,  8);
    memcpy(pkt+11, orig+11, 8);
    uint8_t* fp = pkt+19;
    memcpy(fp, fragID, 8);
    fp[8]=(idx>>8)&0xFF; fp[9]=idx&0xFF;
    fp[10]=(nfr>>8)&0xFF; fp[11]=nfr&0xFF;
    fp[12]=origType;
    memcpy(fp+13, orig+off, clen);
    size_t len = 19+13+clen;
    txEnqueueFrom(pkt, len, origin);
  }
}

static void relayToCentrals(const uint8_t* pkt, size_t len, int origin){
  size_t cap = capServer();
  if(len <= cap){
    txEnqueueFrom(pkt, len, origin);
    if(debugLevel >= 3){
      char sid[17]; hex8(&pkt[11], sid);
      Serial.printf("[REL] direct len=%u cap=%u peers=%u sid=%s\r\n",
        (unsigned)len, (unsigned)cap, (unsigned)gPeerMtus.size(), sid);
    }
  } else {
    if (cap >= 32) {
      sendFragmentedNotifyFrom(pkt, len, cap, origin);
      if (debugLevel >= 3) {
        char sid[17]; hex8(&pkt[11], sid);
        Serial.printf("[REL] fragmented len=%u cap=%u peers=%u sid=%s\r\n",
          (unsigned)len, (unsigned)cap, (unsigned)gPeerMtus.size(), sid);
      }
    } else {
      if (debugLevel >= 2) {
        char sid[17]; hex8(&pkt[11], sid);
        Serial.printf("[REL] drop (cap<32) len=%u cap=%u peers=%u sid=%s\r\n",
          (unsigned)len, (unsigned)cap, (unsigned)gPeerMtus.size(), sid);
      }
    }
  }
}

// ===== Callbacks NimBLE 2.3.6 =====
class SvrCallbacks: public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
    gPeerMtus[info.getConnHandle()] = DEFAULT_MTU;
    DBG(3, String("Central connected (ch=") + String(info.getConnHandle()) + ")");
    NimBLEDevice::startAdvertising();
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int reason) override {
    gPeerMtus.erase(info.getConnHandle());
    gSubscribed.erase(info.getConnHandle());
    DBG(3, String("Central disconnected (ch=") + String(info.getConnHandle()) + String(", reason=") + String(reason) + ")");
    NimBLEDevice::startAdvertising();
  }
  void onMTUChange(uint16_t mtu, NimBLEConnInfo& info) override {
    gPeerMtus[info.getConnHandle()] = mtu;
    DBG(3, String("MTU updated (ch=") + String(info.getConnHandle()) + String(", mtu=") + String(mtu) + ")");
  }
};

class CharCallbacks: public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo& info, uint16_t subVal) override {
    bool wantsNotify = (subVal & 0x0001);
    gSubscribed[info.getConnHandle()] = wantsNotify;
    DBG(3, String("Subscribe ch=") + String(info.getConnHandle()) + String(" notify=") + String(wantsNotify));
  }

  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    std::string v = c->getValue();
    const uint8_t* p = (const uint8_t*)v.data();
    size_t len = v.size();
    if(len<MIN_PKT_LEN || p[0]!=0x01) return;

    // Dedup primeiro (hash de header com TTL zerado + 32B do payload)
    size_t hdr = std::min(len, (size_t)19);
    uint8_t hdrNoTTL[19]; memcpy(hdrNoTTL, p, hdr); if (hdr >= 3) hdrNoTTL[2] = 0;
    size_t extra = (len > hdr) ? std::min(len - hdr, (size_t)32) : 0;
    uint64_t h = fnv1a64(hdrNoTTL, hdr); if (extra) h ^= fnv1a64(p + hdr, extra);

    if (dedupSeen(h)) { 
      gDropsDedup++; 
      if(debugLevel>=2){
        uint8_t ttl=p[2]; uint64_t ts=0; for(int i=0;i<8;i++) ts=(ts<<8)|p[3+i];
        char sid[17]; hex8(&p[11], sid);
        Serial.printf("[PKT] **dropped** type=%u ttl=%u len=%u ts=%llu sid=%s\r\n",
                    p[1], ttl, (unsigned)len, (unsigned long long)ts, sid);
      }
      return; 
    }

    if(debugLevel>=2){
      uint8_t ttl=p[2]; uint64_t ts=0; for(int i=0;i<8;i++) ts=(ts<<8)|p[3+i];
      char sid[17]; hex8(&p[11], sid);
      Serial.printf("[PKT] in type=%u ttl=%u len=%u ts=%llu sid=%s\r\n",
                    p[1], ttl, (unsigned)len, (unsigned long long)ts, sid);
    }

    if (debugLevel >= 4) {
      char sid[17]; hex8(&p[11], sid);
      uint8_t sniff[8] = {0};
      size_t sniffLen = (len > 27) ? 8 : (len > 19 ? len - 19 : 0);
      if (sniffLen) memcpy(sniff, p + 19, sniffLen);
      Serial.printf("[SNIFF] sid=%s type=%u len=%u ttl=%u p0=%02X p1=%02X p2=%02X p3=%02X p4=%02X p5=%02X p6=%02X p7=%02X\r\n",
        sid, p[1], (unsigned)len, p[2],
        sniff[0],sniff[1],sniff[2],sniff[3],sniff[4],sniff[5],sniff[6],sniff[7]);
    }

    gWrites++; gPktsIn++; gBytesIn+=len; if(p[1]==1) gType1In++; else if(p[1]==2) gType2In++;
    kickRxBlink();

    static uint8_t buf[600]; if(len>sizeof(buf)) return; memcpy(buf, p, len);
// TTL handling: drop when zero and after decrement if it hits zero
if (buf[2] == 0) { if (debugLevel>=2) Serial.println("[DROP] ttl=0 before relay"); return; }
buf[2]--; /* TTL-- */
if (buf[2] == 0) { if (debugLevel>=2) Serial.println("[DROP] ttl expired after --"); return; }

    if(p[1]==FRAGMENT_TYPE){
      if(len < 19+13) return;
      uint64_t sender=0; for(int i=0;i<8;i++) sender=(sender<<8)|p[11+i];
      uint64_t fragID=0; for(int i=0;i<8;i++) fragID=(fragID<<8)|p[19+i];
      uint16_t index=(p[27]<<8)|p[28], total=(p[29]<<8)|p[30];
      uint8_t origType=p[31]; size_t chunkLen = len-32; if(total==0 || index>=total) return;

      FragmentKey key{sender, fragID};
      if(incomingFragments.find(key)==incomingFragments.end()){

        // Enforce cota por remetente ANTES de criar assembly
        size_t bs = gInflightBytesBySender[sender];
        uint16_t fs = gInflightFragsBySender[sender];
        if (fs >= MAX_FRAGS_PER_SENDER || (bs + chunkLen) > MAX_INFLIGHT_BYTES_PER_SENDER) {
          // Tenta liberar o assembly MAIS ANTIGO deste sender
          FragmentKey oldestForSender{0,0}; bool found=false;
          uint64_t oldestTs = UINT64_MAX;
          for (auto &kvMeta : fragmentMetadata) {
            if (kvMeta.first.sender == sender && kvMeta.second.second < oldestTs) {
              oldestTs = kvMeta.second.second; oldestForSender = kvMeta.first; found=true;
            }
          }
          if (found) {
            auto mapIt2 = incomingFragments.find(oldestForSender);
            if (mapIt2 != incomingFragments.end()) {
              for (auto &kv : mapIt2->second) {
                gInflightBytes -= kv.second.size();
                gInflightBytesBySender[sender] -= kv.second.size();
              }
              incomingFragments.erase(mapIt2);
            }
            fragmentMetadata.erase(oldestForSender);
            if (gInflightFragsBySender[sender] > 0) gInflightFragsBySender[sender]--;
          } else {
            // Sem nada para liberar deste sender: descarta este fragmento
            return;
          }
        }
        
        if(incomingFragments.size()>=MAX_INFLIGHT_FRAGS || (gInflightBytes+chunkLen)>MAX_INFLIGHT_BYTES){
          auto oldest = fragmentMetadata.begin();
          for (auto it2 = fragmentMetadata.begin(); it2 != fragmentMetadata.end(); ++it2)
            if (it2->second.second < oldest->second.second) oldest = it2;
          auto mapIt = incomingFragments.find(oldest->first);
          //if (mapIt != incomingFragments.end()) { for (auto &kv : mapIt->second) gInflightBytes -= kv.second.size(); incomingFragments.erase(mapIt); }

          if (mapIt != incomingFragments.end()) {
            // Ajusta globais e POR SENDER
            uint64_t oldSender = oldest->first.sender;
            for (auto &kv : mapIt->second) {
              gInflightBytes -= kv.second.size();
              gInflightBytesBySender[oldSender] -= kv.second.size();
            }
            incomingFragments.erase(mapIt);
            if (gInflightFragsBySender[oldSender] > 0) gInflightFragsBySender[oldSender]--;
          }

          fragmentMetadata.erase(oldest);
        }
        //incomingFragments[key] = {}; fragmentMetadata[key] = {origType, nowMs()};

        incomingFragments[key] = {};
        fragmentMetadata[key] = {origType, nowMs()};
        gInflightFragsBySender[sender]++; // novo assembly deste sender

      }
      if(incomingFragments[key].find(index)==incomingFragments[key].end()){
        //incomingFragments[key][index] = std::vector<uint8_t>(p+32, p+len); gInflightBytes += chunkLen;

        incomingFragments[key][index] = std::vector<uint8_t>(p+32, p+len);
        gInflightBytes += chunkLen;
        gInflightBytesBySender[sender] += chunkLen;

      }
      if (incomingFragments[key].size() == total) {
        size_t totalPay = 0; for (uint16_t i = 0; i < total; i++) totalPay += incomingFragments[key][i].size();
        //if (totalPay > 4096) { for (auto &kv : incomingFragments[key]) gInflightBytes -= kv.second.size(); incomingFragments.erase(key); fragmentMetadata.erase(key); return; }

        if (totalPay > 4096) {
          // muito grande: descarta reassembly, ajustando contadores por sender
          for (auto &kv : incomingFragments[key]) {
            gInflightBytes -= kv.second.size();
            gInflightBytesBySender[sender] -= kv.second.size();
          }
          incomingFragments.erase(key);
          fragmentMetadata.erase(key);
          if (gInflightFragsBySender[sender] > 0) gInflightFragsBySender[sender]--;
          return;
        }

        // Reassembly: concatenate original bytes exactly (no header rebuild)
        std::vector<uint8_t> whole; whole.reserve(totalPay);
        for (uint16_t i = 0; i < total; i++) { auto &ch = incomingFragments[key][i]; whole.insert(whole.end(), ch.begin(), ch.end()); }
        //for (auto &kv : incomingFragments[key]) gInflightBytes -= kv.second.size();
        //incomingFragments.erase(key); fragmentMetadata.erase(key);

        // Finaliza: limpa contadores global e por sender
        for (auto &kv : incomingFragments[key]) {
          gInflightBytes -= kv.second.size();
          gInflightBytesBySender[sender] -= kv.second.size();
        }
        incomingFragments.erase(key);
        fragmentMetadata.erase(key);
        if (gInflightFragsBySender[sender] > 0) gInflightFragsBySender[sender]--;

        relayToCentrals(whole.data(), whole.size(), (int)info.getConnHandle());
      }
    } else {
      relayToCentrals(buf, len, (int)info.getConnHandle());
    }
  }
};

// ===== Setup BLE (server) =====
static void setup_ble_server(){
  uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_BT);
  char suf[7]; sprintf(suf, "%02X%02X%02X", mac[3], mac[4], mac[5]);
  String devName="BitChatRelay_"; devName+=suf;

  NimBLEDevice::init(devName.c_str());
  NimBLEDevice::setPower(9);          // dBm
  NimBLEDevice::setMTU(REQUESTED_MTU);

  gServer = NimBLEDevice::createServer();
  gServer->setCallbacks(new SvrCallbacks());

  NimBLEService* svc = gServer->createService(BITCHAT_SERVICE_UUID_MAINNET);
  gChar = svc->createCharacteristic(
    BITCHAT_CHARACTERISTIC_UUID,
    NIMBLE_PROPERTY::READ    |
    NIMBLE_PROPERTY::WRITE   |
    NIMBLE_PROPERTY::WRITE_NR|
    NIMBLE_PROPERTY::NOTIFY
  );
  gChar->setCallbacks(new CharCallbacks());
  gChar->setValue("relay ready");
  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setName(devName.c_str());
  adv->addServiceUUID(BITCHAT_SERVICE_UUID_MAINNET);
  adv->enableScanResponse(true);
  //adv->setPreferredParams(0x06, 0x06);
  adv->setPreferredParams(0x50, 0xA0); // 50ms to 100ms (ms = param * 0.625)
  NimBLEDevice::startAdvertising();
  gServer->advertiseOnDisconnect(true);

  DBG(1, "Peripheral ready (HUB). Advertising service.");
}

// ===== Arduino entry points =====
void setup(){
  Serial.begin(115200);
  delay(100);
  WiFi.mode(WIFI_OFF);
  pinMode(LED_PIN, OUTPUT); ledOff();

  makeSenderID();
  setup_ble_server();

  gTxMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(txWorker, "txw", 4096, nullptr, 3, nullptr, 0);
  gLastTokenRefill = millis(); gNotifyTokens = gNotifyBudgetPerSec;

  refreshAnnounceJitter();
  sendAnnounce();
  DBG(1, "Setup done.");
  gLastStats = millis();
}

void loop(){
  // LED RX blink
  if(rxBlinkUntil && millis()>=rxBlinkUntil){ ledOff(); rxBlinkUntil=0; }

  // LED TX double-blink
  if(txBlinkPhase){
    uint32_t now=millis();
    if(now>=txPhaseUntil){
      if(txBlinkPhase==1){ ledOff(); txBlinkPhase=2; txPhaseUntil=now+LED_TX_GAP_MS; }
      else if(txBlinkPhase==2){ ledOn(); txBlinkPhase=3; txPhaseUntil=now+LED_TX_ON_MS; }
      else { ledOff(); txBlinkPhase=0; }
    }
  }

  // Periodic announce
  if(millis()-gLastAnnounce > nextAnnounceDue()) sendAnnounce();

  // Housekeeping + stats a cada 10s
  if(millis()-gLastStats > 10000UL){
    gLastStats = millis();
    cleanupFragments();

    size_t cap = capServer();
    size_t qsz; xSemaphoreTake(gTxMutex, portMAX_DELAY); qsz=gTxQ.size(); xSemaphoreGive(gTxMutex);

    String mtuList="MTUs: ";
    for(auto &kv: gPeerMtus) mtuList+="ch="+String(kv.first)+":mtu="+String(kv.second)+" ";

    uint32_t centrals = gPeerMtus.size();
    uint32_t subs = 0; for (auto &kv: gSubscribed) if (kv.second) subs++;

    Serial.println("=== BitChat Relay Status ===");
    Serial.printf("[STAT] pktsIn=%u bytesIn=%u pktsOut=%u bytesOut=%u writes=%u notifies=%u heap=%u minCap=%u q=%u tokens=%u dedupWin=%u drops{dedup=%u,backp=%u} inflightB=%u t1_in=%u t2_in=%u peers=%u subs{notify=%u} %s\r\n",
      gPktsIn, gBytesIn, gPktsOut, gBytesOut, gWrites, gNotifies,
      ESP.getFreeHeap(), (unsigned)cap, (unsigned)qsz, (unsigned)gNotifyTokens,
      (unsigned)gSeenDeque.size(), (unsigned)gDropsDedup, (unsigned)gDropsBackpressure,
      (unsigned)gInflightBytes, gType1In, gType2In,
      centrals, subs, mtuList.c_str());
    Serial.println("=== End Status ===");
  }

  delay(5);
}
