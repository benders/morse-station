# Morse Station — Work Plan

Remaining work toward a working modified fox hunt: a stationary **fox** transmits
a Morse description of its location; **hunters** carry receivers that decode it by
ear (and on the display).

Platform: **Heltec WiFi LoRa 32 V4** (ESP32-S3 + SX1262) as the primary unit, with
the **M5Stack Cardputer ADV** as an experimental second fox sharing one `src/`
tree. Starting at low power on a single channel — **FHSS postponed**.

Completed items have moved to **`DONE.md`**. Reference material lives in `docs/`:
- `docs/protocol.md` — over-the-air protocol + parameters (and the regulatory basis / FHSS plan).
- `docs/commands.md` — the serial / BLE command reference.
- `docs/components/` — per-component lessons (Heltec V4, Cardputer ADV, SX1262, PAM8403).

Legend: `[ ]` todo · `[~]` in progress · `[x]` done · `[?]` needs a decision

---

## Stage 1 — Audio out (sidetone)

- [~] Wire the amp: **PAM8403** class-D board (Amazon B0DPMNYR2B) driving a
      **4 Ω** speaker (Amazon B0F3DBRXS5). PAM8403 takes an analog audio input
      and runs off 2.5–5 V — power it from the Heltec 3V3/5V rail, common GND.
      Pin: **GPIO4** → ~1 kΩ series + 100 nF → amp L_IN. (GPIO4, not GPIO7 —
      GPIO7 is FEM power on the V4.) See `docs/components/pam8403.md`.

## Stage 3 — Radio link

- [ ] **Polish/range + compliance:** TX gain mismatched between revisions
      (V4.2 ~+34 dB hotter than V4.3 — different FEM TX path/PA mode under the
      superset config). An SDR several floors away hears us at −80..−90 dBm with
      **no RX gain**, i.e. the V4.2 PA is clearly engaging and we are NOT in the
      +2 dBm low-power regime we configured. Range is plentiful for the camp
      either way. Before any deliberate outdoor deploy: equalise both boards to a
      matched power and decide the legal basis — §15.249 low-power unlicensed vs
      operating under an amateur license in the 902–928 MHz (33 cm) band (see
      `docs/protocol.md`). Not blocking the hunt.
- [ ] **Long-term: deliberately enable + control the Heltec V4 FEM PA.** Today
      `fem_power_on()` writes `PIN_FEM_CPS` LOW once at init (intended PA bypass)
      and never touches it again; `set_tx_power()` only calls the SX1262
      `setOutputPower()`, so the FEM PA mode does **not** track the LO/MED/HI/MAX
      level. Net antenna power is ill-defined and mismatched between revs (V4.3
      KCT8103L runs bypassed; the V4.2 GC1109 PA empirically engages anyway). To
      use the V4 as a real higher-power fox: (1) understand each rev's CPS/mode
      truth table (GC1109 vs KCT8103L differ), (2) drive the PA mode pin
      deliberately — ideally per power level, engaging the PA only at HI/MAX,
      (3) equalise the two revisions to a known EIRP, and (4) settle the legal
      basis before any boosted outdoor deploy. Not needed while the **Cardputer
      ADV is the fox** (no FEM, +22 dBm ceiling). See
      `docs/components/heltec-v4.md`.

## Stage 6 — Fox-hunt integration

- [ ] **New "standby" mode — radio off, BLE alive.** A mode that neither
      transmits nor receives on the SX1262 and powers down the radio + RF
      amplifiers (SX1262 to sleep; on the Heltec drop VFEM/FEM, on the Cardputer
      no FEM), **but keeps NimBLE up and responsive** so an operator can stage,
      transport, or pre-position a node and then arm it over the air. Unlike
      `hibernate()` (deep sleep, only RST wakes, BLE dead), standby keeps the CPU
      + BLE running and the display can show a "STANDBY" status. Add `MODE_STANDBY`
      to the `Mode` enum and the boot menu, and make it remotely selectable: an
      operator sends `mode <standby>` + `reboot` to park a node, later `mode 0/1/2`
      + `reboot` to bring it on-air. Implementation: skip `radio::init()` (or init
      then `radio::sleep()`), guard the per-mode `loop_*` so the main loop only
      services `ble_provision::process()` + display, and add a power-down that
      also drops the FEM rail on the Heltec. Note `mode <n>` currently clamps to
      0..2 in `handle_setup_command`; extend it. (`src/main.cpp`, `src/radio.cpp`.)
