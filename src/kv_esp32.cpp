// kv_esp32.cpp — ESP32 implementation of kv::Store.
//
// This is a 1:1 passthrough wrapper over the Arduino-ESP32 Preferences library
// (NVS-backed key/value store).  Every kv::Store method forwards directly to
// the corresponding Preferences call with identical semantics, so the NVS
// layout and all stored values (callsign, wpm, bootlog history, …) are
// completely unchanged by this refactor.
//
// Implementation note on storage:
//   kv.h intentionally does not include <Preferences.h>, so the Preferences
//   object cannot be a member of kv::Store.  Instead a single file-scope
//   Preferences instance is used here.  This is safe because all callers in
//   this codebase (config.cpp "morse" namespace, main.cpp bootlog "boot"
//   namespace) always call begin/end in strictly non-overlapping pairs — they
//   never hold two different Store instances open simultaneously.  The pattern
//   is identical to how config.cpp used Preferences directly before this seam
//   was introduced.
//
// Guarded to compile only for ESP32-S3 device targets.
// A future nRF52 target supplies kv_nrf52.cpp (Phase 2).

#if defined(DEVICE_HELTEC_V4) || defined(DEVICE_HELTEC_V3) || defined(DEVICE_CARDPUTER_ADV)

#include "kv.h"
#include <Preferences.h>

// One Preferences instance shared across all kv::Store uses (begin/end pairs
// are always non-overlapping in this codebase — see note above).
static Preferences _prefs;

namespace kv {

bool Store::begin(const char* ns, bool read_only) {
    return _prefs.begin(ns, read_only);
}

void Store::end() {
    _prefs.end();
}

bool Store::isKey(const char* k) {
    return _prefs.isKey(k);
}

uint8_t Store::getUChar(const char* k, uint8_t def) {
    return _prefs.getUChar(k, def);
}

void Store::putUChar(const char* k, uint8_t v) {
    _prefs.putUChar(k, v);
}

uint32_t Store::getUInt(const char* k, uint32_t def) {
    return _prefs.getUInt(k, def);
}

void Store::putUInt(const char* k, uint32_t v) {
    _prefs.putUInt(k, v);
}

bool Store::getBool(const char* k, bool def) {
    return _prefs.getBool(k, def);
}

void Store::putBool(const char* k, bool v) {
    _prefs.putBool(k, v);
}

size_t Store::getString(const char* k, char* out, size_t cap) {
    return _prefs.getString(k, out, cap);
}

void Store::putString(const char* k, const char* v) {
    _prefs.putString(k, v);
}

size_t Store::getBytes(const char* k, void* out, size_t cap) {
    return _prefs.getBytes(k, out, cap);
}

void Store::putBytes(const char* k, const void* v, size_t len) {
    _prefs.putBytes(k, v, len);
}

void Store::remove(const char* k) {
    _prefs.remove(k);
}

} // namespace kv

#endif // DEVICE_HELTEC_V4 || DEVICE_CARDPUTER_ADV
