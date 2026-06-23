#pragma once
#ifdef DEVICE_CARDPUTER_ADV

#include <stddef.h>

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
// !g_ctrl.active && !g_bcast.active so it never starves ACK servicing.
// If the operator picks a send action, composes the matching command into
// out[] (NUL-terminated, capacity cap) and returns true — the caller must
// then feed it to handle_setup_command() and let loop() resume so the async
// relay/alert burst runs normally (never block here waiting on an ACK).
// Returns false if the operator only changed settings (fox id) or backed out
// without sending anything; out[] is left untouched in that case.
bool instructor_menu(char* out, size_t cap);

} // namespace config_ui
#endif // DEVICE_CARDPUTER_ADV
