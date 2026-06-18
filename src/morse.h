#pragma once
#include <stdint.h>
#include <vector>

// Morse code: text <-> timed key-state, plus a timing decoder.
//
// Standard timing in "units" where 1 unit = 1200 / wpm milliseconds:
//   dit on = 1, dah on = 3, intra-char gap = 1, inter-char gap = 3,
//   inter-word gap = 7.
//
// Farnsworth timing: characters are keyed at a fast "character wpm" while the
// inter-character (3-unit) and inter-word (7-unit) gaps are stretched so the
// overall word rate matches a slower "overall wpm". Uses the standard ARRL
// formula. When character wpm == overall wpm it reduces to plain timing.

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
    // wpm = overall (effective) speed. char_wpm = Farnsworth character speed;
    // 0 (default) or any value <= wpm means plain timing at wpm.
    void begin(uint8_t wpm = 13, uint8_t char_wpm = 0);
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
    uint16_t unit_      = 92;          // dit/dah/intra-char unit (char speed)
    uint16_t ichar_gap_ = 276;         // inter-character gap (3*unit, stretched)
    uint16_t iword_gap_ = 644;         // inter-word gap (7*unit, stretched)
};

// Timing decoder: feed it the key state on every tick; it emits decoded
// characters as gaps reveal element/character/word boundaries.
class Decoder {
public:
    // wpm = overall speed, char_wpm = Farnsworth character speed (0/<=wpm means
    // plain timing). Thresholds are derived from both so the decoder tracks the
    // sender's actual gaps — feed it the speeds the fox announces over the air.
    void begin(uint8_t wpm = 13, uint8_t char_wpm = 0);
    // Call frequently with the current key state and wall clock. Returns a
    // decoded character when one completes, else 0. May return ' ' for words.
    char update(bool key_down, uint32_t now_ms);

    // Edge-mode entry (docs/edge-events.md): feed one *completed* segment with
    // its true TX-measured duration — on=true is a key-down element (dit/dah),
    // on=false is a key-up gap. Decoded characters (and word spaces) queue up;
    // drain them with take_char() in a loop. Elements for the dit/dah view
    // still surface via take_element(). Reuses the begin() thresholds, so the
    // decode tracks the fox's announced wpm/char_wpm exactly. Independent of the
    // update() time-based state machine — a node uses one path or the other.
    void feed_segment(bool on, uint16_t dur_ms);
    char take_char();                 // next decoded char/space, 0 if none
    void flush();                     // finalize at a hard boundary: emit '?' for
                                      // a damaged/half-built char, then resync

    // Poison the character the current packet loss falls within. The damaged
    // character resolves to exactly ONE '?' when it closes at its next char-gap
    // (or on flush): the partial elements collected so far are discarded and
    // every element until that char-gap is absorbed, so a truncated element-run
    // can never surface as a confident WRONG letter (e.g. a dit dropped from
    // "..." (S) leaving ".." (I)) and the word's letter count is preserved. The
    // RX layer calls this at every detected seq gap instead of guessing — see
    // docs/edge-events.md "Loss handling".
    void poison();

    // Per-element side-channel for live dit/dah display: returns the single
    // element ('.' or '-') classified on the most recent key-up edge, then
    // clears it, so the hunter can scroll one element at a time as it arrives
    // (rather than a whole character's worth at once). Returns 0 if none new.
    char take_element() { char e = new_elem_; new_elem_ = 0; return e; }

private:
    char classify();                  // turn collected elements into a char

    uint16_t dah_ms_     = 184;       // ON >= this -> dah, else dit
    uint16_t chargap_ms_ = 184;       // OFF >= this -> end of character
    uint16_t wordgap_ms_ = 460;       // OFF >= this -> word space
    bool     last_down_  = false;
    uint32_t edge_ms_    = 0;
    bool     have_edge_  = false;
    char     elems_[8]   = {0};
    char     new_elem_   = 0;         // freshest element for live dit/dah display
    uint8_t  n_elems_    = 0;
    bool     pending_    = false;     // have undecoded elements waiting on a gap
    bool     poisoned_   = false;     // current char damaged by loss -> one '?'

    // Edge-mode output queue (feed_segment -> take_char). A single segment can
    // emit up to two chars (letter + word space), so results are buffered.
    char     outq_[8]    = {0};
    uint8_t  oq_head_    = 0;
    uint8_t  oq_tail_    = 0;
    char     last_out_   = 0;         // suppress consecutive word spaces
    void     push_char(char c);
};

} // namespace morse
