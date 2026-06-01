#pragma once

// Battery fuel gauge. Two backends selected by the build define:
//   Heltec V4    : SX1262 board, cell read through a 390k/100k divider on an
//                  ADC pin gated by an enable MOSFET (see pins.h).
//   Cardputer ADV: M5Unified owns the PMIC; M5.Power reports charge directly.
//
// percent() returns a 0..100 state-of-charge estimate, or -1 when no battery
// reading is available (e.g. running on USB with no cell, or unsupported HW).
// Readings are lightly smoothed and rate-limited internally, so callers can
// poll it every display frame cheaply.

namespace battery {

void begin();

// 0..100 state-of-charge, or -1 if unknown.
int percent();

// True while the pack is on external/USB charge (best-effort; may be false on
// platforms that can't sense charge state).
bool charging();

} // namespace battery
