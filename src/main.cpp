#include <Arduino.h>
#include "pins.h"
#include "sidetone.h"
#include "morsekey.h"
#include "morse.h"
#include "radio.h"
#include "protocol.h"
#include "battery.h"
#include "display.h"
#include "config.h"
#include "config_ui.h"
#include "keyboard.h"     // Cardputer 'm' = mute (header self-guards to that board)
#include "ble_provision.h"
#include "platform.h"     // restart() / system_off() / reset_reason() / unique_id_byte()
#include "kv.h"           // kv::Store — persist boot/reset-reason log
#include <atomic>         // SPSC ring head/tail + keyer flags (TODO-fox-timing.md)

// Stage 6 — integrated fox-hunt firmware.
//
// Boot menu (PRG/BOOT button): short press cycles the highlighted mode, a long
// press (>=600 ms) selects it; auto-selects after 8 s. Modes:
//   HUNTER  — receive edge keying (or a text frame), play Morse sidetone, show
//             decoded text + RSSI.
//   FOX     — loop the canned location message: key it as edge Morse, or (default)
//             send it as a text frame the hunter renders locally.
//   LIVEKEY — transmit a live telegraph key as edge events, play local sidetone.
//
// Edge keying (proto::EdgeEvent) carries both FOX (keyed) and LIVEKEY to any
// HUNTER; the canned fox can instead send a proto::TextMsg (msgmode text).

static constexpr uint32_t TONE_HZ      = 750;
// Gap after a full message before it repeats; long enough for a hunter to
// register the end-of-message before the loop restarts.
// Longer than the hunter's 10 s copy-blanking window (see loop_hunter) so each
// message clears from the hunter's screen before the next repeat begins.
static constexpr uint32_t REPEAT_PAUSE = 12000;

// Instructor remote control (docs: instructor station). The fox listens for
// control packets during the tail of its inter-message pause; this window is
// kept under the hunter's 3 s presence timeout so a hunter doesn't drop the fox
// while it's briefly silent.
//
// Burst timing matters: at 4.8 kbps a ~100-byte `msg` command is ~200 ms on air,
// far longer than the 30 ms keystate cadence, so blind continuous bursting would
// collide with the fox's stream and garble hunter copy for the whole delivery.
// Instead the instructor *silence-syncs* to a unicast target: the fox is silent
// only during its RX window (otherwise it streams key-up keystate every 30 ms, or
// in edge mode a heartbeat at least every 700 ms), so a gap longer than
// CTRL_SILENCE_MS uniquely marks the window — the instructor bursts only then,
// landing in the window with no keystate collision. Before the target is heard
// (and for broadcast, which has no single phase to track) it falls back to a
// sparse periodic probe. Bursting stops early once a unicast target acks, or
// after CTRL_BURST_WINDOW (a safety bound covering one long, slow fox cycle).
static constexpr uint32_t CTRL_RX_WINDOW_MS   = 2500;
static constexpr uint32_t CTRL_BURST_INTERVAL = 250;     // min gap between bursts
static constexpr uint32_t CTRL_SILENCE_MS     = 800;     // > 700 ms heartbeat → window
static constexpr uint32_t CTRL_PROBE_INTERVAL = 1500;    // sparse probe before target heard
static constexpr uint32_t CTRL_BURST_WINDOW   = 90000;
static constexpr uint32_t CTRL_ACK_LISTEN_MS  = 300;     // blocking RX right after a
                                                         // burst to catch the prompt ack;
                                                         // covers the target's ack burst
// The target answers each control burst with a short burst of identical acks
// rather than a single packet. A phone attached to the *instructor* makes NimBLE
// connection events preempt its loopTask in the gap between TX-complete and
// re-arming RX, so a lone ack flies past a momentarily-deaf SX1262 and is lost
// (systematically, since the preemption phase is ~periodic). Spreading several
// copies ~ACK_GAP_MS apart guarantees at least one lands after RX is armed and
// outside a BLE blind spot. ACK_REPEATS*ACK_GAP_MS must stay under CTRL_RX_WINDOW
// and is what CTRL_ACK_LISTEN_MS above is sized to cover.
static constexpr uint8_t  ACK_REPEATS = 4;
static constexpr uint32_t ACK_GAP_MS  = 50;

// Text-frame canned-message burst (field note §7, docs/plan-text-message-mode.md).
// In msgmode=text the fox sends the whole clue once per message cycle as a small
// MAGIC_TEXT frame, repeated TEXT_REPEATS times ~TEXT_GAP_MS apart so a single
// dropped copy doesn't lose the cycle's clue — same loss-tolerance idiom as the
// ACK/broadcast bursts. The hunter dedups the redundant copies by seq.
static constexpr uint8_t  TEXT_REPEATS = 4;
static constexpr uint32_t TEXT_GAP_MS  = 50;

// Text-render sync / DF beacon (field note §8, docs/plan-text-sync-beacon.md).
// In msgmode=text the fox emits a MAGIC_SYNC beacon every BEACON_MS across the
// WHOLE loop (render + inter-message pause) so RSSI never dies for DF and every
// hunter slaves its local render to the fox's position. While a clue renders the
// fox also re-bursts the TextMsg every TEXT_RESEND_MS so a station that tuned in
// mid-message picks up the clue text (existing hunters dedup the resend by seq).
static constexpr uint32_t BEACON_MS      = 200;   // sync/DF cadence (<=250 ms req)
static constexpr uint32_t TEXT_RESEND_MS = 2000;  // re-burst clue for mid-joiners

// Callsign and fox message live in NVS (config), set via the boot serial console
// (see run_setup_console). The callsign is sent in the periodic station-ID
// packet; include it in the fox message text too for an audible CW ID.

enum Mode { MODE_HUNTER = 0, MODE_FOX = 1, MODE_LIVEKEY = 2, MODE_INSTRUCTOR = 3,
            MODE_HIBERNATE = 4 };
static Mode mode = MODE_HUNTER;

// Fox TX power levels, cycled with the PRG button (sets the SX1262 *chip*
// output only). Default LO — bump up for open ground. LO keeps the hunter
// volume gradient working in a small space (RSSI saturates at MED/HI).
struct PwrLevel { const char* label; int dbm; };
// dbm is the SX1262 *chip* output, and the LO/MED/HI/MAX labels are deliberately
// VERBALLY APPROXIMATE — a coarse operator gradient, not a calibrated EIRP. On
// the Cardputer / Wio / RAK (no FEM) the chip figure IS the antenna power; +22
// dBm is the SX1262 ceiling (setOutputPower clamps/rejects above this), so MAX
// means +22 dBm there. On the Heltec V4 the external FEM sits after the chip and
// in a transmit mode (Fox / Live Key) we engage its PA (see setup(): radio::
// set_pa(true)), adding ~+6 dB — so the SAME MAX label radiates ~+28 dBm on the
// V4 vs +22 on the Wio. That disparity is accepted for now (the labels are
// approximate); a proper per-board EIRP calculation is the FEM-PA TODO below and
// in docs/protocol.md. The chip figure below is NOT adjusted for FEM gain.
static const PwrLevel PWR_LEVELS[] = {{"LO", -9}, {"MED", 2}, {"HI", 14}, {"MAX", 22}};
static const int N_PWR  = 4;
static int       pwr_idx = 0;   // LO (-9 dBm)

// V4 FEM power-amplifier (CPS) state. Engaged automatically in the transmit run
// modes (Fox / Live Key) by setup(); left bypassed in Hunter (RX) and on non-FEM
// boards. The `pa` console command is a runtime override on top of this default.
static bool g_pa_on = false;

// BLE-UART provisioning power policy. `g_ble_on` is the *actual* advertising
// state (managed by apply_ble); `g_ble_mode` is the policy the `ble` console
// command selects:
//   AUTO (default) — BLE follows the panel: lit = advertising, idle-blanked =
//                    off, so an interacted-with node is reachable and an idle one
//                    reclaims the ~70 mA 2.4 GHz core.
//   ON  — force advertising regardless of the panel (e.g. to reach an idle
//         instructor sitting on its blanked "ready" screen).
//   OFF — force the core down regardless of the panel.
// The instructor control path is over GFSK, not BLE, so OFF/AUTO never strand a
// deployed fox. Not persisted — boot starts AUTO with BLE up.
enum BleMode { BLE_AUTO, BLE_ON, BLE_OFF };
static BleMode g_ble_mode = BLE_AUTO;
static bool    g_ble_on   = true;

// Effective board model for `show`/`model`. The Heltec V4 sub-revision is
// auto-detected from the FEM strap at radio::init() (V4.2 GC1109 vs V4.3
// KCT8103L); every other board has the single fixed compile-time model. Requires
// the radio to be initialised first (it is, by the time any console runs).
static const char* board_model_str() {
#if defined(DEVICE_HELTEC_V4)
    return strstr(radio::fem_name(), "V4.3") ? "heltec-v4.3" : "heltec-v4.2";
#else
    return config::default_board_model();
#endif
}

static morse::Player  player;
static morse::Decoder decoder;
static MorseKey       key;

// True once setup() has finished bringing up the radio and run mode, so the
// provisioning parser may apply changes live (re-key the player, retune the
// SX1262). False during the boot serial/keyboard console, where the parser only
// writes NVS and the values take effect at the normal boot points — keeping the
// bench path behaviorally identical to before. See docs/commands.md.
static bool g_live_apply = false;

static uint16_t seq = 0;

// Fox canned-message delivery mode (field note §7, docs/plan-text-message-mode.md):
// "keyed" keys the clue as edge Morse over the air; "text" sends the clue as one
// MAGIC_TEXT frame + retransmit burst and the hunter renders Morse locally.
// Restored from config::msgmode() at boot; the `msgmode` console command sets it
// live. Applies to the canned loop_fox cycle only — MODE_LIVEKEY always keys
// edges. Default "text" (robust at range); "keyed" is for testing the keyed path.
// All keying uses the edge transport (EdgeEvent); the legacy keystate-broadcast
// "compat" transport was removed (see docs/protocol.md historical note).
enum MsgMode { MSGMODE_KEYED = 0, MSGMODE_TEXT = 1 };
static uint8_t g_msgmode = MSGMODE_TEXT;
static uint32_t last_ident = 0;

// Unattended-test instrumentation (E4, docs/edge-events.md "Unattended test
// instrumentation"): when on, dumps one parseable line per RX/TX/decode event
// to Serial so a harness can assert on decoded text without sidetone/display.
// Off by default — keeps normal operation quiet. Runtime-only (not persisted).
static bool g_debug = false;

// Fox TX halt (`stop`/`start` console commands). When set, the Fox suppresses
// ALL radio output — keystate/edge streams AND the periodic Ident/station-ID
// packets — so the transmitter goes completely silent until `start` is sent.
// Semantics: the halt takes effect IMMEDIATELY at the TX-emission point; we do
// NOT wait for the current element or message to finish. The message player
// keeps running internally (cheap, no air output) so `start` resumes mid-stream
// cleanly without re-initialising it. Runtime-only (not persisted) — a reboot
// always comes up transmitting. Honored by Fox and Live Key (shared TX path);
// works over serial, BLE, and the instructor relay since it is a normal
// handle_setup_command entry. Does not affect Hunter (RX) or Instructor bursts.
static bool g_tx_halted = false;

static uint32_t pause_until = 0;
static uint32_t last_draw = 0;

// ---------------------------------------------------------------------------
// Fox keying timebase (TODO-fox-timing.md mitigation #1).
//
// In MODE_FOX (keyed) the player-advance, edge-detection and duration
// measurement run on platform::keyer_*'s fixed 2 ms cadence (esp_timer task on
// ESP32, a FreeRTOS task on nRF52), NOT from loop(). A slow LCD/audio/BLE frame
// can therefore no longer stretch a measured element/gap (the bug). The keyer
// timestamps each completed segment and pushes a record into a lock-free SPSC
// ring; loop_fox() drains it, builds proto::EdgeEvent (carrying dur_prev across
// pops) and does the blocking radio::send(). radio TX jitter no longer corrupts
// the decode because the duration is already captured.
//
// Concurrency: producer = keyer context, consumer = loop(). Ring head/tail are
// std::atomic for a lock-free SPSC handoff. The keyer OWNS `player` in this
// path (loop never calls player.update/start/down here); loop signals message
// start via g_keyer_start_req and reads completion via g_keyer_finished — no
// payload crosses, so a single atomic flag replaces the planned command mutex
// (message text comes from stable config::fox_message()).
struct EdgeRec {
    uint8_t  flags;     // EDGE_FLAG_DOWN (level ENTERED) | EDGE_FLAG_HEARTBEAT
    uint16_t dur_ms;    // duration of the segment that just ENDED (or elapsed, for HB)
};
static constexpr uint8_t KEYER_RING_SZ = 32;   // power-of-two not required; SPSC
static EdgeRec               g_keyer_ring[KEYER_RING_SZ];
static std::atomic<uint8_t>  g_keyer_head{0};   // consumer (loop) reads here
static std::atomic<uint8_t>  g_keyer_tail{0};   // producer (keyer) writes here
static std::atomic<uint32_t> g_keyer_drops{0};  // ring-full drops (should stay 0)
static std::atomic<bool>     g_keyer_finished{true};   // player.finished(), keyer-owned
static std::atomic<bool>     g_keyer_start_req{false};  // loop -> keyer: (re)start message
static std::atomic<uint32_t> g_keyer_ticks{0};   // tick counter (idle-during-pause proof)
static bool                  g_keyer_active   = false;  // loop-side: keyer timer running
static bool                  g_keyer_starting = false;  // start issued, awaiting keyer ack
static bool                  g_keyer_fallback = false;  // keyer_start() failed -> in-loop tx_edge

// Cycle the SX1262 TX power to the next level and persist it. This is the single
// button-driven power action shared by every mode that exposes it on the PRG
// button (Fox and Instructor) — keep it here so there's exactly one place that
// advances pwr_idx, applies the chip power, and saves it. The debug line prints
// the new index/value on every button-driven change so an unattended test can
// assert the path was reached (the button is physical and can't be scripted).
static void cycle_tx_power(void) {
    pwr_idx = (pwr_idx + 1) % N_PWR;
    radio::set_tx_power(PWR_LEVELS[pwr_idx].dbm);
    config::set_fox_pwr_idx((uint8_t)pwr_idx);
    last_draw = 0;   // force an immediate redraw of the new level
    Serial.printf("BTN pwr: idx=%d level=%s dbm=%d\n",
                  pwr_idx, PWR_LEVELS[pwr_idx].label, PWR_LEVELS[pwr_idx].dbm);
}

// Rolling decoded-text buffer for the hunter view.
static char text_buf[128];
static size_t text_len = 0;

// Raw dit/dah element stream for the hunter's learning-aid display mode, and the
// callsign learned from the last Ident packet ("----" until one is heard).
static char ditdah_buf[64];
static size_t ditdah_len = 0;
static int  rx_station_id = -1;      // station id from last packet, -1 = none
static bool g_showtext = false;       // false = dit/dah (default), true = decoded text

// Last text-frame the hunter received (field note §7). Surfaced in `show` so the
// text-mode render can be validated over either serial OR BLE (the per-event
// Serial trace is not mirrored to the BLE NUS). g_rx_text_elems = key-down
// elements the LOCAL player sounded for the most recent clue.
static char     g_rx_text[97]   = "";   // == TEXT_MSG_MAX + 1
static int      g_rx_text_seq   = -1;
static uint16_t g_rx_text_elems = 0;
static uint32_t g_rx_text_count = 0;    // frames accepted (post-dedup)

// Hunter sidetone volume, cycled by the USER/PRG button: MUTE -> LOW -> MED ->
// HIGH, persisted across power cycles. The three loudness steps map onto the
// config volume scale (GAIN_Q15/1024 units, the same the `vol` command uses); the
// muted state is config::muted(). On a PAM8403 board the analog amp has no usable
// level control, so the button only toggles MUTE on/off (see hunter_cycle_volume).
static const uint8_t HUNTER_VOL_UNITS[]  = {4, 11, 32};       // LOW, MED, HIGH
static const char*   HUNTER_VOL_LABELS[] = {"MUTE", "LOW", "MED", "HIGH"};
static const int     N_HUNTER_VOL = 4;

static void log_line(const char* s) { Serial.print(s); }

static void set_tone(bool on) {
    static bool cur = false;
    if (on != cur) { cur = on; if (on) sidetone_on(); else sidetone_off(); }
}

// Apply a mute state to the sidetone and persist it, so the Cardputer 'm' key
// and the BLE/serial `mute` command stay in lockstep and survive a power cycle.
static void apply_mute(bool m) {
    config::set_muted(m);
    sidetone_set_mute(m);
    last_draw = 0;   // refresh any on-screen state on the next draw
}

static bool handle_setup_command(const char* line, Print& out);  // fwd: BLE re-begin

// Bring the BLE-UART core up or down. Tearing it down (NimBLEDevice::deinit via
// ble_provision::stop) reclaims ~70 mA on the V4 — the single largest idle draw,
// the NimBLE host+controller on core 0. begin() re-advertises. Idempotent; a
// re-begin after a stop leaks the prior NimBLE C++ objects (deinit(false) does
// not walk them), so this is fine to toggle occasionally but not to thrash.
// Driven both by the `ble` console command and by the panel coupling in loop().
static void apply_ble(bool on) {
    if (on == g_ble_on) return;
    if (on) {
        char adv_name[24];
        snprintf(adv_name, sizeof(adv_name), "MorseStn-%u", config::station_id());
        ble_provision::begin(adv_name, handle_setup_command);
    } else {
        ble_provision::stop();
    }
    g_ble_on = on;
}

// Reconstruct the hunter volume cycle index (0=MUTE, 1..3=LOW/MED/HIGH) from the
// persisted state, so the USER button resumes from where it left off after a
// power cycle. Muted -> 0; otherwise pick the loudness step nearest the stored
// volume (the `vol` command can set arbitrary units, so this is a best fit).
static int hunter_vol_idx_from_config() {
    if (config::muted()) return 0;
    uint8_t v = config::volume();
    int best = 1, bestd = 256;
    for (int i = 0; i < 3; i++) {
        int d = (int)v - (int)HUNTER_VOL_UNITS[i];
        if (d < 0) d = -d;
        if (d < bestd) { bestd = d; best = i + 1; }
    }
    return best;
}

