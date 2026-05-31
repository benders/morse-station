#include <Arduino.h>
#include "pins.h"
#include "sidetone.h"
#include "morsekey.h"
#include "morse.h"
#include "radio.h"
#include "protocol.h"
#include "display.h"
#include "config.h"
#include "config_ui.h"
#include <esp_sleep.h>
#include <esp_system.h>   // esp_reset_reason()
#include <Preferences.h>  // persist boot/reset-reason log

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
static constexpr uint32_t REPEAT_PAUSE = 7000;

// Callsign and fox message live in NVS (config), set via the boot serial console
// (see run_setup_console). The callsign is sent in the periodic station-ID
// packet; include it in the fox message text too for an audible CW ID.

enum Mode { MODE_HUNTER = 0, MODE_FOX = 1, MODE_LIVEKEY = 2, MODE_HIBERNATE = 3 };
static Mode mode = MODE_HUNTER;

// Fox TX power levels, cycled with the PRG button (SX1262 output; the FEM PA
// follows). Default LO — bump up for open ground. LO keeps the hunter volume
// gradient working in a small space (RSSI saturates at MED/HI).
struct PwrLevel { const char* label; int dbm; };
// dbm is the SX1262 *chip* output; the external FEM PA adds its gain on top.
// MAX = +22 dBm, the SX1262's own ceiling (setOutputPower clamps/rejects above
// this); through the FEM that's ~28 dBm at the antenna.
static const PwrLevel PWR_LEVELS[] = {{"LO", -9}, {"MED", 2}, {"HI", 14}, {"MAX", 22}};
static const int N_PWR  = 4;
static int       pwr_idx = 0;   // LO (-9 dBm)

static morse::Player  player;
static morse::Decoder decoder;
static MorseKey       key;

static uint16_t seq = 0;
static uint32_t last_tx = 0;
static uint32_t last_ident = 0;
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

// Read a line from Serial into buf (NUL-terminated, trailing CR/LF stripped).
// Returns false on timeout_ms with no complete line. Blocks while typing.
static bool serial_read_line(char* buf, size_t cap, uint32_t timeout_ms) {
    size_t n = 0;
    uint32_t start = millis();
    while (millis() - start < timeout_ms) {
        while (Serial.available()) {
            char c = (char)Serial.read();
            if (c == '\n' || c == '\r') {
                if (n == 0) continue;          // skip leading blank lines
                buf[n] = 0;
                return true;
            }
            if (n + 1 < cap) buf[n++] = c;
            start = millis();                  // keep alive while characters flow
        }
        delay(5);
    }
    buf[n] = 0;
    return n > 0;
}

