#pragma once
#ifdef DEVICE_CARDPUTER_ADV

// On-device, keyboard-driven configuration for the Cardputer ADV: set the
// callsign and fox message from the keyboard + LCD, no laptop. Parallels the
// serial setup console (run_setup_console) and writes the same NVS keys via
// config::set_*. See config_ui_cardputer.cpp.

namespace config_ui {

// Offer a brief "press any key" window; if the user taps a key, open the editor
// menu (callsign / message), otherwise return so normal boot continues. Call
// once at boot after display::begin().
void run();

} // namespace config_ui
#endif // DEVICE_CARDPUTER_ADV
