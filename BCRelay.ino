/*
 * BitChat Relay – HUB (GATT Server only)
 * Goal: extremely simple, like "Bitle": receive on WRITE/WRITE_NR, relay to all centrals via NOTIFY.
 *
 * Features:
 *  - Peripheral-only (no scanning, no client connects) -> never blocks on connection attempts
 *  - Multi-central: any number allowed by stack/heap (typically 2–4 on Classic BLE)
 *  - TTL--, de-duplication (FNV-1a), fragmentation/reassembly
 *  - TX queue + token bucket (rate limit) so we never block the loop
 *  - Periodic ANNOUNCE with jitter
 *  - Dynamic CAP based on minimum negotiated MTU; safe fallback for iOS (no MTU change)
 *  - Logs in English
 */

#include <Arduino.h>
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLE2902.h>
//#include <NimBLEDevice.h>
#include <map>
#include <vector>
#include <queue>
#include <deque>
#include <unordered_set>
#include <algorithm>

// ===== BitChat UUIDs (as observed / placeholder) =====
#define BITCHAT_SERVICE_UUID_MAINNET  "F47B5E2D-4A9E-4C5A-9B3F-8E1D2C3A4B5C"
#define BITCHAT_CHARACTERISTIC_UUID   "A1B2C3D4-E5F6-4A5B-8C9D-0E1F2A3B4C5D"

// ===== Protocol types =====
#define ANNOUNCE_TYPE                 0x01
#define FRAGMENT_TYPE                 0x06
#define MIN_PKT_LEN                   21

// ===== Timers / sizes =====
#define ANNOUNCE_INTERVAL_MS          30000UL
#define REQUESTED_MTU                 517
#define DEFAULT_MTU                   23
#define FALLBACK_CAP                  20     // safe for iOS if MTU not negotiated (ATT 23 -> 20 payload; we keep headroom)
#define MIN_CHUNK_SIZE                64
#define FRAG_LIFETIME_MS              30000UL
#define MAX_INFLIGHT_FRAGS            10
#define MAX_INFLIGHT_BYTES            65536UL

// ===== LED (optional) =====
#ifndef LED_PIN
#define LED_PIN 2
#endif
#define LED_RX_ON_MS 30
#define LED_TX_ON_MS 30
#define LED_TX_GAP_MS 60

// ===== Debug level =====
static uint8_t debugLevel = 3;
#define DBG(lvl, msg) do{ if(debugLevel>=lvl){ Serial.print("[DBG");Serial.print(lvl);Serial.print("] ");Serial.println(msg);} }while(0)

// ===== BLE server state =====
static BLEServer*         gServer = nullptr;
static BLECharacteristic* gChar   = nullptr;
static BLE2902*           gCCCD   = nullptr;   // client config descriptor (notifications / indications)
static std::map<uint16_t, uint16_t> gPeerMtus; // conn_id -> MTU
static uint32_t gLastAnnounce = 0;

// ===== Sender identity (8 bytes) =====
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

// ===== Fragment reassembly store =====
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

// ===== TX queue / rate limit =====
struct TxItem{ std::vector<uint8_t> pkt; };
static std::queue<TxItem> gTxQ;
static SemaphoreHandle_t gTxMutex;
static uint32_t gNotifyBudgetPerSec = 150;   // max notifies per second
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

// Dedup window housekeeping
static void dedupCleanup(){
  uint64_t now = nowMs();
  while(!gSeenDeque.empty() && (now - gSeenDeque.front().tms) > DEDUP_TTL_MS){
    gSeenSet.erase(gSeenDeque.front().h);
    gSeenDeque.pop_front();
  }
}
// Insert-or-check packet hash in the dedup window
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

// Compute current maximum payload cap across centrals (min MTU among connected peers)
static size_t capServer(){
  if (gPeerMtus.empty()) return FALLBACK_CAP; // 20 bytes (ATT 23)
  uint16_t minMtu = 0xFFFF;
  for (auto &kv : gPeerMtus) {
    if (kv.second < minMtu) minMtu = kv.second;
  }
  if (minMtu < DEFAULT_MTU) minMtu = DEFAULT_MTU; // sanidade (mínimo 23)
  size_t cap = (minMtu > 3) ? (minMtu - 3) : FALLBACK_CAP;
  if (cap < FALLBACK_CAP) cap = FALLBACK_CAP;     // nunca abaixo de 20
  return cap;
}


