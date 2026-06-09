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

// Stage 6 — integrated fox-hunt firmware.
//
// Boot menu (PRG/BOOT button): short press cycles the highlighted mode, a long
// press (>=600 ms) selects it; auto-selects after 8 s. Modes:
//   HUNTER  — receive keystate, play Morse sidetone, show decoded text + RSSI.
//   FOX     — loop the canned location message, transmit keystate every 30 ms.
//   LIVEKEY — transmit a live telegraph key, play local sidetone.
//
// One protocol (proto::KeyState) carries both FOX and LIVEKEY to any HUNTER.

static constexpr uint32_t TONE_HZ      = 750;
// Gap after a full message before it repeats; long enough for a hunter to
// register the end-of-message before the loop restarts.
// Longer than the hunter's 10 s copy-blanking window (see loop_hunter) so each
// message clears from the hunter's screen before the next repeat begins.
static constexpr uint32_t REPEAT_PAUSE = 12000;

// Callsign and fox message live in NVS (config), set via the boot serial console
// (see run_setup_console). The callsign is sent in the periodic station-ID
// packet; include it in the fox message text too for an audible CW ID.

enum Mode { MODE_HUNTER = 0, MODE_FOX = 1, MODE_LIVEKEY = 2, MODE_HIBERNATE = 3 };
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
static uint32_t last_tx = 0;

// Runtime keying-protocol toggle (E3): "compat" streams KeyState every
// TX_INTERVAL_MS (current behavior, the default — no change unless flipped);
// "edge" emits EdgeEvents only on key transitions plus an idle heartbeat. Not
// yet NVS-persisted (E5) — a fresh boot always starts in compat.
enum KeyMode { KEYMODE_COMPAT = 0, KEYMODE_EDGE = 1 };
static uint8_t g_keymode = KEYMODE_COMPAT;
static uint32_t last_ident = 0;

// Unattended-test instrumentation (E4, docs/edge-events.md "Unattended test
// instrumentation"): when on, dumps one parseable line per RX/TX/decode event
// to Serial so a harness can assert on decoded text without sidetone/display.
// Off by default — keeps normal operation quiet. Runtime-only (not persisted).
static bool g_debug = false;
static uint32_t pause_until = 0;
static uint32_t last_draw = 0;

// Rolling decoded-text buffer for the hunter view.
static char text_buf[128];
static size_t text_len = 0;

// Raw dit/dah element stream for the hunter's learning-aid display mode, and the
// callsign learned from the last Ident packet ("----" until one is heard).
static char ditdah_buf[64];
static size_t ditdah_len = 0;
static int  rx_station_id = -1;      // station id from last packet, -1 = none
static bool hunter_ditdah = true;    // false = letters, true = dit/dah (default)

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

