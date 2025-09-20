# BitChat Relay (ESP32 • BLE GATT Server)

Ultra-lightweight **BLE peripheral-only hub** that relays BitChat packets between multiple connected **centrals** (phones/tablets).  
It never acts as a BLE client or scanner. Focus: *low latency, predictable memory, loop/dup resilience, and simple deployment.*

> Codebase: single-file Arduino sketch using **NimBLE-Arduino ≥ 2.3.6**.
> Purpose: Extend coverage between other bitchat devices that are out of range of each other but are in range of the ESP (extends coverage area).

<p align="center">
  <img src="assets/coverage.png" alt="BitChat Relay coverage" width="720">
</p>


---

## ✨ Features (what it actually does)

- **Peripheral-only, keeps advertising while connected** (multi-central)
- **Notify-only TX** (no Indicate path)
- **Fan-out per connection** with **no echo** to the sender
- **TTL enforcement**  
  - Drop if `TTL == 0` on arrival  
  - Decrement once; **drop** if it becomes `0` after `--`
- **De-duplication (FNV-1a 64-bit)** on header (+ first payload bytes) with TTL *zeroed* for hashing
- **Fragmentation + reassembly**  
  - Reassembly on inbound `FRAGMENT_TYPE` (bounded)  
  - Outbound **per-peer** fragmentation based on that peer’s MTU
- **Dynamic CAP** = `min(peer MTUs) − 3`, clamped to **≥ 20** (ATT23 → 20B payload)
- **Bounded reassembly RAM**  
  - Global: `MAX_INFLIGHT_FRAGS=10`, `MAX_INFLIGHT_BYTES=65536`  
  - **Per-sender quotas:** `≤ 4` assemblies and `≤ 16 KiB`
  - **LRU eviction** (oldest by timestamp) + **30s** fragment lifetime GC
- **Non-blocking TX queue** with **token bucket** (≈ 150 notifies/s)
  - Queue size **128**; when full, **drop oldest** and keep flowing
  - Light **per-fragment jitter** (4–10 ms) to avoid lock-step
- **Subscription-aware TX** (only notifies subscribed centrals)
- **Periodic ANNOUNCE** every **30 s + jitter (0.5–4.5 s)**
- **LED feedback** (optional) — RX: single blink; TX: double blink
- **Structured logs** for packets and 10-second status snapshots
- **ESP-Now! Backhaul** connect and exchange packets between other HUBs using ESPNow!

---

## 🛰️ Backhaul over ESP-NOW (NEW)

Adds a **hub↔hub backbone** using **ESP-NOW** (backhaul) while keeping phones on **BLE** (fronthaul).

### What it brings

* **Dedicated backbone**: shared `NET_ID` (8B), fixed **channel** (1/6/11), configurable **link MTU** (≤250).
* **Balanced fragmentation** (`balanced_split=true`): splits frames into **near-equal chunks** (Δ≤1B).
  RX reassembles using `(total, cnt, idx)` only → **works even if MTUs differ** across nodes.
* **Per-frame reliability (ARQ)**: after full reassembly, receiver unicasts an **ACK** (control frame) to sender’s MAC.
  Sender waits `ACK_TIMEOUT_MS` and **retries up to `MAX_RETRIES`**.
* **Bounded reassembly RAM**: 6 parallel slots; GC after `LLF_TIMEOUT_MS` (500 ms).
* **BLE+Wi-Fi coexistence sane defaults**: `WiFi.setSleep(true)` + `esp_wifi_set_ps(WIFI_PS_MIN_MODEM)` (+ optional `esp_coex_preference_set(ESP_COEX_PREFER_BALANCE)`).
* **PHY options**: **Long Range (LR)** enabled by default (robust/longer range). Optional 802.11b **1 Mb/s** if you prefer less airtime.
* **TX power**: `WiFi.setTxPower(WIFI_POWER_19_5dBm)` (adjust to your EIRP/regulatory).

---

## ⚙️ BLE details