// Enqueue packet to TX queue (non-blocking)
static void txEnqueue(const uint8_t* p, size_t n){
  if(!p || n==0) return;
  TxItem it; it.pkt.assign(p, p+n);
  xSemaphoreTake(gTxMutex, portMAX_DELAY);
  if(gTxQ.size()<128) gTxQ.push(std::move(it)); else gDropsBackpressure++;
  xSemaphoreGive(gTxMutex);
}

// Background TX worker: rate-limited notify
static void txWorker(void*){
  for(;;){
    uint32_t ms = millis();
    if(ms - gLastTokenRefill >= 1000){
      gNotifyTokens = gNotifyBudgetPerSec;
      gLastTokenRefill = ms;
    }
    xSemaphoreTake(gTxMutex, portMAX_DELAY);
    bool has = !gTxQ.empty() && gNotifyTokens>0;
    TxItem it; if(has){ it=std::move(gTxQ.front()); gTxQ.pop(); gNotifyTokens--; }
    xSemaphoreGive(gTxMutex);

    if(has && gChar){
      gChar->setValue(it.pkt.data(), it.pkt.size());
      gChar->notify(); 
      gNotifies++;       
      gPktsOut++;
      gBytesOut += it.pkt.size();
      kickTxBlink();
    } else {
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }
}

// ===== Announce with jitter =====
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

// Garbage collect stale fragment contexts
static void cleanupFragments(){
  uint64_t now = nowMs();
  for(auto it=fragmentMetadata.begin(); it!=fragmentMetadata.end(); ){
    if(now - it->second.second > FRAG_LIFETIME_MS){
      auto mapIt = incomingFragments.find(it->first);
      if(mapIt!=incomingFragments.end()){
        for(auto &kv: mapIt->second) gInflightBytes -= kv.second.size();
        incomingFragments.erase(mapIt);
      }
      it = fragmentMetadata.erase(it);
    } else ++it;
  }
}

// Fragment a large outbound packet to fit current CAP and enqueue fragments
static void sendFragmentedNotify(const uint8_t* orig, size_t origLen, size_t cap){
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
    txEnqueue(pkt, len);
  }
}