// Dispatch one provisioning command line, writing responses to `out`. Pure of
// any transport: `out` is a Print& so the serial REPL (Serial) and a future
// BLE-UART session can share this parser. Returns true when the session should
// end (the "done"/"exit" command). See docs/commands.md.
static bool handle_setup_command(const char* line, Print& out) {
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
    } else if (!strcmp(line, "model") || !strncmp(line, "model ", 6)) {
        // Board model identifier (admin). Persisted in NVS per physical unit, so
        // the same firmware can be flashed to every board and each retains its
        // identity across reflashes. `model` / `model show` reports; `model
        // <name>` pins a specific model (e.g. heltec-v4.2 vs heltec-v4.3, which
        // are NOT runtime-detectable — no version strap); `model default`
        // reverts to the compile-time platform name. Always printed with the
        // hardware chip id so the model can't be wrong relative to the unit.
        const char* arg = line + 5;
        while (*arg == ' ') arg++;
        if (!*arg || !strcmp(arg, "show")) {
            // report only
        } else if (!strcmp(arg, "default") || !strcmp(arg, "clear")) {
            config::set_board_model("");
        } else {
            config::set_board_model(arg);
        }
        bool dflt = !strcmp(config::board_model(), config::default_board_model());
        out.printf("  model    = %s%s  chip=%s  soc=%s\n",
                   config::board_model(), dflt ? " (default)" : "",
                   platform::chip_id_str(), platform::soc_str());
    } else if (!strcmp(line, "reboot") || !strcmp(line, "restart")) {
        // Remote reboot: the boot menu auto-selects the stored boot_mode after
        // its idle timeout, so this applies a "mode <n>" change with no physical
        // interaction. Give the BLE notify a moment to flush before resetting.
        out.println("# rebooting");
        delay(150);
        platform::restart();
    } else if (!strncmp(line, "mode ", 5)) {
        int m = atoi(line + 5);
        if (m < 0 || m > 2) {            // 0=Hunter 1=Fox 2=Livekey (not Hibernate)
            out.println("  ? mode 0=Hunter 1=Fox 2=Livekey");
        } else {
            config::set_boot_mode((uint8_t)m);
            static const char* names[] = {"Hunter", "Fox", "Livekey"};
            out.printf("  boot mode = %d (%s)\n", m, names[m]);
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
    } else if (!strcmp(line, "keymode") || !strcmp(line, "keymode show") ||
               !strncmp(line, "keymode ", 8)) {
        // Runtime toggle between the compat KeyState stream (default — no
        // behavior change) and EdgeEvent emission (E3). NVS-persisted (E5) and
        // restored into g_keymode at boot, so a fox flipped to edge over the air
        // comes back up in edge mode. See docs/edge-events.md.
        const char* arg = line + 7;
        while (*arg == ' ') arg++;
        if (!*arg || !strcmp(arg, "show")) {
            // report only
        } else if (!strcmp(arg, "compat")) {
            g_keymode = KEYMODE_COMPAT;
            config::set_keymode(g_keymode);
        } else if (!strcmp(arg, "edge")) {
            g_keymode = KEYMODE_EDGE;
            config::set_keymode(g_keymode);
        } else {
            out.println("  ? keymode <compat|edge>");
            return false;
        }
        out.printf("  keymode  = %s\n", g_keymode == KEYMODE_EDGE ? "edge" : "compat");
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
    } else if (!strcmp(line, "show")) {
        static const char* mnames[] = {"Hunter", "Fox", "Livekey", "Hibernate"};
        uint8_t bm = config::boot_mode();
        const char* mn = (bm < 4) ? mnames[bm] : "?";
        out.printf("  id=%u call=%s wpm=%u farns=%u vol=%u mute=%s mode=%s "
                   "keymode=%s msg=\"%s\"\n",
                   config::station_id(), config::callsign(),
                   config::wpm(), config::char_wpm(), config::volume(),
                   config::muted() ? "on" : "off", mn,
                   g_keymode == KEYMODE_EDGE ? "edge" : "compat",
                   config::fox_message());
        bool dflt = !strcmp(config::board_model(), config::default_board_model());
        out.printf("  model    = %s%s  chip=%s  soc=%s\n",
                   config::board_model(), dflt ? " (default)" : "",
                   platform::chip_id_str(), platform::soc_str());
        out.printf("  debug    = %s\n", g_debug ? "on" : "off");
    } else if (!strcmp(line, "batt")) {
        // Raw battery readout — disambiguates a 0% meter (no cell / flat vs a
        // scaling problem). millivolts() is the unsmoothed terminal voltage.
        int mv  = battery::millivolts();
        int pct = battery::percent();
        out.printf("  batt %d mV  %d%%  charging=%s\n",
                   mv, pct, battery::charging() ? "yes" : "no");
    } else if (!strcmp(line, "bootlog")) {
        // Dump the boot/crash reason ring — diagnoses crashes after the fact when
        // no serial was attached at reset time (e.g. the native-USB panic loop).
        bootlog_dump(out);
    } else if (!strcmp(line, "bootlog clear")) {
        bootlog_clear();
        out.println("# bootlog cleared");
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
                    "farns <n> | pwr <0..3> | pa <on|off> | mode <0..2> | "
                    "vol <1..32> | mute [on|off] | keymode <compat|edge> | "
                    "model [<name>|default] | debug [on|off] | show | "
                    "bootlog [clear] | reboot | done");
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
                                   "Hibernate"};
    const int n = 4;
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
#ifdef HAS_FEM
    pinMode(PIN_FEM_VFEM, OUTPUT);  digitalWrite(PIN_FEM_VFEM, LOW);   // FEM off
    pinMode(PIN_VEXT_CTRL, OUTPUT); digitalWrite(PIN_VEXT_CTRL, HIGH); // peripheral rail off
