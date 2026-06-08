// kv_nrf52.cpp — nRF52840 implementation of kv::Store, backed by
// Adafruit_LittleFS + InternalFileSystem (bundled in the Adafruit nRF52
// Arduino core — no extra lib_deps needed).
//
// There is no NVS-equivalent namespaced key/value API in the nRF52 Arduino
// core, so we model each kv::Store "namespace" as a single flat file under
// LittleFS (path "/<ns>.kv") holding a small serialized key→value map. The
// whole map is loaded into RAM on begin() and flushed back to the file on
// every put*()/remove() (this codebase's namespaces are tiny — a handful of
// scalar config values and one ~144-byte bootlog blob — so "rewrite the whole
// file on every write" is simple, robust, and cheap enough; LittleFS is
// wear-levelled and the write rate here is "operator changes a setting" or
// "once per boot", not a hot loop).
//
// On-disk format (deliberately simple — not a general serializer):
//   [u8 count]
//   repeated `count` times:
//     [u8 name_len][name bytes (no NUL)][u8 type][u16 value_len][value bytes]
//   type: 0=UChar(1B) 1=UInt(4B LE) 2=Bool(1B) 3=String(NUL-terminated)
//         4=Bytes(raw)
//
// Semantics matf  ch the ESP32 Preferences wrapper closely enough for this
// codebase's call sites (config.cpp, main.cpp bootlog ring): getX returns the
// default when the key is absent OR stored as a different type; putX
// overwrites/creates; isKey/remove operate on the name regardless of type.
//
// Guarded to compile only for the RAK4631 target. See kv_esp32.cpp for the
// ESP32 (Preferences/NVS) sibling.

#if defined(DEVICE_RAK4631) || defined(DEVICE_WIO_TRACKER_L1)

#include "kv.h"
#include <Arduino.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>

using namespace Adafruit_LittleFS_Namespace;

namespace {

constexpr size_t MAX_ENTRIES   = 32;   // generous: config "morse" has ~10 keys
constexpr size_t MAX_NAME_LEN  = 15;
constexpr size_t MAX_VALUE_LEN = 256;  // bootlog "log" blob is BOOTLOG_N*5 = 80B

enum Type : uint8_t { T_UCHAR = 0, T_UINT = 1, T_BOOL = 2, T_STRING = 3, T_BYTES = 4 };

struct Entry {
    char    name[MAX_NAME_LEN + 1];
    uint8_t type;
    uint16_t len;
    uint8_t value[MAX_VALUE_LEN];
};

// One in-RAM map per open Store, addressed by namespace string. We only ever
// have one Store open at a time in this codebase (see kv_esp32.cpp's note —
// the same non-overlapping-begin/end invariant holds here), so a single
// file-scope working set mirrors that wrapper's approach.
struct Map {
    char    ns[24];
    bool    open = false;
    bool    dirty = false;
    bool    read_only = false;
    size_t  n = 0;
    Entry   entries[MAX_ENTRIES];
};

Map g_map;

// InternalFS must be mounted exactly once before any open/read/write, or
// LittleFS asserts `block < lfs->cfg->block_count` (block_count is 0 until
// begin() configures the geometry) and abort()s the firmware. begin() mounts
// the existing filesystem, auto-formatting on first use if unmounted.
bool g_fs_ready = false;
bool ensure_fs() {
    if (g_fs_ready) return true;
    g_fs_ready = InternalFS.begin();
    return g_fs_ready;
}

void path_for(const char* ns, char* out, size_t cap) {
    snprintf(out, cap, "/%s.kv", ns);
}

// Find an entry by name; returns nullptr if absent.
Entry* find(const char* k) {
    for (size_t i = 0; i < g_map.n; i++)
        if (strncmp(g_map.entries[i].name, k, MAX_NAME_LEN) == 0) return &g_map.entries[i];
    return nullptr;
}

// Find-or-create. Returns nullptr if the table is full and k is new.
Entry* find_or_create(const char* k) {
    Entry* e = find(k);
    if (e) return e;
    if (g_map.n >= MAX_ENTRIES) return nullptr;
    e = &g_map.entries[g_map.n++];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, k, MAX_NAME_LEN);
    e->name[MAX_NAME_LEN] = 0;
    return e;
}

void load() {
    g_map.n = 0;
    char path[32];
    path_for(g_map.ns, path, sizeof(path));
    File f = InternalFS.open(path, FILE_O_READ);
    if (!f) return;                       // no file yet — empty namespace
    uint8_t count = 0;
    if (f.read(&count, 1) != 1) { f.close(); return; }
    for (uint8_t i = 0; i < count && g_map.n < MAX_ENTRIES; i++) {
        uint8_t name_len = 0, type = 0;
        uint16_t value_len = 0;
        if (f.read(&name_len, 1) != 1) break;
        Entry& e = g_map.entries[g_map.n];
        memset(&e, 0, sizeof(e));
        size_t nl = name_len > MAX_NAME_LEN ? MAX_NAME_LEN : name_len;
        if ((size_t)f.read(e.name, nl) != nl) break;
        if (name_len > nl) f.seek(f.position() + (name_len - nl)); // skip overflow
        e.name[nl] = 0;
        if (f.read(&type, 1) != 1) break;
        if (f.read(&value_len, 2) != 2) break;
        e.type = type;
        size_t vl = value_len > MAX_VALUE_LEN ? MAX_VALUE_LEN : value_len;
        if ((size_t)f.read(e.value, vl) != vl) break;
        if (value_len > vl) f.seek(f.position() + (value_len - vl));
        e.len = (uint16_t)vl;
        g_map.n++;
    }
    f.close();
}