// Boot-time serial provisioning. Offers a short window to enter a REPL that
// writes callsign / fox message / station id to NVS. Commands:
//   call <SIGN> | msg <text...> | id <n> | show | done
static void run_setup_console() {
    Serial.printf("\n# config: id=%u call=%s wpm=%u farns=%u msg=\"%s\"\n",
                  config::station_id(), config::callsign(), config::wpm(),
                  config::char_wpm(), config::fox_message());
    Serial.print("# send 's' within 2s for setup console... ");

    char line[160];
    if (!serial_read_line(line, sizeof(line), 2000) ||
        !(line[0] == 's' || line[0] == 'S')) {
        Serial.println("(skipped)");
        return;
    }
    Serial.println("\n# setup: call <SIGN> | msg <text> | id <n> | wpm <n> | "
                   "farns <n> | mode <0..2> | show | done");

    while (true) {
        Serial.print("setup> ");
        if (!serial_read_line(line, sizeof(line), 120000)) continue;

        if (!strncmp(line, "call ", 5)) {
            config::set_callsign(line + 5);
            Serial.printf("  callsign = %s\n", config::callsign());
        } else if (!strncmp(line, "msg ", 4)) {
            config::set_fox_message(line + 4);
            Serial.printf("  fox msg  = %s\n", config::fox_message());
        } else if (!strncmp(line, "id ", 3)) {
            config::set_station_id((uint8_t)atoi(line + 3));
            Serial.printf("  id       = %u\n", config::station_id());
        } else if (!strncmp(line, "wpm ", 4)) {
            config::set_wpm((uint8_t)atoi(line + 4));
            Serial.printf("  wpm      = %u (farns %u)\n",
                          config::wpm(), config::char_wpm());
        } else if (!strncmp(line, "farns ", 6)) {
            config::set_char_wpm((uint8_t)atoi(line + 6));
            Serial.printf("  farns    = %u (overall %u)\n",
                          config::char_wpm(), config::wpm());
        } else if (!strncmp(line, "mode ", 5)) {
            int m = atoi(line + 5);
            if (m < 0 || m > 2) {            // 0=Hunter 1=Fox 2=Livekey (not Hibernate)
                Serial.println("  ? mode 0=Hunter 1=Fox 2=Livekey");
            } else {
                config::set_boot_mode((uint8_t)m);
                static const char* names[] = {"Hunter", "Fox", "Livekey"};
                Serial.printf("  boot mode = %d (%s)\n", m, names[m]);
            }
        } else if (!strcmp(line, "show")) {
            Serial.printf("  id=%u call=%s wpm=%u farns=%u msg=\"%s\"\n",
                          config::station_id(), config::callsign(),
                          config::wpm(), config::char_wpm(),
                          config::fox_message());
        } else if (!strcmp(line, "done") || !strcmp(line, "exit")) {
            Serial.println("# setup done");
            return;
        } else {
            Serial.println("  ? call <SIGN> | msg <text> | id <n> | wpm <n> | "
                           "farns <n> | mode <0..2> | show | done");
        }
    }
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
        if (now - t_start > 8000) return (Mode)sel;     // idle auto-select
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
    esp_deep_sleep_start();         // only RST wakes us -> full restart
}

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 2000) { delay(10); }

#ifndef GIT_REV
#define GIT_REV "unknown"
#endif
    Serial.printf("\n# morse-station build %s\n", GIT_REV);
    {
        // Why did we (re)boot? An intermittent native-USB panic loses its
        // backtrace mid-reset, so we persist each boot's reason to NVS and print
        // the *previous* boot's reason too. After an organic crash-reboot, a
        // controlled reset (easy to capture on serial) reports prev=<crash cause>.
        static const char* rr[] = {"UNKNOWN","POWERON","EXT","SW","PANIC",
                                   "INT_WDT","TASK_WDT","WDT","DEEPSLEEP",
                                   "BROWNOUT","SDIO"};
        auto lbl = [&](int x){ return (x >= 0 && x < 11) ? rr[x] : "?"; };
        int r = (int)esp_reset_reason();
        Preferences bl; bl.begin("boot", false);
        int prev = bl.getUChar("rr", 0);
        uint32_t bc = bl.getUInt("cnt", 0) + 1;
        bl.putUChar("rr", (uint8_t)r);
        bl.putUInt("cnt", bc);
        bl.end();
        Serial.printf("# boot #%u reason now=%d(%s) prev=%d(%s)\n",
                      bc, r, lbl(r), prev, lbl(prev));
    }

    config::begin();
    // Restore the last-used fox TX power level (clamp in case the table shrank).
    pwr_idx = config::fox_pwr_idx();
    if (pwr_idx >= N_PWR) pwr_idx = 0;
    run_setup_console();
    display::begin();
    char splash_id[24];
    snprintf(splash_id, sizeof(splash_id), "build %s", GIT_REV);
    char splash_line2[24];
    snprintf(splash_line2, sizeof(splash_line2), "station id %u", config::station_id());
    display::status("Morse Station", splash_id, splash_line2);
    delay(3500);                       // hold the splash so rev + id are readable
#ifdef DEVICE_CARDPUTER_ADV
    // On-device keyboard provisioning (callsign / fox message). Brief opt-in
    // window; falls through to normal boot if no key is tapped. The serial
    // console above still works as the bench path.
    config_ui::run();
