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
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include <map>
#include <vector>
#include <queue>
#include <deque>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include "espn.h"

// Version
#define VERSION "1.0.1"

// ===== BitChat UUIDs (placeholders) =====
#define BITCHAT_SERVICE_UUID_MAINNET  "F47B5E2D-4A9E-4C5A-9B3F-8E1D2C3A4B5C"
#define BITCHAT_CHARACTERISTIC_UUID   "A1B2C3D4-E5F6-4A5B-8C9D-0E1F2A3B4C5D"

// ===== Connection Limits =====
#define MAX_PEERS                     8   // max BLE connections
#define BLE_POWER                     9   // BLE Power level dBm (0..9 typical)

// ===== Types =====
#define ANNOUNCE_TYPE                 0x01  // BitChat announce
#define FRAGMENT_TYPE                 0x06  // BitChat fragment
#define MIN_PKT_LEN                   21    // 1+8+8+2+1+1 (type+sender+fragID+seq/ttl+ttl+data)

// ===== Timers / Sizes =====
#define ANNOUNCE_INTERVAL_MS          30000UL // 30s
#define REQUESTED_MTU                 517     // max MTU
#define DEFAULT_MTU                   23      // ATT default MTU
#define FALLBACK_CAP                  20      // ATT 23 -> 20 payload
#define MIN_CHUNK_SIZE                64      // min fragment size (avoid excessive fragmentation)
#define FRAG_LIFETIME_MS              30000UL // time to wait for all fragments
#define MAX_INFLIGHT_FRAGS            10      // max assemblies in progress
#define MAX_INFLIGHT_BYTES            65536UL // ~64 KB total inflight

// ===== Backhaul (ESPNOW) =====
#define ESP_BH                        true    // enable ESPNOW backhaul
#define ESP_BH_CHANNEL                13      // Wi-Fi channel (1/6/11 typical)
#define ESP_BH_LINK_MTU               230     // ESPNOW MTU (<=250 safe)
#define ESP_BH_TX_CORE                1       // core for TX (0/1 or -1 no affinity)
#define ESP_BH_RX_CORE                0       // core for RX (0/1 or -1 no affinity)
#define ESP_BH_BALANCED_SPLIT         true    // balanced fragment sizes

// ---- Sender Limits ----
#define MAX_INFLIGHT_BYTES_PER_SENDER 16384UL   // ~16 KB each sender
#define MAX_FRAGS_PER_SENDER          4         // max assemblies per sender
#define MAX_TX_QUEUE                  128       // max TX queue size (global, shared by all senders)

// ===== LED  =====
#ifndef LED_PIN             // built-in LED 
#define LED_PIN 2           // GPIO2 (D2) on most ESP32 boards
#endif
#define LED_RX_ON_MS 30     // time to keep LED on on RX
#define LED_TX_ON_MS 30     // time to keep LED on on TX
#define LED_TX_GAP_MS 60    // time to keep LED off between blinks

// ===== Debug level =====
static uint8_t debugLevel = 3;

// ===== Serial =====
SemaphoreHandle_t gSerialMutex;

// ==== ESPNOW backhaul =====
static espn gEspNow;
static const uint8_t NET_ID[espn::NET_ID_LEN] = { 'B','C','M','E','S','H','0','1' }; // "BCMESH01"

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
static bool     ledState=false;
static uint32_t rxBlinkUntil=0;
static uint8_t  txBlinkPhase=0;
static uint32_t txPhaseUntil=0;

// ===== Dedup (FNV-1a 64-bit) =====
static inline uint64_t fnv1a64(const uint8_t* d, size_t n){
  uint64_t h=1469598103934665603ULL;
  for(size_t i=0;i<n;i++){ 
    h^=d[i]; 
    h*=1099511628211ULL; 
  }
  return h;
}

struct DedupEntry { 
  uint64_t h; 
  uint64_t tms; 
};

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
// Time in microseconds
static inline uint64_t nowUs(){ 
  return (uint64_t)esp_timer_get_time(); 
}

