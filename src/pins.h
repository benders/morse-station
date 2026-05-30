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

// External RF front-end module (PA + LNA). The V4 is the high-power variant;
// the FEM sits between the SX1262 and the antenna and MUST be powered/enabled
// or receive is badly desensitised. The two board revisions wire it
// differently, so we drive the superset of control pins (the pins a given
// revision doesn't use for its FEM are just free GPIOs there):
//   V4.2  (GC1109):   VFEM=7, CSD=2,  CPS=46
//   V4.3.1(KCT8103L): VFEM=7, FEM ctrl=5,  (46 freed)
// TX/RX path (CTX) is driven automatically by SX1262 DIO2 (RF switch).
static constexpr int PIN_FEM_VFEM = 7;     // FEM LDO power enable (both revs)
static constexpr int PIN_FEM_CSD  = 2;     // V4.2 GC1109 chip enable
static constexpr int PIN_FEM_CTRL = 5;     // V4.3.1 KCT8103L FEM control
static constexpr int PIN_FEM_CPS  = 46;    // V4.2 GC1109 PA mode (LOW = bypass)

// OLED (SSD1315 / SSD1306-compatible) on I2C (fixed on-board)
static constexpr int PIN_OLED_SDA  = 17;
static constexpr int PIN_OLED_SCL  = 18;
static constexpr int PIN_OLED_RST  = 21;
static constexpr int PIN_VEXT_CTRL = 36;   // LOW = peripheral 3.3V rail on

// Our wiring (confirm on hardware)
static constexpr int PIN_SIDETONE = 4;     // -> PAM8403 audio input
                                           // (was GPIO 7: that pin is FEM power
                                           //  on the V4 — keep clear of radio)
static constexpr int PIN_KEY      = 6;     // telegraph key -> GND, INPUT_PULLUP
                                           // (was GPIO 5: that pin is FEM ctrl
                                           //  on V4.3 — pull-up wouldn't hold)
static constexpr int PIN_MODE_BTN = 0;     // onboard PRG/BOOT button (mode select)