// USER/PRG button action in hunter mode: cycle the sidetone volume and persist
// it. MUTE -> LOW -> MED -> HIGH on a board with level control; MUTE on/off only
// on a PAM8403 (analog amp, no usable level). apply_mute() persists the mute and
// forces a redraw; the loudness steps persist via config::set_volume().
static void hunter_cycle_volume() {
    static int idx = -1;
    if (idx < 0) idx = hunter_vol_idx_from_config();   // seed from persisted state
#if defined(SIDETONE_PAM8403) || defined(SIDETONE_BUZZER)
    idx = idx ? 0 : 3;                                  // mute <-> on (level unused)
    apply_mute(idx == 0);
#else
    idx = (idx + 1) % N_HUNTER_VOL;
    apply_mute(idx == 0);
    if (idx != 0) {
        config::set_volume(HUNTER_VOL_UNITS[idx - 1]);
        sidetone_set_level(config::volume());
    }
#endif
    char msg[24];
    snprintf(msg, sizeof(msg), "  volume   = %s\n", HUNTER_VOL_LABELS[idx]);
    log_line(msg);
}

// Append n bytes to a rolling, NUL-terminated buffer. When it would overflow,
// drop the oldest half: keep the newest `cap/2` bytes, moving relative to len
// (not a fixed offset) and re-terminating so the NUL never lands mid-buffer —
// that off-by-one used to sever the string and freeze the hunter copy line once
// the buffer first filled. Shared by the decoded-text and dit/dah views.
static void rolling_append(char* buf, size_t& len, size_t cap,
                           const char* s, size_t n) {
    if (len + n + 1 >= cap) {
        const size_t half = cap / 2;
        memmove(buf, buf + len - half, half);
        len = half;
    }
    memcpy(buf + len, s, n);
    len += n;
    buf[len] = 0;
}

static void text_push(char c) {
    rolling_append(text_buf, text_len, sizeof(text_buf), &c, 1);
}

// Append a chunk to the rolling dit/dah stream (elements of one decoded char,
// or a space for a word gap), dropping the oldest half when full.
static void ditdah_push(const char* s) {
    rolling_append(ditdah_buf, ditdah_len, sizeof(ditdah_buf), s, strlen(s));
}

// ── Boot/crash reason ring buffer ───────────────────────────────────────────
// An intermittent native-USB panic loses its backtrace mid-reset, so we persist
// each boot's esp_reset_reason() to NVS and can dump the history later — the
// `bootlog` console/BLE command — without serial attached at crash time. Stored
// in the "boot" namespace as a fixed circular buffer: a `log` blob of BOOTLOG_N
// entries plus `lh`, the head (next slot to write). `cnt` stays the monotonic
// boot counter. (Uptime/run-length is deliberately omitted: a panic can't write
// it on the way down, so it would only ever cover clean reboots.)
// reset_reason_label() now lives behind platform:: (see platform.h) — each
// MCU family has its own small reset-cause enum (ESP32 esp_reset_reason_t vs
// nRF52 RESETREAS bits), so the label table moved into the per-platform .cpp
// (platform_esp32.cpp / platform_nrf52.cpp). Call platform::reset_reason_label.
static inline const char* reset_reason_label(int x) {
    return platform::reset_reason_label(x);
}

static const int BOOTLOG_N = 16;
struct BootLogEntry {
    uint32_t boot;      // boot counter when written (0 = empty/unused slot)
    uint8_t  reason;    // esp_reset_reason() at that boot
};

// Record this boot in the ring. Returns the new (monotonic) boot number and, via
// prev_reason, the previous boot's reason for the banner.
static uint32_t bootlog_record(int reason, int& prev_reason) {
    kv::Store bl; bl.begin("boot", false);
    uint32_t bc = bl.getUInt("cnt", 0) + 1;
    BootLogEntry log[BOOTLOG_N];
    if (bl.getBytes("log", log, sizeof(log)) != sizeof(log))
        memset(log, 0, sizeof(log));
    uint8_t head = bl.getUChar("lh", 0);
    if (head >= BOOTLOG_N) head = 0;

    // Previous boot = newest existing entry, i.e. the slot just before head.
    const BootLogEntry& newest = log[(head + BOOTLOG_N - 1) % BOOTLOG_N];
    prev_reason = newest.boot ? newest.reason : 0;

    log[head].boot   = bc;
    log[head].reason = (uint8_t)reason;
    head = (head + 1) % BOOTLOG_N;

    bl.putUInt("cnt", bc);
    bl.putBytes("log", log, sizeof(log));
    bl.putUChar("lh", head);
    bl.end();
    return bc;
}

// Dump the ring oldest→newest to `out`. The slot at head is the oldest entry.
static void bootlog_dump(Print& out) {
    kv::Store bl; bl.begin("boot", true);
    BootLogEntry log[BOOTLOG_N];
    size_t got  = bl.getBytes("log", log, sizeof(log));
    uint8_t head = bl.getUChar("lh", 0);
    bl.end();
    if (got != sizeof(log)) { out.println("# bootlog empty"); return; }
    if (head >= BOOTLOG_N) head = 0;
    out.println("# bootlog (oldest first):");
    for (int i = 0; i < BOOTLOG_N; i++) {
        const BootLogEntry& e = log[(head + i) % BOOTLOG_N];
        if (!e.boot) continue;           // skip unused slots
        out.printf("  #%u reason=%d(%s)\n", e.boot, e.reason,
                   reset_reason_label(e.reason));
    }
}

// Clear the history ring (leaves the monotonic boot counter `cnt` intact).
static void bootlog_clear() {
    kv::Store bl; bl.begin("boot", false);
    bl.remove("log");
    bl.remove("lh");
    bl.end();
}

// --- Reboot-intent flag (nRF52 reset-cause recovery) -------------------------
// The Adafruit nRF52 bootloader clears the hardware reset-cause register
// (RESETREAS) before our app runs, so platform::reset_reason() can't tell why we
// rebooted — every cause reads POWERON. (GPREGRET2 and .noinit RAM are likewise
// wiped on a watchdog reset; see the git history of this commit for the dead
// ends.) Instead we persist a tiny "why am I about to reset" flag in flash — the
// same kv store the bootlog uses, so it is already usable this early in setup() —
// and read it back next boot:
//   SOFT / OFF -> the reboot was intentional (restart() / system_off() stamped it)
//   RUNNING    -> we reset while running with no intent stamped => the watchdog
//                 fired (or a crash/brownout — all "unexpected")
//   NONE       -> flag never written => genuine first cold boot (keep POWERON)
// Every boot re-arms the flag to RUNNING. ESP32's reset_reason() is accurate, so
// the synthesis is nRF52-only and set_reboot_intent() is a no-op there (the enum
// is defined unconditionally so the shared call sites still compile).
enum { REBOOT_INTENT_NONE = 0, REBOOT_INTENT_RUNNING = 1,
       REBOOT_INTENT_SOFT = 2, REBOOT_INTENT_OFF = 3 };

#if defined(DEVICE_RAK4631) || defined(DEVICE_WIO_TRACKER_L1)
#define HAS_REBOOT_INTENT 1

static void set_reboot_intent(uint8_t v) {
    kv::Store s; s.begin("boot", false);
    if (s.getUChar("intent", REBOOT_INTENT_NONE) != v) s.putUChar("intent", v);
    s.end();
}
static uint8_t get_reboot_intent() {
    kv::Store s; s.begin("boot", true);
    uint8_t v = s.getUChar("intent", REBOOT_INTENT_NONE);
    s.end();
    return v;
}
#else
static inline void set_reboot_intent(uint8_t) {}   // ESP32: reset_reason() is accurate
#endif

// Remote-control (instructor) state. In MODE_INSTRUCTOR the `relay <id> <cmd>`
// console verb stages a command here; loop_instructor() bursts it over GFSK and
// listens for the target's ack. One command in flight at a time.
struct CtrlPending {
    bool     active    = false;   // a command is being delivered
    uint8_t  target_id = proto::BROADCAST_ID;
    uint8_t  seq       = 0;
    char     cmd[proto::CTRL_CMD_MAX + 1] = {0};
    uint32_t start_ms  = 0;       // when bursting began (for the timeout)
    uint32_t last_tx   = 0;       // last burst send (for the inter-burst gap)
    bool     delivered = false;   // a matching ack arrived (unicast)
    uint8_t  acks_from[8];        // station ids that acked (broadcast tally)
    uint8_t  n_acks    = 0;
};
static CtrlPending g_ctrl;
static uint8_t     g_ctrl_seq = 0;   // monotonic command id (wraps mod 256)
// Instructor silence-sync state (file scope so the top-of-loop service and the
// post-burst ack listener — both in/around loop_instructor — share one view of
// when the target fox was last heard transmitting).
static bool        g_fox_heard      = false;
static uint32_t    g_fox_last_heard = 0;
// Explicit-window state: set when the target fox's Listen beacon is decoded.
// While g_fox_listen_until is in the future the instructor bursts immediately,
// short-circuiting the CTRL_SILENCE_MS inference. Silence-sync still runs as the
// fallback for a lost beacon.
static bool        g_fox_listening    = false;
static uint32_t    g_fox_listen_until = 0;

// Sticky ACK display. An instructor relay ack arrives asynchronously and was
// drawn for under a second before the 1 Hz idle redraw wiped it back to "ready"
// — the operator could miss whether a command actually landed. Latch the last
// ack's two lines and a deadline; while g_ack_show_until is in the future the
// idle draw shows the ack instead of the "ready" prompt, so it stays legible.
static constexpr uint32_t ACK_STICKY_MS = 8000;
static char     g_ack_l1[24] = {0};
static char     g_ack_l2[24] = {0};
static uint32_t g_ack_show_until = 0;

// Instructor broadcast banner (docs/plan-instructor-broadcast.md). A short
// plaintext message the instructor pushes to EVERY station's screen, distinct
// from `relay` (which runs a console command silently). Fire-and-forget: no ack.
// Hunters are always in RX so a periodic burst reaches them at once; a Fox only
// listens in the tail of its inter-message pause, so the campaign runs for
// ALERT_CAMPAIGN_MS — long enough to outlast one fox message cycle (msg airtime +
// REPEAT_PAUSE) — and bursts immediately when it hears a fox Listen beacon (§6).
// This is what makes the §6 fox-halt (and its `alert clear` resume) reliably
// reach a Fox, not just the always-listening hunters.
static constexpr uint32_t BCAST_SHOW_MS     = 60000;  // how long a banner stays on the panel (1 min)
static constexpr uint32_t BCAST_INTERVAL    = CTRL_PROBE_INTERVAL;  // periodic burst gap
static constexpr uint32_t ALERT_CAMPAIGN_MS = 20000;  // campaign length (> one fox cycle)

// Shared banner state: set by the RX handler (broadcast_rx_try) or locally when
// the instructor stages its own send; consumed by each mode's draw via
// banner_active(). g_banner_until is a millis() deadline (0 = no banner).
static char     g_banner[proto::BCAST_TEXT_MAX + 1] = {0};
static uint32_t g_banner_until = 0;

// Sticky-alert latch (field note §6). When a node RECEIVES an alert carrying
// proto::BCAST_FLAG_STICKY, this latches: banner_active() then ignores the
// g_banner_until timeout and keeps the banner up forever, and a Fox/Live-Key
// node also halts its TX (g_tx_halted). Released only by `alert clear` (empty
// text), an explicit `start` (TX only — leaves the banner), or reset. RAM-only:
// a reboot always comes up un-alerted. The instructor never latches its OWN
// panel (it must stay usable to send the clear) — see set_banner() callers.
static bool g_alert_latched = false;

// Audible attention tone that rides along with a received/sent alert banner
// (docs/plan-alert-tone.md). A single steady tone at the board's sidetone
// frequency, sounded for ALERT_TONE_MS via sidetone_alert() — which overrides the
// master mute, so a muted node still beeps. g_alert_tone_until is a millis()
// deadline (0 = not sounding); alert_tone_tick() ends it without blocking.
static constexpr uint32_t ALERT_TONE_MS = 1500;      // attention tone length
static uint32_t g_alert_tone_until = 0;

// Pending broadcast campaign (the TX side), staged by the `bcast` verb and
// driven by loop_instructor(). Mirrors CtrlPending but fire-and-forget.
struct BcastPending {
    bool     active       = false;
    uint8_t  seq          = 0;
    uint8_t  flags        = 0;
    char     text[proto::BCAST_TEXT_MAX + 1] = {0};
    uint32_t last_tx      = 0;       // last repeat send (for the inter-repeat gap)
    uint32_t until        = 0;       // campaign deadline (millis); runs while now < until
};
static BcastPending g_bcast;

// fwd: the broadcast banner helpers are defined far below (near the mode loops)
// but referenced earlier — by the `bcast`/`show` console verbs and prg_tapped.
static bool banner_active(uint32_t now);
static void set_banner(const char* text, bool alert, bool sticky, uint32_t now);
static void start_alert_tone(uint32_t now);
static void alert_tone_tick(uint32_t now);

// A Print sink that captures the FIRST line of a command's reply into a small
// buffer, so a station can return a short confirmation in its ack packet. The
// console replies are like "  fox msg  = HELLO"; we keep the leading text up to
// the first newline, trimmed of leading spaces.
class CapturePrint : public Print {
  public:
    size_t write(uint8_t c) override {
        if (c == '\n') { done_ = true; return 1; }
        if (done_) return 1;                       // keep only the first line
        if (len_ == 0 && (c == ' ' || c == '\t')) return 1;  // trim leading ws
        if (len_ < sizeof(buf_) - 1) buf_[len_++] = (char)c;
        return 1;
    }
    const char* c_str() { buf_[len_] = '\0'; return buf_; }
  private:
    char   buf_[proto::ACK_STATUS_MAX + 1] = {0};
    size_t len_  = 0;
    bool   done_ = false;
};

static void hibernate();   // defined below; reached from the `hibernate` command

