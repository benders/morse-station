#pragma once
#ifdef DEVICE_CARDPUTER_ADV

#include <stddef.h>
#include <stdint.h>

// Shared keyboard + LCD input primitives for the Cardputer ADV. Used by both the
// boot config editor (config_ui_cardputer.cpp) and the runtime instructor menu
// (instructor_ui_cardputer.cpp) so the two front-ends share one input layer
// instead of each carrying its own copy. Renders directly with M5.Display and
// reads keys via the keyboard:: driver. See kbd_ui.cpp.

namespace kbd_ui {

// Cardputer ADV LCD geometry.
constexpr int W = 240;
constexpr int H = 135;

// Wait up to timeout_ms for any keypress. Returns the char, or 0 on timeout.
char wait_key(uint32_t timeout_ms);

// Draw a single-line edit field: title + buffer + cursor + key hints.
void draw_field(const char* title, const char* buf);

// Edit `buf` (NUL-terminated, capacity `cap` incl. the NUL) in place. Blocks
// until ENTER. Backspace deletes; printable chars (tab->space) append up to
// cap-1. Other control codes are ignored.
void edit_field(const char* title, char* buf, size_t cap);

} // namespace kbd_ui

#endif // DEVICE_CARDPUTER_ADV