void safePrintf(const char* fmt, ...){
  va_list args; 
  va_start(args, fmt);
  if (gSerialMutex)  {
    xSemaphoreTake(gSerialMutex, portMAX_DELAY);
    vprintf(fmt, args);
    xSemaphoreGive(gSerialMutex);
  } else vprintf(fmt, args);
  va_end(args);
}

// Time in milliseconds
static inline uint64_t nowMs(){ 
  return nowUs()/1000ULL; 
}

// Write big-endian u64
static inline void writeU64BE(uint8_t* dst, uint64_t v){ 
  for(int i=7;i>=0;--i) dst[7-i]=(v>>(i*8))&0xFF; 
}

// Write big-endian u32
static void hex8(const uint8_t* p, char* out17){
  static const char* H="0123456789abcdef";
  for(int i=0;i<8;i++){ 
    out17[i*2]=H[(p[i]>>4)&0xF]; 
    out17[i*2+1]=H[p[i]&0xF]; 
  } 
  out17[16]='\0';
}

// Generate sender ID from MAC + efuse
static void makeSenderID(){
  uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_BT);
  for(int i=0;i<6;i++) gSenderID[i]=mac[i];
  uint16_t s=(uint16_t)(ESP.getEfuseMac()&0xFFFF);
  gSenderID[6]=(uint8_t)(s>>8); 
  gSenderID[7]=(uint8_t)s;
}

// Turn LED on
static inline void ledOn(){ 
  if(!ledState){ 
    ledState=true; 
    digitalWrite(LED_PIN, HIGH);
  } 
}

// Turn LED off
static inline void ledOff(){ 
  if(ledState){ 
    ledState=false; 
    digitalWrite(LED_PIN, LOW);
  } 
}

// LED state machine (call periodically)
static void kickRxBlink(){ 
  ledOn(); 
  rxBlinkUntil = millis()+LED_RX_ON_MS; 
}

// LED state machine (call periodically)
static void kickTxBlink(){ 
  txBlinkPhase=1; 
  ledOn(); 
  txPhaseUntil = millis()+LED_TX_ON_MS; 
}

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

// Enqueue (with origin)
static void txEnqueueFrom(const uint8_t* p, size_t n, int origin){
  if(!p || n==0) return;
  TxItem it; it.pkt.assign(p, p+n); it.origin = origin;
  xSemaphoreTake(gTxMutex, portMAX_DELAY);
  if(gTxQ.size() < MAX_TX_QUEUE) {
    gTxQ.push(std::move(it));
  } else {
    // drop oldest
    gTxQ.pop();
    gTxQ.push(std::move(it));
    gDropsBackpressure++;
  }
  xSemaphoreGive(gTxMutex);
}

// Enqueue (no origin)
static void txEnqueue(const uint8_t* p, size_t n){ 
  txEnqueueFrom(p,n,-1); 
}