// Relay helper: either direct or re-fragment + verbose log at DBG3
static void relayToCentrals(const uint8_t* pkt, size_t len){
  size_t cap = capServer();
  if(len <= cap){
    txEnqueue(pkt, len);
    if(debugLevel >= 3){
      char sid[17]; hex8(&pkt[11], sid);
      Serial.printf("[REL] direct len=%u cap=%u peers=%u sid=%s\r\n",
        (unsigned)len, (unsigned)cap, (unsigned)gPeerMtus.size(), sid);
    }
  } else {
    //sendFragmentedNotify(pkt, len, cap);
    //if(debugLevel >= 3){
    //  char sid[17]; hex8(&pkt[11], sid);
    //  Serial.printf("[REL] fragmented len=%u cap=%u peers=%u sid=%s\r\n",
    //    (unsigned)len, (unsigned)cap, (unsigned)gPeerMtus.size(), sid);
    //}
    if (cap >= 32) {
      sendFragmentedNotify(pkt, len, cap);
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


// ===== BLE callbacks =====
class SvrCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer*, esp_ble_gatts_cb_param_t *p) override {
    gPeerMtus[p->connect.conn_id] = DEFAULT_MTU;
    DBG(3, String("Central connected (conn_id=") + String(p->connect.conn_id) + ")");
    BLEDevice::getAdvertising()->start(); // keep advertising for more centrals
  }
  void onDisconnect(BLEServer*, esp_ble_gatts_cb_param_t *p) override {
    gPeerMtus.erase(p->disconnect.conn_id);
    DBG(3, String("Central disconnected (conn_id=") + String(p->disconnect.conn_id) + ")");
    BLEDevice::getAdvertising()->start();
  }
  void onMtuChanged(BLEServer*, esp_ble_gatts_cb_param_t *p) override {
    gPeerMtus[p->mtu.conn_id] = p->mtu.mtu;
    DBG(3, String("MTU updated (conn_id=") + String(p->mtu.conn_id) + String(", mtu=") + String(p->mtu.mtu) + ")");
  }
};

class CharCallbacks: public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    std::string v = c->getValue();
    const uint8_t* p = (const uint8_t*)v.data();
    size_t len = v.size();
    if(len<MIN_PKT_LEN || p[0]!=0x01) return;

    if (debugLevel >= 4) {
      // imprime 8B do payload logo após header base (offset 19)
      char sid[17]; hex8(&p[11], sid);
      uint8_t sniff[8] = {0};
      size_t sniffLen = (len > 27) ? 8 : (len > 19 ? len - 19 : 0);
      if (sniffLen) memcpy(sniff, p + 19, sniffLen);
      Serial.printf("[SNIFF] sid=%s type=%u len=%u ttl=%u p0=%02X p1=%02X p2=%02X p3=%02X p4=%02X p5=%02X p6=%02X p7=%02X\r\n",
        sid, p[1], (unsigned)len, p[2],
        sniff[0],sniff[1],sniff[2],sniff[3],sniff[4],sniff[5],sniff[6],sniff[7]);
    }

    gWrites++; gPktsIn++; gBytesIn+=len;
    if(p[1]==1) gType1In++; else if(p[1]==2) gType2In++;
    if(debugLevel>=2){
      uint8_t ttl=p[2]; uint64_t ts=0; for(int i=0;i<8;i++) ts=(ts<<8)|p[3+i];
      char sid[17]; hex8(&p[11], sid);
      Serial.printf("[PKT] in type=%u ttl=%u len=%u ts=%llu sid=%s\r\n",
                    p[1], ttl, (unsigned)len, (unsigned long long)ts, sid);
    }
    kickRxBlink();

    // Dedup on header + first 32 payload bytes (stable enough, avoids loops/echo)
    //size_t hdr = std::min(len,(size_t)19), extra=(len>hdr)? std::min(len-hdr,(size_t)32):0;
    //uint64_t h = fnv1a64(p, hdr); if(extra) h ^= fnv1a64(p+hdr, extra);
    //if(dedupSeen(h)){ gDropsDedup++; return; }

    // Dedup: ignora o TTL (byte 2) no hash para não quebrar em loops com ttl--.
    size_t hdr = std::min(len, (size_t)19);
    uint8_t hdrNoTTL[19];
    memcpy(hdrNoTTL, p, hdr);
    if (hdr >= 3) hdrNoTTL[2] = 0; // zera o TTL
    size_t extra = (len > hdr) ? std::min(len - hdr, (size_t)32) : 0;

    uint64_t h = fnv1a64(hdrNoTTL, hdr);
    if (extra) h ^= fnv1a64(p + hdr, extra);

    if (dedupSeen(h)) { gDropsDedup++; return; }


    static uint8_t buf[600]; if(len>sizeof(buf)) return;
    memcpy(buf, p, len);
    if(buf[2]>0) buf[2]--; // TTL--

    size_t cap = capServer();

    // Reassembly path for incoming fragments
    if(p[1]==FRAGMENT_TYPE){
      if(len < 19+13) return;
      uint64_t sender=0; for(int i=0;i<8;i++) sender=(sender<<8)|p[11+i];
      uint64_t fragID=0; for(int i=0;i<8;i++) fragID=(fragID<<8)|p[19+i];
      uint16_t index=(p[27]<<8)|p[28], total=(p[29]<<8)|p[30];
      uint8_t origType=p[31];
      size_t chunkLen = len-32;
      if(total==0 || index>=total) return;

      FragmentKey key{sender, fragID};
      if(incomingFragments.find(key)==incomingFragments.end()){
        if(incomingFragments.size()>=MAX_INFLIGHT_FRAGS || (gInflightBytes+chunkLen)>MAX_INFLIGHT_BYTES){
          // Drop realmente o mais antigo (menor timestamp)
          auto oldest = fragmentMetadata.begin();
          for (auto it2 = fragmentMetadata.begin(); it2 != fragmentMetadata.end(); ++it2) {
            if (it2->second.second < oldest->second.second) oldest = it2;
          }
          auto mapIt = incomingFragments.find(oldest->first);
          if (mapIt != incomingFragments.end()) {
            for (auto &kv : mapIt->second) gInflightBytes -= kv.second.size();
            incomingFragments.erase(mapIt);
          }
          fragmentMetadata.erase(oldest);
        }
        incomingFragments[key] = {};
        fragmentMetadata[key] = {origType, nowMs()};
      }
      if(incomingFragments[key].find(index)==incomingFragments[key].end()){
        incomingFragments[key][index] = std::vector<uint8_t>(p+32, p+len);
        gInflightBytes += chunkLen;
      }
      // Completed?
        if (incomingFragments[key].size() == total) {
          // calcula o payload total
          size_t totalPay = 0;
          for (uint16_t i = 0; i < total; i++) totalPay += incomingFragments[key][i].size();

          // guarda-chuva: limita reassembly gigante
          if (totalPay > 4096) {
            for (auto &kv : incomingFragments[key]) gInflightBytes -= kv.second.size();
            incomingFragments.erase(key); fragmentMetadata.erase(key);
            return;
          }

          std::vector<uint8_t> out(19 + totalPay);
          out[0] = 0x01; out[1] = origType; out[2] = buf[2];
          memcpy(out.data()+3,  buf+3,  8);
          memcpy(out.data()+11, buf+11, 8);

          size_t pay = 0;
          for (uint16_t i = 0; i < total; i++) {
            auto &ch = incomingFragments[key][i];
            memcpy(out.data()+19+pay, ch.data(), ch.size());
            pay += ch.size();
          }

          for (auto &kv : incomingFragments[key]) gInflightBytes -= kv.second.size();
          incomingFragments.erase(key); fragmentMetadata.erase(key);

          relayToCentrals(out.data(), out.size()); // re-fragmenta conforme CAP atual
        }
    } else {
      // Direct relay or re-fragment to match CAP
      //if(len<=cap) txEnqueue(buf, len);
      //else         sendFragmentedNotify(buf, len, cap);
      relayToCentrals(buf, len);
    }
  }
};