// Dispatch one provisioning command line, writing responses to `out`. Pure of
// any transport: `out` is a Print& so the serial REPL (Serial) and a future
// BLE-UART session can share this parser. Returns true when the session should
// end (the "done"/"exit" command). See docs/commands.md.
static bool handle_setup_command(const char* line, Print& out) {
    // Any console command — over serial, BLE, or an instructor relay — counts as
    // activity and wakes a blanked panel (display.h idle-blanking).
    display::activity();
    if (!strncmp(line, "call ", 5)) {
        config::set_callsign(line + 5);
        out.printf("  callsign = %s\n", config::callsign());
    } else if (!strncmp(line, "msg ", 4)) {
        config::set_fox_message(line + 4);
        out.printf("  fox msg  = %s\n", config::fox_message());
    } else if (!strncmp(line, "id ", 3)) {
        config::set_station_id((uint8_t)atoi(line + 3));
        out.printf("  id       = %u\n", config::station_id());
    } else if (!strncmp(line, "wpm ", 4)) {
        config::set_wpm((uint8_t)atoi(line + 4));
        // Live fox: re-arm the player timing; takes effect at the next message
        // loop (the current message finishes at its existing speed).
        if (g_live_apply && mode == MODE_FOX)
            player.begin(config::wpm(), config::char_wpm());
        out.printf("  wpm      = %u (farns %u)\n",
                   config::wpm(), config::char_wpm());
    } else if (!strncmp(line, "farns ", 6)) {
        config::set_char_wpm((uint8_t)atoi(line + 6));
        if (g_live_apply && mode == MODE_FOX)
            player.begin(config::wpm(), config::char_wpm());
        out.printf("  farns    = %u (overall %u)\n",
                   config::char_wpm(), config::wpm());
    } else if (!strncmp(line, "pwr ", 4)) {
        int p = atoi(line + 4);
        if (p < 0 || p >= N_PWR) {
            out.println("  ? pwr 0=LO 1=MED 2=HI 3=MAX");
        } else {
            pwr_idx = p;
            config::set_fox_pwr_idx((uint8_t)pwr_idx);
            // Live fox: retune the SX1262 now and force a redraw of the level.
            if (g_live_apply && mode == MODE_FOX) {
                radio::set_tx_power(PWR_LEVELS[pwr_idx].dbm);
                last_draw = 0;
            }
            out.printf("  tx pwr   = %d (%s, %d dBm)\n", pwr_idx,
                       PWR_LEVELS[pwr_idx].label, PWR_LEVELS[pwr_idx].dbm);
        }
    } else if (!strncmp(line, "ipwr ", 5)) {
        // Instructor TX power override. Boots at MAX; use this to drop for
        // bench work. Not persisted — next boot resets to MAX.
        if (mode != MODE_INSTRUCTOR) {
            out.println("  ? ipwr is only valid in Instructor mode");
        } else {
            int p = atoi(line + 5);
            if (p < 0 || p >= N_PWR) {
                out.println("  ? ipwr 0=LO 1=MED 2=HI 3=MAX");
            } else {
                pwr_idx = p;
                radio::set_tx_power(PWR_LEVELS[pwr_idx].dbm);
                last_draw = 0;
                out.printf("  ipwr     = %d (%s, %d dBm)\n", pwr_idx,
                           PWR_LEVELS[pwr_idx].label, PWR_LEVELS[pwr_idx].dbm);
            }
        }
    } else if (!strcmp(line, "pa") || !strncmp(line, "pa ", 3)) {
        // Runtime override of the V4 FEM power amplifier (CPS). The PA is engaged
        // automatically in the transmit run modes (Fox / Live Key) at boot; this
        // command lets a bench force it off to A/B +22 dBm (bypass) vs ~+28 dBm
        // (PA), or back on, without a reflash. NOT persisted — the run-mode default
        // re-applies on every boot. No-op on non-FEM boards and on the V4.3
        // (KCT8103L, no CPS bypass pin). Mind FCC §15.249 when engaged.
        const char* arg = line + 2;
        while (*arg == ' ') arg++;
        if (!radio::has_fem()) {
            out.println("  pa: no FEM on this board (n/a)");
        } else {
            if (!*arg || !strcmp(arg, "show")) {
                // report only
            } else if (!strcmp(arg, "on") || !strcmp(arg, "1")) {
                g_pa_on = true;  radio::set_pa(true);
            } else if (!strcmp(arg, "off") || !strcmp(arg, "0")) {
                g_pa_on = false; radio::set_pa(false);
            } else {
                out.println("  ? pa <on|off>");
                return false;
            }
            out.printf("  pa       = %s (FEM CPS; +~6 dB on V4.2; auto-on in Fox/Livekey)\n",
                       g_pa_on ? "on" : "off");
        }
    } else if (!strcmp(line, "ble") || !strncmp(line, "ble ", 4)) {
        // Runtime BLE-UART power policy: on | off | auto (default). AUTO ties the
        // NimBLE core to the panel (see the loop() coupling); ON/OFF pin it.
        // `ble on` is the override for reaching an idle node whose panel has
        // blanked (e.g. an instructor on its "ready" screen). NOT persisted —
        // boot starts AUTO with BLE up. Re-begin after a stop leaks the prior
        // NimBLE C++ objects (deinit(false) doesn't walk them — see
        // ble_provision::stop), so this is a bench affordance, not loop-scriptable.
        const char* arg = line + 3;
        while (*arg == ' ') arg++;
        if (!*arg || !strcmp(arg, "show")) {
            // report only
        } else if (!strcmp(arg, "on") || !strcmp(arg, "1")) {
            g_ble_mode = BLE_ON;  apply_ble(true);
        } else if (!strcmp(arg, "off") || !strcmp(arg, "0")) {
            g_ble_mode = BLE_OFF; apply_ble(false);
        } else if (!strcmp(arg, "auto")) {
            g_ble_mode = BLE_AUTO; apply_ble(!display::blanked());  // sync to panel now
        } else {
            out.println("  ? ble <on|off|auto>");
            return false;
        }
        out.printf("  ble      = %s [%s]\n", g_ble_on ? "on (advertising)" : "off",
                   g_ble_mode == BLE_ON ? "on" : g_ble_mode == BLE_OFF ? "off" : "auto");
    } else if (!strcmp(line, "lna") || !strncmp(line, "lna ", 4)) {
        // Runtime RX LNA select for the V4.3 FEM (KCT8103L). `lna on` keeps the
        // FEM enabled in receive so its LNA is in the path (boot default); `lna
        // off` holds CSD LOW in RX, bypassing the FEM (antenna straight to the
        // SX1262) — for A/B-ing front-end gain or chasing LNA overload near a
        // strong fox. RX only; TX always re-engages the FEM PA. NOT persisted.
        const char* arg = line + 3;
        while (*arg == ' ') arg++;
        if (!radio::has_fem()) {
            out.println("  lna: no FEM on this board (n/a)");
        } else {
            if (!*arg || !strcmp(arg, "show")) {
                // report only
            } else if (!strcmp(arg, "on") || !strcmp(arg, "1")) {
                config::set_lna(true);  radio::set_lna(true);
            } else if (!strcmp(arg, "off") || !strcmp(arg, "0")) {
                config::set_lna(false); radio::set_lna(false);
            } else {
                out.println("  ? lna <on|off>");
                return false;
            }
            out.printf("  lna      = %s (FEM CTX; RX path; V4.3 KCT8103L; persisted)\n",
                       radio::lna_on() ? "on" : "off");
        }
    } else if (!strcmp(line, "rxbw") || !strncmp(line, "rxbw ", 5)) {
        // FSK receive-bandwidth filter (SX1262 DSB). `rxbw <khz>` sets it live and
        // persists; `rxbw` / `rxbw show` just reports. RadioLib snaps the request
        // to the nearest supported step (4.8..467 kHz) and `show`/this line report
        // the value actually programmed. The default is 23.4 kHz: the CW/SDR drift
        // test put every unit <1 ppm (~<1 kHz) apart, so against the ~15 kHz Carson
        // signal width 78.2 kHz was needlessly wide and was narrowed for ~3 dB/octave
        // more sensitivity. Narrow past the signal width and you start clipping
        // edges — A/B with the round-robin.
        const char* arg = line + 4;
        while (*arg == ' ') arg++;
        if (*arg && strcmp(arg, "show")) {
            float khz = atof(arg);
            if (khz < 4.0f || khz > 467.0f) {
                out.println("  ? rxbw <4.8..467 kHz>");
                return false;
            }
            if (!radio::set_rx_bw(khz)) {       // snaps + reprograms the modem
                out.println("  rxbw: set failed (wrong modem?)");
                return false;
            }
            config::set_rx_bw_khz(radio::rx_bw_khz());   // persist the snapped value
        }
        out.printf("  rxbw     = %.1f kHz (FSK RX filter; persisted)\n",
                   radio::rx_bw_khz());
    } else if (!strcmp(line, "txcw") || !strncmp(line, "txcw ", 5)) {
        // Continuous-wave test carrier for SDR frequency-drift measurement. Emits
        // an UNMODULATED carrier at radio::frequency_mhz() so an SDR (e.g. an
        // rtl_tcp dongle + scripts/sdr_drift.py) reads this transmitter's true
        // centre frequency as a single spectral peak; compare the peak across
        // stations to map per-device TCXO drift. `txcw [secs]` (default 10, cap
        // 120) transmits then auto-stops; `txcw off` (or `txcw 0`) stops now. The
        // carrier uses the current `pwr` level and `pa` state (set those first).
        // Blocks for the duration — the radio can't receive while keying CW — and
        // a keypress on the serial console aborts early. Works over `relay <id>
        // txcw ...` to a distant station. Mind FCC §15.249: a pure carrier is a
        // continuous emission, so keep bursts short and the power low.
        const char* arg = line + 4;
        while (*arg == ' ') arg++;
        if (!strcmp(arg, "off") || !strcmp(arg, "0")) {
            radio::tx_cw(false);
            out.println("  txcw: off");
        } else {
            int secs = *arg ? atoi(arg) : 10;
            if (secs < 1)   secs = 1;
            if (secs > 120) secs = 120;
            radio::set_tx_power(PWR_LEVELS[pwr_idx].dbm);
            out.printf("  txcw: %.4f MHz @ %d dBm (%s, pa %s) for %ds — keypress aborts\n",
                       radio::frequency_mhz(), PWR_LEVELS[pwr_idx].dbm,
                       PWR_LEVELS[pwr_idx].label, g_pa_on ? "on" : "off", secs);
            if (!radio::tx_cw(true)) {
                out.println("  txcw: start failed");
            } else {
                uint32_t end = millis() + (uint32_t)secs * 1000;
                while ((int32_t)(end - millis()) > 0) {
                    if (Serial.available()) {       // abort on any serial keypress
                        while (Serial.available()) Serial.read();
                        break;
                    }
                    delay(100);
                }
                radio::tx_cw(false);
                out.println("  txcw: done");
            }
        }
    } else if (!strcmp(line, "model")) {
        // Board model (read-only). The Heltec V4 sub-revision is auto-detected
        // from the FEM strap at boot (see radio::fem_name / fem_power_on); every
        // other board has a single fixed model. Printed with the hardware chip id
        // so the model is always tied to a verifiable unit. There is no manual
        // override — detection can't be wrong relative to the silicon present.
        out.printf("  model    = %s  chip=%s  soc=%s\n",
                   board_model_str(), platform::chip_id_str(), platform::soc_str());
    } else if (!strcmp(line, "reboot") || !strcmp(line, "restart")) {
        // Remote reboot: the boot menu auto-selects the stored boot_mode after
        // its idle timeout, so this applies a "mode <n>" change with no physical
        // interaction. Give the BLE notify a moment to flush before resetting.
        out.println("# rebooting");
        set_reboot_intent(REBOOT_INTENT_SOFT);   // so next boot logs "SOFT", not a false watchdog
        delay(150);
        platform::restart();
    } else if (!strncmp(line, "mode ", 5)) {
        int m = atoi(line + 5);
        if (m < 0 || m > 3) {       // 0=Hunter 1=Fox 2=Livekey 3=Instructor (not Hibernate)
            out.println("  ? mode 0=Hunter 1=Fox 2=Livekey 3=Instructor");
        } else {
            config::set_boot_mode((uint8_t)m);
            static const char* names[] = {"Hunter", "Fox", "Livekey", "Instructor"};
            out.printf("  boot mode = %d (%s)\n", m, names[m]);
        }
    } else if (!strncmp(line, "relay ", 6) || !strncmp(line, "fox ", 4)) {
        // Instructor relay (docs: instructor station): packetize a console command
        // and burst it over GFSK to a distant station, identical to what BLE/serial
        // would run locally. `relay <id> <cmd...>` (alias `fox <id> <cmd>`): id is
        // the target station_id, or 255 to broadcast to every station. The actual
        // bursting + ack collection happens in loop_instructor(); here we just stage
        // it. Only meaningful in MODE_INSTRUCTOR.
        const char* arg = line + (line[0] == 'f' ? 4 : 6);
        while (*arg == ' ') arg++;
        if (mode != MODE_INSTRUCTOR) {
            out.println("  relay: only in Instructor mode");
        } else if (!*arg || !(*arg >= '0' && *arg <= '9')) {
            out.println("  ? relay <id|255> <command...>");
        } else {
            int tid = atoi(arg);
            while (*arg && *arg != ' ') arg++;   // skip the id
            while (*arg == ' ') arg++;           // to the command line
            if (tid < 0 || tid > 255 || !*arg) {
                out.println("  ? relay <id|255> <command...>");
            } else if (strlen(arg) > proto::CTRL_CMD_MAX) {
                out.printf("  relay: command too long (max %u)\n",
                           (unsigned)proto::CTRL_CMD_MAX);
            } else {
                g_ctrl.active    = true;
                g_ctrl.target_id = (uint8_t)tid;
                g_ctrl.seq       = g_ctrl_seq++;
                config::set_ctrl_seq(g_ctrl_seq);  // persist so a reboot doesn't
                                                   // restart at 0 (dup collision)
                strncpy(g_ctrl.cmd, arg, proto::CTRL_CMD_MAX);
                g_ctrl.cmd[proto::CTRL_CMD_MAX] = '\0';
                g_ctrl.start_ms  = millis();
                g_ctrl.last_tx   = 0;            // send the first burst immediately
                g_ctrl.delivered = false;
                g_ctrl.n_acks    = 0;
                out.printf("  relaying to id %d (seq %u): %s\n",
                           tid, g_ctrl.seq, g_ctrl.cmd);
            }
        }
    } else if (!strcmp(line, "alert") || !strncmp(line, "alert ", 6)) {
        // Instructor alert (docs/plan-alert-tone.md): push a short plaintext
        // message to EVERY station's screen AND sound an attention tone there —
        // distinct from `relay`, which runs a console command silently. Every
        // alert beeps (overriding a receiver's mute); `alert clear` dismisses any
        // showing banner everywhere (and is silent). Fire-and-forget: no ack; the
        // ALERT_CAMPAIGN_MS listen-synced burst from loop_instructor() reaches both
        // hunters and the Fox. The actual bursting happens there; here we just stage
        // it. Instructor-mode only.
        const char* arg = line + 5;
        while (*arg == ' ') arg++;
        bool clear = !strcmp(arg, "clear");
        if (clear) arg = "";              // empty text → receivers dismiss
        if (mode != MODE_INSTRUCTOR) {
            out.println("  alert: only in Instructor mode");
        } else if (!*arg && !clear) {
            out.println("  ? alert <text...> | alert clear");
        } else if (strlen(arg) > proto::BCAST_TEXT_MAX) {
            out.printf("  alert: text too long (max %u)\n",
                       (unsigned)proto::BCAST_TEXT_MAX);
        } else {
            g_bcast.active       = true;
            g_bcast.seq          = g_ctrl_seq++;
            config::set_ctrl_seq(g_ctrl_seq);   // shared monotonic id, persisted
            // Every alert beeps; a real alert (not `clear`) also latches on the
            // RECEIVING fleet via BCAST_FLAG_STICKY (§6). `alert clear` carries
            // empty text → receivers hit the release branch in set_banner().
            g_bcast.flags        = proto::BCAST_FLAG_ALERT;
            if (!clear) g_bcast.flags |= proto::BCAST_FLAG_STICKY;
            strncpy(g_bcast.text, arg, proto::BCAST_TEXT_MAX);
            g_bcast.text[proto::BCAST_TEXT_MAX] = '\0';
            g_bcast.last_tx      = 0;            // send the first repeat immediately
            g_bcast.until        = millis() + ALERT_CAMPAIGN_MS;
            g_fox_listening      = false;        // fresh campaign: re-sync to a beacon
            // The instructor sees its own banner (it does not RX its own send) and
            // hears the same tone; `alert clear` (empty text) stays silent. It does
            // NOT latch its own panel (sticky=false) — the instructor must stay
            // usable to send the clear (§6). Only the receiving fleet latches.
            set_banner(arg, true, /*sticky=*/false, millis());
            if (clear)
                out.printf("  alert clear (seq %u)\n", g_bcast.seq);
            else {
                start_alert_tone(millis());
                out.printf("  alerting (seq %u): %s\n", g_bcast.seq, g_bcast.text);
            }
        }
    } else if (!strcmp(line, "mute") || !strncmp(line, "mute ", 5)) {
        // Bare "mute" toggles; "mute on|off" (or 1|0) sets explicitly. Persisted
        // and applied live so a node near people can be silenced over the air.
        const char* arg = line + 4;
        while (*arg == ' ') arg++;
        bool m;
        if (!strcmp(arg, "on") || !strcmp(arg, "1"))       m = true;
        else if (!strcmp(arg, "off") || !strcmp(arg, "0")) m = false;
        else                                               m = !config::muted();
        apply_mute(m);
        out.printf("  mute     = %s\n", m ? "on" : "off");
    } else if (!strcmp(line, "vol") || !strncmp(line, "vol ", 4)) {
        // Sidetone volume in GAIN_Q15/1024 units (1..32). Bare "vol" reports the
        // current level; "vol <n>" sets it. Persisted and applied live.
        const char* arg = line + 3;
        while (*arg == ' ') arg++;
        if (*arg) {
            config::set_volume((uint8_t)atoi(arg));
            sidetone_set_level(config::volume());
        }
        out.printf("  vol      = %u (gain %u)\n",
                   config::volume(), (unsigned)config::volume() * 1024);
    } else if (!strcmp(line, "showtext") || !strncmp(line, "showtext ", 9)) {
        // Hunter copy-view toggle: decoded text vs dit/dah elements. Runtime-only
        // (not persisted) — it's a display preference, not station config. Default
        // off (show dit/dah). Bare "showtext" toggles; "showtext on" shows text.
        const char* arg = line + 8;
        while (*arg == ' ') arg++;
        if      (!strcmp(arg, "on") || !strcmp(arg, "1"))  g_showtext = true;
        else if (!strcmp(arg, "off") || !strcmp(arg, "0")) g_showtext = false;
        else if (*arg)                { out.println("  ? showtext [on|off]"); }
        else                          g_showtext = !g_showtext;   // toggle
        last_draw = 0;   // refresh the hunter copy line on the next draw
        out.printf("  showtext = %s\n", g_showtext ? "on" : "off");
    } else if (!strcmp(line, "msgmode") || !strcmp(line, "msgmode show") ||
               !strncmp(line, "msgmode ", 8)) {
        // Fox canned-message delivery (field note §7, docs/plan-text-message-mode.md):
        // "keyed" keys the clue as edge Morse over the air; "text" (default) sends
        // it as one MAGIC_TEXT frame + retransmit burst and the hunter renders Morse
        // locally — robust at the edge of range where a single lost edge becomes a
        // '?'. NVS-persisted and restored into g_msgmode at boot. Canned loop_fox
        // only; MODE_LIVEKEY always keys edges.
        const char* arg = line + 7;
        while (*arg == ' ') arg++;
        if (!*arg || !strcmp(arg, "show")) {
            // report only
        } else if (!strcmp(arg, "keyed")) {
            g_msgmode = MSGMODE_KEYED;
            config::set_msgmode(g_msgmode);
        } else if (!strcmp(arg, "text")) {
            g_msgmode = MSGMODE_TEXT;
            config::set_msgmode(g_msgmode);
        } else {
            out.println("  ? msgmode <keyed|text>");
            return false;
        }
        out.printf("  msgmode  = %s\n", g_msgmode == MSGMODE_TEXT ? "text" : "keyed");
    } else if (!strcmp(line, "debug") || !strcmp(line, "debug show") ||
               !strncmp(line, "debug ", 6)) {
        // Unattended-test dump (E4): "debug on|off" toggles a per-event Serial
        // trace (RX/TX/EL/CH lines, see docs/edge-events.md); bare "debug" or
        // "debug show" reports the current state. Reply goes to `out` (the
        // command channel); the dump itself always goes to Serial — see g_debug.
        const char* arg = line + 5;
        while (*arg == ' ') arg++;
        if (!*arg || !strcmp(arg, "show")) {
            // report only
        } else if (!strcmp(arg, "on") || !strcmp(arg, "1")) {
            g_debug = true;
        } else if (!strcmp(arg, "off") || !strcmp(arg, "0")) {
            g_debug = false;
        } else {
            out.println("  ? debug <on|off>");
            return false;
        }
        out.printf("  debug    = %s\n", g_debug ? "on" : "off");
    } else if (!strcmp(line, "stop")) {
        // Halt ALL Fox/Live-Key transmission until `start` (see g_tx_halted).
        // Normal handle_setup_command entry, so it works over serial, BLE, and
        // the instructor relay path identically. Idempotent.
        g_tx_halted = true;
        Serial.println("# TX HALT (stop)");
        out.println("  tx       = halted");
    } else if (!strcmp(line, "start")) {
        g_tx_halted = false;
        Serial.println("# TX RESUME (start)");
        out.println("  tx       = running");
    } else if (!strcmp(line, "show")) {
        static const char* mnames[] = {"Hunter", "Fox", "Livekey", "Instructor",
                                       "Hibernate"};
        uint8_t bm = config::boot_mode();
        const char* mn = (bm < 5) ? mnames[bm] : "?";
        out.printf("  id=%u call=%s wpm=%u farns=%u vol=%u mute=%s mode=%s "
                   "msgmode=%s tx=%s msg=\"%s\"\n",
                   config::station_id(), config::callsign(),
                   config::wpm(), config::char_wpm(), config::volume(),
                   config::muted() ? "on" : "off", mn,
                   g_msgmode == MSGMODE_TEXT ? "text" : "keyed",
                   g_tx_halted ? "halted" : "running",
                   config::fox_message());
        out.printf("  rxtext   = \"%s\" seq=%d elems=%u rx=%lu\n",
                   g_rx_text, g_rx_text_seq, g_rx_text_elems,
                   (unsigned long)g_rx_text_count);
        out.printf("  model    = %s  chip=%s  soc=%s\n",
                   board_model_str(), platform::chip_id_str(), platform::soc_str());
        out.printf("  build    = %s\n", GIT_REV);
        out.printf("  cpu      = %lu MHz  cores=%d\n",
                   (unsigned long)platform::cpu_freq_mhz(), platform::cpu_cores());
        out.printf("  screen   = %s (idle-blank 60s)\n",
                   display::blanked() ? "BLANKED" : "on");
        out.printf("  debug    = %s\n", g_debug ? "on" : "off");
        out.printf("  ble      = %s [%s]\n", g_ble_on ? "on (advertising)" : "off",
                   g_ble_mode == BLE_ON ? "on" : g_ble_mode == BLE_OFF ? "off" : "auto");
        out.printf("  rxbw     = %.1f kHz\n", radio::rx_bw_khz());
        // Fox keyer (TODO-fox-timing.md #1): drops MUST stay 0 (ring not drained
        // fast enough otherwise); ticks proves the keyer is idle during the pause
        // (flat across REPEAT_PAUSE, advancing only while keying).
        out.printf("  keyer    = %s  drops=%lu  ticks=%lu\n",
                   g_keyer_fallback ? "fallback(in-loop)"
                                    : (g_keyer_active ? "active" : "idle"),
                   (unsigned long)g_keyer_drops.load(std::memory_order_relaxed),
                   (unsigned long)g_keyer_ticks.load(std::memory_order_relaxed));
        out.printf("  banner   = %s%s\n",
                   banner_active(millis()) ? g_banner : "(none)",
                   g_alert_latched ? "  [ALERTED/LATCHED]" : "");
        if (radio::has_fem()) {
            out.printf("  fem      = %s  pa %s  lna %s\n", radio::fem_name(),
                       g_pa_on ? "on" : "off", radio::lna_on() ? "on" : "off");
        }
    } else if (!strcmp(line, "batt")) {
        // Raw battery readout — disambiguates a 0% meter (no cell / flat vs a
        // scaling problem). millivolts() is the unsmoothed terminal voltage.
        int mv  = battery::millivolts();
        int pct = battery::percent();
        out.printf("  batt %d mV  %d%%  charging=%s\n",
                   mv, pct, battery::charging() ? "yes" : "no");
    } else if (!strcmp(line, "screen") || !strncmp(line, "screen ", 7)) {
        // Idle display-blanking (battery saver). Bare "screen" / "screen show"
        // reports state; "screen on" forces the panel awake and resets the idle
        // timer; "screen off" blanks it now. The panel also blanks on its own
        // after 60 s with no activity and wakes on any button/serial/BLE command
        // (and inbound keying on a hunter). The reported idle_ms / blanked let an
        // unattended test confirm blank+wake over the console (no human at the glass).
        const char* arg = line + 6;
        while (*arg == ' ') arg++;
        if (!*arg || !strcmp(arg, "show")) {
            // report only
        } else if (!strcmp(arg, "on") || !strcmp(arg, "1")) {
            display::activity();
        } else if (!strcmp(arg, "off") || !strcmp(arg, "0")) {
            display::blank_now();
        } else {
            out.println("  ? screen <on|off>");
            return false;
        }
        out.printf("  screen   = %s  idle %lu ms / 60000 ms\n",
                   display::blanked() ? "BLANKED" : "on",
                   (unsigned long)display::idle_ms());
    } else if (!strcmp(line, "hibernate")) {
        // Remote power-down: puts the SX1262 to sleep then enters MCU deep sleep
        // (the same path as the boot-menu Hibernate item, which otherwise needs
        // the physical button). Only a hardware RST — or, on the dev bench, a
        // serial reconnect that pulses the auto-reset line — brings the node back.
        // The radio-sleep result is logged for verification (see hibernate()).
        out.println("# hibernating: SX1262 sleep + MCU deep sleep — RST/reconnect to wake");
        delay(150);             // let the reply flush over BLE/serial first
        hibernate();            // does not return
    } else if (!strcmp(line, "power")) {
        // Power/clock state (battery saver verification). Reports the running CPU
        // clock and the physical core count so an unattended test can confirm the
        // 80 MHz low-power clock took effect. On the ESP32-S3 both cores are in
        // use (core 1 runs the Arduino app loop, core 0 hosts the always-on BLE
        // stack) — there is no idle/unused core to shut down; on the nRF52840 the
        // single Cortex-M4 runs both the app and the SoftDevice BLE stack.
        out.printf("  cpu      = %lu MHz  cores=%d\n",
                   (unsigned long)platform::cpu_freq_mhz(), platform::cpu_cores());
        out.printf("  screen   = %s  idle %lu ms / 60000 ms\n",
                   display::blanked() ? "BLANKED" : "on",
                   (unsigned long)display::idle_ms());
#ifdef ESP_PLATFORM
    } else if (!strcmp(line, "ipc")) {
        // DIAG (ipc1 stack-canary boot-loop, field 2026-06-21): report the
        // per-core IDF IPC task stack headroom. esp_ipc_call dispatches cross-core
        // work (e.g. interrupt allocation) onto these 1 KB tasks; the 80 MHz V4
        // boot-loop is one of them overflowing. uxTaskGetStackHighWaterMark is the
        // *minimum free bytes ever seen* on that stack — a small number = near
        // overflow. Run this on a stable build to quantify the margin.
        for (const char* nm : {"ipc0", "ipc1"}) {
            TaskHandle_t h = xTaskGetHandle(nm);
            if (h) out.printf("  %s free=%u bytes (min ever)\n",
                              nm, (unsigned)uxTaskGetStackHighWaterMark(h));
            else   out.printf("  %s not found\n", nm);
        }
#endif
    } else if (!strcmp(line, "bootlog")) {
        // Dump the boot/crash reason ring — diagnoses crashes after the fact when
        // no serial was attached at reset time (e.g. the native-USB panic loop).
        bootlog_dump(out);
    } else if (!strcmp(line, "bootlog clear")) {
        bootlog_clear();
        out.println("# bootlog cleared");
    } else if (!strcmp(line, "btn")) {
        // Test hook: inject a PRG-button press. In Fox mode this cycles TX power
        // (the only button action Fox has). In all other modes it just wakes the
        // screen, same as the physical button.
        if (mode == MODE_FOX) {
            cycle_tx_power();
            out.printf("  btn: pwr idx=%d (%s, %d dBm)\n",
                       pwr_idx, PWR_LEVELS[pwr_idx].label, PWR_LEVELS[pwr_idx].dbm);
        } else {
            display::activity();
            out.println("  btn: screen wake");
        }
    } else if (!strcmp(line, "stall") || !strncmp(line, "stall ", 6)) {
        // Resiliency test: deliberately wedge loop() by spinning WITHOUT feeding
        // the watchdog, to prove the hardware watchdog actually reboots a hung
        // node in the field. Normally the WDT (platform::watchdog_begin, 8 s)
        // fires first and we never return; next boot's bootlog shows the watchdog
        // cause (TASK_WDT on ESP32, WATCHDOG on nRF52). The optional arg caps the
        // spin (default 30 s) as a backstop in case the watchdog is disabled.
        // Unlike `panic`, this is always compiled — it's the intended way to
        // verify field resiliency, and a bounded spin is harmless on its own.
        const char* arg = line + 5;
        while (*arg == ' ') arg++;
        int secs = *arg ? atoi(arg) : 30;
        if (secs <= 0) secs = 30;
        out.printf("# stalling %ds with no watchdog feed — expect a reboot\n", secs);
        delay(150);   // let the BLE notify / serial line flush before we wedge
        uint32_t end = millis() + (uint32_t)secs * 1000;
        while ((int32_t)(millis() - end) < 0) { /* spin — never feed the WDT */ }
        out.println("# stall ended — watchdog did NOT fire (is it armed?)");
#ifdef DEBUG_PANIC_CMD
    } else if (!strcmp(line, "panic")) {
        // Debug-only: deliberately crash to exercise the panic/backtrace capture
        // path (UART0 GPIO43 + the boot/crash-reason ring). A volatile null
        // store forces a "Guru Meditation Error: StoreProhibited" with a real
        // backtrace; next boot's bootlog records reason=4(PANIC). Opt in with
        // -DDEBUG_PANIC_CMD in platformio.ini — never compiled into a release.
        out.println("# forcing panic (StoreProhibited) ...");
        delay(150);
        volatile int *p = (volatile int *)0;
        *p = 0xDEAD;
#endif
    } else if (!strcmp(line, "done") || !strcmp(line, "exit")) {
        out.println("# setup done");
        return true;
    } else {
        out.println("  ? call <SIGN> | msg <text> | id <n> | wpm <n> | "
                    "farns <n> | pwr <0..3> | ipwr <0..3> | pa <on|off> | lna <on|off> | "
                    "rxbw <khz> | mode <0..2> | "
                    "vol <1..32> | mute [on|off] | showtext [on|off] | "
                    "msgmode <keyed|text> | "
                    "relay <id|255> <cmd> | alert <text> | "
                    "model | debug [on|off] | show | screen [on|off] | power | "
                    "bootlog [clear] | btn | stall [secs] | reboot | hibernate | done");
    }
    return false;
}