#if ESP_BH  
// Worker de RX do ESPNOW
static void bhRxWorker(void*) {
  for (;;) {
    // === Backhaul ingress: ESPNOW -> (dedup/TTL--) -> BLE e re-flood ESPNOW ===
    while (gEspNow.rxAvailable()) {
      uint8_t buf[espn::MAX_FRAME];
      size_t  n = sizeof(buf);
      uint8_t srcMac[6];
      if (!gEspNow.rxEx(buf, &n, srcMac, /*timeout_ms*/0)) break;
      if (n < MIN_PKT_LEN || buf[0] != 0x01) continue;

      // LOG de RX (antes do TTL--)
      if (debugLevel >= 3) {
        char sid[17]; hex8(&buf[11], sid);
        safePrintf("[ESPN-RXDEQ] type=%u len=%u ttl=%u sid=%s\r\n",
                  buf[1], (unsigned)n, buf[2], sid);                  
      }

      // Dedup (mesma regra do caminho BLE)
      size_t hdr = (n < 19) ? n : 19;
      uint8_t hdrNoTTL[19]; memcpy(hdrNoTTL, buf, hdr); if (hdr >= 3) hdrNoTTL[2] = 0;
      size_t extra = (n > hdr) ? ((n - hdr) < 32 ? (n - hdr) : 32) : 0;
      uint64_t h = fnv1a64(hdrNoTTL, hdr); if (extra) h ^= fnv1a64(buf + hdr, extra);
      if (dedupSeen(h)) { 
        gDropsDedup++; 
        if (debugLevel >= 3) {
          char sid[17]; hex8(&buf[11], sid);
          safePrintf("[ESPN-DEDUP] type=%u len=%u ttl=%u sid=%s\r\n",
                    buf[1], (unsigned)n, buf[2], sid);                  
        }
        continue; 
      }

      // Contadores e LED
      kickRxBlink(); gPktsIn++; gBytesIn += n;

      // TTL--
      if (buf[2] == 0) continue;
      buf[2]--;
      if (buf[2] == 0) continue;

      // 1) entrega aos centrals BLE
      relayToCentrals(buf, n, -1);

      // 2) re-flood no backhaul SEM eco + dedup BH (conteúdo e MAC+conteúdo)
      relayToBackhaulExcept(buf, n, srcMac);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
#endif

// Worker de TX: rate-limited; global token-bucket, connection fan-out, per-peer fragmentation (no echo)
static void txWorker(void*) {
  for (;;) {
    uint32_t ms = millis();
    if (ms - gLastTokenRefill >= 1000) {
      gNotifyTokens = gNotifyBudgetPerSec;
      gLastTokenRefill = ms;
    }

    // Take one from queue if we have tokens
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

    // Subscribed peers fan-out
    bool anySent = false;
    for (auto &kv : gSubscribed) {
      uint16_t h = kv.first;
      if (!kv.second) continue;             // not subscribed
      if (it.origin >= 0 && h == (uint16_t)it.origin) continue; // no echo to origin

      // MTU/cap by peer
      uint16_t mtu = DEFAULT_MTU;
      auto itM = gPeerMtus.find(h);
      if (itM != gPeerMtus.end()) mtu = itM->second;
      size_t capPeer = (mtu > 3) ? (mtu - 3) : FALLBACK_CAP;

      // fits directly on this peer?
      if (it.pkt.size() <= capPeer) {
        gChar->setValue(it.pkt.data(), it.pkt.size());
        if (gChar->notify(h)) {
          gNotifies++; gPktsOut++; gBytesOut += it.pkt.size();
          anySent = true;
        }
        continue;
      }

      // Needs fragmentation to this peer?
      if (capPeer < 32) {
        // too small even for fragment header
        if (debugLevel >= 2) {
          char sid[17]; hex8(&it.pkt[11], sid);
          Serial.printf("[REL-DROP] (cap<32) len=%u cap=%u peer=%u sid=%s\r\n",
                        (unsigned)it.pkt.size(), (unsigned)capPeer, (unsigned)h, sid);
        }
        continue;
      }

      // by peer fragmentation
      const size_t overhead = 19 + 13;                 // header bitchat (19) + frag hdr (13)
      size_t maxChunk = (capPeer > overhead) ? (capPeer - overhead) : 0;
      if (maxChunk == 0) {
        if (debugLevel >= 2) {
          char sid[17]; hex8(&it.pkt[11], sid);
          Serial.printf("[REL-DROP] (cap<=overhead) len=%u cap=%u peer=%u sid=%s\r\n",
                        (unsigned)it.pkt.size(), (unsigned)capPeer, (unsigned)h, sid);
        }
        continue;
      }

      uint8_t origType = it.pkt[1];
      uint8_t fragID[8]; for (int i = 0; i < 8; i++) fragID[i] = (uint8_t)esp_random();
      size_t totalFrags = (it.pkt.size() + maxChunk - 1) / maxChunk;

      for (size_t off = 0, idx = 0; off < it.pkt.size(); off += maxChunk, idx++) {
        size_t clen = std::min(maxChunk, it.pkt.size() - off);
        // 19 bytes (base header)
        // [0]=0x01, [1]=FRAGMENT_TYPE, [2]=TTL keep untouched
        // [3..10]=timestamp(8), [11..18]=senderID(8)
        // + 13 bytes fragment header
        // [19..26]=fragID(8), [27..28]=idx(2), [29..30]=total(2), [31]=origType
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
        Serial.printf("[REL-FRAG] len=%u cap=%u peers=%u peer=%u sid=%s\r\n",
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
  buf[0]=0x01; 
  buf[1]=ANNOUNCE_TYPE; 
  buf[2]=7;
  writeU64BE(&buf[3], nowMs());
  memcpy(&buf[11], gSenderID, 8);
  memset(&buf[19], 0, 32);
  txEnqueue(buf, sizeof(buf));
  gLastAnnounce = millis();
  refreshAnnounceJitter();
  safePrintf("[BLE-ANN] Announce sent\r\n");
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
  if(nfr==0 || nfr>65535 || cap<32){ 
    safePrintf("[FRG-DROP] Invalid fragmentation, drop\r\n");
    return; 
  }
  uint8_t origType = orig[1];
  uint8_t fragID[8]; 
  for(int i=0;i<8;i++) fragID[i]=(uint8_t)esp_random();
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
    // Log
    if (debugLevel>=3){
      char sid[17]; hex8(&orig[11], sid);
      Serial.printf("[FRG-ENQ] enq frag idx=%u/%u len=%u cap=%u sid=%s\r\n",
        (unsigned)idx, (unsigned)nfr, (unsigned)len, (unsigned)cap, sid);
    }
  }
}

static void relayToCentrals(const uint8_t* pkt, size_t len, int origin){
  size_t cap = capServer();
  if(len <= cap){
    txEnqueueFrom(pkt, len, origin);
    if(debugLevel >= 3){
      char sid[17]; hex8(&pkt[11], sid);
      Serial.printf("[BLE-REL] direct len=%u cap=%u peers=%u sid=%s\r\n",
        (unsigned)len, (unsigned)cap, (unsigned)gPeerMtus.size(), sid);
    }
  } else {
    if (cap >= 32) {
      sendFragmentedNotifyFrom(pkt, len, cap, origin);
      if (debugLevel >= 3) {
        char sid[17]; hex8(&pkt[11], sid);
        Serial.printf("[BLE-REL] fragmented len=%u cap=%u peers=%u sid=%s\r\n",
          (unsigned)len, (unsigned)cap, (unsigned)gPeerMtus.size(), sid);
      }
    } else {
      if (debugLevel >= 3) {
        char sid[17]; hex8(&pkt[11], sid);
        Serial.printf("[BLE-REL] drop (cap<32) len=%u cap=%u peers=%u sid=%s\r\n",
          (unsigned)len, (unsigned)cap, (unsigned)gPeerMtus.size(), sid);
      }
    }
  }
}

#if ESP_BH
// === Backhaul (ESPNOW) fan-out ===
static inline void relayToBackhaul(const uint8_t* pkt, size_t len) {
  // não tenta enviar se não tiver peers
  if (gEspNow.peerCount() == 0) {
    if (debugLevel >= 2) safePrintf("[ESPN-TXENQ] no peers, skip\r\n");
    return;
  }

  if (!pkt || len < MIN_PKT_LEN || pkt[0] != 0x01) return;
  uint8_t type = pkt[1];
  uint8_t ttl  = (len >= 3) ? pkt[2] : 0;
  char sid[17]; hex8(&pkt[11], sid);
  if (debugLevel >= 3) safePrintf("[ESPN-TXENQ] type=%u len=%u ttl=%u sid=%s\r\n", type, (unsigned)len, ttl, sid);
  if (!gEspNow.tx(pkt, len)) {
    if (debugLevel >= 1) safePrintf("[ESPN-TXENQ] enqueue FAIL len=%u sid=%s\r\n", (unsigned)len, sid);
  }
}

// === Backhaul (ESPNOW) fan-out, exclui remetente + dedup BH (conteúdo e MAC+conteúdo)
static inline void relayToBackhaulExcept(const uint8_t* pkt, size_t len, const uint8_t src_mac[6]) {
  if (gEspNow.peerCount() == 0 || !pkt || len < MIN_PKT_LEN || pkt[0] != 0x01) return;

  // Dedup BH: header (TTL zerado) + até 32B de payload
  const size_t hdr = (len < 19) ? len : 19;
  uint8_t hdrNoTTL[19]; memcpy(hdrNoTTL, pkt, hdr); if (hdr >= 3) hdrNoTTL[2] = 0;
  const size_t extra = (len > hdr) ? ((len - hdr) < 32 ? (len - hdr) : 32) : 0;

  const uint64_t hContent = fnv1a64(hdrNoTTL, hdr) ^ (extra ? fnv1a64(pkt + hdr, extra) : 0);
  static std::deque<DedupEntry> dQ; static std::unordered_set<uint64_t> dS;
  auto bhSeen = [&](uint64_t h)->bool{
    uint64_t now = nowMs();
    while(!dQ.empty() && (now - dQ.front().tms) > 15000){ dS.erase(dQ.front().h); dQ.pop_front(); }
    if (dS.count(h)) return true; if (dQ.size()>=512){ dS.erase(dQ.front().h); dQ.pop_front(); }
    dQ.push_back({h, now}); dS.insert(h); return false;
  };
  if (bhSeen(hContent)) return;

  // Dedup BH: MAC + conteúdo
  uint8_t mix[6 + 19 + 32] = {0};
  memcpy(mix, src_mac, 6); memcpy(mix + 6, hdrNoTTL, hdr); if (extra) memcpy(mix + 6 + hdr, pkt + hdr, extra);
  const uint64_t hMac = fnv1a64(mix, 6 + hdr + extra);
  static std::deque<DedupEntry> dQM; static std::unordered_set<uint64_t> dSM;
  if (bhSeen(hMac)) return;

  // Enfileira no BH excluindo o remetente; o driver já não espera ACK dele
  (void)gEspNow.txExcept(pkt, len, src_mac);
}
#endif

// ===== Callbacks NimBLE 2.3.6 =====
class SvrCallbacks: public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
    gPeerMtus[info.getConnHandle()] = DEFAULT_MTU;
    // se já atingiu o teto, desconecta imediatamente
    if (gPeerMtus.size() > MAX_PEERS) {
      if (gServer)gServer->disconnect(info.getConnHandle());
      if (debugLevel >= 1) safePrintf("[CLI-DROP] Max peers reached, disconnecting new central (ch=%u)\r\n", (unsigned)info.getConnHandle());
      return;
    }
    if (debugLevel >= 1) safePrintf("[CLI-CONN] Central connected (ch=%u)\r\n", (unsigned)info.getConnHandle());
    NimBLEDevice::startAdvertising();
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int reason) override {
    gPeerMtus.erase(info.getConnHandle());
    gSubscribed.erase(info.getConnHandle());
    safePrintf("[CLI-DISC] Central disconnected (ch=%u, reason=%d)\r\n", (unsigned)info.getConnHandle(), reason);
    NimBLEDevice::startAdvertising();
  }
  void onMTUChange(uint16_t mtu, NimBLEConnInfo& info) override {
    gPeerMtus[info.getConnHandle()] = mtu;
    safePrintf("[CLI-MTU] MTU updated (ch=%u, mtu=%u)\r\n", (unsigned)info.getConnHandle(), (unsigned)mtu);
  }
};

class CharCallbacks: public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo& info, uint16_t subVal) override {
    bool wantsNotify = (subVal & 0x0001);
    gSubscribed[info.getConnHandle()] = wantsNotify;
    safePrintf("[CLI-SUB] Subscribe ch=%u notify=%u\r\n", (unsigned)info.getConnHandle(), (unsigned)wantsNotify);
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
      if(debugLevel>=3){
        uint8_t ttl=p[2]; 
        uint64_t ts=0; 
        for(int i=0;i<8;i++) ts=(ts<<8)|p[3+i];
        char sid[17]; 
        hex8(&p[11], sid);
        Serial.printf("[PKT-DEDUP] type=%u ttl=%u len=%u ts=%llu sid=%s\r\n",
                    p[1], ttl, (unsigned)len, (unsigned long long)ts, sid);
      }
      return; 
    }

    if(debugLevel>=2){
      uint8_t ttl=p[2]; uint64_t ts=0; for(int i=0;i<8;i++) ts=(ts<<8)|p[3+i];
      char sid[17]; hex8(&p[11], sid);
      Serial.printf("[PKT-IN] type=%u ttl=%u len=%u ts=%llu sid=%s\r\n",
                    p[1], ttl, (unsigned)len, (unsigned long long)ts, sid);
    }

    if (debugLevel >= 4) {
      char sid[17]; 
      hex8(&p[11], sid);
      uint8_t sniff[8] = {0};
      size_t sniffLen = (len > 27) ? 8 : (len > 19 ? len - 19 : 0);
      if (sniffLen) memcpy(sniff, p + 19, sniffLen);
      Serial.printf("[PKT-DATA] sid=%s type=%u len=%u ttl=%u [%02X %02X %02X %02X %02X %02X %02X %02X]\r\n",
        sid, p[1], (unsigned)len, p[2],
        sniff[0],sniff[1],sniff[2],sniff[3],sniff[4],sniff[5],sniff[6],sniff[7]);
    }

    gWrites++; gPktsIn++; gBytesIn+=len; if(p[1]==1) gType1In++; else if(p[1]==2) gType2In++;
    kickRxBlink();

    static uint8_t buf[600]; if(len>sizeof(buf)) return; memcpy(buf, p, len);
    // TTL handling: drop when zero and after decrement if it hits zero
    if (buf[2] == 0) { 
      if (debugLevel>=2) Serial.println("[PKT-DROP] ttl=0 before relay"); 
      return; 
    }
    buf[2]--; /* TTL-- */
    if (buf[2] == 0) { 
      if (debugLevel>=2) Serial.println("[PKT-DROP] ttl expired after --"); 
      return; 
    }

    if(p[1]==FRAGMENT_TYPE){
      if(len < 19+13) return;
      uint64_t sender=0; 
      for(int i=0;i<8;i++) sender=(sender<<8)|p[11+i];

      uint64_t fragID=0; 
      for(int i=0;i<8;i++) fragID=(fragID<<8)|p[19+i];

      uint16_t index=(p[27]<<8)|p[28], total=(p[29]<<8)|p[30];
      uint8_t origType=p[31]; 
      size_t chunkLen = len-32; 
      if(total==0 || index>=total) return;

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
        incomingFragments[key] = {};
        fragmentMetadata[key] = {origType, nowMs()};
        gInflightFragsBySender[sender]++; // novo assembly deste sender
      }
      if(incomingFragments[key].find(index)==incomingFragments[key].end()){
        incomingFragments[key][index] = std::vector<uint8_t>(p+32, p+len);
        gInflightBytes += chunkLen;
        gInflightBytesBySender[sender] += chunkLen;

      }
      if (incomingFragments[key].size() == total) {
        size_t totalPay = 0; 
        for (uint16_t i = 0; i < total; i++) totalPay += incomingFragments[key][i].size();
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
        // Finaliza: limpa contadores global e por sender
        for (auto &kv : incomingFragments[key]) {
          gInflightBytes -= kv.second.size();
          gInflightBytesBySender[sender] -= kv.second.size();
        }
        incomingFragments.erase(key);
        fragmentMetadata.erase(key);
        if (gInflightFragsBySender[sender] > 0) gInflightFragsBySender[sender]--;
        relayToCentrals(whole.data(), whole.size(), (int)info.getConnHandle());
#if ESP_BH        
        // também propaga no backhaul (TTL já foi -- lá em cima)        
        relayToBackhaul(whole.data(), whole.size());        
#endif        
      }
    } else {
      relayToCentrals(buf, len, (int)info.getConnHandle());
#if ESP_BH      
      // e backhaul
      relayToBackhaul(buf, len);
#endif      
    }
  }
};

