#pragma once
#ifdef DEVICE_CARDPUTER_ADV

// One-time M5Unified bring-up for the Cardputer ADV.
//
// M5.begin() owns *both* onboard peripherals we need: the ES8311 audio codec
// (driven via M5.Speaker for the sidetone) and the ST7789V2 LCD (M5.Display /
// M5GFX). They share the StampS3 and can't be brought up piecemeal, so both the
// sidetone and display layers funnel through this idempotent init — first call
// wins, later calls are no-ops. (This is why the Cardputer screen uses
// M5.Display rather than a standalone LovyanGFX instance: M5GFX is the same
// LovyanGFX family, but M5.begin already claims the LCD pins.)
void cardputer_m5_begin();

#endif // DEVICE_CARDPUTER_ADV
