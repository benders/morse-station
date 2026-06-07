#pragma once

// Central pin map. Three targets share this file, selected by the build define
// (-DDEVICE_HELTEC_V4 / -DDEVICE_HELTEC_V3 / -DDEVICE_CARDPUTER_ADV in platformio.ini):
//
//   Heltec WiFi LoRa 32 V4 : ESP32-S3 + on-board SX1262 + external FEM (PA/LNA),
//                            SSD1306 OLED, our own amp/key wiring.
//   Heltec WiFi LoRa 32 V3 : ESP32-S3 + on-board SX1262, NO external FEM.
//                            Same radio/OLED/I2S pins as V4; only difference
//                            that touches firmware is battery-gate polarity
//                            (V3 gate is active-LOW; V4 gate is active-HIGH).
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
// revision doesn't use for its FEM are just free GPIOs there). Pin roles per
// the Heltec/MeshCore V4 variant (meshcore-dev/MeshCore PR #1867):
//   V4.2  (GC1109):   VFEM=7, CSD(EN)=2, CPS(TX_EN)=46;  CTX(TX/RX) on DIO2.
//   V4.3.1(KCT8103L): VFEM=7, CSD=2,     CTX=5;          (46 freed).
//
// IMPORTANT difference: on the GC1109 (V4.2) the FEM TX/RX switch (CTX) is wired
// to the SX1262 DIO2 and flips automatically (setDio2AsRfSwitch). On the
// KCT8103L (V4.3) that CTX line is GPIO5 — driven by the MCU in software — and
// its level ALSO selects the RX LNA: CTX=LOW -> LNA in RX path, CTX=HIGH -> TX
// path / LNA bypass. So GPIO5 must track TX vs RX (see radio.cpp fem_set_rx/tx);
// holding it HIGH leaves V4.3 RX permanently LNA-bypassed (badly desensitised).
#define HAS_FEM 1
static constexpr int PIN_FEM_VFEM = 7;     // FEM LDO power enable (both revs)
static constexpr int PIN_FEM_CSD  = 2;     // chip enable: GC1109 CSD / KCT8103L CSD
static constexpr int PIN_FEM_CTX  = 5;     // KCT8103L CTX: LOW=RX/LNA, HIGH=TX
                                           // (V4.2: free GPIO; its CTX is DIO2)
static constexpr int PIN_FEM_CPS  = 46;    // V4.2 GC1109 PA mode (LOW = bypass)

// OLED (SSD1315 / SSD1306-compatible) on I2C (fixed on-board)
static constexpr int PIN_OLED_SDA  = 17;
static constexpr int PIN_OLED_SCL  = 18;
static constexpr int PIN_OLED_RST  = 21;
static constexpr int PIN_VEXT_CTRL = 36;   // LOW = peripheral 3.3V rail on

// Battery fuel gauge: the cell is read through a 390k/100k divider on GPIO1,
// gated by an ADC-enable MOSFET on GPIO37. NOTE the gate is active-HIGH on V4
// (both 4.2 and 4.3) — opposite of the V3 "GPIO37 low" convention. Verified on
// hardware: drive HIGH to connect the divider, LOW to disconnect it and stop
// the idle drain (LOW reads 0 on both V4 revisions). See battery.cpp.
static constexpr int PIN_VBAT_ADC  = 1;
static constexpr int PIN_VBAT_CTRL = 37;

// Our wiring (confirm on hardware)
//
// Sidetone is now an I2S stream into a MAX98357A class-D amp (replaces the old
// PWM-on-GPIO4 + RC low-pass + PAM8403 analog chain). I2S is 3-wire. The
// authoritative pin assignment lives in platformio.ini ([env:heltec_v4]
// build_flags: -DPIN_I2S_BCLK / _LRCLK / _DIN); define pin maps THERE, not
// here. The #ifndef fallbacks below only let this header compile if a flag is
// missing — they mirror the platformio.ini values. The trio is GPIO4/47/48,
// clear of the radio (8-14), FEM (2/5/7/46), OLED (17/18/21), VEXT (36),
// battery (1/37), key (6), boot strap (0) and native USB (19/20).
#ifndef PIN_I2S_BCLK
#define PIN_I2S_BCLK 48                     // -> MAX98357A BCLK (bit clock)
#endif
#ifndef PIN_I2S_LRCLK
#define PIN_I2S_LRCLK 4                     // -> MAX98357A LRC  (word/LR select)
#endif
#ifndef PIN_I2S_DIN
#define PIN_I2S_DIN 47                      // -> MAX98357A DIN  (serial data)
#endif
static constexpr int PIN_SIDETONE = PIN_I2S_DIN;  // back-compat alias; the I2S
                                           // path uses the PIN_I2S_* trio above
                                           // and ignores the value passed in.