// Runtime serial console: the non-blocking sibling of ble_provision::process().
// The boot REPL (run_setup_console) blocks the task, so it only works before a
// mode starts; this drains whatever complete command lines have arrived on
// Serial and feeds them to the same handle_setup_command() the BLE-UART path
// uses. Result: a USB terminal can drive a running station (show / mute / mode
// / pwr / wpm ...) for development, with close parity to the over-the-air BLE
// console. Kept transport-neutral like BLE — no prompt or banner; type a
// command + Enter and read the echoed result. The handler's done/exit return is
// ignored: there is no session to end at runtime.
static void serial_console_process() {
    static char   buf[160];          // matches serial_read_line / BLE RX_LINE_MAX
    static size_t len = 0;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (len == 0) continue;              // skip blank lines / CRLF pairs
            buf[len] = 0;
            handle_setup_command(buf, Serial);
            len = 0;
        } else if (len + 1 < sizeof(buf)) {
            buf[len++] = c;
        }
        // else: line too long — drop the byte until a terminator arrives.
    }
}

// Print the persisted config at boot (diagnostics only). There is no blocking
// "send 's' for setup" REPL: only the Cardputer has a keyboard, and over serial
// it raced the persisted-mode boot (a probe like "show" tripped the 's' window
// and trapped the node in the prompt). Provisioning is done live instead via
// serial_console_process() and BLE (ble_provision) — both share
// handle_setup_command() and need no reset.
static void print_boot_config() {
    Serial.printf("\n# config: id=%u call=%s wpm=%u farns=%u msg=\"%s\"\n",
                  config::station_id(), config::callsign(), config::wpm(),
                  config::char_wpm(), config::fox_message());
}

// Blocking boot menu; returns the chosen mode.
static Mode run_menu() {
    static const char* items[] = {"Hunter (RX)", "Fox (TX loop)", "Live key (TX)",
                                   "Instructor (RC)", "Hibernate"};
    const int n = 5;
    // Start highlighted on the last-used mode (persisted in NVS), so a power
    // cycle defaults back to it. Hibernate is never stored, so this is 0..2.
    int sel = config::boot_mode();
    if (sel < 0 || sel >= n) sel = 0;

    pinMode(PIN_MODE_BTN, INPUT_PULLUP);
    display::menu(items, n, sel);

    uint32_t t_start = millis();
    bool pressed = false;
    uint32_t press_t = 0;

    while (true) {
        uint32_t now = millis();
        bool down = (digitalRead(PIN_MODE_BTN) == LOW);

        if (down && !pressed) { pressed = true; press_t = now; }
        else if (!down && pressed) {
            pressed = false;
            uint32_t dur = now - press_t;
            if (dur >= 600) return (Mode)sel;          // long press selects
            sel = (sel + 1) % n;                        // short press cycles
            display::menu(items, n, sel);
            t_start = now;                              // reset idle timeout
        }
        if (now - t_start > 5000) return (Mode)sel;     // idle auto-select
        delay(10);
    }
}

// Power-off stand-in: there is no hardware switch, so we power down the
// peripherals and enter deep sleep with no wake source. The board draws ~uA
// until the user presses RST, which restarts the sketch back into this menu.
static void hibernate() {
    display::status("Hibernating", "Press RST to wake", nullptr);
    delay(1500);
    sidetone_off();
    // Put the SX1262 to sleep before we power the MCU down: the hibernate path
    // never reaches radio::init() in the normal run flow, so the radio would
    // otherwise sit in its post-reset standby (~1.5 mA) for the whole hibernate.
    // init() brings it up just far enough to issue SetSleep over SPI; we log the
    // result so the power-down is verifiable from the serial log alone (no meter).
    {
        int rerr;
        if (radio::init(rerr)) {
            int rc = radio::sleep();
            Serial.printf("# hibernate: radio init ok, SX1262 sleep rc=%d (0=ok)\n", rc);
        } else {
            Serial.printf("# hibernate: radio init failed (code=%d); radio left in reset\n", rerr);
        }
    }
#ifdef HAS_FEM
    pinMode(PIN_FEM_VFEM, OUTPUT);  digitalWrite(PIN_FEM_VFEM, LOW);   // FEM off
    pinMode(PIN_VEXT_CTRL, OUTPUT); digitalWrite(PIN_VEXT_CTRL, HIGH); // peripheral rail off
#endif
    set_reboot_intent(REBOOT_INTENT_OFF);   // so the RST-wake boot logs "OFF_WAKE", not a watchdog
    platform::system_off();         // only RST wakes us -> full restart
}

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 500) { delay(10); }

    // NOTE: the 80 MHz battery-saver clock drop (platform::set_cpu_low_power) used
    // to run HERE, first thing. It now runs at the END of setup() instead — see
    // the call site below for why (ipc1 cross-core-dispatch overflow at 80 MHz
    // during boot init). Boot init runs at the 240 MHz default; the drop happens
    // once everything is up.

#ifndef GIT_REV
#define GIT_REV "unknown"
#endif
    Serial.printf("\n# morse-station build %s\n", GIT_REV);
    {
        // Why did we (re)boot? An intermittent native-USB panic loses its
        // backtrace mid-reset, so we persist each boot's reason to an NVS ring
        // buffer (see bootlog_record) and print the *previous* boot's reason too.
        // After an organic crash-reboot, a controlled reset (easy to capture on
        // serial) reports prev=<crash cause>; the full history survives in NVS for
        // the `bootlog` command even when several reboots land back to back.
        int r = platform::reset_reason();
#ifdef HAS_REBOOT_INTENT
        // reset_reason() is the unreliable POWERON catch-all on this bootloader;
        // refine it from the persisted reboot-intent flag, then re-arm to RUNNING.
        {
            uint8_t intent = get_reboot_intent();
            if      (intent == REBOOT_INTENT_SOFT)    r = platform::reset_code_soft();
            else if (intent == REBOOT_INTENT_OFF)     r = platform::reset_code_off();
            else if (intent == REBOOT_INTENT_RUNNING) r = platform::reset_code_watchdog();
            // REBOOT_INTENT_NONE: never armed => genuine first cold boot, keep POWERON.
            set_reboot_intent(REBOOT_INTENT_RUNNING);
        }
#endif
        int prev = 0;
        uint32_t bc = bootlog_record(r, prev);
        Serial.printf("# boot #%u reason now=%d(%s) prev=%d(%s)\n",
                      bc, r, reset_reason_label(r), prev, reset_reason_label(prev));
    }

    config::begin();
    // Restore the persisted canned-message delivery mode (keyed/text, field note
    // §7) so a fox flipped between modes over the air stays there. Default text.
    g_msgmode = config::msgmode() ? MSGMODE_TEXT : MSGMODE_KEYED;
    // Restore the last-used fox TX power level (clamp in case the table shrank).
    pwr_idx = config::fox_pwr_idx();
    if (pwr_idx >= N_PWR) pwr_idx = 0;
    // Continue the instructor control-packet seq where it left off, so a reboot
    // doesn't restart at 0 and collide with a fox's remembered last_seq.
    g_ctrl_seq = config::ctrl_seq();
    print_boot_config();

    // Start BLE-UART (NUS) field provisioning and leave it up for the whole
    // session, so an operator can adjust parameters on a running node over the
    // air (docs/commands.md, BLE transport). Advertise a per-unit name so the
    // units are distinguishable from a Mac (where CoreBluetooth hides the MAC).
    {
        char adv_name[24];
        snprintf(adv_name, sizeof(adv_name), "MorseStn-%u", config::station_id());
        ble_provision::begin(adv_name, handle_setup_command);
        Serial.printf("# BLE provisioning advertising as \"%s\"\n", adv_name);
    }

    display::begin();
    battery::begin();
    char splash_id[24];
    snprintf(splash_id, sizeof(splash_id), "build %s", GIT_REV);
    char splash_line2[24];
    snprintf(splash_line2, sizeof(splash_line2), "station id %u", config::station_id());
    display::status("Morse Station", splash_id, splash_line2);
    delay(3000);                       // hold the splash so rev + id are readable
#ifdef DEVICE_CARDPUTER_ADV
    // On-device keyboard provisioning (callsign / fox message). Brief opt-in
    // window; falls through to normal boot if no key is tapped. The serial
    // console above still works as the bench path.
    config_ui::run();
