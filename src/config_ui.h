#pragma once
#ifdef DEVICE_CARDPUTER_ADV

// On-device, keyboard-driven UI for the Cardputer ADV — drive the station from
// its onboard keyboard + LCD with no laptop. Two entry points, sharing the input
// primitives in kbd_ui:
//   - run():            boot-time callsign / fox-message editor
//                       (config_ui_cardputer.cpp).
//   - instructor_menu(): runtime Instructor command menu
//                       (instructor_ui_cardputer.cpp).
// Both write the same NVS keys (config::set_*) and reuse the existing command
// handlers, so they add no new behavior beyond a front-end.

namespace config_ui {

// Offer a brief "press any key" window; if the user taps a key, open the editor
// menu (callsign / message), otherwise return so normal boot continues. Call
// once at boot after display::begin().
void run();

// Open the runtime Instructor menu (fox power / message / mute / alert /
// settings). Modal while the instructor is idle — call ONLY when
// !g_ctrl.active && !g_bcast.active so it never starves ACK servicing — and
// returns once the operator backs out; the caller's idle draw then repaints.
// Each action composes a command string and feeds it to handle_setup_command(),
// reusing the proven async relay/alert path.
void instructor_menu();

} // namespace config_ui
#endif // DEVICE_CARDPUTER_ADV
