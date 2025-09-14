# BitChat Relay (ESP32 BLE Hub)

Ultra-lightweight **BLE GATT server** that relays BitChat packets between multiple connected **centrals** (phones, tablets, etc.) while **never acting as a BLE client**. Focus: *low latency, zero blocking, iOS-safe MTU handling, loop/dup resilience, and predictable memory use.*

## Highlights
- **Peripheral-only** (never initiates connections; keeps advertising while connected)
- **Multi-central** (limited by stack/heap; typically 2–4)
- **Automatic MTU awareness** + safe fallback capacity
- **TTL decrement** to reduce endless loops
- **Hash de-duplication** window (FNV-1a 64-bit)
- **Fragmentation + reassembly** (both directions), with bounded memory
- **Non-blocking TX queue** with token-bucket rate limiting
- **Periodic ANNOUNCE** with jitter for presence
- **Dual NOTIFY + INDICATE** support (covers picky mobile apps)
- **Subscription-aware TX** (only sends if CCCD is subscribed)
- **Diagnostic stats** every 10s
- **LED feedback** (RX single blink / TX double blink)
- **Compact single file** (Arduino style)

---

## What’s new
- **Safer CAP/MTU handling**
  - `FALLBACK_CAP` now **20** (ATT 23 → 20-byte payload), safe when no MTU negotiation happened.
  - CAP = **min(MTU across peers) − 3**, clamped to never below 20.
- **Subscription-aware transmit**
  - `notify()`/`indicate()` only when the **CCCD is subscribed**; avoids churn/latency and pointless errors.
- **Robust fragment reassembly**
  - **Eviction** truly removes the **oldest by timestamp** (not map order).
  - Reassembly buffer is **dynamic** (`19 + totalPayload`) with a hard ceiling of **4 KiB** to bound RAM.
  - After reassembly, the hub **re-fragments** to the current CAP before broadcasting.

---

## Hardware / Platform
- **Target:** ESP32 (Arduino core, classic ESP32 BLE library)
- **Requires:** BLE + FreeRTOS (provided by the standard ESP32 Arduino environment)
- **Optional:** On-board LED (default **GPIO 2**; override with `-DLED_PIN=...`)

---

## Building
1. Copy the source into an Arduino sketch (e.g., `BCRelay.ino`).
2. Select your ESP32 board/variant.
3. (Optional) Tune partition & PSRAM for larger buffers.
4. Flash and open **Serial Monitor @ 115200**.

---

## Runtime behavior
At boot:
- Generates an **8-byte sender ID** (BT MAC + efuse bits).
- Initializes BLE name `BitChatRelay_<MAC_SUFFIX>`.
- Creates one **service** and a single **characteristic** with `READ / WRITE / WRITE_NR / NOTIFY / INDICATE`.
- Starts **advertising continuously** (even while centrals are connected).
- Spawns a **TX worker** task (queued, rate-limited).
- Sends an initial **ANNOUNCE**.

Main loop:
- LED timing, announce scheduling, fragment GC, stats logging, dedup window pruning.

---

## Protocol (Observed / Assumed)
Packets begin with:
- **Byte 0:** `0x01` (version / marker)
- **Byte 1:** **Type** (e.g., `0x01` ANNOUNCE, `0x06` FRAGMENT; others relayed transparently)
- **Byte 2:** **TTL** (decremented on relay if `> 0`)
- **Bytes 3–10:** 8-byte timestamp (ms, big-endian) or semantic field
- **Bytes 11–18:** 8-byte **sender ID**
- **Bytes 19…:** type-dependent payload

**Minimum packet length:** 21 bytes.

---

## Capacity / MTU
- **CAP** (max payload per packet over ATT) = **min(MTU among connected peers) − 3** (ATT header).
- If no MTU negotiation (typical iOS default **23**), **fallback CAP = 20**.
- **Note:** Fragment packets require **`cap ≥ 32`** (19 base + 13 fragment header). If CAP < 32, large packets will be dropped until a peer negotiates a larger MTU.

---

## Fragmentation
Triggered whenever an outbound packet exceeds **current CAP**.

**Fragment packet layout** (`Type = 0x06`):
- **Base header** (first 19 bytes preserved)
- **Fragment header** (13 bytes):
  - Bytes 19–26: 8-byte random **fragID**
  - Bytes 27–28: **fragment index** (big-endian)
  - Bytes 29–30: **total fragments**
  - Byte 31: **original (inner) type**
  - Bytes 32…: fragment **payload chunk**

