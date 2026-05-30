# 900 MHz MCU + Radio Boards (FSK / OOK focus)

Filtered for **FSK/GFSK + OOK** capability, **external antenna connector** (u.FL or SMA), and built-in MCU. LoRa not required; range and simplicity preferred. Prices fetched May 2026.

"PA" / "LNA" columns refer to a **separate external front-end** (RF PA module / LNA stage). All listed radios have an **on-chip PA and LNA**; the column reads "integrated only" when no external part is added on the board.

| Name | Price | Radio | TX Power | Ext. PA | Ext. LNA | FSK/GFSK | OOK | Battery + Charger | Screen | Antenna Conn. | Link |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Adafruit Feather 32u4 RFM69HCW 900 MHz | $24.95 | RFM69HCW | +20 dBm | ❌ integrated only | ❌ integrated only | ✅ | ✅ | JST-PH + LiPo charger | ❌ | u.FL pad / wire | [3076](https://www.adafruit.com/product/3076) |
| Adafruit Feather M0 RFM69HCW 900 MHz | ~$34.95 | RFM69HCW | +20 dBm | ❌ integrated only | ❌ integrated only | ✅ | ✅ | JST-PH + LiPo charger | ❌ | u.FL pad / wire | [3176](https://www.adafruit.com/product/3176) |
| Adafruit Feather 32u4 RFM95 LoRa 900 MHz | $34.95 | SX1276 (RFM95W) | +20 dBm (PA_BOOST) | ❌ integrated only | ❌ integrated only | ✅ | ✅ | JST-PH + LiPo charger | ❌ | u.FL pad / wire | [3078](https://www.adafruit.com/product/3078) |
| Adafruit Feather M0 RFM95 LoRa 900 MHz | $34.95 | SX1276 (RFM95W) | +20 dBm (PA_BOOST) | ❌ integrated only | ❌ integrated only | ✅ | ✅ | JST-PH + LiPo charger | ❌ | u.FL pad / wire | [3178](https://www.adafruit.com/product/3178) |
| Adafruit RFM69HCW Radio FeatherWing 900 MHz | $9.95 | RFM69HCW | +20 dBm | ❌ integrated only | ❌ integrated only | ✅ | ✅ | via host Feather | via host | u.FL pad / wire | [3229](https://www.adafruit.com/product/3229) |
| Adafruit RFM95W LoRa FeatherWing 900 MHz | $19.95 | SX1276 (RFM95W) | +20 dBm (PA_BOOST) | ❌ integrated only | ❌ integrated only | ✅ | ✅ | via host Feather | via host | u.FL pad / wire | [3231](https://www.adafruit.com/product/3231) |
| SparkFun Pro RF – LoRa 915 MHz (SAMD21) | $36.95 | SX1276 (RFM95W) | +20 dBm (PA_BOOST) | ❌ integrated only | ❌ integrated only | ✅ | ✅ | JST + LiPo charger | ❌ | u.FL (populated) | [Pro RF](https://www.sparkfun.com/sparkfun-pro-rf-lora-915mhz-samd21.html) |
| LILYGO TTGO LoRa32 V2.1 915 MHz | $26.97 | SX1276 | +20 dBm (PA_BOOST) | ❌ integrated only | ❌ integrated only | ✅ | ✅ | JST + LiPo charger | 0.96″ OLED | SMA | [Rokland](https://store.rokland.com/products/lilygo-ttgo-lora32-v2-1_1-6-version-915mhz-esp32-lora-oled-0-96-inch-sd-card-bluetooth-wifi-wireless-module-esp-32-sma-q211) |
| LILYGO T-Echo 915 MHz | $64.97 | SX1262 | +22 dBm | ❌ integrated only | ❌ integrated only | ✅ | ❌ | Built-in LiPo + charger | 1.54″ ePaper | IPEX (u.FL) | [Rokland](https://store.rokland.com/products/lilygo-ttgo-meshtastic-t-echo-white-lora-sx1262-wireless-module-915mhz-nrf52840-gps-for-arduino) |
| LILYGO T-Beam Supreme 915 MHz | $59.97 | SX1262 | +22 dBm | ❌ integrated only | ❌ integrated only | ✅ | ❌ | 18650 holder + charger | 0.96″ OLED | IPEX + SMA | [Rokland](https://store.rokland.com/products/lilygo-t-beam-supreme-esp32-s3-lora-development-board-sx1262-915mhz-gps-l76k-or-u-blox) |
| LILYGO T-Deck 915 MHz | $67.97 | SX1262 | +22 dBm | ❌ integrated only | ❌ integrated only | ✅ | ❌ | Built-in LiPo + charger | 2.8″ TFT + keyboard | IPEX/SMA | [Rokland](https://store.rokland.com/products/lilygo-t-deck-portable-microcontroller-programmer-lora-915-mhz-h642) |
| Heltec WiFi LoRa 32 V3 915 MHz | $26.97 | SX1262 | +22 dBm | ❌ integrated only | ❌ integrated only | ✅ | ❌ | SH1.25-2 + LiPo charger | 0.96″ OLED | u.FL + SMA pigtail | [Rokland](https://store.rokland.com/products/heltec-wifi-lora-32v3) |
| Heltec WiFi LoRa 32 V4 915 MHz | ~$22–30 | SX1262 (27 dBm variant) | +27 dBm (external PA) | ✅ external PA | ❌ integrated only | ✅ | ❌ | SH1.25-2 + LiPo charger | 0.96″ OLED | u.FL + SMA pigtail | [Amazon](https://www.amazon.com/Heltec-Development-Display-Meshtastic-Communication/dp/B0FS1R4HXH) |

## Notes

- **SX1262 boards (Heltec V3/V4, all LILYGO T-* devices) do not support OOK.** Semtech dropped OOK from the SX126x family. If you need OOK, pick an **RFM69HCW** or **SX1276/RFM95W** board.
- **External PA/LNA front-ends** are uncommon at this price tier. The notable exception is the **Heltec V4 "high power" / Ultimate** variant, which adds an external PA stage to push +27 dBm (~500 mW). The base Heltec V3 and all Adafruit/SparkFun/LILYGO boards above rely on the radio's integrated PA/LNA only.
- For more TX power on a no-LoRa, FSK/OOK build, options are: (a) add an external 915 MHz PA/LNA module (e.g. RFX2401C or SKY66xxx) to a Feather RFM69 board, or (b) step up to the SparkFun MicroMod 1 W LoRa Function Board (SX127x + external 1 W PA, supports OOK) paired with a MicroMod processor board.
- All Adafruit Feathers and FeatherWings expose a **u.FL footprint** but the connector is **not populated** — solder a wire antenna or hand-mount a u.FL jack. SparkFun Pro RF ships with u.FL installed. LILYGO/Heltec boards ship with SMA or u.FL pigtail.

### Recommended picks for OOK + FSK + range + simplicity
1. **Adafruit Feather M0 RFM69HCW** — pure FSK/GFSK/OOK, JST + LiPo charger, +20 dBm, simplest software stack (RadioLib / RadioHead).
2. **Adafruit Feather M0 RFM95 LoRa** — SX1276, gives you OOK/FSK *and* LoRa fallback for max range.
3. **SparkFun Pro RF (SAMD21 + RFM95W)** — only board here with u.FL pre-populated; same SX1276 capability set.