static constexpr int PIN_KEY      = 6;     // telegraph key -> GND, INPUT_PULLUP
                                           // (was GPIO 5: that pin is FEM ctrl
                                           //  on V4.3 — pull-up wouldn't hold)
static constexpr int PIN_MODE_BTN = 0;     // onboard PRG/BOOT button (mode select)

// Battery gate polarity: V4 gate (GPIO37) is active-HIGH — see battery.cpp.
#define BATT_GATE_ACTIVE_HIGH 1

#elif defined(DEVICE_HELTEC_V3)

// Heltec WiFi LoRa 32 V3 — ESP32-S3 + on-board SX1262, no external FEM.
// Radio and display pin assignments are identical to V4.  The only firmware
// difference is the battery-ADC gate polarity (active-LOW on V3, see below).

// SX1262 radio (fixed on-board — same as V4)
static constexpr int PIN_NSS  = 8;
static constexpr int PIN_SCK  = 9;
static constexpr int PIN_MOSI = 10;
static constexpr int PIN_MISO = 11;
static constexpr int PIN_NRST = 12;
static constexpr int PIN_BUSY = 13;
static constexpr int PIN_DIO1 = 14;

// No external RF front-end on V3: do NOT define HAS_FEM, do not declare
// PIN_FEM_* constants.  radio.cpp's HAS_FEM blocks compile out automatically.

// OLED (SSD1315 / SSD1306-compatible) on I2C (fixed on-board — same as V4)
static constexpr int PIN_OLED_SDA  = 17;
static constexpr int PIN_OLED_SCL  = 18;
static constexpr int PIN_OLED_RST  = 21;
static constexpr int PIN_VEXT_CTRL = 36;   // LOW = peripheral 3.3V rail on

// Battery fuel gauge: same 390k/100k divider on GPIO1 as V4; same gate GPIO37.
// POLARITY DIFFERENCE: on V3 the gate MOSFET is active-LOW (drive LOW to
// connect the divider / enable the ADC read, HIGH to disconnect and stop idle
// drain).  This is opposite to the V4 (active-HIGH).  Verified in the Heltec
// V3 schematic and documented in project memory.  See battery.cpp.
static constexpr int PIN_VBAT_ADC  = 1;
static constexpr int PIN_VBAT_CTRL = 37;

// Battery gate polarity: V3 gate (GPIO37) is active-LOW — see battery.cpp.
#define BATT_GATE_ACTIVE_HIGH 0

// Sidetone — same I2S → MAX98357A path as V4.  The authoritative pin
// assignment is in platformio.ini ([env:heltec_v3] build_flags); the #ifndef
// fallbacks below mirror those values so the header compiles if a flag is
// missing.  GPIO4/47/48 are clear of the radio (8-14), OLED (17/18/21),
// VEXT (36), battery (1/37), key (6), boot strap (0), and native USB (19/20).
#ifndef PIN_I2S_BCLK
#define PIN_I2S_BCLK 48                     // -> MAX98357A BCLK (bit clock)
#endif
#ifndef PIN_I2S_LRCLK
#define PIN_I2S_LRCLK 4                     // -> MAX98357A LRC  (word/LR select)
#endif
#ifndef PIN_I2S_DIN
#define PIN_I2S_DIN 47                      // -> MAX98357A DIN  (serial data)
#endif
static constexpr int PIN_SIDETONE = PIN_I2S_DIN;  // back-compat alias
static constexpr int PIN_KEY      = 6;     // telegraph key -> GND, INPUT_PULLUP
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

#elif defined(DEVICE_RAK4631)

