# Over-the-Air Protocol

Complete specification of the morse-station radio link: physical-layer
parameters, packet formats, timing, addressing, and the regulatory basis. The
authoritative source is the firmware (`src/radio.cpp`, `src/protocol.h`,
`src/main.cpp`); this document mirrors it.

The link supports **two keying transports**, selected per transmitter by the
`keymode` command (default `compat`). Hunters are **bilingual** — they decode
either and auto-follow whatever a fox is sending, so the two coexist on one net.

- **`compat` — keystate broadcast** (legacy, "approach A"). Below.
- **`edge` — edge keying.** On-edge packets carrying TX-measured durations. See
  *Model — edge keying* and the `EdgeEvent` format. Bench-validated to decode
  far more reliably than keystate; see `edge-events.md`.

## Model — keystate broadcast (`keymode compat`)

The transmitter sends its **current key-down/up state** on a fixed 30 ms cadence;
receivers reconstruct element timing from the stream and drive their own
sidetone and decoder.

- **Loss behavior.** No ACKs, no retries; a dropped packet is nominally
  corrected by the next one ~30 ms later. **But** the stream is high-rate
  (~25–33 pkt/s): on a real bench it sheds ~19 % of packets *even at point-blank
  range* (airtime saturation), and because the receiver **samples** key level,
  every drop perturbs timing. At point-blank this measurably garbles the decode
  — the error floor that motivated edge keying. See the comparison in
  `edge-events.md`.
- **One format, two sources.** The canned fox loop feeds the same keystate
  stream the live telegraph key produces, so a single protocol carries both the
  pre-programmed fox and live keying to any hunter.
- **Stateless receivers.** A hunter needs no association, pairing, or prior
  knowledge — it locks onto the first valid packet it hears.

## Model — edge keying (`keymode edge`)

Instead of sampling key level on a clock, the transmitter sends a packet only on
each key **edge**, carrying the **TX-measured duration** of the segment that just
ended. This decouples decode timing from lossy, jittered radio arrival times: the
receiver feeds those exact durations to its element classifier, so on-air jitter
and the inter-packet rate no longer corrupt timing.

- **8× fewer packets.** Only edges (plus an idle heartbeat) transmit, not a 30 ms
  clock — ~3 pkt/s vs ~25. On the bench that alone dropped loss from ~19 % to
  ~2 %.
- **Single-loss self-heal.** Each `EdgeEvent` carries the duration of the segment
  before the current one too (`dur_prev_ms`), so one lost packet is fully
  reconstructed from the next (detected via a `seq` gap of 1). A gap ≥ 2 flushes
  the half-built character rather than emit wrong timing — graceful degradation.
- **Idle heartbeat (`HEARTBEAT_MS` = 700 ms).** When the key holds steady longer
  than this, the current state is re-sent as a *heartbeat* `EdgeEvent` (flag set,
  **same `seq`, not incremented**). Provides presence, late-joiner re-anchor, and
  double-loss recovery without the 30 ms stream. Heartbeats keep the prior edge's
  `seq` because at slow speeds (5 WPM: dah = 720 ms, word gaps > 700 ms) they fire
  mid-segment; consuming a `seq` would be indistinguishable from a lost edge and
  break the self-heal.
- **Bilingual / migration.** A `compat` hunter decodes an `edge` fox and vice
  versa; a fox flips per-unit with `keymode edge` (persisted). Default `compat`
  keeps fresh/legacy units byte-for-byte unchanged.

## Physical layer (SX1262 GFSK)

All values from `src/radio.cpp`. Identical on both platforms (the radio is an
SX1262 either way — see `components/sx1262.md`).

| Parameter | Value | Notes |
|---|---|---|
| Modulation | GFSK | SX126x has no OOK |
| Centre frequency | **905.0 MHz** | single fixed channel, no hopping |
| Bit rate | **4.8 kbps** | sweet spot: sensitivity vs. 30 ms airtime |
| Frequency deviation | **5.0 kHz** | |
| RX bandwidth | **39.0 kHz** | ≈ Carson BW for 4.8k/5k with margin |
| Preamble | **16 bits** | |
| Sync word | **`{0x2D, 0xD4}`** | identifies our net; foreign traffic rejected in HW |
| TCXO voltage | **1.8 V** | confirmed on Heltec V4 and the Cap LoRa-1262 |
| RF switch | **DIO2** | `setDio2AsRfSwitch(true)` drives the TX/RX antenna switch on both boards |
| Default chip power | **+2 dBm** at init | overridden per fox power level (see below) |

Per-packet airtime at these settings is ~12 ms; the 30 ms send interval leaves
margin.

## Packet formats