#endif
    platform::system_off();         // only RST wakes us -> full restart
}

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 500) { delay(10); }

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
        int prev = 0;
        uint32_t bc = bootlog_record(r, prev);
        Serial.printf("# boot #%u reason now=%d(%s) prev=%d(%s)\n",
                      bc, r, reset_reason_label(r), prev, reset_reason_label(prev));
    }

    config::begin();
    // Restore the persisted keying mode (compat/edge) so a fox flipped to edge
    // over the air stays in edge across reboots. Default is compat.
    g_keymode = config::keymode() ? KEYMODE_EDGE : KEYMODE_COMPAT;
    // Restore the last-used fox TX power level (clamp in case the table shrank).
    pwr_idx = config::fox_pwr_idx();
    if (pwr_idx >= N_PWR) pwr_idx = 0;
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
        case MODE_HIBERNATE:
            break;   // handled before radio init; never reached
    }

    // Radio and run mode are up — let BLE provisioning apply changes live.
    g_live_apply = true;
}

// Send the current key state as a packet on the 30 ms cadence.
static void tx_keystate(uint32_t now, bool down) {
    if (now - last_tx < proto::TX_INTERVAL_MS) return;
    last_tx = now;
    proto::KeyState ks{proto::MAGIC, config::station_id(), (uint8_t)down, seq++};
    uint8_t buf[proto::PACKET_LEN];
    proto::encode(ks, buf);
    radio::send(buf, proto::PACKET_LEN);
}

// Edge-event TX (E3, docs/edge-events.md): send a packet only when the key
// changes level, carrying the *measured* duration of the segment that just
// ended plus the one before it (self-heals a single lost packet), and an idle
// heartbeat re-asserting the current level so a receiver has presence/re-anchor
// even when the key sits still for a long gap. Mirrors tx_keystate's role in
// the compat path but is timed by the key, not a fixed cadence.
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

// Send a station-ID packet now (callsign + keying speeds). A rare, ~ms
// transmission, so it doesn't perturb the 30 ms keystate stream.
static void send_ident(uint32_t now) {
    last_ident = now;
    uint8_t buf[proto::IDENT_LEN];
    proto::encode_ident(config::station_id(), config::callsign(),
                        config::wpm(), config::char_wpm(), buf);
    radio::send(buf, proto::IDENT_LEN);
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
    if (edge) last = now;
    was_down = down;
    return edge;
}

static void loop_fox(uint32_t now) {
    if (prg_tapped(now)) {
        pwr_idx = (pwr_idx + 1) % N_PWR;
        radio::set_tx_power(PWR_LEVELS[pwr_idx].dbm);
        config::set_fox_pwr_idx((uint8_t)pwr_idx);
        last_draw = 0;   // force an immediate redraw of the new level
    }

    if (!player.finished()) {
        player.update(now);
        if (player.finished()) pause_until = now + REPEAT_PAUSE;
    } else if (now >= pause_until) {
        send_ident(now);                     // announce timing at each loop top
        player.start(config::fox_message());
    }
    bool down = player.down();
    // Fox runs SILENT: a transmitter that beeps is easy to find by ear, which
    // defeats the hunt. The fox only keys the radio; hunters hear the Morse from
    // the received keystate. (The display still shows "*" as a keying indicator.)
    set_tone(false);
    // keymode gate (E3): "edge" emits EdgeEvents on transitions + heartbeat;
    // "compat" (default) keeps the existing 30 ms KeyState stream untouched.
    if (g_keymode == KEYMODE_EDGE) tx_edge(now, down);
    else                           tx_keystate(now, down);
    tx_ident(now);   // periodic station-ID packet (Part 97 callsign ID) — both modes

    if (now - last_draw >= 100) {
        last_draw = now;
        display::fox(seq, config::fox_message(), down, PWR_LEVELS[pwr_idx].label);
    }
}

static void loop_livekey(uint32_t now) {
    key.update();
    bool down = key.down();
    set_tone(down);
    if (g_keymode == KEYMODE_EDGE) tx_edge(now, down);
    else                           tx_keystate(now, down);

    if (now - last_draw >= 100) { last_draw = now; display::livekey(seq, down); }
}

