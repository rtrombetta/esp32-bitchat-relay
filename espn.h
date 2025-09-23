#pragma once
#include <Arduino.h>
#include <esp_now.h>
#include <string.h>

// Declaração do semáforo global definido no .ino
extern SemaphoreHandle_t gSerialMutex;

// ================== Configs por #define (override no .ino se quiser) ==================
#ifndef ESPN_ACK_TIMEOUT_MS
#define ESPN_ACK_TIMEOUT_MS   200   // espera pelo ACK de aplicação (por gid)
#endif
#ifndef ESPN_MAX_RETRIES
#define ESPN_MAX_RETRIES       4   // tentativas de reenvio
#endif
#ifndef ESPN_ANNOUNCE_IV_MS
#define ESPN_ANNOUNCE_IV_MS 10000  // intervalo do broadcast ANNOUNCE (descoberta)
#endif
#ifndef ESPN_PEER_TTL_MS
#define ESPN_PEER_TTL_MS     90000 // expira peer se não ouvir ANNOUNCE nesse tempo
#endif
#ifndef ESPN_MAX_PEERS
#define ESPN_MAX_PEERS         6   // máximo de peers (fan-out unicast)
#endif

// Backbone ESPNOW com filas TX/RX e fragmentação LLF balanceada.
// Envia/recebe FRAMES COMPLETOS (ex.: 520 B do BitChat), fragmentando só no link.
class espn {
public:
  static constexpr size_t MAX_FRAME   = 550;   // >= 520 do teu app
  static constexpr size_t NET_ID_LEN  = 8;

  espn();

  // net_id[8]  : identificador lógico da malha (todos os hubs iguais)
  // mac[6]     : só armazenado (opcional, pode passar nullptr)
  // channel    : canal Wi-Fi fixo do ESP-NOW (1/6/11 típico)
  // link_mtu   : MTU do link (ESPNOW ~250; será limitado por MAX_LINK_MTU)
  // tx_core/rx_core: 0/1 ou -1 p/ sem afinidade
  // balanced_split : true => fragmentos ~iguais (dif. <= 1 byte)
  bool init(const uint8_t net_id[NET_ID_LEN], const uint8_t mac[6],
            uint8_t channel, uint16_t link_mtu,
            int tx_core, int rx_core, bool balanced_split = true);

  // Enfileira um frame completo para enviar no backbone (fragmenta dentro)
  bool tx(const uint8_t* frame, size_t len, uint32_t timeout_ms = 0);

  // Enfileira excluindo um MAC (não envia pra ele nem espera ACK dele)
  bool txExcept(const uint8_t* frame, size_t len, const uint8_t exclude_mac[6], uint32_t timeout_ms = 0);

  // Se houver frame completo, copia pra 'out' (até *inout_len) e retorna true
  bool rx(uint8_t* out, size_t* inout_len, uint32_t timeout_ms = 0);

  // Variante que também retorna o MAC de origem do frame backbone.
  bool rxEx(uint8_t* out, size_t* inout_len, uint8_t src_mac[6], uint32_t timeout_ms = 0);

  // Quantos frames completos aguardando leitura
  size_t rxAvailable() const;

  // ajusta nivel de debug (0=off, 1=erros, 2=info, 3=verbose, 4=debug, 5=ack)
  void setDebugLevel(uint8_t level) { m_debug = level; }

  // ---- Peers (fan-out unicast; máx. 6) ----
  bool addPeer(const uint8_t mac[6]);
  bool delPeer(const uint8_t mac[6]);
  void clearPeers();
  int  peerCount() const { return m_nPeers; }  

private:
  // Flags de log
  volatile uint8_t m_debug;

  // ---------- Confiabilidade (ACK/ARQ) ----------
  static constexpr bool     RELIABLE        = true;   // liga ARQ por hop
  static constexpr uint32_t ACK_TIMEOUT_MS  = ESPN_ACK_TIMEOUT_MS;  // ms p/ esperar ACK
  static constexpr uint8_t  MAX_RETRIES     = ESPN_MAX_RETRIES;   // tentativas de reenvio
  static constexpr int      MAX_PEERS       = ESPN_MAX_PEERS;     // até 6 peers
  static constexpr uint8_t  LCTL_SIG        = 0xA6;   // assinatura de controle
  static constexpr uint8_t  LCTL_ACK        = 0x01;   // tipo ACK
  static constexpr uint8_t  LCTL_HELLO      = 0x02;   // descoberta (ANNOUNCE, broadcast)

