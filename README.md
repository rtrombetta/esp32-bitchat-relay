# BitChat Relay (ESP32 BLE Hub)

Ultra‑lightweight BLE GATT server that relays arbitrary BitChat packets between multiple connected centrals (phones, tablets, etc.) without ever acting as a BLE client. Focus: low latency, no blocking, safe under iOS MTU limitations, and resilient against loops, duplicates, and oversized payloads.

---

## Highlights

- Peripheral‑only (never initiates connections)
- Multi‑central (limited only by stack/heap; typically 2–4)
- Automatic MTU awareness + safe fallback capacity for iOS
- TTL decrement to reduce endless loops
- Hash‑based de‑duplication window (FNV‑1a 64‑bit)
- Fragmentation + reassembly (outbound + inbound)
- Non‑blocking TX queue with token bucket rate limiting
- Periodic ANNOUNCE with jitter for presence
- Dual NOTIFY + INDICATE support (covers picky mobile apps)
- Diagnostic stats every 10s
- LED feedback (RX blink / TX double blink)
- Compact single‑file implementation (Arduino style)

---

## Hardware / Platform

- Target: ESP32 (Arduino core)
- Requires: BLE + FreeRTOS (provided by standard ESP32 Arduino environment)
- Optional: On‑board LED (default GPIO 2; override via -DLED_PIN=...).

---

## Building

1. Copy source into an Arduino sketch (e.g., `BCRelay.ino`).
2. Board: ESP32 / variant.
3. Set Partition + PSRAM (optional) for larger buffers.
4. Flash + open serial monitor @ 115200 baud.

---

## Runtime Behavior

At boot:
1. Generates an 8‑byte sender ID (BT MAC + efuse bits).
2. Initializes BLE with a deterministic device name: `BitChatRelay_<MAC_SUFFIX>`.
3. Creates a primary service + a single characteristic with READ / WRITE / WRITE_NR / NOTIFY / INDICATE.
4. Starts advertising continuously (even while centrals are connected).
5. Spawns a TX worker task for queued outbound frames.
6. Sends initial ANNOUNCE.

Loop tasks:
- LED timing, announce scheduling, fragment GC, stats logging, dedup window pruning.

---

## Protocol (Observed / Assumed)

Packets begin with:
- Byte 0: 0x01 (version / marker)
- Byte 1: Type (e.g., 0x01 ANNOUNCE, 0x06 FRAGMENT, others relayed transparently)
- Byte 2: TTL (decremented on relay if >0)
- Bytes 3–10: 8‑byte timestamp (ms, big‑endian) or semantic field
- Bytes 11–18: 8‑byte sender ID
- Bytes 19…: Type‑dependent payload

Minimum packet length enforced: 21 bytes.

---

## Fragmentation

Triggered when an outbound packet exceeds current CAP (capacity = min(MTU − 3) across connected peers, or FALLBACK_CAP if default MTU only).

Fragment packet layout (Type = 0x06):
- Base header (first 19 bytes preserved)
- Fragment header (13 bytes):
    - Bytes 19–26: 8‑byte random fragID
    - Bytes 27–28: fragment index (big‑endian)
    - Bytes 29–30: total fragments
    - Byte 31: original (inner) type
    - Bytes 32…: fragment payload chunk

Reassembly:
- Keyed by (senderID, fragID)
- Stored in maps until all indices collected or lifetime expires
- Reconstructed packet uses original type and concatenated payload
- Size safety cap (600 bytes); overflow aborts

---

## De‑duplication

Window:
- Up to 1024 recent hashes
- 60s time‑based eviction
Hash input:
- First 19 bytes (base header) plus up to 32 bytes of payload
Hash: FNV‑1a 64‑bit
If seen → drop silently (prevents echo storms).

---

## Rate Limiting / Queueing

- Queue holds up to 128 pending packets (beyond: counted as backpressure drops).
- Token bucket: gNotifyBudgetPerSec (default 150) refilled each second.
- Each dequeued packet triggers notify(); if any indication subscribers exist, also indicate().
- Never blocks the main loop.

