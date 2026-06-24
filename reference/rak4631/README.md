# RAK4631 (WisCore) — upstream reference

External-source material for the RAK4631 (nRF52840 + on-board SX1262, WisBlock).
Project decisions, the operational pin map, and lessons learned live in
[`docs/components/rak4631.md`](../../docs/components/rak4631.md).

## Vendored variant files — source

The board JSON (`boards/wiscore_rak4631.json`) names `variant:
WisCore_RAK4631_Board`, which the **stock** Adafruit nRF52 Arduino core does not
ship — only RAKwireless's fork does. The two variant files are vendored into the
tree unmodified:

    variants/WisCore_RAK4631_Board/variant.h
    variants/WisCore_RAK4631_Board/variant.cpp

- **Source:** `https://github.com/RAKWireless/RAK-nRF52-Arduino`
  → `variants/WisCore_RAK4631_Board/{variant.h,variant.cpp}` (unmodified copies;
  `--depth 1` clone of `master`, fetched 2026-06-06).
- This is RAKwireless's fork of `Adafruit_nRF52_Arduino` — the canonical source
  for this variant, from which Meshtastic/MeshCore ultimately derive their copies.

## Board JSON provenance

`boards/wiscore_rak4631.json` matches Meshtastic's `wiscore_rak4631.json` shape:
nRF5 core, Adafruit BSP, **SoftDevice S140 6.1.1**, `nrf52840_s140_v6.ld`,
settings @ `0xFF000`, max flash 815104 / RAM 248832.

## Facts asserted by the vendored `variant.h`

- `PIN_WIRE_SDA = 13`, `PIN_WIRE_SCL = 14` — the default `Wire` (I2C) bus (the
  RAK1921 OLED sits here).
- `PIN_SPI_SCK = 3`, `PIN_SPI_MOSI = 30`, `PIN_SPI_MISO = 29` — the
  **default/Feather-header SPI bus** (e.g. an SD card on some WisBlock modules),
  **not** the SX1262 bus.
- `variant.h` defines **no** `PIN_LORA_*` / SX1262 macros — RAK board variants
  leave the LoRa wiring to the application.

## Datasheets

External PDFs (RAKwireless docs / schematic) were not reachable from the build
environment at port time; the variant source above was reachable via `git clone`.
If needed later, these are linked from
`https://docs.rakwireless.com/product-categories/wisblock/`:

- RAK4631 WisCore module datasheet
- RAK19007 base board schematic
- RAK1921 OLED datasheet
</content>
