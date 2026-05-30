#include <Arduino.h>
#include <RadioLib.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <esp_sleep.h>

// Heltec WiFi LoRa 32 V3/V4 SX1262 pin map
static constexpr int PIN_NSS  = 8;
static constexpr int PIN_SCK  = 9;
static constexpr int PIN_MOSI = 10;
static constexpr int PIN_MISO = 11;
static constexpr int PIN_NRST = 12;
static constexpr int PIN_BUSY = 13;
static constexpr int PIN_DIO1 = 14;

// OLED (SSD1315, SSD1306-compatible) on I2C
static constexpr int PIN_OLED_SDA = 17;
static constexpr int PIN_OLED_SCL = 18;
static constexpr int PIN_OLED_RST = 21;
static constexpr int PIN_VEXT_CTRL = 36;  // LOW = peripheral 3.3V on

SPIClass radioSpi(HSPI);
SX1262 radio = new Module(PIN_NSS, PIN_DIO1, PIN_NRST, PIN_BUSY, radioSpi);

U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, PIN_OLED_RST, PIN_OLED_SCL, PIN_OLED_SDA);

static constexpr float  FREQ_START_MHZ = 902.0f;
static constexpr float  FREQ_STEP_MHZ  = 0.5f;
static constexpr int    NUM_CHANNELS   = 53;   // 902.0 .. 928.0 inclusive
static constexpr uint32_t SCAN_WINDOW_MS = 30000;
static constexpr uint32_t SETTLE_US = 2500;

struct ChanStat {
    float    min_dbm;
    float    max_dbm;
    double   sum_dbm;
    uint32_t samples;
};

static ChanStat stats[NUM_CHANNELS];

static void oled_status(const char* line1, const char* line2 = nullptr, const char* line3 = nullptr) {
    oled.clearBuffer();
    oled.setFont(u8g2_font_6x12_tr);
    oled.drawStr(0, 12, line1);
    if (line2) oled.drawStr(0, 28, line2);
    if (line3) oled.drawStr(0, 44, line3);
    oled.sendBuffer();
}

static void oled_progress(uint32_t elapsed_ms, uint32_t sweeps, int chan, float freq) {
    oled.clearBuffer();
    oled.setFont(u8g2_font_6x12_tr);
    oled.drawStr(0, 12, "Scanning 902-928 MHz");

    char buf[24];
    snprintf(buf, sizeof(buf), "%lus / %lus",
             (unsigned long)(elapsed_ms / 1000),
             (unsigned long)(SCAN_WINDOW_MS / 1000));
    oled.drawStr(0, 28, buf);

    snprintf(buf, sizeof(buf), "sweeps=%lu", (unsigned long)sweeps);
    oled.drawStr(0, 42, buf);

    snprintf(buf, sizeof(buf), "f=%.1f MHz", freq);
    oled.drawStr(0, 56, buf);

    // Progress bar across bottom
    int w = (int)((elapsed_ms * 128UL) / SCAN_WINDOW_MS);
    if (w > 128) w = 128;
    oled.drawFrame(0, 60, 128, 4);
    oled.drawBox(0, 60, w, 4);

    oled.sendBuffer();
}

static void halt(const char* msg, int code) {
    Serial.printf("FATAL: %s (code=%d)\n", msg, code);
    oled_status("FATAL", msg);
    while (true) { delay(1000); }
}

static void enter_deep_sleep() {
    Serial.println("# entering deep sleep");
    Serial.flush();
    oled_status("Done.", "Sleeping...");
    delay(500);

    radio.sleep();
    oled.setPowerSave(1);          // OLED into low-power
    Wire.end();
    // Cut Ve rail to peripherals (drives Vext_Ctrl HIGH).
    pinMode(PIN_VEXT_CTRL, OUTPUT);
    digitalWrite(PIN_VEXT_CTRL, HIGH);

    esp_deep_sleep_start();        // no wake source -> wakes only via RST
}

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 2000) { delay(10); }

    // Power on peripheral rail (OLED needs this).
    pinMode(PIN_VEXT_CTRL, OUTPUT);
    digitalWrite(PIN_VEXT_CTRL, LOW);
    delay(50);

    oled.begin();
    oled_status("RSSI scanner", "booting...");

    Serial.println();
    Serial.println("# 915 MHz RSSI scanner — Heltec V4");
    Serial.printf("# range=%.1f..%.1f MHz step=%.3f MHz channels=%d window=%lu ms\n",
                  FREQ_START_MHZ,
                  FREQ_START_MHZ + (NUM_CHANNELS - 1) * FREQ_STEP_MHZ,
                  FREQ_STEP_MHZ, NUM_CHANNELS, (unsigned long)SCAN_WINDOW_MS);

    radioSpi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_NSS);

    // FSK mode: broad RX bandwidth, no sync-word filter, just energy detection.
    // Args: freq, bitrate, freqDev, rxBw, power, preambleLen, tcxoVoltage, useRegulatorLDO
    int state = radio.beginFSK(FREQ_START_MHZ, 4.8f, 5.0f, 467.0f, 10, 16, 1.8f, false);
    if (state != RADIOLIB_ERR_NONE) halt("beginFSK failed", state);

    state = radio.setSyncWord(nullptr, 0);
    // Some RadioLib versions reject 0-length sync; ignore failure here.
    (void)state;

    for (int i = 0; i < NUM_CHANNELS; i++) {
        stats[i].min_dbm = 0.0f;
        stats[i].max_dbm = -200.0f;
        stats[i].sum_dbm = 0.0;
        stats[i].samples = 0;
    }

    oled_status("Scanning...", "0s elapsed");
    uint32_t start_ms = millis();
    uint32_t sweeps = 0;
    uint32_t last_ui_ms = 0;
    while (millis() - start_ms < SCAN_WINDOW_MS) {
        for (int i = 0; i < NUM_CHANNELS; i++) {
            float f = FREQ_START_MHZ + i * FREQ_STEP_MHZ;
            if (radio.setFrequency(f) != RADIOLIB_ERR_NONE) continue;
            if (radio.startReceive() != RADIOLIB_ERR_NONE) continue;
            delayMicroseconds(SETTLE_US);
            // false = instantaneous RSSI (GetRssiInst), not last-packet RSSI.
            float r = radio.getRSSI(false);
            ChanStat& s = stats[i];
            if (r < s.min_dbm) s.min_dbm = r;
            if (r > s.max_dbm) s.max_dbm = r;
            s.sum_dbm += r;
            s.samples++;

            uint32_t now = millis();
            if (now - last_ui_ms >= 250) {
                oled_progress(now - start_ms, sweeps, i, f);
                last_ui_ms = now;
            }
        }
        sweeps++;
    }
    radio.standby();

    oled_status("Reporting...", "see serial");
    Serial.printf("# sweeps=%lu elapsed_ms=%lu\n",
                  (unsigned long)sweeps, (unsigned long)(millis() - start_ms));
    Serial.println("freq_mhz,min_dbm,max_dbm,mean_dbm,samples");
    for (int i = 0; i < NUM_CHANNELS; i++) {
        float f = FREQ_START_MHZ + i * FREQ_STEP_MHZ;
        ChanStat& s = stats[i];
        float mean = s.samples ? (float)(s.sum_dbm / s.samples) : 0.0f;
        Serial.printf("%.3f,%.1f,%.1f,%.1f,%lu\n",
                      f, s.min_dbm, s.max_dbm, mean, (unsigned long)s.samples);
    }
    Serial.println("DONE");

    enter_deep_sleep();
}

void loop() {
    // Never reached — setup() ends in deep sleep.
}
