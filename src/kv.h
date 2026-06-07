#pragma once
#include <stdint.h>
#include <stddef.h>

// kv.h — namespaced key/value persistence seam.
//
// Wraps whatever the current platform uses for non-volatile storage so that
// config.cpp and the main.cpp bootlog ring do not call platform-specific APIs
// directly.  The interface is modelled on the Arduino-ESP32 Preferences class
// because that is the current backing store; call sites remain textually
// identical (same method names, same argument types, same return types).
//
// Each target provides exactly one implementation file:
//   src/kv_esp32.cpp  — thin passthrough wrapper over Preferences / NVS.
//   src/kv_nrf52.cpp  — LittleFS-backed implementation (Phase 2).
//
// The NVS namespace string and every key name are unchanged by this refactor,
// so existing stored values (callsign, wpm, bootlog history, …) survive a
// reflash that preserves NVS.

namespace kv {

class Store {
public:
    // Open the namespace for reading (read_only=true) or read+write.
    // Returns true on success. Must be called before any get/put.
    bool begin(const char* ns, bool read_only);

    // Close the namespace. Always pair with a successful begin().
    void end();

    // Returns true if the key exists in the open namespace.
    bool isKey(const char* k);

    uint8_t  getUChar (const char* k, uint8_t  def);
    void     putUChar (const char* k, uint8_t  v);

    uint32_t getUInt  (const char* k, uint32_t def);
    void     putUInt  (const char* k, uint32_t v);

    bool     getBool  (const char* k, bool     def);
    void     putBool  (const char* k, bool     v);

    // getString copies the value into out[0..cap-1] (NUL-terminated) and
    // returns the number of bytes written (excluding NUL), or 0 on miss.
    size_t   getString(const char* k, char*       out, size_t cap);
    void     putString(const char* k, const char* v);

    // getBytes / putBytes work on arbitrary binary blobs.
    // getBytes returns the number of bytes actually copied, or 0 on miss.
    size_t   getBytes (const char* k, void*       out, size_t cap);
    void     putBytes (const char* k, const void* v,   size_t len);

    // Remove a single key from the open namespace.
    void     remove   (const char* k);

private:
    // The backing store is declared in the implementation file to keep
    // platform headers out of this header.  A void* opaque pointer would work
    // too, but the Preferences object is small enough that we forward-declare
    // the implementation's private type and include the right header in the
    // .cpp only.  For now the impl simply puts the Preferences member in the
    // .cpp as a file-scope companion — see kv_esp32.cpp for the pattern.
    //
    // Nothing is stored here on purpose: each platform impl may choose a
    // different layout (NVS handle, file descriptor, etc.).  The Store object
    // is used as a value type (stack-allocated local); the impl .cpp manages
    // its own backing storage indexed by the Store instance pointer or as a
    // singleton per namespace.  See kv_esp32.cpp for the concrete approach.
};

} // namespace kv