// ===== BLE server setup =====
static void setup_ble_server(){
  uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_BT);
  char suf[7]; sprintf(suf, "%02X%02X%02X", mac[3], mac[4], mac[5]);
  String devName="BitChatRelay_"; devName+=suf;

  BLEDevice::init(devName.c_str());
  BLEDevice::setPower(ESP_PWR_LVL_P9);
  BLEDevice::setMTU(REQUESTED_MTU);

  gServer = BLEDevice::createServer();
  gServer->setCallbacks(new SvrCallbacks());

  BLEService* svc = gServer->createService(BITCHAT_SERVICE_UUID_MAINNET);

    gChar = svc->createCharacteristic(
    BITCHAT_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ    |
    BLECharacteristic::PROPERTY_WRITE   |
    BLECharacteristic::PROPERTY_WRITE_NR|
    BLECharacteristic::PROPERTY_NOTIFY
  );

  gChar->addDescriptor(new BLE2902());
  gCCCD = (BLE2902*) gChar->getDescriptorByUUID(BLEUUID((uint16_t)0x2902)); // cache CCCD for subscription checks
  gChar->setCallbacks(new CharCallbacks());
  gChar->setValue("relay ready");
  svc->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(BITCHAT_SERVICE_UUID_MAINNET);
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);
  adv->setMaxPreferred(0x06);
  adv->start();

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
  // LED RX single blink
  if(rxBlinkUntil && millis()>=rxBlinkUntil){ ledOff(); rxBlinkUntil=0; }

  // LED TX double-blink pattern
  if(txBlinkPhase){
    uint32_t now=millis();
    if(now>=txPhaseUntil){
      if(txBlinkPhase==1){ ledOff(); txBlinkPhase=2; txPhaseUntil=now+LED_TX_GAP_MS; }
      else if(txBlinkPhase==2){ ledOn(); txBlinkPhase=3; txPhaseUntil=now+LED_TX_ON_MS; }
      else { ledOff(); txBlinkPhase=0; }
    }
  }

  // Periodic announce with jitter
  if(millis()-gLastAnnounce > nextAnnounceDue()) sendAnnounce();

  // Housekeeping + stats every 10s
  if(millis()-gLastStats > 10000UL){
    gLastStats = millis();
    cleanupFragments();

    size_t cap = capServer();
    size_t qsz; xSemaphoreTake(gTxMutex, portMAX_DELAY); qsz=gTxQ.size(); xSemaphoreGive(gTxMutex);

    String mtuList="MTUs: ";
    for(auto &kv: gPeerMtus) mtuList+="id="+String(kv.first)+":mtu="+String(kv.second)+" ";

    uint32_t centrals = gPeerMtus.size();
    bool anyNotif = gCCCD ? gCCCD->getNotifications() : false;
    Serial.println("=== BitChat Relay Status ===");
    Serial.printf("[STAT] pktsIn=%u bytesIn=%u pktsOut=%u bytesOut=%u writes=%u notifies=%u heap=%u minCap=%u q=%u tokens=%u dedupWin=%u drops{dedup=%u,backp=%u} inflightB=%u t1_in=%u t2_in=%u peers=%u subs{notify=%u} %s\r\n",
      gPktsIn, gBytesIn, gPktsOut, gBytesOut, gWrites, gNotifies,
      ESP.getFreeHeap(), (unsigned)cap, (unsigned)qsz, (unsigned)gNotifyTokens,
      (unsigned)gSeenDeque.size(), (unsigned)gDropsDedup, (unsigned)gDropsBackpressure,
      (unsigned)gInflightBytes, gType1In, gType2In,
      centrals, (unsigned)anyNotif, mtuList.c_str());
    Serial.println("=== End Status ===");
  }

  delay(5);
}