// ===== Setup BLE (server) =====
static void setup_ble_server(){
  uint8_t mac[6]; 
  esp_read_mac(mac, ESP_MAC_BT);

  char suf[7]; 
  sprintf(suf, "%02X%02X%02X", mac[3], mac[4], mac[5]);

  String devName="BitChatRelay_"; 
  devName+=suf;

  NimBLEDevice::init(devName.c_str());
  NimBLEDevice::setPower(BLE_POWER);          // dBm
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
  adv->setPreferredParams(0x50, 0xA0); // 50ms to 100ms (ms = param * 0.625)
  NimBLEDevice::startAdvertising();
  gServer->advertiseOnDisconnect(true);

  safePrintf("[BLE] Peripheral ready (HUB). Advertising service...\r\n");
}

// ===== Setup ESPNOW =====
static void setup_espn(){
  uint8_t mac[6]; 
  WiFi.macAddress(mac);
  safePrintf("[WIFI] MAC Address: %02X:%02X:%02X:%02X:%02X:%02X\r\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  if (!gEspNow.init(NET_ID, mac, ESP_BH_CHANNEL, ESP_BH_LINK_MTU, ESP_BH_TX_CORE, ESP_BH_RX_CORE, ESP_BH_BALANCED_SPLIT)) {
    Serial.println("espn init FAIL");
    for(;;) delay(1000);
  }
  safePrintf("[ESPN] ESPNOW initialized on channel %u\r\n", ESP_BH_CHANNEL);

  gEspNow.setDebugLevel(debugLevel);

  // BH RX worker
  xTaskCreatePinnedToCore(bhRxWorker, "bhrxw", 4096, nullptr, 3, nullptr, 0);
}