- **Service UUID (placeholder):** `F47B5E2D-4A9E-4C5A-9B3F-8E1D2C3A4B5C`  
- **Characteristic UUID (placeholder):** `A1B2C3D4-E5F6-4A5B-8C9D-0E1F2A3B4C5D`
- **Characteristic props:** `READ | WRITE | WRITE_NR | NOTIFY`
- **Requested MTU:** `517` (capable payload ≈ `517−3 = 514` bytes)
- **Advertising interval:** **50–100 ms** (`setPreferredParams(0x50, 0xA0)`)
- **TX path:** `notify(connHandle)` per subscribed peer (no global broadcast API)

> **Note:** Indications are not used. If a central requires Indicate, add that path explicitly.

---

## 🔀 Packet handling

### De-dup
- Hash: **FNV-1a 64-bit** over:
  - First **19B** header with **TTL forced to 0**  
  - Plus up to **32B** of payload
- Window: **deque + set**, up to 1024 entries, evicted after **60 s**
- If seen → **drop** (prevents echo storms)

### TTL
- If `TTL == 0` **before** relay → drop (`[DROP] ttl=0 before relay`)
- Else decrement → if becomes `0` → drop (`[DROP] ttl expired after --`)

### Fragmentation (outbound)
- For each subscribed peer:
  - Compute **capPeer = MTU(peer) − 3**
  - If `len ≤ capPeer` → send direct
  - Else if `capPeer < 32` → cannot fit fragment overhead → drop for that peer
  - Else split into `FRAGMENT_TYPE` frames with header:
    ```
    base 19B header
    + 8B fragID (random)
    + 2B index
    + 2B total
    + 1B originalType
    + chunk
    ```
  - Small **per-fragment jitter** (4…10 ms) between notifies

### Reassembly (inbound `FRAGMENT_TYPE`)
- Key: **(senderID, fragID)**
- Store by index; complete when all indices are present
- Hard ceiling: payload **≤ 4 KiB** (drop if over)
- **Per-sender quotas** enforced before creating an assembly
- **GC:** lifetime **30 s**; evicts oldest with proper counter accounting

---

## 📊 Status log (every ~10 s)
A typical line:

[STAT] pktsIn=… bytesIn=… pktsOut=… bytesOut=… writes=… notifies=… heap=…
minCap=… q=… tokens=… dedupWin=… drops{dedup=…,backp=…}
inflightB=… t1_in=… t2_in=… peers=… subs{notify=…} MTUs: ch=H:mtu=M …
- **minCap**: current global CAP (`min(MTU)−3`)
- **q / tokens**: TX queue size / available notify tokens
- **drops**: dedup / backpressure counters
- **inflightB**: bytes buffered in reassembly
- **t1_in / t2_in**: counters per type (as observed)
- **peers / subs**: connected centrals / subscribed centrals
- **MTUs**: per-connection MTU list

---

## 🧪 Quick start

1. **Flash** the sketch to ESP32 (Arduino core).  
2. Device name: `BitChatRelay_<MAC_SUFFIX>` (auto).  
3. Connect with one or more centrals and **subscribe** (CCCD).  
4. **WRITE / WRITE_NR** BitChat frames to the characteristic.  
5. Watch relaying and `[STAT]` in Serial (115200 baud).

---

## 🔧 Configuration knobs (in code)

- `REQUESTED_MTU` (default `517`)
- Advertising: `adv->setPreferredParams(0x50, 0xA0)` (50–100 ms)
- **Dedup:** `DEDUP_MAX`, `DEDUP_TTL_MS`
- **Reassembly:** `FRAG_LIFETIME_MS`, `MAX_INFLIGHT_FRAGS`, `MAX_INFLIGHT_BYTES`
- **Per-sender quotas:** `MAX_FRAGS_PER_SENDER`, `MAX_INFLIGHT_BYTES_PER_SENDER`
- **TX queue / rate:** `MAX_TX_QUEUE` (128), `gNotifyBudgetPerSec` (≈150/s)
- LED timings: `LED_RX_ON_MS`, `LED_TX_ON_MS`, `LED_TX_GAP_MS`
- Debug level: `debugLevel` (1..4)