static void loop_hunter(uint32_t now) {
    static bool     rx_down = false;
    static float    rssi = 0.0f;
    static bool     rssi_valid = false;
    static uint32_t last_rx = 0;
    static uint32_t last_signal = 0;   // any decoded packet (keystate or ident)
    static uint32_t last_code = 0;     // last newly decoded char/dit-dah element

    // Bilingual RX (E4, docs/edge-events.md): a hunter follows whatever the fox
    // currently sends, flipping mode packet-by-packet. `rx_is_edge` selects the
    // decode path below (feed_segment vs update); `last_edge_seq`/`reanchor`
    // drive the EdgeEvent self-heal (-1 = no anchor yet -> first edge anchors
    // with no healing attempted).
    static bool rx_is_edge     = false;
    static int  last_edge_seq  = -1;
    static bool reanchor       = false;

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

    // Signal-loss watchdog (T_silence) — mode-aware (E4, docs/edge-events.md
    // "Loss handling"):
    //  - compat: TX streams keystate every 30 ms. If we hear nothing for ~one
    //    dah (3 units at the fast character speed), the link dropped mid-key —
    //    force key-up so the sidetone doesn't latch and the decoder doesn't
    //    hang. A real dah is never mistaken for signal loss.
    //  - edge: the 3-dah-unit rule is too short — at 13 WPM it's ~414 ms, well
    //    under the 700 ms heartbeat, so it would false-trip mid-dah on a
    //    perfectly healthy link. Heartbeats arrive every HEARTBEAT_MS even
    //    while the key sits steady, so 2.5x that bounds T_silence above any
    //    legit gap between heartbeats while staying under the 3 s presence
    //    timeout below.
    uint32_t rx_timeout_ms = rx_is_edge
        ? (uint32_t)(2.5 * proto::HEARTBEAT_MS)
        : (1200u / rx_char_wpm) * 3u;

    // PRG/BOOT tap toggles the copy display between decoded letters and raw
    // dit/dah elements (same debounced detector the fox uses for power).
    if (prg_tapped(now)) { hunter_ditdah = !hunter_ditdah; last_draw = 0; }

    uint8_t buf[32];
    size_t n;
    float r;
    if (radio::poll(buf, sizeof(buf), n, r)) {
        proto::KeyState  ks;
        proto::Ident     id;
        proto::EdgeEvent ev;
        if (proto::decode(buf, n, ks)) {
            // KeyState (compat): a fox sending this is NOT in edge mode, so a
            // bilingual hunter follows it back to the polling decode path —
            // byte-identical to pre-E4 behavior once rx_is_edge is false.
            rx_is_edge = false;
            rx_down = ks.key_down != 0;
            rx_station_id = ks.station_id;
            last_rx = now;
            last_signal = now;
            rssi = r;
            rssi_valid = true;
            // RSSI no longer modulates loudness — always play the pure tone at
            // full volume. (The RSSI bar on the display still tracks signal.)
            if (g_debug) {
                Serial.printf("RX K %u seq=%u lvl=%u rssi=%d\n",
                              ks.station_id, ks.seq, rx_down ? 1 : 0, (int)r);
            }
        } else if (proto::decode_ident(buf, n, id) && id.wpm && id.char_wpm) {
            // Retune the decoder to the fox's announced timing (only on change).
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
        } else if (proto::decode_edge(buf, n, ev)) {
            // EdgeEvent (E4, docs/edge-events.md "Bilingual RX"): the packet's
            // flags level is the level being ENTERED, so the segment that JUST
            // ENDED (dur_now_ms) had the OPPOSITE level, and the one before
            // that (dur_prev_ms, the self-heal copy) had `level_down`'s level.
            rx_is_edge = true;
            bool level_down = (ev.flags & proto::EDGE_FLAG_DOWN) != 0;
            bool hb         = (ev.flags & proto::EDGE_FLAG_HEARTBEAT) != 0;

            // Presence/level/sidetone update on EVERY edge packet, heartbeat or
            // not — that's the whole point of the heartbeat (presence + level
            // re-anchor without waiting on a real edge).
            rx_down = level_down;
            last_rx = now;
            last_signal = now;
            rssi = r;
            rssi_valid = true;
            rx_station_id = ev.station_id;

            if (g_debug) {
                Serial.printf("RX E %u seq=%u lvl=%u hb=%u dn=%u dp=%u rssi=%d\n",
                              ev.station_id, ev.seq, level_down ? 1 : 0,
                              hb ? 1 : 0, ev.dur_now_ms, ev.dur_prev_ms, (int)r);
            }

            if (!hb) {
                // Self-heal via seq (docs/edge-events.md "Loss handling"): a
                // gap of 1 is the normal case (strictly incrementing real-edge
                // seq); 0 is a duplicate; 2 means exactly one edge was lost and
                // dur_prev_ms restates it; >=2 is an unrecoverable boundary.
                if (reanchor || last_edge_seq < 0) {
                    decoder.feed_segment(!level_down, ev.dur_now_ms);  // anchor, no heal
                    reanchor = false;
                } else {
                    uint8_t gap = (uint8_t)((uint8_t)ev.seq - (uint8_t)last_edge_seq);
                    if (gap == 0) {
                        // duplicate edge: skip feeding
                    } else if (gap == 1) {
                        decoder.feed_segment(!level_down, ev.dur_now_ms);
                    } else if (gap == 2) {              // exactly one edge lost -> recover it
                        decoder.feed_segment(level_down, ev.dur_prev_ms);   // the missing segment
                        decoder.feed_segment(!level_down, ev.dur_now_ms);   // this segment
                    } else {                            // >=2 lost: unrecoverable boundary
                        decoder.flush();
                        decoder.feed_segment(!level_down, ev.dur_now_ms);
                    }
                }
                last_edge_seq = ev.seq;
            }
            // Heartbeat: presence/level already updated above; do NOT decode,
            // do NOT touch last_edge_seq (it doesn't own a seq — see tx_edge §0).
        }
    }

    // No fresh packet for too long → drop to key-up (silent/idle). In edge
    // mode also flush the decoder and request a re-anchor: otherwise the next
    // edge after the gap would be diffed against a stale last_edge_seq and
    // could trigger a phantom self-heal (docs/edge-events.md "Loss handling").
    if (rx_down && (now - last_rx) > rx_timeout_ms) {
        rx_down = false;
        if (rx_is_edge) {
            decoder.flush();
            reanchor = true;
        }
    }

    // Prolonged silence → the station is gone. Clear the RECV id and RSSI bar so
    // the display shows "RECV ---" with no bar instead of a stale last reading.
    if (rssi_valid && (now - last_signal) > signal_timeout_ms) {
        rssi_valid = false;
        rx_station_id = -1;
    }

    // No fresh decoded code for 10 s → blank the copy lines (text and dit/dah).
    if (last_code != 0 && (now - last_code) > copy_blank_ms &&
        (text_len || ditdah_len)) {
        text_buf[0] = '\0';   text_len = 0;
        ditdah_buf[0] = '\0'; ditdah_len = 0;
        last_draw = 0;        // force an immediate redraw of the cleared copy
    }

    set_tone(rx_down);

    // Decoder step — branches on mode (E4, docs/edge-events.md "Decode path"):
    //  - compat: the only timing source is local arrival, so the decoder must
    //    be polled every loop with the live key state (unchanged from pre-E4 —
    //    byte-identical when receiving KeyState).
    //  - edge: the decode already happened on packet arrival via feed_segment
    //    (TX-measured durations, not jittered local timestamps), so update()
    //    would double-decode from noisy local timing. Just drain the queue the
    //    feed_segment calls filled.
    // take_element() drains the dit/dah view in BOTH modes — it surfaces the
    // same per-element classification however it was produced.
    if (rx_is_edge) {
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
    } else {
        char c = decoder.update(rx_down, now);

        // Live dit/dah scroll: push each element ('.'/'-') the instant the
        // decoder classifies it, so the dit/dah view advances one element at a
        // time rather than a whole character at once.
        char e = decoder.take_element();
        if (e) {
            rolling_append(ditdah_buf, ditdah_len, sizeof(ditdah_buf), &e, 1);
            last_code = now;
            if (g_debug) Serial.printf("EL %d %c\n", rx_station_id, e);
        }

        if (c) {
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
        const char* copy = hunter_ditdah ? ditdah_buf : text_buf;
        display::hunter(copy, radio::frequency_mhz(), rx_station_id, hunter_ditdah,
                        rssi, rssi_valid, rx_down);
    }
}

void loop() {
    uint32_t now = millis();
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
    if (keyboard::read_char(kc) && (kc == 'm' || kc == 'M')) {
        bool m = !config::muted();
        apply_mute(m);
        Serial.printf("# mute %s (key)\n", m ? "on" : "off");
    }
#endif
    switch (mode) {
        case MODE_FOX:     loop_fox(now);     break;
        case MODE_LIVEKEY: loop_livekey(now); break;
        case MODE_HUNTER:  loop_hunter(now);  break;
        case MODE_HIBERNATE: break;   // never reached (slept in setup)
    }
}