void save() {
    if (g_map.read_only || !g_map.dirty) return;
    char path[32];
    path_for(g_map.ns, path, sizeof(path));
    // Write to a temp file then rename, so a power-loss mid-write can't corrupt
    // the namespace (LittleFS rename is atomic).
    char tmp[36];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    InternalFS.remove(tmp);
    File f = InternalFS.open(tmp, FILE_O_WRITE);
    if (!f) return;
    uint8_t count = (uint8_t)g_map.n;
    f.write(&count, 1);
    for (size_t i = 0; i < g_map.n; i++) {
        Entry& e = g_map.entries[i];
        uint8_t name_len = (uint8_t)strlen(e.name);
        f.write(&name_len, 1);
        f.write((const uint8_t*)e.name, name_len);
        f.write(&e.type, 1);
        f.write((const uint8_t*)&e.len, 2);
        f.write(e.value, e.len);
    }
    f.close();
    InternalFS.remove(path);
    InternalFS.rename(tmp, path);
    g_map.dirty = false;
}

} // namespace

namespace kv {

bool Store::begin(const char* ns, bool read_only) {
    if (!ensure_fs()) return false;     // FS mount failed — caller sees defaults
    strncpy(g_map.ns, ns, sizeof(g_map.ns) - 1);
    g_map.ns[sizeof(g_map.ns) - 1] = 0;
    g_map.read_only = read_only;
    g_map.dirty = false;
    g_map.open = true;
    load();
    return true;
}

void Store::end() {
    save();
    g_map.open = false;
}

bool Store::isKey(const char* k) {
    return find(k) != nullptr;
}

uint8_t Store::getUChar(const char* k, uint8_t def) {
    Entry* e = find(k);
    if (!e || e->type != T_UCHAR || e->len < 1) return def;
    return e->value[0];
}

void Store::putUChar(const char* k, uint8_t v) {
    if (g_map.read_only) return;
    Entry* e = find_or_create(k);
    if (!e) return;
    e->type = T_UCHAR;
    e->len = 1;
    e->value[0] = v;
    g_map.dirty = true;
    save();
}

uint32_t Store::getUInt(const char* k, uint32_t def) {
    Entry* e = find(k);
    if (!e || e->type != T_UINT || e->len < 4) return def;
    uint32_t v;
    memcpy(&v, e->value, 4);
    return v;
}

void Store::putUInt(const char* k, uint32_t v) {
    if (g_map.read_only) return;
    Entry* e = find_or_create(k);
    if (!e) return;
    e->type = T_UINT;
    e->len = 4;
    memcpy(e->value, &v, 4);
    g_map.dirty = true;
    save();
}

bool Store::getBool(const char* k, bool def) {
    Entry* e = find(k);
    if (!e || e->type != T_BOOL || e->len < 1) return def;
    return e->value[0] != 0;
}

void Store::putBool(const char* k, bool v) {
    if (g_map.read_only) return;
    Entry* e = find_or_create(k);
    if (!e) return;
    e->type = T_BOOL;
    e->len = 1;
    e->value[0] = v ? 1 : 0;
    g_map.dirty = true;
    save();
}

size_t Store::getString(const char* k, char* out, size_t cap) {
    Entry* e = find(k);
    if (!e || e->type != T_STRING || cap == 0) { if (cap) out[0] = 0; return 0; }
    size_t n = e->len;
    if (n > cap - 1) n = cap - 1;
    memcpy(out, e->value, n);
    out[n] = 0;
    return n;
}

void Store::putString(const char* k, const char* v) {
    if (g_map.read_only) return;
    Entry* e = find_or_create(k);
    if (!e) return;
    size_t n = strlen(v);
    if (n > MAX_VALUE_LEN - 1) n = MAX_VALUE_LEN - 1;
    e->type = T_STRING;
    memcpy(e->value, v, n);
    e->value[n] = 0;
    e->len = (uint16_t)(n + 1);
    g_map.dirty = true;
    save();
}

size_t Store::getBytes(const char* k, void* out, size_t cap) {
    Entry* e = find(k);
    if (!e || e->type != T_BYTES) return 0;
    size_t n = e->len;
    if (n > cap) n = cap;
    memcpy(out, e->value, n);
    return n;
}

void Store::putBytes(const char* k, const void* v, size_t len) {
    if (g_map.read_only) return;
    Entry* e = find_or_create(k);
    if (!e) return;
    size_t n = len;
    if (n > MAX_VALUE_LEN) n = MAX_VALUE_LEN;
    e->type = T_BYTES;
    memcpy(e->value, v, n);
    e->len = (uint16_t)n;
    g_map.dirty = true;
    save();
}

void Store::remove(const char* k) {
    if (g_map.read_only) return;
    for (size_t i = 0; i < g_map.n; i++) {
        if (strncmp(g_map.entries[i].name, k, MAX_NAME_LEN) == 0) {
            // shift the tail down — order doesn't matter, but keep it stable
            for (size_t j = i; j + 1 < g_map.n; j++) g_map.entries[j] = g_map.entries[j + 1];
            g_map.n--;
            g_map.dirty = true;
            save();
            return;
        }
    }
}

} // namespace kv

#endif // DEVICE_RAK4631 || DEVICE_WIO_TRACKER_L1