#endif
    sidetone_init(PIN_SIDETONE, TONE_HZ);
    sidetone_set_level(config::volume()); // restore the persisted sidetone level
    sidetone_set_mute(config::muted());   // restore a persisted silent node

    mode = run_menu();
    // Remember the choice as the boot default (Hibernate is transient — never
    // store it, or the unit would sleep again on every restart).
    if (mode != MODE_HIBERNATE) config::set_boot_mode((uint8_t)mode);
    Serial.printf("\n# mode=%d station_id=%u\n", (int)mode, config::station_id());

    if (mode == MODE_HIBERNATE) {
        ble_provision::stop();   // free the radio core before deep sleep
        hibernate();             // does not return
    }

    int err;
    if (!radio::init(err)) {
        Serial.printf("FATAL: radio init failed (code=%d)\n", err);
        while (true) delay(1000);
    }
    radio::set_lna(config::lna());   // restore persisted V4.3 RX LNA select (no-op w/o FEM)
    radio::set_rx_bw(config::rx_bw_khz());   // restore persisted FSK RX bandwidth

    switch (mode) {
        case MODE_FOX:
            player.begin(config::wpm(), config::char_wpm());
            player.start(config::fox_message());
            radio::set_tx_power(PWR_LEVELS[pwr_idx].dbm);
            radio::set_pa(true); g_pa_on = true;   // V4 FEM PA on for TX (no-op w/o FEM)
            break;
        case MODE_LIVEKEY:
            key.begin(PIN_KEY);
            radio::set_pa(true); g_pa_on = true;   // V4 FEM PA on for TX (no-op w/o FEM)
            break;
        case MODE_HUNTER:
            // Seed from our own config; the fox's Ident packet retunes this to
            // the actual on-air timing (see loop_hunter).
            decoder.begin(config::wpm(), config::char_wpm());
            radio::start_receive();
            break;
        case MODE_INSTRUCTOR:
            // Remote control: always MAX so every fox in the field hears the
            // command bursts. `ipwr` can override at runtime without reflash.
            pwr_idx = N_PWR - 1;
            radio::set_tx_power(PWR_LEVELS[pwr_idx].dbm);
            radio::set_pa(true); g_pa_on = true;       // no-op w/o FEM
            radio::start_receive();
            break;
        case MODE_HIBERNATE:
            break;   // handled before radio init; never reached
    }

    // BLE power policy, decided once the run mode is known (single source of
    // truth). An instructor sits on a blanked "ready" screen but must stay
    // reachable from the companion app/phone over BLE at all times, so it must
    // never let the AUTO panel-follow coupling drop the NimBLE core. Pin BLE ON
    // for instructors; every other mode keeps the default AUTO (panel-follow,
    // ~70 mA idle saving). `mode` is fixed for the life of this boot — the only
    // way to change it is the `mode <n>` command, which writes NVS and takes
    // effect on the next reboot, re-running this setup() — so this decision needs
    // no runtime re-evaluation. A manual `ble off`/`ble auto` later is still a
    // deliberate operator override and works (it just reassigns g_ble_mode).
    if (mode == MODE_INSTRUCTOR) {
        g_ble_mode = BLE_ON;
        apply_ble(true);
        Serial.println("# ble: instructor -> pinned ON (panel-follow disabled)");
    } else {
        Serial.println("# ble: AUTO (follows panel; blank drops the core)");
    }

    // Radio and run mode are up — let BLE provisioning apply changes live.
    g_live_apply = true;

    // Battery saver: drop to the low-power core clock (80 MHz on ESP32-S3, no-op
    // on nRF52) now that boot init is done. Deliberately LAST: every boot-time
    // cross-core IPC dispatch (driver interrupt allocation, NimBLE controller
    // bring-up, NVS flash writes) has already run at the 240 MHz default, where
    // it stays within the ~1 KB ipc1 stack budget. Running that init window at
    // 80 MHz is what overflowed ipc1 and boot-looped the V4 (see
    // platform::set_cpu_low_power). From here only the steady-state loop runs at
    // 80 MHz. arduino-esp32 retracks the UART so the console keeps working.
    platform::set_cpu_low_power();

    // Arm the hardware watchdog now that boot is done (the splash delays and the
    // blocking run_menu() above would have tripped it). From here loop() must
    // pet it every pass; a wedged loop reboots the node and the next boot's
    // bootlog records the watchdog cause. 8 s is generous — the run-mode loops
    // are non-blocking state machines whose longest single window is ~200 ms.
    // Exercise this path with the `stall` console command.
    platform::watchdog_begin(8000);
}

// Edge-event TX (docs/edge-events.md): send a packet only when the key
// changes level, carrying the *measured* duration of the segment that just
// ended plus the one before it (self-heals a single lost packet), and an idle
// heartbeat re-asserting the current level so a receiver has presence/re-anchor
// even when the key sits still for a long gap. This is the sole keying TX path,
// timed by the key, not a fixed cadence.
static void tx_edge(uint32_t now, bool down) {
    static bool     inited      = false;
    static bool     last_level  = false;  // level of the currently-open segment
    static uint32_t seg_start_ms = 0;     // when the open segment began
    static uint16_t prev_dur_ms  = 0;     // duration of the segment before that
    static uint32_t last_tx_ms   = 0;     // last time we transmitted anything
    static uint8_t  edge_seq     = 0;     // EdgeEvent's own seq space (not `seq`)

    proto::EdgeEvent ev{};
    bool send = false;

    if (!inited) {
        // First call: nothing has "ended" yet. Open a segment at the current
        // level and immediately announce it as a heartbeat so a receiver learns
        // our presence and absolute level without waiting a full HEARTBEAT_MS.
        inited       = true;
        last_level   = down;
        seg_start_ms = now;
        prev_dur_ms  = 0;
        last_tx_ms   = now;
        ev.flags     = (uint8_t)((last_level ? proto::EDGE_FLAG_DOWN : 0) |
                                 proto::EDGE_FLAG_HEARTBEAT);
        ev.dur_now_ms = 0;
        ev.dur_prev_ms = prev_dur_ms;
        send = true;
    } else if (down != last_level) {
        // Real edge: the segment at `last_level` just ended after `dur` ms.
        // flags carries the level being ENTERED (= `down`).
        uint16_t dur = (uint16_t)(now - seg_start_ms);
        ev.flags       = (uint8_t)(down ? proto::EDGE_FLAG_DOWN : 0);
        ev.dur_now_ms  = dur;
        ev.dur_prev_ms = prev_dur_ms;
        send = true;

        prev_dur_ms  = dur;
        last_level   = down;
        seg_start_ms = now;
        last_tx_ms   = now;
    } else if (now - last_tx_ms >= proto::HEARTBEAT_MS) {
        // No edge, but the key has been steady long enough to need a presence
        // re-assert. Reports elapsed-so-far of the still-open segment; does NOT
        // touch seg_start_ms/last_level — the segment keeps accumulating.
        uint32_t elapsed = now - seg_start_ms;
        ev.flags       = (uint8_t)((last_level ? proto::EDGE_FLAG_DOWN : 0) |
                                   proto::EDGE_FLAG_HEARTBEAT);
        ev.dur_now_ms  = (uint16_t)(elapsed > 65535 ? 65535 : elapsed);
        ev.dur_prev_ms = prev_dur_ms;
        send = true;

        last_tx_ms = now;
    }

    if (!send) return;

    ev.magic      = proto::MAGIC_EDGE;
    ev.station_id = config::station_id();
    // Real edges own and increment seq; heartbeats repeat the CURRENT seq
    // unconsumed (identified by EDGE_FLAG_HEARTBEAT) so the receiver can
    // distinguish "lost edge" from "intervening heartbeat" — see docs/edge-events.md.
    if (!(ev.flags & proto::EDGE_FLAG_HEARTBEAT)) {
        ev.seq = edge_seq++;
    } else {
        ev.seq = edge_seq;
    }

    uint8_t buf[proto::EDGE_LEN];
    proto::encode_edge(ev, buf);
    radio::send(buf, proto::EDGE_LEN);
    if (g_debug) {
        Serial.printf("TX E seq=%u lvl=%u hb=%u dn=%u dp=%u\n",
                      ev.seq, (ev.flags & proto::EDGE_FLAG_DOWN) ? 1 : 0,
                      (ev.flags & proto::EDGE_FLAG_HEARTBEAT) ? 1 : 0,
                      ev.dur_now_ms, ev.dur_prev_ms);
    }
}