- [ ] Field test at range; tune power and message cadence (hardware).
- [ ] "RECV" station ID and signal strength bar should clear after timeout from
      last received packet.
- [ ] When receiving from one Fox (not timed out), packets from other stations
      must be ignored.

## Stage 7 — Range & polish

- [ ] Field-test low-power range across the camp area.
- [ ] **Later phase, only if range falls short:** FHSS — 50-channel hop table +
      RX scan-and-lock for §15.247 full-power operation. Design sketch in
      `docs/protocol.md`.
- [ ] Callsign ID: fox sends a periodic station-ID packet (`proto::Ident`, 8-min
      cadence, `tx_ident` in `src/main.cpp`) for Part 97 operation; the callsign
      also rides in the fox message text for an audible CW ID. Still missing an
      end-of-communication ID (no clean fox-exit path today). Only relevant under
      an amateur license — see `docs/protocol.md`.
- [ ] Enclosure, antenna build (hardware).

---

## Cardputer ADV port (experimental fox)

See `docs/components/cardputer-adv.md` for the hardware facts and lessons.

### Stage C1 — Fox bring-up on hardware

- [ ] **Sidetone (needs an ear):** confirm a clean keyed tone from the onboard
      speaker / 3.5 mm jack via M5.Speaker at the fox WPM; volume sane.
- [~] **Volume / mute control for the Cardputer.** Mute is done (see `DONE.md`).
      Still TODO: a graduated `vol <0..n>` level (M5.Speaker master volume) for
      finer control. (`sidetone_cardputer.cpp`, `handle_setup_command`/`loop` in
      `src/main.cpp`, `config.{h,cpp}`.)
- [ ] **On-air TX (needs a hunter):** confirm a Heltec hunter copies the
      Cardputer fox on 905.0 MHz. Check `setOutputPower` LO/MED/HI/MAX (no FEM →
      MAX +22 dBm is the real ceiling). G0 tap should cycle power.
- [ ] **Poor RX sensitivity on the Cardputer.** As a hunter the cap reads weak —
      the RSSI gauge sits ~halfway with devices ~1 ft apart, where a Heltec pins
      near full. The cap is a bare SX1262 with **no LNA/FEM**, so some gap is
      expected, but halfway at a foot is worse than that. Diagnose: confirm the
      RP-SMA antenna is seated, compare raw RSSI cap vs Heltec at a fixed
      distance, recheck the cap's SX1262 RX config (rxBw, no accidental gain
      reduction / `setRxBoostedGainMode`), and verify the shared SPI/SD bus isn't
      degrading the link. Decide whether the cap is RX-capable enough to be a
      hunter or is fox-only. (`src/radio.cpp`, `loop_hunter` in `src/main.cpp`.)
- [ ] Fox `setOutputPower` LO/MED/HI/MAX + G0 power-cycle; Ident cadence; LCD "*".

### Stage C2 — Keyboard entry

- [ ] **Verify on hardware:** keymap/shift correctness (esp. symbols), debounce
      feel, that the opt-in window times out cleanly, and that edited values
      persist + drive the fox loop.
- [ ] **Intermittent crash while typing a message on the keyboard**, followed by
      several reboots (seen once on HW 2026-05-31, did not reproduce). No serial
      capture of the panic yet — the USB port re-enumerates on the reboot so a
      plain `cat` of the port dies; capture with a reconnecting loop and watch for
      the ESP32 backtrace. Suspects: TCA8418 FIFO handling / keynum decode in
      `keyboard_cardputer.cpp`, or the editor buffer in `config_ui_cardputer.cpp`
      (bounds on append?). The `# boot #N reason` NVS log records the reset reason.
- [?] Decide whether keyboard config also covers wpm/farns/id, or those stay
      serial-only (currently serial-only).

### Stage C3 — Polish (later)

- [ ] Confirm legal power basis at +22 dBm (no FEM) — see `docs/protocol.md`.
- [ ] Optional: use the cap's ATGM336H GNSS to put the fox's grid/coordinates in
      the message automatically.
- [ ] Battery/run-time check (1750 mAh onboard) for a fox left running.
- [ ] Live-key mode: needs a physical key on a free GPIO (Grove G1/G2 — confirm);
      out of scope for the experimental fox.