// RAKwireless RAK4631 (WisCore module on a RAK19007 base board) — nRF52840 +
// on-board SX1262, RAK1921 SSD1306 OLED, no FEM, no sidetone hardware.
// Cross-MCU port (Phase 2; see RAK-port-plan.md §2 for full provenance).
//
// SX1262 radio: the authoritative pin assignment lives in platformio.ini
// ([env:rak4631] build_flags: -DPIN_NSS / _DIO1 / _NRST / _BUSY / _SCK / _MISO
// / _MOSI / -DSX126X_POWER_EN). The #ifndef fallbacks below mirror those values
// so this header still compiles standalone. radio.cpp drives the global SPI
// (re-pinned via SPI.begin(sck, miso, mosi, ss)), not HSPI — there is no
// second hardware SPI peripheral exposed the way the ESP32-S3 has HSPI/VSPI.
#ifndef PIN_NSS
#define PIN_NSS  42
#endif
#ifndef PIN_DIO1
#define PIN_DIO1 47
#endif
#ifndef PIN_NRST
#define PIN_NRST 38
#endif
#ifndef PIN_BUSY
#define PIN_BUSY 46    // CONFIRM ON HW: some RAK4631 variant.h copies use 39 —
                       // the vendored variant.h does not define a LoRa BUSY
                       // pin, so this is taken from the plan/Meshtastic and
                       // wants confirmation against the RAK19007 schematic.
#endif
#ifndef PIN_SCK
#define PIN_SCK  43
#endif
#ifndef PIN_MISO
#define PIN_MISO 45
#endif
#ifndef PIN_MOSI
#define PIN_MOSI 44
#endif
#ifndef SX126X_POWER_EN
#define SX126X_POWER_EN 37  // MUST be driven HIGH before radio init — powers
                            // the SX1262 LDO (radio.cpp). Easy to miss: the
                            // chip simply won't respond on SPI if left low.
#endif

// No external RF front-end on the RAK4631 (bare SX1262, chip ceiling +22 dBm):
// do NOT define HAS_FEM, do not declare PIN_FEM_* constants — radio.cpp's
// HAS_FEM blocks compile out automatically, same as Heltec V3.

// RAK1921 OLED (SSD1306 128x64) on the default Wire (I2C) bus. The WisCore
// variant.h fixes SDA=13/SCL=14 as the Wire defaults, so U8g2's HW-I2C
// constructor needs no explicit pins. There is NO reset line on this module —
// U8X8_PIN_NONE. No VEXT rail to gate either (RAK19007 always powers the OLED).
static constexpr int PIN_OLED_RST = -1;   // sentinel; display.cpp uses
                                          // U8X8_PIN_NONE directly for RAK,
                                          // this constant is unused there.

// Battery: analog read on PIN_VBAT_READ (AIN) through the RAK19007 resistor
// divider. TODO calibrate the divider ratio against the schematic / a meter —
// see battery.cpp DIVIDER_RAK comment. charging() is best-effort false (no
// charge-sense line wired up in this port).
#ifndef PIN_VBAT_READ
#define PIN_VBAT_READ 5
#endif

// No sidetone hardware on this board (no amp/codec/piezo wired up yet).
// sidetone_nrf52.cpp implements the API as a silent stub; PIN_SIDETONE = -1
// signals "no GPIO" the same way the Cardputer's codec path does.
static constexpr int PIN_SIDETONE = -1;

// RAK19007 buttons: the base board exposes a USER button (commonly wired to
// the WB_IO pins / nRF52 GPIO). CONFIRM ON HARDWARE which IO-slot pin the unit
// actually has wired to a usable button before relying on these for menu nav —
// picking a plausible WisBlock IO pin here so the firmware boots and the menu
// is at least nominally reachable; remap once a unit is on the bench.
static constexpr int PIN_KEY      = 17;   // WB_IO1-ish — CONFIRM ON HARDWARE
static constexpr int PIN_MODE_BTN = 34;   // RAK19007 USER button — CONFIRM ON HARDWARE

#else
#error "Define DEVICE_HELTEC_V4, DEVICE_HELTEC_V3, DEVICE_CARDPUTER_ADV, or DEVICE_RAK4631 in build_flags"
#endif
