#pragma once
#include <stdint.h>
#include <vector>

// Morse code: text <-> timed key-state, plus a timing decoder.
//
// Standard timing in "units" where 1 unit = 1200 / wpm milliseconds:
//   dit on = 1, dah on = 3, intra-char gap = 1, inter-char gap = 3,
//   inter-word gap = 7.

namespace morse {

// Returns the dot/dash pattern for a character (e.g. "A" -> ".-"), uppercased.
// Returns nullptr for space or unsupported characters.
const char* pattern(char c);

inline uint16_t unit_ms(uint8_t wpm) { return (uint16_t)(1200u / wpm); }

// Non-blocking player: converts text to a timeline of key-down/up segments and
// reports the current key state as time advances. Drive sidetone and/or radio
// from down().
class Player {
public:
    void begin(uint8_t wpm = 13);
    void start(const char* text);     // build timeline and begin playback
    void update(uint32_t now_ms);     // advance based on wall clock
    bool down() const { return down_; }
    bool finished() const { return finished_; }

private:
    struct Seg { bool on; uint16_t ms; };
    std::vector<Seg> segs_;
    size_t   idx_       = 0;
    uint32_t seg_start_ = 0;
    bool     down_      = false;
    bool     finished_  = true;
    uint16_t unit_      = 92;          // ~13 wpm
};

// Timing decoder: feed it the key state on every tick; it emits decoded
// characters as gaps reveal element/character/word boundaries.
class Decoder {
public:
    void begin(uint8_t wpm = 13);
    // Call frequently with the current key state and wall clock. Returns a
    // decoded character when one completes, else 0. May return ' ' for words.
    char update(bool key_down, uint32_t now_ms);

private:
    char classify();                  // turn collected elements into a char

    uint16_t unit_       = 92;
    bool     last_down_  = false;
    uint32_t edge_ms_    = 0;
    bool     have_edge_  = false;
    char     elems_[8]   = {0};
    uint8_t  n_elems_    = 0;
    bool     pending_    = false;     // have undecoded elements waiting on a gap
};

} // namespace morse
