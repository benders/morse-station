# Over-the-Air Protocol

Complete specification of the morse-station radio link: physical-layer
parameters, packet formats, timing, addressing, and the regulatory basis. The
authoritative source is the firmware (`src/radio.cpp`, `src/protocol.h`,
`src/main.cpp`); this document mirrors it.

## Model — keystate broadcast

The transmitter sends its **current key-down/up state** on a fixed cadence;
receivers reconstruct element timing from the stream and drive their own
sidetone and decoder. This is "approach A" from the original design.

- **Self-healing.** No ACKs, no retries. A dropped packet is corrected by the
  next one ~30 ms later. At 15–30 WPM a single lost packet never elides an
  element.
- **One format, two sources.** The canned fox loop feeds the same keystate
  stream the live telegraph key produces, so a single protocol carries both the
  pre-programmed fox and live keying to any hunter.
- **Stateless receivers.** A hunter needs no association, pairing, or prior
  knowledge — it locks onto the first valid packet it hears.

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

Two packet types share the link and are disambiguated by their first byte (the
"magic"). A receiver parsing only `KeyState` simply ignores `Ident` (wrong
magic), and vice-versa. Both are little-endian (native ESP32-S3 layout).

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

## Timing

| Constant | Value | Meaning |
|---|---|---|
| `TX_INTERVAL_MS` | **30 ms** | keystate send period |
| `IDENT_INTERVAL_MS` | **8 min** | periodic Ident cadence (well under the 10-min Part-97 limit) |
| `REPEAT_PAUSE` | **12 s** | gap after a full fox message before it repeats |
| RX key watchdog | ~3 units at the current WPM | no keystate for this long forces key-up → sidetone off → idle, so a mid-key signal loss can't latch the key on |
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

+22 dBm is the SX1262 ceiling (`setOutputPower` clamps above it).

- **Cardputer ADV** (no FEM): chip output *is* the antenna power. +22 dBm is the
  real ceiling.
- **Heltec V4** (has a FEM PA+LNA): the FEM PA mode is **not** switched per
  level — `CPS` is set to bypass once at init and `set_tx_power()` only changes
  the SX1262 chip output. So the antenna power tracks the chip through the FEM
  bypass path (less a small insertion loss). In practice the V4.2 (GC1109) PA
  appears to engage anyway, so its real EIRP runs hotter and mismatched vs. the
  V4.3 — see the FEM-PA item in `TODO.md` and `components/heltec-v4.md`.

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