---

## 🛠 Build / Environment

- **ESP32 Arduino Core** (tested with `esp32:esp32 2.0.x`)
- **NimBLE-Arduino ≥ 2.3.6**
- Board: **DOIT ESP32 DevKit v1**
- Serial: **115200**
- Optional: define `LED_PIN` (default `2`)

---

## 🧭 Design notes & limits

- Pure **peripheral** (no scan/client).  
- **No Indicate** path implemented (Notify only).  
- Reassembly **RAM-bounded**; large payloads **> 4 KiB** are dropped on reassembly.  
- If a peer is stuck at **MTU 23** (cap=20), outbound fragmentation to that peer is **not possible** (`capPeer < 32`).  
- No built-in crypto; BitChat payload is treated opaque.

---

## 🔒 Privacy & Security

**Transport-only relay.**  
This hub is intentionally dumb: it relays BitChat frames as-is. It never decrypts, parses or rewrites the payload. The only header changes are:
- TTL is **validated** and **decremented**; frames with `TTL=0` are dropped.
- When needed, the hub **fragments** large frames and peers **reassemble**.

### What is (and isn't) collected
- **No payload storage.** The hub does not persist payloads. Fragment buffers are in RAM only and are evicted on completion/timeout (≤30 s) with strict global and per-sender limits.
- **No scanning / no client role.** The device never scans or initiates connections.
- **Stats/logs** include *only* metadata (type, TTL, length, timestamp, senderID, MTUs, counters).  
  ⚠️ At `debugLevel >= 4` a short 8-byte payload **sniff** is printed for troubleshooting. Use `debugLevel <= 3` in production to avoid any payload leakage.
- **De-dup hash.** To suppress loops, the hub keeps a **non-cryptographic FNV-1a 64-bit hash** of the 19-byte header (with TTL zeroed) plus up to 32 bytes of payload. Hashes are kept ~60 s, then evicted. This is not a cryptographic commitment and is used only for duplicate detection.

### Identifiers & linkability
- The **senderID (8 bytes)** defaults to a deterministic value derived from hardware (BT MAC + eFuse). This helps diagnostics but is **linkable** across time/places. If unlinkability is desired, derive `senderID` from an **ephemeral key** or **randomize per boot** upstream.

### BLE link security
- The characteristic is open to `READ/WRITE/WRITE_NR/NOTIFY` and does **not** require pairing/bonding or link-layer encryption by default. Treat the hub as an **untrusted transport** and enforce **end-to-end encryption + integrity** at the BitChat/application layer (e.g., AEAD with nonces, replay protection).
- If your deployment needs link privacy, consider enabling BLE privacy/RPA and encrypted characteristics (depends on your NimBLE build/central support).

### Side effects of MTU/capacity
- When any peer connects with **MTU 23**, the global capacity (`minCap`) becomes **20 B**, which prevents outbound fragmentation to that peer (`cap < 32`). Payload remains opaque; only reachability changes.

### DoS considerations
- TX is rate-limited (token bucket) and queue-bounded; inbound writes are still a potential flood vector. In hostile environments, rate-limit inbound per connection and/or require application-layer auth.

### Recommendations
- Always use **end-to-end encryption & signatures** in BitChat payloads.
- Keep `debugLevel <= 3` in production.
- Rotate or derive `senderID` from session keys if you need unlinkability.
- If needed, modify the dedup hash to operate over **ciphertext only** (still works, avoids hashing plaintext bytes).

---

## 🔍 Troubleshooting

- **Nothing received by phone**  
  Ensure the app **subscribed** to notifications (CCCD). Check `subs{notify=?}` in stats.

- **Queue/backpressure increasing**  
  Raise `MAX_TX_QUEUE` moderately or reduce sending rate (lower `gNotifyBudgetPerSec`), or increase per-fragment jitter.

- **Unexpected fragmentation**  
  Check `minCap` and per-peer MTUs in stats. If `minCap = 20`, your central didn’t negotiate MTU.

---

## 📄 License

MIT License © 2025 Ricardo
