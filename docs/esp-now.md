# ESP-NOW Morse Station — Design Notes

## Goals

- Wireless Morse code practice for youth-group use, ~1 km range max.
- No HAM license required → operate in the 2.4 GHz ISM band under FCC Part 15.
- Cheap (~$30/unit), USB-rechargeable, robust enough for kids.
- One instructor "fox" can broadcast to many receivers simultaneously.

## Why ESP-NOW

- Runs on the ESP32's built-in 2.4 GHz Wi-Fi radio — no extra RF module.
- Connectionless: peers exchange frames without association, DHCP, or an AP.
- Latency is 2–5 ms end to end — imperceptible even at 30+ WPM.
- Supports broadcast (`FF:FF:FF:FF:FF:FF`) so a single transmitter reaches every
  listener on the channel without per-peer registration.
- Optional AES encryption and delivery callbacks if needed later.

## Protocol

Two viable approaches:

1. **Edge events.** Send a packet on every key transition: `{state, timestamp}`.
   Minimal airtime, but a dropped packet can leave the receiver stuck.
   Requires heartbeats or a watchdog timeout.
2. **State broadcast (recommended).** Send the current key state every 20 ms
   regardless. Self-healing: a dropped packet is replaced 20 ms later.
   At ~50 packets/sec and <1 ms airtime each, the channel is barely used.

For a youth group, use state broadcast — simpler, more forgiving, easier to
debug.

## Channel planning

- ESP-NOW rides on Wi-Fi channels 1–14 (2.412–2.484 GHz, 20 MHz wide).
- Common AP channels in the US are 1, 6, 11 — pick something else (e.g., 3 or 9)
  to dodge camp Wi-Fi.
- All peers must be on the same channel; set it explicitly in `esp_now_peer_info_t`.

## Addressing model

- Each station prints its MAC at boot (`WiFi.macAddress()`); record once and
  hardcode peer lists, or do broadcast discovery on first boot.
- Instructor "fox" transmits to the broadcast MAC; every receiver hears it.
- Partner practice: two stations register each other as unicast peers.

## Sidetone

The local sidetone (the operator hearing their own keying) must be generated
**directly from the key input**, not routed through the radio link. Otherwise the
2–5 ms round trip becomes audible as a slap-back echo and disrupts rhythm.

- Key pin drives a square-wave tone (~600 Hz) into the PAM8302 amplifier locally.
- The received key state from the radio drives the same tone generator on the
  receiving end, mixed or switched to the same speaker.

## Power architecture

```
USB-C ──► TP4056 IN
              │
              ├──► 18650 (charge)
              │
              └──► TP4056 OUT+ ──► SPDT switch ──► main rail (3.7–4.2V)
                                                       │
                                                       ├──► ESP32 VIN
                                                       ├──► PAM8302 V+
                                                       └──► Power LED
```

- Switch sits *after* TP4056 OUT+ so charging works with the unit off.
- TP4056 doesn't do clean load-sharing; charge-while-using can misbehave. For a
  kids' project that's acceptable (charge with the switch off). If charge-while-
  use is important, swap to an IP5306 module or a TP4056 + load-sharing MOSFET.
- Use the protected "HW-373" TP4056 variant (4 pins on battery side, integrated
  DW01 + FS8205). Bare 2-pin TP4056s have no over-discharge protection and will
  ruin cells.

## Decoupling

The PA+LNA TX bursts pull large transient currents. Put **100 µF electrolytic ∥
0.1 µF ceramic** across the ESP32's 3.3V rail, close to the module. Without this,
expect random brownout resets during transmission.

## Wiring summary

| ESP32 pin | Connection |
|---|---|
| VIN | Main rail (post-switch) |
| GND | Common ground |
| GPIO 14 (INPUT_PULLUP) | Telegraph key → GND |
| GPIO 25 (DAC1) or PWM-capable pin | PAM8302 IN+ → speaker |
| GPIO 4 | Signal LED → 330Ω → GND |
| GPIO 21 / 22 | OLED SDA / SCL |
| GPIO 35 (ADC) | Battery voltage divider (2× 100kΩ from rail to GND) |

Add a 0.1 µF cap across the telegraph key contacts for debounce.

## Software outline (Arduino)

```cpp
#include <WiFi.h>
#include <esp_now.h>

const int KEY_PIN = 14;
const int LED_PIN = 4;
const int TONE_PIN = 25;
const int TONE_HZ = 600;
const uint8_t BROADCAST[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

void onRecv(const uint8_t* mac, const uint8_t* data, int len) {
  bool down = data[0];
  digitalWrite(LED_PIN, down);
  if (down) tone(TONE_PIN, TONE_HZ); else noTone(TONE_PIN);
}

void setup() {
  pinMode(KEY_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  WiFi.mode(WIFI_STA);
  esp_now_init();
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST, 6);
  peer.channel = 3;
  esp_now_add_peer(&peer);
  esp_now_register_recv_cb(onRecv);
}

void loop() {
  static uint32_t last = 0;
  bool down = (digitalRead(KEY_PIN) == LOW);
  // local sidetone, generated directly from the key
  if (down) tone(TONE_PIN, TONE_HZ); else noTone(TONE_PIN);
  // broadcast state every 20 ms
  if (millis() - last >= 20) {
    last = millis();
    uint8_t state = down;
    esp_now_send(BROADCAST, &state, 1);
  }
}
```

(Sidetone handling above is simplified — in practice gate the local tone on a
local-station flag and the received tone on incoming packets, so the speaker
behaves sensibly when both are active.)

## Range and antenna notes

- Stock ESP32 with PCB antenna: ~200 m line-of-sight.
- ESP32 with external 3–5 dBi antenna and PA+LNA: 500 m to >1 km line-of-sight.
- 2.4 GHz is line-of-sight dominant — trees and buildings cost significant signal.
  For camp use across open fields, expect to exceed 1 km easily.
- Orient antennas vertically for best polarization match between stations.

## Future ideas

- OLED shows decoded Morse text in real time (table-driven decoder).
- RSSI display for "fox hunt" exercises.
- Battery percentage from ADC divider on GPIO 35.
- Multiple "channels" via different ESP-NOW peer addresses for parallel classes.
