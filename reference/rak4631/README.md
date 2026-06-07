# RAK4631 (WisCore) — Phase 2 references

## Variant strategy: Option A (vendored), sourced via Option B's fork

The board JSON (`boards/wiscore_rak4631.json`, copied verbatim from the plan/
Meshtastic) names `variant: WisCore_RAK4631_Board`, which the **stock**
Adafruit nRF52 Arduino core does not ship — only RAKwireless's fork does.

We tried Option B first (per the task brief's stated preference order) by
locating a fork that already contains the variant:

- Cloned `https://github.com/RAKWireless/RAK-nRF52-Arduino` (RAKwireless's own
  fork of `Adafruit_nRF52_Arduino`, the canonical source for this variant —
  it is what Meshtastic/MeshCore ultimately derive their copies from).
- It contains `variants/WisCore_RAK4631_Board/{variant.h,variant.cpp}` plus a
  matching bootloader.

Rather than pin `platform_packages` to that whole fork (which replaces the
*entire* Arduino core — a much bigger, harder-to-pin dependency, and the
fork's `package.json` / version metadata isn't structured the way PlatformIO's
`platform_packages` URL+ref override expects without more plumbing), we copied
just the two variant files into our tree:

    variants/WisCore_RAK4631_Board/variant.h
    variants/WisCore_RAK4631_Board/variant.cpp

and pointed PlatformIO at them with `board_build.variants_dir = variants` in
`[nrf52_base]`. This is **Option A** (vendor the variant dir), using
RAKWireless/RAK-nRF52-Arduino as the **provenance source** for the two files
(commit fetched 2026-06-06, `--depth 1` clone of `master`).

This keeps the stock `framework-arduinoadafruitnrf52` core (no
`platform_packages` override, no fork-pinning risk) while still satisfying the
board JSON's `variant` name — matching "vendor everything" from §3 of the plan
and avoiding the BLE-lockup-fork question entirely (we can revisit
`platform_packages` later only if we hit that specific bug on hardware).

## Source / provenance

- Variant files: `https://github.com/RAKWireless/RAK-nRF52-Arduino`
  → `variants/WisCore_RAK4631_Board/{variant.h,variant.cpp}` (unmodified copies).
- Board JSON: written verbatim from the port-plan spec (matches Meshtastic's
  `boards/wiscore_rak4631.json` shape: nRF5 core, adafruit BSP, S140 6.1.1,
  `nrf52840_s140_v6.ld`, settings @ 0xFF000, max flash 815104 / RAM 248832).

## Pin map confirmation (against the vendored variant.h)

The variant.h does **not** define any `PIN_LORA_*` / SX1262 macros — RAK board
variants leave the LoRa wiring to the application (consistent with our
`-DPIN_*` build_flags approach; nothing to "confirm against" there beyond what
the plan already states). It does confirm the parts that matter to us:

- `PIN_WIRE_SDA = 13`, `PIN_WIRE_SCL = 14` → matches §2 "RAK1921 OLED on
  default Wire (SDA=13/SCL=14)".
- `PIN_SPI_SCK = 3`, `PIN_SPI_MOSI = 30`, `PIN_SPI_MISO = 29` → this is the
  **default/Feather-header SPI bus** (used by e.g. an SD card on some
  WisBlock modules), **not** the SX1262 bus. Our radio uses the **global
  `SPI`** re-pinned explicitly to NSS=42/SCK=43/MISO=45/MOSI=44 via
  `SPI.begin()` + `new Module(...)` — RadioLib/Module drives CS itself, and
  `SPIClass::begin(sck, miso, mosi, ss)` overrides the variant defaults for
  those four lines. This matches how Meshtastic/MeshCore wire the RAK4631
  SX1262 (it is on its own dedicated SPI pins per the WisCore schematic, not
  the Feather SPI header).

§2's authoritative SX1262 pin map (NSS=42 DIO1=47 NRST=38 BUSY=46 SCK=43
MISO=45 MOSI=44, SX126X_POWER_EN=37, DIO2=RF switch, DIO3=TCXO 1.8V) and the
VBAT read on PIN_VBAT_READ=5 (RAK19007 divider) are taken directly from the
plan / Meshtastic & MeshCore RAK4631 variants — **BUSY 46 vs 39 still wants
hardware confirmation** (we could not reach the RAK19007 schematic PDF from
this environment; both numbers appear in different community variants
depending on revision). Likewise the **VBAT divider ratio is a TODO** —
`battery.cpp` leaves a clearly-commented placeholder ratio pending the RAK19007
schematic; calibrate against a multimeter reading on real hardware before
trusting the percentage.

## Datasheets

Could not fetch external PDFs (RAKwireless docs / schematic) from this
environment — no network reachability for those hosts at the time of writing.
The variant source above was reachable via `git clone` from GitHub. If
datasheets are needed later: RAK4631 WisCore module datasheet, RAK19007 base
board schematic, RAK1921 OLED datasheet, all linked from
`https://docs.rakwireless.com/product-categories/wisblock/`.