Three packet types share the link and are disambiguated by their first byte (the
"magic"): `KeyState` (`'M'`), `Ident` (`'I'`), and `EdgeEvent` (`'E'`). A
receiver ignores any packet whose magic it doesn't parse, so the keystate and
edge transports coexist and legacy nodes ignore edge traffic. All are
little-endian (native ESP32-S3 layout).

### KeyState — 5 bytes (`proto::KeyState`)

Sent every `TX_INTERVAL_MS` while transmitting.

| Offset | Field | Type | Value |
|---|---|---|---|
| 0 | `magic` | u8 | **`0x4D`** (`'M'`) — sanity/version byte |
| 1 | `station_id` | u8 | transmitter's ID, 1..254 |
| 2 | `key_down` | u8 | 0 = key up, 1 = key down |
| 3–4 | `seq` | u16 LE | wraps; for loss/ordering diagnostics only |

`decode()` validates length ≥ 5 **and** `buf[0] == 0x4D` before accepting.

### Ident — 14 bytes (`proto::Ident`)

The station-ID / timing packet. Sent at the top of each fox message loop **and**
on the periodic legal cadence. Carries the operator callsign and the fox's
keying speeds so a hunter can seed its decoder thresholds to the fox's *actual*
timing instead of its own config — required to correctly decode slow or
Farnsworth sending.

| Offset | Field | Type | Value |
|---|---|---|---|
| 0 | `magic` | u8 | **`0x49`** (`'I'`) |
| 1 | `station_id` | u8 | transmitter's ID |
| 2 | `wpm` | u8 | overall (effective) speed |
| 3 | `char_wpm` | u8 | Farnsworth character speed (≥ `wpm`) |
| 4–13 | `call` | char[10] | ASCII callsign, NUL-padded |

`CALLSIGN_MAX` is 10. `decode_ident()` validates length ≥ 14 and `buf[0] ==
0x49`.

### EdgeEvent — 8 bytes (`proto::EdgeEvent`)

Sent only on a key edge (or an idle heartbeat) when the transmitter is in
`keymode edge`. Disambiguated from `KeyState` by its magic, so legacy
keystate-only receivers ignore it.

| Offset | Field | Type | Value |
|---|---|---|---|
| 0 | `magic` | u8 | **`0x45`** (`'E'`) |
| 1 | `station_id` | u8 | transmitter's ID |
| 2 | `seq` | u8 | per-**edge**, wraps mod 256; loss/dup/reorder detect. Heartbeats reuse the last edge's `seq` (not incremented). |
| 3 | `flags` | u8 | bit 0 `EDGE_FLAG_DOWN` = key level **entered** (1 = down, 0 = up); bit 1 `EDGE_FLAG_HEARTBEAT` = re-assert, not a real edge |
| 4–5 | `dur_now_ms` | u16 LE | duration of the segment that **just ended** |
| 6–7 | `dur_prev_ms` | u16 LE | duration of the segment **before that** (single-loss heal) |

`decode_edge()` validates length ≥ 8 and `buf[0] == 0x45`.

> **Semantics gotcha (cost real debugging time):** `flags` carries the level
> being *entered*, while `dur_now_ms` is the segment that just *ended* — i.e. the
> **opposite** level. So in the `debug` dump `RX E <id> seq=N lvl=L hb=H dn=D
> dp=P`: when `lvl=1` (entering key-down), `dn` is the duration of the key-**up**
> gap that just ended; when `lvl=0`, `dn` is the key-**down** element that just
> ended. Reading `dn` as belonging to `lvl` inverts every element↔gap and makes a
> perfectly-timed stream look broken.

## Timing

| Constant | Value | Meaning |
|---|---|---|
| `TX_INTERVAL_MS` | **30 ms** | keystate (`compat`) send period |
| `HEARTBEAT_MS` | **700 ms** | edge (`edge`) idle re-assert period (presence / re-anchor / double-loss recovery) |
| `IDENT_INTERVAL_MS` | **8 min** | periodic Ident cadence (well under the 10-min Part-97 limit) |
| `REPEAT_PAUSE` | **12 s** | gap after a full fox message before it repeats |
| RX key watchdog | `compat`: ~3 units at the current WPM; `edge`: ~2.5 × `HEARTBEAT_MS` (~1.75 s) | no packet for this long forces key-up → sidetone off → idle, so a mid-key signal loss can't latch the key on. The edge timeout is mode-aware so a slow keyer's normal heartbeat gaps don't trip it. |
| Hunter copy blanking | 10 s | the decoded line clears this long after the last packet (shorter than `REPEAT_PAUSE`, so each message clears before the next repeat) |

## Addressing

