#pragma once

// Central pin map for the Heltec WiFi LoRa 32 V4 (ESP32-S3 + SX1262).
// Radio + OLED pins are fixed by the board; the rest are our wiring choices
// and should be confirmed against the silkscreen on hardware.

// SX1262 radio (fixed on-board)
static constexpr int PIN_NSS  = 8;
static constexpr int PIN_SCK  = 9;
static constexpr int PIN_MOSI = 10;
static constexpr int PIN_MISO = 11;
static constexpr int PIN_NRST = 12;
static constexpr int PIN_BUSY = 13;
static constexpr int PIN_DIO1 = 14;

// OLED (SSD1315 / SSD1306-compatible) on I2C (fixed on-board)
static constexpr int PIN_OLED_SDA  = 17;
static constexpr int PIN_OLED_SCL  = 18;
static constexpr int PIN_OLED_RST  = 21;
static constexpr int PIN_VEXT_CTRL = 36;   // LOW = peripheral 3.3V rail on

// Our wiring (confirm on hardware)
static constexpr int PIN_SIDETONE = 7;     // -> PAM8403 audio input
static constexpr int PIN_KEY      = 5;     // telegraph key -> GND, INPUT_PULLUP
static constexpr int PIN_MODE_BTN = 0;     // onboard PRG/BOOT button (mode select)