  // Limites estáticos do driver (buffers)
  static constexpr uint16_t MAX_LINK_MTU = 250;   // ESPNOW seguro no Arduino-ESP32
  static constexpr uint8_t  LLF_SIG      = 0xA5;
  static constexpr uint32_t LLF_TIMEOUT_MS = 500;
  static constexpr int TX_QUEUE_LEN = 10;
  static constexpr int RX_QUEUE_LEN = 10;
  static constexpr int FRAG_QUEUE_LEN = 20;

  struct __attribute__((packed)) LLF {
    uint8_t  sig;                 // 0xA5
    uint8_t  gid;                 // id do grupo (frame)
    uint8_t  idx;                 // índice 0..cnt-1
    uint8_t  cnt;                 // total de frags
    uint16_t total;               // tamanho do frame completo
    uint8_t  net_id[NET_ID_LEN];  // filtro de rede
  };

  struct TxItem {
    uint16_t len;
    uint8_t  buf[MAX_FRAME];
    bool     have_exclude;
    uint8_t  exclude[6];
  };

  struct RxItem {
    uint16_t len;
    uint8_t  buf[MAX_FRAME];
    uint8_t  mac[6];
  };

  struct FragItem { uint8_t len; uint8_t buf[MAX_LINK_MTU]; uint8_t mac[6]; };

  struct FragSlot {
    bool     used;
    uint8_t  gid;
    uint8_t  cnt;
    uint16_t total;
    uint16_t bitmap;              // até 16 frags
    uint32_t ts_start;
    uint8_t  buf[MAX_FRAME];
    uint8_t  mac[6];
  };

  // Estado
  static espn*  s_self;           // para o callback C
  uint8_t       m_net_id[NET_ID_LEN]{};
  uint8_t       m_my_mac[6]{};
  uint8_t       m_channel{6};
  uint16_t      m_link_mtu{MAX_LINK_MTU};
  bool          m_balanced{true};

  QueueHandle_t m_txQ{nullptr}, m_rxQ{nullptr}, m_fragQ{nullptr};
  TaskHandle_t  m_txTask{nullptr}, m_rxTask{nullptr};
  FragSlot      m_slots[6]{};     // até 6 frames em remontagem

  // ACKs por gid (0..255)
  volatile uint8_t m_ack_seen[256] = {};

  // Tabela simples de peers p/ ACK unicast
  struct Peer { uint8_t mac[6]; uint32_t last_seen; };
  Peer m_peers[MAX_PEERS]{}; int m_nPeers = 0;  

  // Bitmap de ACKs recebidos por peer (para o gid corrente)
  volatile uint16_t m_ack_mask_got{0};
  uint16_t          m_ack_mask_expect{0};

  // Init/link
  bool initWifiEspNow();
  static void onRecvStatic(const uint8_t* mac, const uint8_t* data, int len);
  static void onSendStatic(const uint8_t* mac, esp_now_send_status_t s);
  void  onRecv(const uint8_t* mac, const uint8_t* data, int len);
  void  onSend(const uint8_t* mac, esp_now_send_status_t s);

  // Tasks
  static void txTaskThunk(void* arg);
  static void rxTaskThunk(void* arg);
  void txTask();
  void rxTask();

  // Fragmentação/Remontagem
  bool llf_sendFrame(const uint8_t* frame, size_t flen, const uint8_t* exclude_mac = nullptr);
  bool llf_feed(const uint8_t* pkt, size_t len, const uint8_t** out, size_t* outlen,
                const uint8_t src_mac[6]);
  void llf_gc();

  // Util
  static inline uint8_t make_gid();
  // Peers/ACK helpers
  bool ensurePeer(const uint8_t mac[6]);
  void sendAck(uint8_t gid, const uint8_t dst_mac[6]);  
  uint8_t m_nextHop[6]{}; 
  bool m_haveNextHop=false;
  int  findPeerIdx(const uint8_t mac[6]) const;

  // Descoberta (ANNOUNCE sem ACK)
  void sendAnnounce();
  void discoveryTick();
  uint32_t m_nextAnnounceMs{0};
  static constexpr uint32_t ANNOUNCE_IV_MS = ESPN_ANNOUNCE_IV_MS;
  static constexpr uint32_t PEER_TTL_MS    = ESPN_PEER_TTL_MS;
};