- Each station has a **1-byte ID, 1..254**. ID **255 is broadcast**
  (`BROADCAST_ID`).
- The default ID is derived from the factory eFuse MAC (6 bytes XOR-folded into
  1..254), so it is stable per unit with no provisioning. An explicit `id <n>`
  override persists in NVS. See `src/config.cpp`.
- The fox transmits with its own `station_id`; hunters display it as `RECV <id>`
  and key off the first one they hear.
- **Open item:** a hunter does not yet filter to a single fox — once locked to
  one fox it should ignore other stations until timeout (see `TODO.md`).

## Fox TX power levels

The fox cycles four levels (PRG/G0 tap, or `pwr <0..3>`). `dbm` is the **SX1262
chip output**. The selected index persists in NVS and is restored on boot
(default LO).

| Index | Label | Chip dBm |
|---|---|---|
| 0 | LO | −9 |
| 1 | MED | +2 |
| 2 | HI | +14 |
| 3 | MAX | +22 |

The labels are **verbally approximate** — a coarse operator gradient, not a
calibrated EIRP. `dbm` is the SX1262 chip output and is **not** adjusted for any
FEM gain; +22 dBm is the SX1262 ceiling (`setOutputPower` clamps above it).

- **No-FEM boards** (Cardputer ADV, Wio Tracker L1, RAK4631): chip output *is*
  the antenna power, so MAX = **+22 dBm** at the antenna.
- **Heltec V4** (external FEM PA+LNA): in a transmit run mode (**Fox** / **Live
  Key**) the firmware engages the FEM PA at boot (`radio::set_pa(true)` in
  `setup()`), adding **~+6 dB**. The PA is *not* re-switched per level —
  `set_tx_power()` only moves the chip output — so the whole curve shifts up by
  the PA gain and MAX radiates **~+28 dBm** (22 + 6). Hunter (RX) leaves the PA
  bypassed. The `pa` console command can override the PA at runtime for bench A/B.

**Cross-board disparity (accepted):** because the labels are approximate, the
same MAX means **~+28 dBm on the Heltec V4** but **+22 dBm on the Wio / no-FEM
boards**. This is intentional for now. A proper per-board EIRP calculation
(reading the actual FEM gain per V4 revision, since the V4.3 KCT8103L PA gain
differs from the V4.2 GC1109) is the **FEM-PA item in `TODO.md`** and
`components/heltec-v4.md`.

Run the fox at **LO** in a small space: at MED/HI the link saturates RSSI across
the whole search area and the hunter's "tune for max volume" gradient stops
working.

## Regulatory basis

The link runs in the **902–928 MHz** band. Two legal frameworks apply; the
firmware is compatible with both (plain sync word, **no encryption**).

- **FCC §15.249 (unlicensed, default).** Narrowband, non-hopping operation is
  capped at ~94 mV/m at 3 m (≈ **−1 dBm EIRP**). This is the mode the firmware
  runs in today: single fixed channel, low power. Range is short (~200–400 m)
  but legal with zero extra work. **Keep the fox at LO/MED for unlicensed use.**
- **FCC Part 97 (licensed fox).** If the fox is operated by a licensed amateur,
  it can run on the 33 cm amateur band (902–928 MHz) at higher power with no FHSS
  requirement, subject to two conditions the firmware already meets: (1)
  **identify with the operator callsign at least every 10 minutes** — the Ident
  packet (8-min cadence) and the callsign in the keyed message both satisfy this;
  (2) **no encryption / no obscuring** — the link uses only a public sync word.

### FHSS (postponed)

§15.247 permits up to **+30 dBm** unlicensed in this band, but **only** for
frequency-hopping (or DSSS) systems. The firmware does **not** hop today. If
range under §15.249 proves insufficient, the planned design is:

- ~50 channels spaced 200 kHz from 903.0–912.8 MHz (clear of cellular at 928+).
- Hop on every TX; the SX1262 retunes in well under 1 ms.
- Both ends run the same pseudo-random sequence seeded from a fixed key + shared
  epoch (e.g. a 256-entry table indexed by `packet_counter mod 50`). Any hop
  sequence must be **publicly documented** to remain Part-97-legal.
- Receivers acquire by scanning all channels until they catch a beacon, then
  lock to the schedule via the `seq` field. (RX scan-and-lock state machine is
  unbuilt.)
- Dwell check: 30 ms × 50 channels = 1.5 s full cycle → each channel sees ~13
  hits per 20 s window, satisfying §15.247(a)(1) "approximately equal use".

## Commands & provisioning

Station identity and parameters (callsign, message, ID, speeds, power, mode,
mute) are set over a serial or BLE console, not on the air. See
`commands.md`.