**Reassembly:**
- Keyed by **(senderID, fragID)** with per-index storage.
- Completed when all indices are present or aborted after lifetime expires.
- Reconstructed packet uses **original type** and concatenated payload.
- **Bounded RAM:** reassembly payload limited to **≤ 4 KiB**; exceeding this aborts the assembly.
- After reassembly, the hub passes the packet to the relay path which **re-fragments** according to the current CAP.

---

## De-duplication
- **Window size:** up to **1024** recent hashes, **60 s** time-based eviction.
- **Hash input:** first **19 bytes** (base header) + up to **32** bytes of payload.
- **Hash:** FNV-1a 64-bit. If seen → drop silently (prevents echo storms).

---

## Rate limiting / Queueing
- TX queue holds up to **128** pending packets (beyond that → counted as **backpressure drops**).
- **Token bucket:** `gNotifyBudgetPerSec` (default **150**) refilled every second.
- **Subscription-aware:** the TX worker only calls `notify()` if **notifications** are enabled or `indicate()` if **indications** are enabled on the CCCD. If the CCCD doesn’t exist, it attempts a single `notify()` as fallback.
- Never blocks the main loop.

---

## TTL handling
- If **TTL (byte 2) > 0**, decrement **before** relay.  
- Packets with `TTL = 0` are currently **forwarded** (design choice; you can change to “hard drop” if needed).

---

## LED semantics
- **RX** (incoming write): single short blink.
- **TX** (outgoing relay/send): double blink (on → gap → on).  
Configure via `LED_RX_ON_MS`, `LED_TX_ON_MS`, `LED_TX_GAP_MS`.

---

## Stats log (every 10 s)
Example fields:

[STAT] pktsIn bytesIn pktsOut bytesOut writes notifies indicates heap minCap q tokens dedupWin drops{dedup,backp} inflightB t1_in t2_in peers subs{notify,indicate} MTUs: ...

Use these to tune:
- **Backpressure** (increase queue or `gNotifyBudgetPerSec` carefully)
- **Fragmentation** load
- **Dedup** effectiveness
- **MTU negotiation** success
- **Subscriptions** (make sure clients enabled notify/indicate)

---

## Characteristic behavior
- **READ:** returns last value (informational)
- **WRITE / WRITE_NR:** ingestion path (relay engine entry)
- **NOTIFY / INDICATE:** broadcast path to subscribed centrals  
All outbound frames are set via `setValue()` right before TX.

---

## Configurable constants (selected)
- `ANNOUNCE_INTERVAL_MS`
- `REQUESTED_MTU` (default **517**)
- `FALLBACK_CAP` (**20**)
- `DEDUP_MAX`, `DEDUP_TTL_MS`
- Fragment lifetimes + inflight limits
- `gNotifyBudgetPerSec` (rate control)

---

## Safety / Robustness notes
- Memory pressure is controlled via **bounded reassembly** and **LRU eviction** of the oldest assembly by timestamp.
- Dedup prevents loops; peers should still add layered safeguards in multi-hop topologies.
- No crypto built-in—add signing/encryption upstream if required.
- No persistent storage (RAM only).

---

## Limitations
- Assumes moderate reassembled payloads (**≤ 4 KiB**).
- No flow-control feedback from centrals (fire-and-forget).
- Fragment overhead reduces efficiency for many tiny packets.
- Single characteristic multiplexes all types (intentional minimalism).

---

## Troubleshooting
**Phone sees nothing**
- Ensure the app **subscribed** to notifications or indications (CCCD).  
  Check stats: `subs{notify=?,indicate=?}`.

**High backpressure drops**
- Increase queue depth or raise `gNotifyBudgetPerSec` (carefully).

**Packets are small but still fragmenting**
- Check `minCap` and **peer MTUs** in stats. If stuck at **20**, your central didn’t negotiate MTU (typical for some iOS paths).

---

## Code structure (single file)
- `setup()` – HW + BLE + tasks + initial announce
- `loop()` – housekeeping (LED, announce, stats, fragment GC)
- `txWorker()` – queued, rate-limited, **subscription-aware** notifier/indicator
- `CharCallbacks::onWrite()` – ingest → dedup → TTL-- → (re)assembly → relay
- Fragment utilities + dedup + capacity helpers

---

## License
**MIT License**

Copyright (c) 2024 Ricardo (and contributors)

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the “Software”), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice (including this paragraph) shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

---

## Quick start
1) Flash  
2) Open Serial  
3) Pair a BLE **central**  
4) **Subscribe** to notifications (or indications)  
5) **Write** a properly formatted packet  
6) Observe relay + stats in the console

---

Happy relaying. Contributions welcome.