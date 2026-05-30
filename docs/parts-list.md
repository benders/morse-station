# ESP-NOW Morse Station — Parts List

Per-station bill of materials for a 2.4 GHz ESP-NOW based Morse code practice
transmitter/receiver. Target: ~$25–35 per unit, ~1km line-of-sight range,
USB-C rechargeable, kid-friendly.

## Core radio / MCU

| Part | Notes | ~Price |
|---|---|---|
| ESP32 DevKit with PA+LNA and external antenna | Look for "ESP32-WROOM-32U" boards (the `-U` suffix = u.FL connector, no PCB antenna) or branded "ESP32 long range" boards with onboard PA+LNA. Reliable choices: DFRobot FireBeetle ESP32-E with IPEX, or AI-Thinker NodeMCU-32U. For true PA+LNA, look at Ebyte E28-2G4M20S modules paired with a plain ESP32 — but simpler is an all-in-one "ESP32 long range" board. | $8–12 |
| 2.4 GHz antenna, SMA or u.FL | 3–5 dBi rubber duck WiFi antenna. If the board has u.FL, add a u.FL-to-SMA pigtail (~$1) to use a standard SMA antenna. | $2–4 |

## Audio

| Part | Notes | ~Price |
|---|---|---|
| Small 8Ω speaker, 0.5–1W | 28–40mm diameter mylar speaker. Avoid raw piezos — a real cone speaker gives a clean sidetone. | $1–2 |
| PAM8302 mono amplifier board | Class-D, 2.5W, single-supply, with a gain pot. Drives the speaker from an ESP32 PWM/DAC pin. Alternative: MAX98357A (I²S) — overkill for a square-wave tone. | $1–2 |

## Power & charging

| Part | Notes | ~Price |
|---|---|---|
| 18650 Li-ion cell, 2500–3500 mAh, button-top | Samsung 30Q, LG MJ1, or similar reputable cells. Avoid no-name "9900 mAh" fakes. | $4–6 |
| 18650 holder, single-cell, with leads | Through-hole or PCB-mount with wire leads. | $0.50 |
| TP4056 charging module with protection — "HW-373" variant | USB-C input, integrated DW01 + FS8205 over/under/short protection. **Critical:** buy the version *with* protection (4 pins on battery side, not 2). | $1 |
| MT3608 boost converter (optional) | Only if your speaker amp needs 5V. The PAM8302 runs fine at 3.7–5.5V, so usually skip this and run everything from the battery directly. | $1 |
| SPDT slide or rocker switch, panel mount | Wire between TP4056 OUT+ and the rest of the circuit so charging works with the switch off. | $0.50 |

## Indicators & display

| Part | Notes | ~Price |
|---|---|---|
| Power LED (green, 3mm) + 1kΩ resistor | Across the switched battery rail. | $0.10 |
| Signal LED (red or blue, 5mm) + 330Ω resistor | Driven by an ESP32 GPIO; mirrors received key state for visual Morse. | $0.10 |
| 0.96" SSD1306 OLED, I²C, 128×64 (optional) | Cheap, readable, 3.3V-tolerant. Shows decoded Morse, RSSI, peer MAC, battery voltage. SH1106 1.3" variant is a slightly bigger alternative. | $3–4 |

## Input

| Part | Notes | ~Price |
|---|---|---|
| Telegraph key | Sourced separately. Wire one contact to a GPIO with `INPUT_PULLUP`, the other to GND. 0.1µF cap across contacts for debounce. | — |
| 3.5mm mono jack, panel mount (recommended) | Lets the key plug in with a standard cable instead of being hardwired. | $0.50 |

## Enclosure & wiring

| Part | Notes | ~Price |
|---|---|---|
| Project box, ~100×60×25mm | Plastic, with mounting bosses. Hammond 1591B or generic "Arduino project box." | $3–5 |
| Perfboard or small protoboard | 5×7cm. Or design a tiny PCB if making many units. | $0.50 |
| Hookup wire, headers, heat-shrink, screws | Bulk consumables. | $2 |
| USB-C cable | For charging. Comes with some TP4056 kits. | $1 |

## Decoupling (don't skip)

| Part | Notes | ~Price |
|---|---|---|
| 100µF electrolytic + 0.1µF ceramic across the ESP32's 3.3V rail | PA+LNA TX bursts pull serious current; without bulk capacitance you get brownouts and reboots. | $0.20 |

## Approximate total

**$25–35 per station**, depending on OLED inclusion and ESP32 variant.