---

## TTL Handling

If TTL (byte 2) > 0, decrement before relay. Packets with TTL=0 still forward (design choice; modify if you need hard stop).

---

## LED Semantics

- RX (incoming write): single short blink.
- TX (outgoing relay / send): double blink (on → gap → on).
- Timing constants configurable at top (LED_RX_ON_MS etc.).

---

## Stats Log (every 10s)

Example fields:
[STAT] pktsIn bytesIn pktsOut bytesOut writes notifies indicates heap minCap q tokens dedupWin drops{dedup,backp} inflightB t1_in t2_in peers subs{notify,indicate} MTUs: ...

Use these to tune:
- Backpressure (increase queue or rate)
- Fragmentation load
- Dedup effectiveness
- MTU negotiation success

---

## Characteristic Behavior

Property set:
- READ: returns last value (informational)
- WRITE / WRITE_NR: ingestion path (relay engine entry)
- NOTIFY + INDICATE: broadcast out path

All outbound frames set via setValue() just before notify()/indicate().

---

## Configurable Constants (selected)

- ANNOUNCE_INTERVAL_MS
- REQUESTED_MTU (517)
- FALLBACK_CAP (safe if MTU renegotiation fails)
- DEDUP_MAX / DEDUP_TTL_MS
- Fragment lifetimes + inflight limits
- gNotifyBudgetPerSec (rate control)

---

## Safety / Robustness Notes

- Memory pressure controlled by fragment caps + LRU eviction of oldest assembly.
- Dedup prevents loops, but protocol peers should still implement additional safeguards if multi‑hop.
- No cryptographic authenticity / integrity included—add signing/encryption upstream if required.
- No persistent storage (all RAM, volatile).

---

## Extending

Ideas:
- Add CRC per packet or per fragment chunk.
- Add optional encryption layer (e.g., ChaCha20 + Poly1305).
- Implement TTL=0 drop rule.
- Provide simpler JSON or CBOR debug output for logs.
- Add command characteristic for runtime tuning (debug level, rate budget).
- Support compressed payload (LZ4 tiny dictionary).
- Host metrics on a secondary UART or WebSerial (if Wi‑Fi enabled).

---

## Limitations

- Assumes moderate packet sizes (< ~600B reassembled).
- No flow control feedback from centrals (fire-and-forget).
- Fragment payload overhead reduces efficiency for many small packets.
- Single characteristic multiplexes all types (intentionally minimalistic).

---

## Troubleshooting

Issue: Phone sees nothing.
- Ensure it subscribed (some apps only set Indications; both enabled here).
- Check stats line for subs{notify=1,indicate=1}.

High drops backpressure.
- Increase queue depth or raise gNotifyBudgetPerSec cautiously.

All packets small but still fragmenting.
- Likely MTU renegotiation failed; minCap will show fallback (182). Investigate central.

---

## Code Structure (Single File)

- setup(): HW + BLE + tasks + initial announce
- loop(): housekeeping (LED, announce, stats, fragment GC)
- txWorker(): queued, rate-limited notifier
- CharCallbacks::onWrite(): core ingest → dedup → TTL-- → fragment logic → relay
- Fragment utilities + dedup + capacity calculation helpers

---

## License

MIT License

Rationale (summary):
- Allows commercial and private use, modification, distribution, sublicensing.
- No copyleft; minimal obligations (retain copyright + license notice).
- Fits small single‑file Arduino / embedded utility; encourages adoption and contributions.
- If you later add crypto or security layers, remains compatible with most ecosystems.

License Notice:
Copyright (c) 2024 Ricardo (and contributors)

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice (including this paragraph) shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

End of License.

---

## Quick Start Summary

Flash → Open Serial → Pair a BLE central → Subscribe to notifications (or indications) → Write a properly formatted packet → Observe relay + stats.

---

Happy relaying. Contributions welcome.