// ---------------------------------------------------------------------------
// Fox keyer (TODO-fox-timing.md mitigation #1): the fixed-cadence callback.
//
// Runs in platform::keyer_*'s timer/task context every ~2 ms — NEVER from
// loop(). It is the ONLY caller of player.update/start/down in the fox edge
// path. It advances the player off a stable monotonic clock
// (platform::keyer_now_us → ms), detects key-level transitions, MEASURES the
// duration of each completed segment, and pushes a small EdgeRec into the SPSC
// ring. It does NO radio SPI / Serial / flash — loop_fox() drains the ring and
// transmits. This mirrors tx_edge()'s measurement state machine but on the
// jitter-free timebase, so a slow loop() can no longer stretch a measured
// element/gap.
static void keyer_tick() {
    g_keyer_ticks.fetch_add(1, std::memory_order_relaxed);

    static bool     last_level   = false;   // level of the currently-open segment
    static uint32_t seg_start_ms = 0;       // when the open segment began (keyer clock)
    static uint32_t last_hb_ms   = 0;       // last heartbeat emission (keyer clock)
    static bool     in_message   = false;   // a message timeline is playing

    uint32_t now = platform::keyer_now_us() / 1000u;   // keyer monotonic ms

    // (Re)start command from loop at a message boundary.
    if (g_keyer_start_req.exchange(false, std::memory_order_acq_rel)) {
        player.start(config::fox_message());
        last_level   = player.down();
        seg_start_ms = now;
        last_hb_ms   = now;
        in_message   = true;
        g_keyer_finished.store(false, std::memory_order_release);
        // Announce presence/level immediately as a heartbeat (mirrors tx_edge
        // §init) so a receiver re-anchors without waiting a full HEARTBEAT_MS.
        EdgeRec r{(uint8_t)((last_level ? proto::EDGE_FLAG_DOWN : 0) |
                            proto::EDGE_FLAG_HEARTBEAT), 0};
        uint8_t tail = g_keyer_tail.load(std::memory_order_relaxed);
        uint8_t next = (uint8_t)((tail + 1) % KEYER_RING_SZ);
        if (next != g_keyer_head.load(std::memory_order_acquire)) {
            g_keyer_ring[tail] = r;
            g_keyer_tail.store(next, std::memory_order_release);
        } else {
            g_keyer_drops.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }

    if (!in_message) return;   // idle: keyer stays quiet between messages

    player.update(now);
    bool down = player.down();

    EdgeRec r{};
    bool have = false;

    if (down != last_level) {
        // Real edge: the segment at last_level just ended after `dur` ms.
        uint16_t dur = (uint16_t)(now - seg_start_ms);
        r.flags  = (uint8_t)(down ? proto::EDGE_FLAG_DOWN : 0);
        r.dur_ms = dur;
        have = true;
        last_level   = down;
        seg_start_ms = now;
        last_hb_ms   = now;
    } else if (now - last_hb_ms >= proto::HEARTBEAT_MS) {
        // Steady too long → presence heartbeat with elapsed-so-far.
        uint32_t elapsed = now - seg_start_ms;
        r.flags  = (uint8_t)((last_level ? proto::EDGE_FLAG_DOWN : 0) |
                             proto::EDGE_FLAG_HEARTBEAT);
        r.dur_ms = (uint16_t)(elapsed > 65535 ? 65535 : elapsed);
        have = true;
        last_hb_ms = now;
    }

    if (have) {
        uint8_t tail = g_keyer_tail.load(std::memory_order_relaxed);
        uint8_t next = (uint8_t)((tail + 1) % KEYER_RING_SZ);
        if (next != g_keyer_head.load(std::memory_order_acquire)) {
            g_keyer_ring[tail] = r;
            g_keyer_tail.store(next, std::memory_order_release);
        } else {
            g_keyer_drops.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (player.finished()) {
        in_message = false;
        g_keyer_finished.store(true, std::memory_order_release);
    }
}

// Drain the keyer ring in loop context: pop EdgeRecs, build proto::EdgeEvents
// (maintaining dur_prev across pops + the heartbeat/real-edge seq rules from
// tx_edge §seq), and radio::send(). Gated by the caller (rx_window/g_tx_halted).
static void fox_keyer_drain(uint32_t now) {
    static uint16_t prev_dur_ms = 0;   // duration of the segment before the one in dur_now
    static uint8_t  edge_seq    = 0;   // EdgeEvent seq space (matches tx_edge)

    for (;;) {
        uint8_t head = g_keyer_head.load(std::memory_order_relaxed);
        if (head == g_keyer_tail.load(std::memory_order_acquire)) break;   // empty
        EdgeRec rec = g_keyer_ring[head];
        g_keyer_head.store((uint8_t)((head + 1) % KEYER_RING_SZ),
                           std::memory_order_release);

        bool hb = (rec.flags & proto::EDGE_FLAG_HEARTBEAT) != 0;
        proto::EdgeEvent ev{};
        ev.magic      = proto::MAGIC_EDGE;
        ev.station_id = config::station_id();
        ev.flags      = rec.flags;
        ev.dur_now_ms = rec.dur_ms;
        ev.dur_prev_ms = prev_dur_ms;
        // Real edges own and increment seq; heartbeats repeat the current seq
        // unconsumed so the receiver distinguishes "lost edge" from "heartbeat".
        if (!hb) {
            ev.seq = edge_seq++;
            prev_dur_ms = rec.dur_ms;   // self-heal: next packet carries this as dur_prev
        } else {
            ev.seq = edge_seq;
        }

        uint8_t buf[proto::EDGE_LEN];
        proto::encode_edge(ev, buf);
        radio::send(buf, proto::EDGE_LEN);
        if (g_debug) {
            Serial.printf("TX E seq=%u lvl=%u hb=%u dn=%u dp=%u\n",
                          ev.seq, (ev.flags & proto::EDGE_FLAG_DOWN) ? 1 : 0,
                          hb ? 1 : 0, ev.dur_now_ms, ev.dur_prev_ms);
        }
    }
}

// Send a station-ID packet now (callsign + keying speeds). A rare, ~ms
// transmission, so it doesn't perturb the 30 ms keystate stream.
static void send_ident(uint32_t now) {
    last_ident = now;
    uint8_t buf[proto::IDENT_LEN];
    proto::encode_ident(config::station_id(), config::callsign(),
                        config::wpm(), config::char_wpm(), buf);
    radio::send(buf, proto::IDENT_LEN);
}

// Transmit the canned clue as a repeated TextMsg burst (field note §7,
// docs/plan-text-message-mode.md). One seq per message cycle; the hunter dedups
// the burst's redundant copies by seq and renders Morse locally. Mirrors the
// ACK-burst idiom (TEXT_REPEATS copies ~TEXT_GAP_MS apart) for loss tolerance at
// the edge of range. Caller gates this on !g_tx_halted/!rx_window like the other
// TX paths. The text frame carries no timing — the fox still sends Ident at the
// cycle top so the hunter renders at the fox's wpm/char_wpm.
static uint8_t g_text_seq = 0;   // current clue's seq; advanced once per cycle

// Burst the clue as TEXT_REPEATS copies with a CALLER-CHOSEN seq. A fresh cycle
// passes a new seq (via tx_text); a mid-render re-send (field note §8) reuses the
// cycle's seq so existing hunters dedup it while a mid-joiner picks up the text.
static void tx_text_seq(uint8_t seq) {
    uint8_t buf[proto::TEXT_HDR_LEN + proto::TEXT_MSG_MAX];
    size_t len = proto::encode_text(config::station_id(), seq,
                                    /*flags=*/0, config::fox_message(), buf);
    for (uint8_t i = 0; i < TEXT_REPEATS; ++i) {
        radio::send(buf, len);
        if (i + 1 < TEXT_REPEATS) delay(TEXT_GAP_MS);
    }
    if (g_debug) Serial.printf("TX T seq=%u len=%u\n", seq, (unsigned)len);
}

// Start a new clue cycle: advance the seq and burst it. Returns the seq used so
// the caller can re-send and beacon with the same seq for the rest of the cycle.
static uint8_t tx_text(uint32_t now) {
    (void)now;
    uint8_t seq = g_text_seq++;
    tx_text_seq(seq);
    return seq;
}

// Emit one text-render sync / DF beacon (field note §8, docs/plan-text-sync-
// beacon.md). Single send (not a burst) — the next beacon is BEACON_MS away, so a
// lost one self-heals. pos_ms is the fox's render position, or POS_IDLE between
// clues. Carries the fox's wpm/char_wpm so a mid-joiner renders without an Ident.
static void tx_sync(uint32_t now, uint8_t seq, uint16_t pos_ms) {
    (void)now;
    uint8_t buf[proto::SYNC_LEN];
    proto::encode_sync(config::station_id(), seq, config::wpm(),
                       config::char_wpm(), pos_ms, buf);
    radio::send(buf, proto::SYNC_LEN);
    if (g_debug) Serial.printf("TX S seq=%u pos=%u\n", seq, pos_ms);
}

// Announce the RX window as the fox opens it, so an instructor bursts at once
// instead of waiting out CTRL_SILENCE_MS of inferred silence (see protocol.h
// Listen). Sent once per window, immediately before switching to receive.
static void send_listen(uint16_t window_ms) {
    uint8_t buf[proto::LISTEN_LEN];
    proto::encode_listen(config::station_id(), window_ms, buf);
    radio::send(buf, proto::LISTEN_LEN);
}

// Transmit the station-ID packet on the IDENT cadence (well under the Part 97
// 10-minute limit). Sent once at fox start, then every IDENT_INTERVAL_MS — plus
// the fox forces one at the top of each message loop (see loop_fox) so a hunter
// learns the timing within a loop.
static void tx_ident(uint32_t now) {
    if (last_ident != 0 && now - last_ident < proto::IDENT_INTERVAL_MS) return;
    send_ident(now);
}

// Debounced rising-edge detector for the PRG/BOOT button (already INPUT_PULLUP
// from the boot menu). Used in fox mode to cycle TX power.
static bool prg_tapped(uint32_t now) {
    static bool was_down = false;
    static uint32_t last = 0;
    bool down = (digitalRead(PIN_MODE_BTN) == LOW);
    bool edge = down && !was_down && (now - last > 200);
    if (edge) {
        last = now;
        // A press while the panel is blanked is a "wake" press only: it lights
        // the screen and is swallowed so it doesn't also trigger this mode's
        // button action (TX-power cycle / volume cycle). When the panel is
        // already on, wake (refresh the idle timer) and let the edge through.
        bool was_blanked = display::blanked();
        display::activity();
        if (was_blanked) edge = false;
        else if (banner_active(now) && !g_alert_latched) {
            // A press while a banner is showing dismisses it instead of doing the
            // mode's button action, and is swallowed (mirrors the wake-press rule).
            // A §6 sticky alert is NOT dismissable locally (only `alert clear` or
            // reset) — the press is still swallowed below to avoid the button action.
            g_banner_until = 0;
            edge = false;
        } else if (banner_active(now)) {
            edge = false;               // latched alert: swallow the press, keep banner
        }
    }
    was_down = down;
    return edge;
}

// Apply an inbound control packet (instructor remote control, docs: instructor
// station). Decodes the command, runs it through the SAME handle_setup_command()
// the BLE/serial console uses, and acks the instructor. Returns true if the
// buffer was a control packet addressed to us (handled), false otherwise so the
// caller can keep trying other decoders. Call only while the radio is in RX.
static bool control_rx_try(const uint8_t* buf, size_t n, uint32_t now) {
    proto::ControlCmd c;
    if (!proto::decode_ctrl(buf, n, c)) return false;
    // Only the instructor (id 0) may command; only packets addressed to us (or
    // broadcast) are honoured. A foreign control packet is still "handled" here
    // (return true) so the caller doesn't misparse it as another type.
    if (c.src_id != proto::INSTRUCTOR_ID) return true;
    if (c.target_id != config::station_id() && c.target_id != proto::BROADCAST_ID)
        return true;

    // Dedup: the instructor bursts the same seq repeatedly. Apply the command
    // once, but re-ack EVERY copy so a lost ack still gets answered.
    static int  last_seq = -1;
    static bool have_last = false;
    bool fresh = !(have_last && (int)c.seq == last_seq);
    char status[proto::ACK_STATUS_MAX + 1];
    if (fresh) {
        CapturePrint cap;
        handle_setup_command(c.cmd, cap);
        strncpy(status, cap.c_str(), proto::ACK_STATUS_MAX);
        status[proto::ACK_STATUS_MAX] = '\0';
        last_seq = c.seq; have_last = true;
    } else {
        snprintf(status, sizeof(status), "dup seq %u", c.seq);
    }

    if (g_debug) {
        Serial.printf("RX C from=%u tgt=%u seq=%u fresh=%u cmd=%s\n",
                      c.src_id, c.target_id, c.seq, fresh ? 1 : 0, c.cmd);
    }

    // Ack the instructor with a short confirmation, then re-arm RX. Send the ack
    // as a small burst of identical copies (see ACK_REPEATS): a phone on the
    // instructor makes NimBLE preempt its RX-arming, so a lone ack lands on a deaf
    // radio and is lost — spreading copies guarantees one lands once it is armed.
    // The instructor stops bursting on the first copy it hears; the rest are
    // harmless (it dedups acks by src_id, see instructor_service_rx).
    uint8_t ackbuf[proto::CTRL_HDR_LEN + proto::ACK_STATUS_MAX];
    size_t  alen = proto::encode_ack(config::station_id(), c.src_id, c.seq,
                                     status, ackbuf);

    // Always ACK at MAX. The command reaches the fox fine at play power (good RX
    // sensitivity), but the return ACK goes out at the fox's *current* TX power —
    // typically LO for open-field play — and dies at distance, so the instructor
    // only hears it up close. The ACK is a tiny, infrequent packet, so sending it
    // hot costs almost nothing on duty cycle / §15.249: command at play power,
    // acknowledgement at full power so the instructor always copies it. Save,
    // bump, restore strictly around the burst (no early returns between) so the
    // fox never keeps keying its *message* at MAX afterward.
    const int  saved_pwr_idx = pwr_idx;
    const bool saved_pa      = g_pa_on;
    radio::set_tx_power(PWR_LEVELS[N_PWR - 1].dbm);   // MAX
    radio::set_pa(true);                              // engage FEM PA (no-op w/o FEM)
    for (uint8_t i = 0; i < ACK_REPEATS; ++i) {
        radio::send(ackbuf, alen);
        if (i + 1 < ACK_REPEATS) delay(ACK_GAP_MS);
    }
    radio::set_tx_power(PWR_LEVELS[saved_pwr_idx].dbm);
    radio::set_pa(saved_pa);

    radio::start_receive();
    return true;
}

// True while an instructor broadcast banner should be shown (set by
// broadcast_rx_try or a local instructor send; cleared by timeout/dismiss).
static bool banner_active(uint32_t now) {
    if (g_alert_latched) return true;   // §6 sticky alert: never expires
    return g_banner_until != 0 && (int32_t)(g_banner_until - now) > 0;
}

// Arm the banner overlay from `text`. A broadcast must be seen, so it always
// force-wakes a blanked panel (the `alert` flag now only affects any extra
// emphasis a panel chooses to add — every banner wakes the screen). Used by both
// the RX handler and the local instructor echo. Empty text clears the banner AND
// releases any §6 sticky latch. `sticky` (field note §6) latches the banner: it
// never auto-expires (g_banner_until = 0 sentinel; banner_active() short-circuits
// on g_alert_latched) until an empty-text clear or reset. Last-writer-wins.
static void set_banner(const char* text, bool alert, bool sticky, uint32_t now) {
    (void)alert;
    if (!text || !text[0]) {            // `alert clear` / empty → dismiss + release
        g_banner[0] = '\0';
        g_banner_until = 0;
        g_alert_latched = false;
        return;
    }
    strncpy(g_banner, text, proto::BCAST_TEXT_MAX);
    g_banner[proto::BCAST_TEXT_MAX] = '\0';
    g_alert_latched = sticky;           // last-writer-wins
    g_banner_until = sticky ? 0 : now + BCAST_SHOW_MS;  // latched = no deadline
    display::activity();                // force-wake the panel so the banner shows
}

// Sound the attention tone for ALERT_TONE_MS. Overrides the master mute (a
// broadcast must be heard); sidetone_alert() forces the tone on at the board's
// fixed frequency. Re-arming while already sounding just extends the deadline.
static void start_alert_tone(uint32_t now) {
    g_alert_tone_until = now + ALERT_TONE_MS;
    sidetone_alert(true);
    if (g_debug) Serial.printf("# alert: tone %ums\n", (unsigned)ALERT_TONE_MS);
}

// End the attention tone once its deadline passes. Cheap, non-blocking — called
// every loop() so it never holds the speaker past ALERT_TONE_MS. Restores the
// underlying key/mute state (sidetone_alert(false) re-evaluates it).
static void alert_tone_tick(uint32_t now) {
    if (g_alert_tone_until && (int32_t)(g_alert_tone_until - now) <= 0) {
        g_alert_tone_until = 0;
        sidetone_alert(false);
    }
}

// Paint the active banner as a full-screen overlay. The display layer handles
// centering / two-line wrap / horizontal scroll per panel (display::banner). We
// call activity() every frame so the panel stays lit for the banner's full life
// — a broadcast must be seen, so it overrides the idle-blank regardless of the
// operator's blank state (and regardless of the alert flag).
static void draw_banner() {
    display::activity();
    display::banner(g_banner, millis());
}

// Apply an inbound broadcast banner packet. Decodes, requires src_id ==
// INSTRUCTOR_ID, dedups by seq (the instructor bursts the same seq repeatedly),
// and on a fresh copy paints the banner. Fire-and-forget — no ack is sent.
// Returns true if the buffer was a broadcast packet (handled), so the RX cascade
// stops trying other decoders. Receivers ignore unknown flags bits (mask bit0).
static bool broadcast_rx_try(const uint8_t* buf, size_t n, uint32_t now) {
    proto::BroadcastMsg b;
    if (!proto::decode_bcast(buf, n, b)) return false;
    if (b.src_id != proto::INSTRUCTOR_ID) return true;   // only the instructor

    static int  last_seq  = -1;
    static bool have_last = false;
    bool fresh = !(have_last && (int)b.seq == last_seq);
    last_seq = b.seq; have_last = true;
    if (fresh) {
        bool alert  = (b.flags & proto::BCAST_FLAG_ALERT) != 0;
        bool sticky = (b.flags & proto::BCAST_FLAG_STICKY) != 0;
        set_banner(b.text, alert, sticky, now);
        // Every alert beeps (text present); `alert clear` (empty text) is silent.
        if (b.text[0]) start_alert_tone(now);
        // §6 all-stop: a sticky alert halts our own TX (Fox + Live Key share
        // g_tx_halted); an empty-text clear resumes it. `start`/reset are the
        // other two resume triggers and are handled elsewhere.
        if (mode == MODE_FOX || mode == MODE_LIVEKEY) {
            if (sticky)            g_tx_halted = true;
            else if (!b.text[0])   g_tx_halted = false;
        }
        last_draw = 0;   // force an immediate redraw to show the banner
        if (g_debug)
            Serial.printf("RX B seq=%u flags=%u sticky=%d text=%s\n",
                          b.seq, b.flags, sticky ? 1 : 0, b.text);
    }
    return true;
}

static void loop_fox(uint32_t now) {
    if (prg_tapped(now)) cycle_tx_power();

    // Remote-control RX window (instructor station): the fox is otherwise
    // TX-only, but it goes silent for the *tail* of its inter-message pause and
    // listens for instructor control packets. The window is kept under the
    // hunter's 3 s presence timeout (loop_hunter signal_timeout_ms) so a hunter
    // doesn't drop the fox during it. Outside the window the fox keeps streaming
    // keystate as before; the instructor bursts a command long enough to overlap
    // one of these windows (see loop_instructor). Half-duplex: we must suppress
    // our own TX while listening, so `rx_window` gates the keystate/edge TX below.
    static bool rx_armed = false;
    // In msgmode=text no keyer/player runs, so "idle between messages" is just the
    // pause window — don't gate the instructor RX window on player.finished()
    // (which would never advance after a runtime keyed→text switch).
    bool tx_idle = (g_msgmode == MSGMODE_TEXT) ? true : player.finished();
    bool rx_window = tx_idle && pause_until != 0 &&
                     now < pause_until && (now + CTRL_RX_WINDOW_MS >= pause_until);
    if (rx_window) {
        if (!rx_armed) {
            // Beacon the window open, then switch to RX (half-duplex: TX first).
            // window_ms = the remaining pause = how long we'll listen.
            send_listen((uint16_t)(pause_until - now));
            radio::start_receive();
            rx_armed = true;
        }
        uint8_t cbuf[128];
        size_t cn; float cr;
        if (radio::poll(cbuf, sizeof(cbuf), cn, cr)) {
            if (!broadcast_rx_try(cbuf, cn, now)) control_rx_try(cbuf, cn, now);
        }
    } else {
        rx_armed = false;
    }

    // Text-frame canned-message cycle (field note §7, docs/plan-text-message-mode.md).
    // No keying, no edges: at each cycle top announce timing (Ident) so the hunter
    // renders at the fox's wpm, then burst the whole clue as a TextMsg. The hunter
    // renders Morse locally — robust at the edge of range where a single lost edge
    // would be a '?'. Gated by the instructor RX window and the §6 fox-halt exactly
    // like the keyed paths; the fox still runs silent. MODE_LIVEKEY is unaffected
    // (it always keys edges) — this branch is the canned loop_fox cycle only.
    if (g_msgmode == MSGMODE_TEXT) {
        // If the keyer was running from a prior keyed cycle (runtime switch), stop
        // it so it isn't left keying the radio.
        if (g_keyer_active) {
            platform::keyer_stop();
            g_keyer_active = false; g_keyer_starting = false;
        }
        set_tone(false);   // fox is silent (a beeping fox is easy to find by ear)

        // Master render clock (field note §8): the fox advances a SILENT local
        // Player mirroring the render each hunter performs, so the sync beacon can
        // carry an accurate render position (pos_ms). text_rendering is true from a
        // cycle's clue burst until that render finishes; the inter-message pause
        // (pause_until) follows, during which beacons keep flowing with POS_IDLE.
        static bool     text_rendering = false;
        static uint8_t  clue_seq       = 0;
        static uint32_t last_beacon    = 0;
        static uint32_t last_resend    = 0;

        // Cycle top: a new clue (only once the previous render AND its pause are
        // done). Announce timing, burst the clue, and start the silent render clock
        // at the fox's wpm so pos_ms tracks the hunters' local renders.
        if (!rx_window && now >= pause_until && !text_rendering) {
            if (!g_tx_halted) {
                send_ident(now);                 // announce wpm/char_wpm
                clue_seq = tx_text(now);         // burst clue; capture its seq
                seq++;                           // on-screen cycle counter (text
                                                 // mode never hits the keyed path)
                player.begin(config::wpm(), config::char_wpm());
                player.start(config::fox_message());
                text_rendering = true;
                last_resend = now;
            } else {
                if (g_debug) {
                    static uint32_t last_skip_log = 0;
                    if (now - last_skip_log >= 1000) {
                        last_skip_log = now;
                        Serial.println("TX SKIP (halted) text");
                    }
                }
                // Keep cycling quietly so `start` resumes cleanly next cycle.
                pause_until = now + REPEAT_PAUSE;
            }
        }

        bool down = false;
        if (text_rendering) {
            player.update(now);
            down = player.down();                // cosmetic "*" on the panel
            // Re-burst the SAME clue periodically for any mid-message joiner.
            if (!g_tx_halted && !rx_window && now - last_resend >= TEXT_RESEND_MS) {
                last_resend = now;
                tx_text_seq(clue_seq);
            }
            if (player.finished()) {             // render done → open the pause
                text_rendering = false;
                pause_until = now + REPEAT_PAUSE;
            }
        }

        // Sync / DF beacon every BEACON_MS across the WHOLE loop — render AND pause
        // — so RSSI never dies and all hunters slave to pos_ms. Suppressed only
        // while halted or inside the instructor RX window (half-duplex: can't TX
        // while listening). pos_ms is the live render position, clamped below the
        // POS_IDLE sentinel, or POS_IDLE between clues.
        if (!g_tx_halted && !rx_window && now - last_beacon >= BEACON_MS) {
            last_beacon = now;
            uint16_t pos = proto::POS_IDLE;
            if (text_rendering) {
                uint32_t p = player.position_ms();
                pos = (p >= proto::POS_IDLE) ? (uint16_t)(proto::POS_IDLE - 1)
                                             : (uint16_t)p;
            }
            tx_sync(now, clue_seq, pos);
        }

        if (now - last_draw >= 100) {
            last_draw = now;
            if (banner_active(now)) draw_banner();
            else display::fox(seq, config::fox_message(), down,
                              PWR_LEVELS[pwr_idx].label);
        }
        return;
    }

    // Keying-timebase split (TODO-fox-timing.md mitigation #1): with a working
    // keyer, the keyer task OWNS the player + measures durations off-loop, and
    // loop_fox only manages the message lifecycle + drains/sends. The keyer-start
    // fallback keeps the legacy in-loop player.update + tx_edge path.
    bool use_keyer = !g_keyer_fallback;

    bool down;
    if (use_keyer) {
        // Power-gated lifecycle, explicit state machine:
        //   idle  (!active)            : waiting out REPEAT_PAUSE, keyer stopped.
        //   start (active && starting) : start command issued, waiting for the
        //                                keyer to clear g_keyer_finished. We must
        //                                NOT treat the still-true finished flag as
        //                                "message done" here, or we'd stop the
        //                                keyer the very next loop before it ran.
        //   run   (active && !starting): keyer is keying; stop when it reports
        //                                finished.
        bool finished = g_keyer_finished.load(std::memory_order_acquire);
        if (!g_keyer_active) {
            if (now >= pause_until) {
                // Start the next message: Ident announce, arm the keyer, hand it
                // the start command. keyer_start re-arms cheaply (start_periodic /
                // task resume). g_keyer_starting holds until the keyer acks.
                if (!g_tx_halted) send_ident(now);
                if (!platform::keyer_start(keyer_tick, 2000)) {
                    g_keyer_fallback = true;   // never worse than the in-loop path
                } else {
                    g_keyer_active   = true;
                    g_keyer_starting = true;
                    g_keyer_start_req.store(true, std::memory_order_release);
                }
            }
        } else if (g_keyer_starting) {
            if (!finished) g_keyer_starting = false;   // keyer accepted the start
        } else if (finished) {
            // Message done → stop the keyer (idle/sleepable pause) and open the
            // inter-message pause window.
            platform::keyer_stop();
            g_keyer_active = false;
            pause_until = now + REPEAT_PAUSE;
        }
        down = player.down();   // cosmetic (display "*"); single-bool read is benign
        set_tone(false);        // Fox runs SILENT (see below)
        if (!rx_window && !g_tx_halted) {
            fox_keyer_drain(now);   // pop measured edges → radio::send
            tx_ident(now);          // periodic station-ID packet (Part 97)
        } else if (g_tx_halted) {
            static uint32_t last_skip_log = 0;
            if (g_debug && now - last_skip_log >= 1000) {
                last_skip_log = now;
                Serial.printf("TX SKIP (halted) seq=%u\n", seq);
            }
        }
    } else {
        // Legacy in-loop path (keyer-start fallback).
        if (g_keyer_active) {
            platform::keyer_stop();
            g_keyer_active = false; g_keyer_starting = false;
        }
        if (!player.finished()) {
            player.update(now);
            if (player.finished()) pause_until = now + REPEAT_PAUSE;
        } else if (now >= pause_until) {
            // The Ident (timing announce) is part of TX — gate it on the halt
            // too, so a halted fox is truly silent. The player still cycles so
            // `start` resumes mid-stream cleanly.
            if (!g_tx_halted) send_ident(now);   // announce timing at each loop top
            player.start(config::fox_message());
        }
        down = player.down();
        // Fox runs SILENT: a transmitter that beeps is easy to find by ear,
        // which defeats the hunt. The fox only keys the radio; hunters hear the
        // Morse from the received edges. (Display still shows "*".)
        set_tone(false);
        if (!rx_window && !g_tx_halted) {
            tx_edge(now, down);
            tx_ident(now);   // periodic station-ID packet (Part 97 callsign ID)
        } else if (g_tx_halted) {
            static uint32_t last_skip_log = 0;
            if (g_debug && now - last_skip_log >= 1000) {
                last_skip_log = now;
                Serial.printf("TX SKIP (halted) seq=%u\n", seq);
            }
        }
    }

    if (now - last_draw >= 100) {
        last_draw = now;
        if (banner_active(now)) draw_banner();   // instructor banner borrows the panel
        else display::fox(seq, config::fox_message(), down, PWR_LEVELS[pwr_idx].label);
    }
}

static void loop_livekey(uint32_t now) {
    key.update();
    bool down = key.down();
    if (down) display::activity();   // active keying = operator present; keep panel awake
    set_tone(down);
    if (!g_tx_halted) tx_edge(now, down);   // live keying always keys edges

    if (now - last_draw >= 100) {
        last_draw = now;
        if (banner_active(now)) draw_banner();
        else display::livekey(seq, down);
    }
}

// Single-fox lock gate. In a classroom several foxes may share the channel; the
// hunter follows exactly ONE so their edge streams don't interleave into the
// single shared decoder. Adopts the first fox heard, refreshes liveness on each
// packet from it, and reports false for any OTHER fox so the caller drops it.
// The lock is released (locked=-1) by loop_hunter when the fox goes silent.
static bool fox_lock(uint8_t s, int& locked, uint32_t& last_fox_rx, uint32_t now) {
    if (locked < 0) locked = s;            // adopt the first fox heard
    if ((int)s != locked) return false;    // a competing fox: ignore its keying
    last_fox_rx = now;
    return true;
}

static void loop_hunter(uint32_t now) {
    static bool     rx_down = false;
    static float    rssi = 0.0f;
    static bool     rssi_valid = false;
    static uint32_t last_rx = 0;
    static uint32_t last_signal = 0;   // any decoded packet (edge or ident)
    static uint32_t last_code = 0;     // last newly decoded char/dit-dah element

    // Edge RX (docs/edge-events.md): `rx_is_edge` is true while receiving edge
    // keying (vs a text frame); it gates the decode path below (feed_segment) and
    // the self-heal. `last_edge_seq`/`reanchor` drive the EdgeEvent self-heal
    // (-1 = no anchor yet -> first edge anchors with no healing attempted).
    static bool rx_is_edge     = false;
    static int  last_edge_seq  = -1;
    static bool reanchor       = false;

    // Text-frame mode mirror of rx_is_edge (field note §7): true once a TextMsg is
    // received, false again on any EdgeEvent. In text mode the clue is
    // rendered locally and the copy buffers are filled directly from the frame, so
    // the polling/edge decoder must NOT also run — otherwise a stale half-built
    // character left over from a prior edge fox flushes a phantom '?' into the same
    // buffer (field-observed regression). Guards the decoder step below.
    static bool rx_is_text     = false;

    // Single-fox lock (see fox_lock above): the station this hunter is currently
    // following, and the last time we heard its keying. -1 = unlocked, adopt the
    // next fox heard. Released when last_fox_rx ages past signal_timeout_ms.
    static int      locked_fox  = -1;
    static uint32_t last_fox_rx = 0;

    // Copy-blanking timeout: after this long with no fresh decoded code, wipe the
    // text and dit/dah copy lines so a stale message doesn't linger. The fox's
    // inter-message gap (REPEAT_PAUSE) is deliberately longer than this, so the
    // screen clears between repeats.
    const uint32_t copy_blank_ms = 10000;

    // Presence timeout: the fox streams keystate every 30 ms and an Ident in the
    // message gap, so a few seconds of total silence means the station is gone.
    // Clear the RECV id and RSSI bar so a stale reading doesn't linger on screen.
    const uint32_t signal_timeout_ms = 3000;

    // On-air keying speeds, seeded from our config and retuned by the fox's
    // Ident packet so the decoder and watchdog track the actual sender.
    static uint8_t  rx_wpm      = config::wpm();
    static uint8_t  rx_char_wpm = config::char_wpm();

    // Text-frame render (field note §7, docs/plan-text-message-mode.md): when the
    // fox sends the clue as a TextMsg instead of edges, the hunter renders Morse
    // LOCALLY — a morse::Player turns the received text into key-down/up segments
    // that drive the sidetone, while the decoded text is shown verbatim. `text_seq`
    // dedups the retransmit burst's redundant copies (-1 = none seen yet).
    static morse::Player text_player;
    static bool          text_playing  = false;
    static int           last_text_seq = -1;
    static bool          text_prev_down = false;  // for render key-down edge count
    static uint16_t      text_elems     = 0;      // key-down elements rendered (debug)
    static char          rendering_text[97] = ""; // clue the player is sounding now
    // Progressive-reveal cursor (field note §7): the clue is NOT dumped to the
    // copy buffers on receipt. Instead each character is surfaced as the local
    // player sounds it, so the display builds up in lock-step with the audio and
    // visibly wipes-then-rebuilds on every message repeat (a clear "new message"
    // cue for learners). text_reveal_pos = next char in rendering_text to show;
    // text_reveal_need = cumulative key-down elements that must have sounded
    // before that char is due. Both reset when a new render (re)starts.
    static size_t        text_reveal_pos  = 0;
    static uint16_t      text_reveal_need = 0;

    // Sync/DF beacon slaving (field note §8, docs/plan-text-sync-beacon.md). The
    // fox emits a MAGIC_SYNC beacon every BEACON_MS carrying the live render
    // position (pos_ms) of the clue currently sounding. Each hunter free-runs its
    // local text_player between beacons (smooth audio) and only hard-seeks when its
    // position drifts past RESYNC_SLACK_MS — so every hunter in earshot beeps in
    // unison. want_text_seq = a clue seq a beacon announced before we had its text
    // (mid-join / lost burst); last_beacon_pos = the position to seek to once that
    // text arrives, so we join the in-progress render in lock-step (-1/POS_IDLE =
    // none pending).
    static int           want_text_seq   = -1;
    static uint16_t      last_beacon_pos  = proto::POS_IDLE;

    // Signal-loss watchdog (T_silence) — edge keying (docs/edge-events.md "Loss
    // handling"): heartbeats arrive every HEARTBEAT_MS even while the key sits
    // steady, so 2.5x that bounds T_silence above any legit gap between heartbeats
    // (a fixed 3-dah-unit rule would be ~414 ms at 13 WPM and false-trip mid-dah)
    // while staying under the 3 s presence timeout below. If we hear nothing for
    // this long, force key-up so the sidetone doesn't latch and the decoder hangs.
    uint32_t rx_timeout_ms = (uint32_t)(2.5 * proto::HEARTBEAT_MS);

    // USER/PRG tap cycles the sidetone volume (MUTE -> LOW -> MED -> HIGH,
    // persisted; mute-only on a PAM8403). The decoded-vs-dit/dah copy view is now
    // a console command (`showtext`) rather than a button toggle.
    if (prg_tapped(now)) hunter_cycle_volume();

    uint8_t buf[128];   // large enough for a control packet (4 + up to 100 bytes)
    size_t n;
    float r;
    if (radio::poll(buf, sizeof(buf), n, r)) {
        proto::Ident     id;
        proto::EdgeEvent ev;
        proto::TextMsg   tm;
        proto::Sync      sy;
        if (broadcast_rx_try(buf, n, now)) {
            // Instructor broadcast banner: painted inside broadcast_rx_try. Count
            // it as signal so the presence bar doesn't blink. No ack/re-arm needed
            // (fire-and-forget); the radio stays in RX.
            last_signal = now;
        } else if (control_rx_try(buf, n, now)) {
            // Instructor remote control: applied + acked inside control_rx_try.
            // Count it as signal so the presence bar doesn't blink during a
            // run of control packets. The radio is re-armed by control_rx_try.
            last_signal = now;
        } else if (proto::decode_ident(buf, n, id) && id.wpm && id.char_wpm &&
                   fox_lock(id.station_id, locked_fox, last_fox_rx, now)) {
            // Retune the decoder to the fox's announced timing (only on change).
            // Gated by fox_lock so we never retune to a competing fox's speed.
            if (id.wpm != rx_wpm || id.char_wpm != rx_char_wpm) {
                rx_wpm = id.wpm;
                rx_char_wpm = id.char_wpm;
                decoder.begin(id.wpm, id.char_wpm);
            }
            rx_station_id = id.station_id;
            last_signal = now;
            if (g_debug) {
                Serial.printf("RX I %u wpm=%u cwpm=%u call=%s\n",
                              id.station_id, id.wpm, id.char_wpm, id.call);
            }
        } else if (proto::decode_edge(buf, n, ev) &&
                   fox_lock(ev.station_id, locked_fox, last_fox_rx, now)) {
            // EdgeEvent (E4, docs/edge-events.md "Bilingual RX"): the packet's
            // flags level is the level being ENTERED, so the segment that JUST
            // ENDED (dur_now_ms) had the OPPOSITE level, and the one before
            // that (dur_prev_ms, the self-heal copy) had `level_down`'s level.
            rx_is_edge = true;
            rx_is_text = false;
            bool level_down = (ev.flags & proto::EDGE_FLAG_DOWN) != 0;
            bool hb         = (ev.flags & proto::EDGE_FLAG_HEARTBEAT) != 0;

            // Presence/level/sidetone update on EVERY edge packet, heartbeat or
            // not — that's the whole point of the heartbeat (presence + level
            // re-anchor without waiting on a real edge).
            rx_down = level_down;
            last_rx = now;
            last_signal = now;
            display::activity();   // inbound keying keeps the hunter's panel awake
            rssi = r;
            rssi_valid = true;
            rx_station_id = ev.station_id;

            if (g_debug) {
                Serial.printf("RX E %u seq=%u lvl=%u hb=%u dn=%u dp=%u rssi=%d\n",
                              ev.station_id, ev.seq, level_down ? 1 : 0,
                              hb ? 1 : 0, ev.dur_now_ms, ev.dur_prev_ms, (int)r);
            }

            if (!hb) {
                // Loss handling WITHOUT guessing (docs/edge-events.md): we never
                // reconstruct a lost edge from dur_prev_ms — a confident wrong
                // letter confuses a first-time learner more than an honest '?'.
                // A gap of 1 is the normal case (strictly incrementing real-edge
                // seq); 0 is a duplicate; ANY gap >= 2 means one or more edges
                // were lost, so the character spanning the loss is ambiguous:
                // mark it '?' and re-anchor on the current segment. (dur_prev_ms
                // is no longer consumed on RX — see protocol.h.)
                if (reanchor || last_edge_seq < 0) {
                    decoder.feed_segment(!level_down, ev.dur_now_ms);  // anchor
                    reanchor = false;
                } else {
                    uint8_t gap = (uint8_t)((uint8_t)ev.seq - (uint8_t)last_edge_seq);
                    if (gap == 0) {
                        // duplicate edge: skip feeding
                    } else if (gap == 1) {
                        decoder.feed_segment(!level_down, ev.dur_now_ms);
                    } else {
                        // gap >= 2: edge(s) lost. Poison the character the loss
                        // falls within so it resolves to a single '?' at its next
                        // char-gap (preserving the word's letter count) instead of
                        // a truncated element-run decoding as a confident wrong
                        // letter (e.g. a dropped dit turning "..."/S into ".."/I).
                        decoder.poison();
                        decoder.feed_segment(!level_down, ev.dur_now_ms);
                    }
                }
                last_edge_seq = ev.seq;
            }
            // Heartbeat: presence/level already updated above; do NOT decode,
            // do NOT touch last_edge_seq (it doesn't own a seq — see tx_edge §0).
        } else if (proto::decode_text(buf, n, tm) &&
                   fox_lock(tm.station_id, locked_fox, last_fox_rx, now)) {
            // Canned clue as a text frame (field note §7, docs/plan-text-message-
            // mode.md): the whole clue arrived as plaintext, so there is nothing to
            // decode and no '?' holes. Dedup the retransmit burst by seq; on a fresh
            // clue, render Morse LOCALLY (a morse::Player drives the sidetone at the
            // fox's announced timing) and show the text verbatim.
            rx_station_id = tm.station_id;
            last_signal = now;
            last_rx = now;
            display::activity();   // inbound clue keeps the panel awake
            rssi = r;
            rssi_valid = true;
            // Text mode owns the copy buffers now; keep the edge/compat decoder
            // out of them and wipe any half-built char it was holding so it can't
            // flush a phantom '?' into the clue (begin() resets without emitting).
            rx_is_edge = false;
            rx_is_text = true;
            decoder.begin(rx_wpm, rx_char_wpm);
            bool fresh = (last_text_seq < 0) || ((uint8_t)tm.seq != (uint8_t)last_text_seq);
            if (fresh) {
                last_text_seq = tm.seq;
                last_code = now;
                Serial.println(tm.text);
                // Record for `show` (validatable over serial OR BLE).
                strncpy(g_rx_text, tm.text, sizeof(g_rx_text) - 1);
                g_rx_text[sizeof(g_rx_text) - 1] = '\0';
                g_rx_text_seq = tm.seq;
                g_rx_text_count++;
                // Render the clue audibly at the fox's announced speed. The fox
                // re-sends the SAME clue (new seq) every REPEAT_PAUSE, which can be
                // SHORTER than the clue's render time — so don't restart a render
                // that's already sounding this exact text (that would stutter and
                // never complete). Re-render only when the player is free, or
                // immediately if the clue text actually changed.
                bool changed = strncmp(tm.text, rendering_text,
                                       sizeof(rendering_text)) != 0;
                if (!text_playing || changed) {
                    // A genuinely new render (not just a redundant burst copy):
                    // wipe the copy buffers and rewind the progressive-reveal
                    // cursor so the display visibly clears and rebuilds in step
                    // with the player below. NOT done on every fresh seq — at slow
                    // speeds a new-seq burst arrives every REPEAT_PAUSE while the
                    // SAME clue is still sounding; rewinding then would erase the
                    // partially-revealed line each cycle.
                    text_buf[0] = '\0';   text_len = 0;
                    ditdah_buf[0] = '\0'; ditdah_len = 0;
                    text_reveal_pos = 0;  text_reveal_need = 0;
                    strncpy(rendering_text, tm.text, sizeof(rendering_text) - 1);
                    rendering_text[sizeof(rendering_text) - 1] = '\0';
                    text_player.begin(rx_wpm, rx_char_wpm);
                    text_player.start(tm.text);
                    text_playing = true;
                    text_prev_down = false; text_elems = 0;
                    // Mid-join (field note §8): if a beacon already announced this
                    // seq while we lacked the text, we're joining a render that's
                    // been sounding on the other hunters for last_beacon_pos ms.
                    // Seek straight to that position so we land in lock-step instead
                    // of beeping the start of the clue mid-word. The progressive
                    // reveal still counts from the join point (we only surface the
                    // tail we actually sounded) — a late joiner sees a partial line,
                    // which is honest. Normal cycle-top renders have want_text_seq
                    // for the PREVIOUS seq (or -1), so they start at pos 0 as before.
                    if (want_text_seq >= 0 &&
                        (uint8_t)want_text_seq == (uint8_t)tm.seq &&
                        last_beacon_pos != proto::POS_IDLE) {
                        text_player.resync(millis(), last_beacon_pos);
                        if (g_debug)
                            Serial.printf("RX T mid-join seq=%u seek=%u\n",
                                          tm.seq, last_beacon_pos);
                    }
                    want_text_seq = -1;
                }
                if (g_debug)
                    Serial.printf("RX T %u seq=%u text=%s\n",
                                  tm.station_id, tm.seq, tm.text);
            }
            // A duplicate seq (the burst's redundant copies) is silently dropped;
            // the player keeps rendering the copy already in flight.
        } else if (proto::decode_sync(buf, n, sy) &&
                   fox_lock(sy.station_id, locked_fox, last_fox_rx, now)) {
            // Sync/DF beacon (field note §8, docs/plan-text-sync-beacon.md). Two
            // jobs in one packet: (1) keep the DF/presence bar alive — a beacon
            // lands every <=250 ms across the whole fox loop, render AND pause,
            // unlike the bursty TextMsg; (2) slave the local render to the fox's
            // master clock so all hunters beep in unison.
            rx_station_id = sy.station_id;
            last_signal = now;
            last_rx = now;
            display::activity();
            rssi = r;
            rssi_valid = true;

            // Adopt the beacon's timing if it changed (lets a mid-joiner that
            // missed the Ident still render). Don't disturb a render already in
            // flight — the Ident leads each cycle, so wpm is normally set before
            // the clue starts; this only catches a station that came up on a sync.
            if (sy.wpm && sy.char_wpm &&
                (sy.wpm != rx_wpm || sy.char_wpm != rx_char_wpm)) {
                rx_wpm = sy.wpm;
                rx_char_wpm = sy.char_wpm;
                decoder.begin(sy.wpm, sy.char_wpm);
            }

            // RESYNC_SLACK_MS ~ one dit: below this we let the player free-run (no
            // audible stutter); above it we hard-seek. A missed beacon costs
            // nothing — the next one re-aligns within one dit.
            const uint32_t RESYNC_SLACK_MS = morse::unit_ms(rx_wpm);

            if (sy.pos_ms == proto::POS_IDLE) {
                // Fox is between clues: presence/DF only, no render action.
            } else if (text_playing &&
                       (uint8_t)sy.seq == (uint8_t)last_text_seq) {
                // We're rendering this very clue: slave to the fox's position.
                int32_t drift = (int32_t)text_player.position_ms() -
                                (int32_t)sy.pos_ms;
                if (drift < 0) drift = -drift;
                if ((uint32_t)drift > RESYNC_SLACK_MS) {
                    text_player.resync(millis(), sy.pos_ms);
                }
            } else if (!text_playing && last_text_seq >= 0 &&
                       (uint8_t)sy.seq == (uint8_t)last_text_seq) {
                // Already finished this clue's render locally — nothing to do; just
                // hold presence until the next clue's TextMsg arrives.
            } else {
                // A clue is rendering that we have no text for (powered on mid-clue,
                // or the cycle-top TextMsg burst was lost). Remember the seq + live
                // position; the next TextMsg re-send (<= TEXT_RESEND_MS) completes
                // the mid-join and seeks the player here.
                want_text_seq   = sy.seq;
                last_beacon_pos = sy.pos_ms;
            }

            if (g_debug)
                Serial.printf("RX S %u seq=%u pos=%u wpm=%u rssi=%d\n",
                              sy.station_id, sy.seq, sy.pos_ms, sy.wpm, (int)r);
        }
    }

    // No fresh packet for too long → drop to key-up (silent/idle). In edge
    // mode also flush the decoder and request a re-anchor: otherwise the next
    // edge after the gap would be diffed against a stale last_edge_seq and
    // could trigger a phantom self-heal (docs/edge-events.md "Loss handling").
    if (rx_down && (now - last_rx) > rx_timeout_ms) {
        rx_down = false;
        if (rx_is_edge) {
            decoder.flush();   // '?' for a damaged/half-built char, then resync
            reanchor = true;
        }
    }

    // Prolonged silence → the station is gone. Clear the RECV id and RSSI bar so
    // the display shows "RECV ---" with no bar instead of a stale last reading.
    if (rssi_valid && (now - last_signal) > signal_timeout_ms) {
        rssi_valid = false;
        rx_station_id = -1;
    }

    // Fox-lock release: the locked fox's keying has gone silent this long, so
    // drop the lock and let the next fox heard be adopted. Flush the decoder and
    // re-anchor so a half-built character from the departed fox doesn't bleed
    // into the next one. (Instructor control/broadcast traffic does not refresh
    // last_fox_rx, so it can't hold a dead fox's lock open.)
    if (locked_fox >= 0 && (now - last_fox_rx) > signal_timeout_ms) {
        locked_fox = -1;
        if (rx_is_edge) { decoder.flush(); reanchor = true; }
    }

    // No fresh decoded code for 10 s → blank the copy lines (text and dit/dah).
    // Suppressed while a text clue is actively sounding: the player owns the copy
    // line and reveals it progressively (below), so the idle-blank must not erase
    // a line that's still building (last_code only refreshes once per burst).
    if (!text_playing && last_code != 0 && (now - last_code) > copy_blank_ms &&
        (text_len || ditdah_len)) {
        text_buf[0] = '\0';   text_len = 0;
        ditdah_buf[0] = '\0'; ditdah_len = 0;
        last_draw = 0;        // force an immediate redraw of the cleared copy
    }

    // Text-frame render (field note §7): while a received clue is playing out, the
    // LOCAL player owns the sidetone (not rx_down — there are no edges on the air).
    // Keep presence alive across the render so the RSSI/RECV display doesn't blink
    // out during a long clue (the burst itself is over in ~150 ms).
    if (text_playing) {
        // Drive the player off millis() (the same clock start() used) — not the
        // loop's cached `now`, which was sampled BEFORE start() this iteration and
        // would underflow `now - seg_start_` on the first update, finishing the
        // render instantly with no tone.
        text_player.update(millis());
        bool pd = text_player.down();
        if (pd && !text_prev_down) text_elems++;   // count rendered key-down edges
        text_prev_down = pd;

        // Progressive reveal: surface each character once the player has sounded
        // its key-down elements, so the copy line builds up in step with the audio
        // (and stays blank for the first few seconds of every message — the wipe
        // cue). `avail` is the count of key-down edges heard so far; on the final
        // flush (0xFFFF) any trailing char/space is released. ' ' carries no
        // elements, so it appears as soon as its preceding character is revealed.
        auto reveal_to = [&](uint16_t avail) {
            while (rendering_text[text_reveal_pos]) {
                char c = rendering_text[text_reveal_pos];
                if (c == ' ') {
                    text_push(' '); ditdah_push("/ ");
                    text_reveal_pos++;
                    continue;
                }
                const char* p = morse::pattern(c);
                uint16_t need = text_reveal_need + (p ? (uint16_t)strlen(p) : 0);
                if (avail < need) break;          // not sounded yet — wait
                text_push(c);
                if (p) { ditdah_push(p); ditdah_push(" "); }
                text_reveal_need = need;
                text_reveal_pos++;
            }
        };
        reveal_to(text_elems);

        if (text_player.finished()) {
            reveal_to(0xFFFF);              // flush any trailing char/space
            text_playing = false;
            g_rx_text_elems = text_elems;   // surfaced in `show` (serial + BLE)
            // Test observable (gated on g_debug): proves the hunter generated key
            // state locally from the text (elems = dits+dahs sounded).
            if (g_debug) Serial.printf("RX T render done elems=%u\n", text_elems);
        } else {
            last_signal = now; display::activity();
        }
        set_tone(pd);
    } else {
        set_tone(rx_down);
    }

    // Decoder step (docs/edge-events.md "Decode path"): the decode already
    // happened on packet arrival via feed_segment (TX-measured durations, not
    // jittered local timestamps), so here we just drain the queues those calls
    // filled. take_element() drains the dit/dah view.
    //
    // Text mode (§7) is the exception: the copy buffers were filled directly from
    // the received frame, so skip the decoder entirely — running it would only let
    // a stale half-built char surface a phantom '?' over the clean clue.
    if (rx_is_text) {
        // nothing to decode — the clue render owns text_buf/ditdah_buf
    } else if (rx_is_edge) {
        char e;
        while ((e = decoder.take_element())) {
            rolling_append(ditdah_buf, ditdah_len, sizeof(ditdah_buf), &e, 1);
            last_code = now;
            if (g_debug) Serial.printf("EL %d %c\n", rx_station_id, e);
        }
        char c;
        while ((c = decoder.take_char())) {
            last_code = now;
            text_push(c);
            Serial.print(c);
            if (g_debug) Serial.printf("CH %d %c\n", rx_station_id, c);
            // Separate the dit/dah stream: a single space between letters,
            // " / " between words (the word-gap char ' ' arrives after the
            // letter's space).
            if (c == ' ') ditdah_push("/ ");
            else          ditdah_push(" ");
        }
    }

    if (now - last_draw >= 100) {
        last_draw = now;
        if (banner_active(now)) {
            draw_banner();   // banner borrows the glass; copy continues underneath
        } else {
            const char* copy = g_showtext ? text_buf : ditdah_buf;
            display::hunter(copy, radio::frequency_mhz(), rx_station_id, !g_showtext,
                            rssi, rssi_valid, rx_down);
        }
    }
}

// Instructor remote control (docs: instructor station). Drives the command
// staged by the `relay` verb: bursts the control packet over GFSK and listens
// for the target's ack between bursts. Sits idle in RX (hearing nothing) when no
// command is pending, so it can also still be driven over its own BLE/serial.
// Service one inbound packet for the instructor: catch the target's ack (and stop
// a unicast burst the instant it arrives) or note the fox's own traffic so
// silence-sync knows when its RX window opens. Returns true if a packet was read.
// Shared by the top-of-loop poll and the post-burst ack listener.
static bool instructor_service_rx(uint32_t now) {
    uint8_t buf[128];
    size_t n; float r;
    if (!radio::poll(buf, sizeof(buf), n, r)) return false;
    proto::ControlAck a;
    proto::Listen     l;
    if (proto::decode_ack(buf, n, a) && a.target_id == proto::INSTRUCTOR_ID) {
        Serial.printf("ACK %u seq=%u: %s\n", a.src_id, a.seq, a.status);
        // Echo the ack to a connected phone over BLE too — without this it only
        // ever reached USB serial, so a field operator driving the instructor
        // from a phone had no confirmation a command was received.
        char bleline[48];
        snprintf(bleline, sizeof(bleline), "ACK %u seq=%u: %s",
                 a.src_id, a.seq, a.status);
        ble_provision::notify(bleline);
        // Latch the ack on the panel so the 1 Hz idle redraw can't wipe it back
        // to "ready" before the operator reads it (see g_ack_show_until).
        snprintf(g_ack_l1, sizeof(g_ack_l1), "ACK id %u", a.src_id);
        snprintf(g_ack_l2, sizeof(g_ack_l2), "%s", a.status);
        g_ack_show_until = now + ACK_STICKY_MS;
        display::instructor(PWR_LEVELS[pwr_idx].label, g_ack_l1, g_ack_l2);
        last_draw = now;
        if (g_ctrl.active && a.seq == g_ctrl.seq) {
            // Tally the responder; stop a unicast burst once its target acks.
            bool known = false;
            for (uint8_t i = 0; i < g_ctrl.n_acks; ++i)
                if (g_ctrl.acks_from[i] == a.src_id) known = true;
            if (!known && g_ctrl.n_acks < sizeof(g_ctrl.acks_from))
                g_ctrl.acks_from[g_ctrl.n_acks++] = a.src_id;
            if (g_ctrl.target_id != proto::BROADCAST_ID) {
                g_ctrl.delivered = true;
                g_ctrl.active = false;     // unicast done
            }
        }
    } else if (proto::decode_listen(buf, n, l) &&
               ((g_ctrl.active && l.station_id == g_ctrl.target_id) ||
                g_bcast.active)) {
        // A fox announced its RX window — burst NOW, no CTRL_SILENCE_MS wait. For a
        // relay (g_ctrl) only the target fox's beacon counts; for an alert campaign
        // (g_bcast, all-stations §6) ANY fox's beacon does, so the alert lands in
        // its listen window. g_fox_heard so the sync path is taken even if we caught
        // the beacon before any heartbeat.
        g_fox_heard        = true;
        g_fox_listening    = true;
        g_fox_listen_until = now + l.window_ms;
    } else if (g_ctrl.active && n >= 2 && buf[1] == g_ctrl.target_id &&
               (buf[0] == proto::MAGIC_IDENT || buf[0] == proto::MAGIC_EDGE ||
                buf[0] == proto::MAGIC_TEXT)) {
        // Heard the target fox transmitting — it is alive and NOT in its RX
        // window right now. Track the time so a following gap reveals the window.
        g_fox_heard = true;
        g_fox_last_heard = now;
    }
    return true;
}

static void loop_instructor(uint32_t now) {
    // Re-sample the clock: `now` was captured at the top of loop(), but a `relay`
    // command is staged later in that SAME iteration (serial_console_process runs
    // before this dispatch and writes g_ctrl.start_ms = millis(), after an NVS
    // seq write). Using the stale `now` makes `now - start_ms` underflow on the
    // first pass → the burst window looks elapsed and the command gives up
    // instantly. A fresh sample keeps all the CTRL_* timers self-consistent.
    now = millis();
    // PRG button wakes the screen only (no power cycling in instructor mode).
    if (prg_tapped(now)) display::activity();
    // Reset silence-sync when a new command (seq) starts; g_fox_heard/_last_heard
    // are file scope (see above) so the post-burst listener shares them.
    static int synced_seq = -1;
    if (g_ctrl.active && synced_seq != (int)g_ctrl.seq) {
        synced_seq = g_ctrl.seq;
        g_fox_heard = false;
        g_fox_listening = false;
    }

    // Always service inbound packets: acks (our confirmation) and the target fox's
    // own traffic (silence tracking).
    instructor_service_rx(now);

    // Broadcast banner / alert campaign (docs/plan-instructor-broadcast.md, §6):
    // fire-and-forget, independent of the relay/ack path. Runs until g_bcast.until,
    // bursting the same seq every BCAST_INTERVAL (prompt for always-listening
    // hunters) AND immediately whenever a fox announces its RX window via a Listen
    // beacon (so the alert/halt — and its `alert clear` resume — reliably reaches a
    // Fox, which only listens in its inter-message pause). Takes priority over the
    // relay/idle draw while active.
    if (g_bcast.active) {
        bool listening = g_fox_listening && (int32_t)(g_fox_listen_until - now) > 0;
        bool due       = (now - g_bcast.last_tx >= BCAST_INTERVAL);
        if (listening || due) {
            g_bcast.last_tx = now;
            if (listening) g_fox_listening = false;   // consume: one burst per window
            uint8_t bbuf[proto::BCAST_HDR_LEN + proto::BCAST_TEXT_MAX];
            size_t  blen = proto::encode_bcast(proto::INSTRUCTOR_ID, g_bcast.seq,
                                               g_bcast.flags, g_bcast.text, bbuf);
            radio::send(bbuf, blen);
            radio::start_receive();
            char l1[24];
            snprintf(l1, sizeof(l1), "ALERT seq %u", g_bcast.seq);
            display::instructor(PWR_LEVELS[pwr_idx].label, l1, g_bcast.text, true);
            last_draw = now;
            if (g_debug)
                Serial.printf("TX B seq=%u %s text=%s\n", g_bcast.seq,
                              listening ? "sync" : "probe", g_bcast.text);
        }
        if ((int32_t)(now - g_bcast.until) >= 0) {   // campaign window elapsed
            g_bcast.active  = false;
            g_fox_listening = false;
        }
        return;
    }

    if (!g_ctrl.active) {
        // While the instructor's own banner echo is up, redraw fast enough to
        // animate a scrolling marquee (100 ms); otherwise the idle status is a
        // sparse 1 Hz redraw.
        uint32_t interval = banner_active(now) ? 100 : 1000;
        if (now - last_draw >= interval) {
            last_draw = now;
            if (banner_active(now)) draw_banner();   // its own banner echo still up
            else if ((int32_t)(g_ack_show_until - now) > 0)
                // Keep the most recent ack on screen for ACK_STICKY_MS before
                // reverting to the idle prompt.
                display::instructor(PWR_LEVELS[pwr_idx].label, g_ack_l1, g_ack_l2);
            else display::instructor(PWR_LEVELS[pwr_idx].label, "ready",
                                     "relay <id> <cmd>");
        }
        return;
    }

    // Burst window elapsed without (enough) acks — give up on this command.
    if (now - g_ctrl.start_ms >= CTRL_BURST_WINDOW) {
        g_ctrl.active = false;
        char l2[24];
        snprintf(l2, sizeof(l2), "%u ack(s)", g_ctrl.n_acks);
        display::instructor(PWR_LEVELS[pwr_idx].label,
                            g_ctrl.n_acks ? "done" : "no response", l2);
        Serial.printf("# relay seq=%u finished: %u ack(s)\n",
                      g_ctrl.seq, g_ctrl.n_acks);
        return;
    }

    // Decide whether to burst now. Unicast, target heard: only while the fox is
    // silent (in its RX window), rate-limited so we don't machine-gun the window.
    // Otherwise (broadcast, or target not yet heard): a sparse periodic probe that
    // keeps the channel mostly clear so we can still detect the fox / reach all.
    bool sync = g_ctrl.target_id != proto::BROADCAST_ID && g_fox_heard;
    // Explicit window: the fox's Listen beacon said it is listening until
    // g_fox_listen_until. Signed compare so it reads false once that passes.
    bool listening = g_fox_listening && (int32_t)(g_fox_listen_until - now) > 0;
    bool burst_now;
    if (sync) {
        // Beacon present → burst at once; else fall back to inferring the window
        // from CTRL_SILENCE_MS of silence. Both still rate-limited per burst.
        burst_now = (listening || (now - g_fox_last_heard >= CTRL_SILENCE_MS)) &&
                    (now - g_ctrl.last_tx >= CTRL_BURST_INTERVAL);
    } else {
        burst_now = (now - g_ctrl.last_tx >= CTRL_PROBE_INTERVAL);
    }

    if (burst_now) {
        g_ctrl.last_tx = now;
        uint8_t cbuf[proto::CTRL_HDR_LEN + proto::CTRL_CMD_MAX];
        size_t clen = proto::encode_ctrl(proto::INSTRUCTOR_ID, g_ctrl.target_id,
                                         g_ctrl.seq, g_ctrl.cmd, cbuf);
        radio::send(cbuf, clen);
        radio::start_receive();
        // The target acks the instant it decodes this burst — on a reliable link
        // that ack lands in the tens of ms right after, exactly while we're
        // settling back into RX. Poll a short blocking window now so a lockstep
        // ack isn't missed (the old failure: every ack fell in this blind spot, so
        // the instructor never saw "delivered" and bursted the whole 90 s window).
        // Exits early the moment instructor_service_rx() clears g_ctrl.active.
        uint32_t t0 = millis();
        while (g_ctrl.active && (uint32_t)(millis() - t0) < CTRL_ACK_LISTEN_MS)
            instructor_service_rx(millis());
        if (g_ctrl.active && now - last_draw >= 500) {   // not if the ack just landed
            last_draw = now;
            char l1[24], l2[24];
            snprintf(l1, sizeof(l1), "-> id %u seq %u", g_ctrl.target_id, g_ctrl.seq);
            snprintf(l2, sizeof(l2), "%us %u ack %s", (now - g_ctrl.start_ms) / 1000,
                     g_ctrl.n_acks, sync ? "sync" : "probe");
            display::instructor(PWR_LEVELS[pwr_idx].label, l1, l2, true);
        }
    }
}

void loop() {
    uint32_t now = millis();
    // Pet the hardware watchdog once per pass. If loop() ever wedges (a hung
    // radio call, a runaway handler) this stops happening and the node reboots
    // itself — the field-resiliency contract. Armed at the end of setup().
    platform::watchdog_feed();
    // Dispatch any BLE provisioning commands on this (main) task, so the parser
    // never races the loop's config reads. Cheap when idle.
    ble_provision::process();
    // Same parser over USB serial, so a terminal can drive a running station
    // during development (parity with the BLE console). Non-blocking.
    serial_console_process();
#ifdef DEVICE_CARDPUTER_ADV
    // 'm' toggles the sidetone mute — the Cardputer has no volume pot, so this
    // is the local "quiet it now" control for a node sitting near people. The
    // keyboard was brought up in config_ui::run() at boot.
    char kc;
    if (keyboard::read_char(kc)) {
        // A press while blanked is a wake-only press: light the panel and
        // swallow it so it doesn't also toggle mute. (See prg_tapped().)
        bool was_blanked = display::blanked();
        display::activity();   // any keyboard press wakes the panel
        if (!was_blanked && banner_active(now) && !g_alert_latched) {
            g_banner_until = 0;   // dismiss the banner; swallow the key
        } else if (!was_blanked && g_alert_latched) {
            /* §6 latched alert: swallow the key, keep the banner up */
        } else if (!was_blanked && (kc == 'm' || kc == 'M')) {
            bool m = !config::muted();
            apply_mute(m);
            Serial.printf("# mute %s (key)\n", m ? "on" : "off");
        }
    }
#endif
    // §6: keep the panel awake while a sticky alert is latched so the 60 s idle-
    // blank never hides it (display::activity() just stamps a millis — cheap).
    if (g_alert_latched) display::activity();
    switch (mode) {
        case MODE_FOX:        loop_fox(now);        break;
        case MODE_LIVEKEY:    loop_livekey(now);    break;
        case MODE_HUNTER:     loop_hunter(now);     break;
        case MODE_INSTRUCTOR: loop_instructor(now); break;
        case MODE_HIBERNATE:  break;   // never reached (slept in setup)
    }
    // End the instructor attention tone once its deadline passes (non-blocking;
    // every mode can receive an alert, so tick it here regardless of mode).
    alert_tone_tick(now);
    // Battery saver: power the panel down after the idle timeout. Wake sources
    // (button, serial/BLE command, hunter RX keying) call display::activity();
    // this only acts once the timeout elapses with none of them firing.
    display::tick(now);

    // AUTO policy: couple the BLE-UART core to the panel. A node whose screen is
    // lit is one someone is interacting with (button, console command, hunter RX
    // keying all call display::activity), so keep it reachable over the air; once
    // the panel idle-blanks, drop the 2.4 GHz core too — ~70 mA on the V4, the
    // dominant idle draw. Re-wake needs no BLE: any source that relights the panel
    // brings BLE back. apply_ble is idempotent, so this level-follow is cheap and
    // self-syncs the moment the policy returns to AUTO. ON/OFF pin the core and
    // skip this entirely (use `ble on` to reach an idle, blanked node).
    //
    // Instructors are pinned BLE_ON in setup() so this AUTO branch never runs for
    // them; the explicit mode guard is belt-and-suspenders making that intent
    // local and obvious here — an instructor must stay reachable through a blank.
    if (g_ble_mode == BLE_AUTO && mode != MODE_INSTRUCTOR) {
        bool want = !display::blanked();
        if (g_debug && want != g_ble_on)
            Serial.printf("# ble: AUTO panel %s -> %s\n",
                          want ? "lit" : "blanked", want ? "up" : "down");
        apply_ble(want);
    }
}