// ===== Arduino entry points =====
void setup(){
  Serial.begin(115200);
  delay(100);

  pinMode(LED_PIN, OUTPUT); 
  ledOff();

  safePrintf("\r\n=== BitChat Relay v%s ===\r\n", VERSION);
  safePrintf("Compiled: %s %s\r\n", __DATE__, __TIME__);
  safePrintf("Free heap: %u bytes\r\n", ESP.getFreeHeap());

  makeSenderID();
  setup_ble_server();

#if ESP_BH
  setup_espn();
#endif

  gSerialMutex = xSemaphoreCreateMutex();
  gTxMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(txWorker, "txw", 4096, nullptr, 3, nullptr, 0);
  gLastTokenRefill = millis(); gNotifyTokens = gNotifyBudgetPerSec;

  refreshAnnounceJitter();
  sendAnnounce();
  safePrintf("[BLE] Setup done.\r\n");
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
    uint8_t cnt=0;
    for(auto &kv: gPeerMtus) {
      cnt++;
      mtuList+="ch="+String(kv.first)+":mtu="+String(kv.second)+" ";
      if (cnt==4) mtuList+="\r\n";
    }

    uint32_t centrals = gPeerMtus.size();
    uint32_t subs = 0; for (auto &kv: gSubscribed) if (kv.second) subs++;

    Serial.println("===================================[STATUS]===================================");
    Serial.printf("pktsIn: %u | bytesIn: %u | pktsOut: %u | bytesOut: %u | writes: %u | notifies: %u \r\n" \
                  "heap: %u | minCap: %u | q: %u | tokens: %u | dedupWin: %u | drops{dedup: %u, backp: %u} \r\n" \
                  "inflightB: %u | t1_in: %u | t2_in: %u | peers: %u | subs{notify: %u} \r\n" \
                  "%s \r\n",
      gPktsIn, gBytesIn, gPktsOut, gBytesOut, gWrites, gNotifies,
      ESP.getFreeHeap(), (unsigned)cap, (unsigned)qsz, (unsigned)gNotifyTokens,
      (unsigned)gSeenDeque.size(), (unsigned)gDropsDedup, (unsigned)gDropsBackpressure,
      (unsigned)gInflightBytes, gType1In, gType2In, centrals, subs, 
      mtuList.c_str());
    Serial.println("==============================================================================");
  }

  // Comandos seriais
  if (gSerialMutex) {
    xSemaphoreTake(gSerialMutex, portMAX_DELAY);
    if (Serial.available()) {
      char k = Serial.read();
      switch (k) {
        case 'r':
          ESP.restart();
          break;
        case '+':
          if (debugLevel < 5) debugLevel++;
#if ESP_BH
          gEspNow.setDebugLevel(debugLevel);
#endif
          Serial.printf("[DBG] Debug level now %d\r\n", debugLevel);
          break;
        case '-':
          if (debugLevel > 0) debugLevel--;
#if ESP_BH
          gEspNow.setDebugLevel(debugLevel);
#endif
          Serial.printf("[DBG] Debug level now %d\r\n", debugLevel);
          break;
      }
    }
    xSemaphoreGive(gSerialMutex);
  }  
  vTaskDelay(10);
}