#endif
    sidetone_init(PIN_SIDETONE, TONE_HZ);

    mode = run_menu();
    // Remember the choice as the boot default (Hibernate is transient — never
    // store it, or the unit would sleep again on every restart).
    if (mode != MODE_HIBERNATE) config::set_boot_mode((uint8_t)mode);
    Serial.printf("\n# mode=%d station_id=%u\n", (int)mode, config::station_id());

    if (mode == MODE_HIBERNATE) hibernate();   // does not return

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
            break;
        case MODE_LIVEKEY:
            key.begin(PIN_KEY);
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
    tx_keystate(now, down);
    tx_ident(now);   // periodic station-ID packet (Part 97 callsign ID)

    if (now - last_draw >= 100) {
        last_draw = now;
        display::fox(seq, config::fox_message(), down, PWR_LEVELS[pwr_idx].label);
    }
}

static void loop_livekey(uint32_t now) {
    key.update();
    bool down = key.down();
    set_tone(down);
    tx_keystate(now, down);

    if (now - last_draw >= 100) { last_draw = now; display::livekey(seq, down); }
}

static void loop_hunter(uint32_t now) {
    static bool     rx_down = false;
    static float    rssi = 0.0f;
    static bool     rssi_valid = false;
    static uint32_t last_rx = 0;

    // On-air keying speeds, seeded from our config and retuned by the fox's
    // Ident packet so the decoder and watchdog track the actual sender.
    static uint8_t  rx_wpm      = config::wpm();
    static uint8_t  rx_char_wpm = config::char_wpm();

    // Signal-loss watchdog: TX streams keystate every 30 ms. If we hear nothing
    // for ~one dah (3 units at the fast character speed), the link dropped
    // mid-key — force key-up so the sidetone doesn't latch and the decoder
    // doesn't hang. A real dah is never mistaken for signal loss.
    uint32_t rx_timeout_ms = (1200u / rx_char_wpm) * 3u;

    // PRG/BOOT tap toggles the copy display between decoded letters and raw
    // dit/dah elements (same debounced detector the fox uses for power).
    if (prg_tapped(now)) { hunter_ditdah = !hunter_ditdah; last_draw = 0; }

    uint8_t buf[32];
    size_t n;
    float r;
    if (radio::poll(buf, sizeof(buf), n, r)) {
        proto::KeyState ks;
        proto::Ident    id;
        if (proto::decode(buf, n, ks)) {
            rx_down = ks.key_down != 0;
            rx_station_id = ks.station_id;
            last_rx = now;
            rssi = r;
            rssi_valid = true;
            // RSSI no longer modulates loudness — always play the pure tone at
            // full volume. (The RSSI bar on the display still tracks signal.)
        } else if (proto::decode_ident(buf, n, id) && id.wpm && id.char_wpm) {
            // Retune the decoder to the fox's announced timing (only on change).
            if (id.wpm != rx_wpm || id.char_wpm != rx_char_wpm) {
                rx_wpm = id.wpm;
                rx_char_wpm = id.char_wpm;
                decoder.begin(id.wpm, id.char_wpm);
            }
            rx_station_id = id.station_id;
        }
    }

    // No fresh keystate for too long → drop to key-up (silent/idle).
    if (rx_down && (now - last_rx) > rx_timeout_ms) {
        rx_down = false;
    }
    set_tone(rx_down);

    char c = decoder.update(rx_down, now);

    // Live dit/dah scroll: push each element ('.'/'-') the instant the decoder
    // classifies it, so the dit/dah view advances one element at a time rather
    // than a whole character at once.
    char e = decoder.take_element();
    if (e) rolling_append(ditdah_buf, ditdah_len, sizeof(ditdah_buf), &e, 1);

    if (c) {
        text_push(c);
        Serial.print(c);
        // Separate the dit/dah stream: a single space between letters, " / "
        // between words (the word-gap char ' ' arrives after the letter's space).
        if (c == ' ') ditdah_push("/ ");
        else          ditdah_push(" ");
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
    switch (mode) {
        case MODE_FOX:     loop_fox(now);     break;
        case MODE_LIVEKEY: loop_livekey(now); break;
        case MODE_HUNTER:  loop_hunter(now);  break;
        case MODE_HIBERNATE: break;   // never reached (slept in setup)
    }
}
