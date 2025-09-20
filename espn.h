#pragma once
#include <Arduino.h>

// Backbone ESPNOW com filas TX/RX e fragmentação LLF balanceada.
// Envia/recebe FRAMES COMPLETOS (ex.: 520 B do BitChat), fragmentando só no link.
class espn {
public:
  static constexpr size_t MAX_FRAME   = 600;   // >= 520 do teu app
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

  // Se houver frame completo, copia pra 'out' (até *inout_len) e retorna true
  bool rx(uint8_t* out, size_t* inout_len, uint32_t timeout_ms = 0);

  // Quantos frames completos aguardando leitura
  size_t rxAvailable() const;

private:
  // ---------- Confiabilidade (ACK/ARQ) ----------
  static constexpr bool     RELIABLE        = true;   // liga ARQ por hop
  static constexpr uint32_t ACK_TIMEOUT_MS  = 300;    // espera por ACK do gid
  static constexpr uint8_t  MAX_RETRIES     = 9;      // nº de retransmissões
  static constexpr int      MAX_PEERS       = 12;     // peers unicast p/ ACK
  static constexpr uint8_t  LCTL_SIG        = 0xA6;   // assinatura de controle
  static constexpr uint8_t  LCTL_ACK        = 0x01;   // tipo ACK

  // Limites estáticos do driver (buffers)
  static constexpr uint16_t MAX_LINK_MTU = 250;   // ESPNOW seguro no Arduino-ESP32
  static constexpr uint8_t  LLF_SIG      = 0xA5;
  static constexpr uint32_t LLF_TIMEOUT_MS = 500;
  static constexpr int TX_QUEUE_LEN = 16;
  static constexpr int RX_QUEUE_LEN = 16;
  static constexpr int FRAG_QUEUE_LEN = 24;

  struct __attribute__((packed)) LLF {
    uint8_t  sig;                 // 0xA5
    uint8_t  gid;                 // id do grupo (frame)
    uint8_t  idx;                 // índice 0..cnt-1
    uint8_t  cnt;                 // total de frags
    uint16_t total;               // tamanho do frame completo
    uint8_t  net_id[NET_ID_LEN];  // filtro de rede
  };

  struct TxItem { uint16_t len; uint8_t buf[MAX_FRAME]; };
  struct RxItem { uint16_t len; uint8_t buf[MAX_FRAME]; };
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

  // Init/link
  bool initWifiEspNow();
  static void onRecvStatic(const uint8_t* mac, const uint8_t* data, int len);
  void  onRecv(const uint8_t* mac, const uint8_t* data, int len);

  // Tasks
  static void txTaskThunk(void* arg);
  static void rxTaskThunk(void* arg);
  void txTask();
  void rxTask();

  // Fragmentação/Remontagem
  bool llf_sendFrame(const uint8_t* frame, size_t flen);
  bool llf_feed(const uint8_t* pkt, size_t len, const uint8_t** out, size_t* outlen,
                const uint8_t src_mac[6]);
  void llf_gc();

  // Util
  static inline uint8_t make_gid();
  // Peers/ACK helpers
  bool ensurePeer(const uint8_t mac[6]);
  void sendAck(uint8_t gid, const uint8_t dst_mac[6]);  
};
