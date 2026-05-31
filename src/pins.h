#pragma once

// Central pin map. Two targets share this file, selected by the build define
// (-DDEVICE_HELTEC_V4 / -DDEVICE_CARDPUTER_ADV in platformio.ini):
//
//   Heltec WiFi LoRa 32 V4 : ESP32-S3 + on-board SX1262 + external FEM (PA/LNA),
//                            SSD1306 OLED, our own amp/key wiring.
//   M5Stack Cardputer ADV  : Stamp-S3A (ESP32-S3) + ST7789V2 LCD + ES8311 codec
//                            + TCA8418 keyboard. The SX1262 is NOT on-board: it
//                            rides on the removable "Cap LoRa-1262" (bare SX1262
//                            + ATGM336H GNSS, RP-SMA antenna, NO external PA).
//                            Pin map per M5Stack docs (docs.m5stack.com).

#if defined(DEVICE_HELTEC_V4)

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
#define HAS_FEM 1
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

#elif defined(DEVICE_CARDPUTER_ADV)

// SX1262 on the Cap LoRa-1262 (EXT/Grove SPI + freed keyboard-scan GPIOs).
// SPI bus is shared with the microSD slot (G12/G14/G39/G40). Control lines use
// the GPIO3-7 block freed by the TCA8418 keyboard controller.
//   NSS=G5  DIO1=G4  RST=G3  BUSY=G6   SCK=G40  MOSI=G14  MISO=G39
// No FEM: max TX is the SX1262 chip ceiling, +22 dBm (no external PA).
static constexpr int PIN_NSS  = 5;
static constexpr int PIN_SCK  = 40;
static constexpr int PIN_MOSI = 14;
static constexpr int PIN_MISO = 39;
static constexpr int PIN_NRST = 3;
static constexpr int PIN_BUSY = 6;
static constexpr int PIN_DIO1 = 4;
// VERIFY ON HW: does the Cap LoRa-1262 SX1262 use a TCXO (typical for M5 LoRa
// modules) or a plain crystal? Wrong tcxoVoltage makes beginFSK() fail. See
// radio.cpp TCXO_V. Cross-check a Meshtastic variant for this cap if unsure.

// ST7789V2 LCD (240x135), SPI. Per M5 docs PinMap:
//   BL=G38 (shared with RGB-LED power-enable), RST=G33, DC/RS=G34,
//   MOSI/DAT=G35, SCK=G36, CS=G37.
static constexpr int PIN_LCD_BL   = 38;
static constexpr int PIN_LCD_RST  = 33;
static constexpr int PIN_LCD_DC   = 34;
static constexpr int PIN_LCD_MOSI = 35;
static constexpr int PIN_LCD_SCK  = 36;
static constexpr int PIN_LCD_CS   = 37;

// Internal I2C bus (GPIO8/9), shared by TCA8418 keyboard (0x34), ES8311 codec
// (0x18) and BMI270 IMU (0x68). The codec is driven by M5Unified (M5.Speaker);
// the keyboard we read directly off the TCA8418.
static constexpr int PIN_I2C_SDA  = 8;
static constexpr int PIN_I2C_SCL  = 9;
static constexpr int PIN_KBD_INT  = 11;    // TCA8418 INT, active-low / falling edge
static constexpr int I2C_ADDR_TCA8418 = 0x34;

// ES8311 audio I2S pins (FYI; M5.Speaker configures these). SCLK=G41,
// ASDOUT=G46, LRCK=G43, DSDIN=G42. There is no MCLK line.

// The Cardputer has no FEM and no PWM-to-amp sidetone pin; audio is the ES8311
// codec over I2S (see sidetone_cardputer.cpp / M5Unified).
static constexpr int PIN_SIDETONE = -1;    // unused (codec, not a GPIO tone)
static constexpr int PIN_KEY      = 1;     // telegraph key on Grove (CONFIRM);
                                           // live-key is out of scope for the
                                           // experimental fox, kept for parity.
static constexpr int PIN_MODE_BTN = 0;     // onboard G0 button (BtnA / BOOT)

#else
#error "Define DEVICE_HELTEC_V4 or DEVICE_CARDPUTER_ADV in build_flags"
#endif
