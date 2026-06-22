#include "morse.h"
#include <Arduino.h>
#include <string.h>

namespace morse {

namespace {
struct Entry { char c; const char* p; };
const Entry TABLE[] = {
    {'A', ".-"},   {'B', "-..."}, {'C', "-.-."}, {'D', "-.."},  {'E', "."},
    {'F', "..-."}, {'G', "--."},  {'H', "...."}, {'I', ".."},   {'J', ".---"},
    {'K', "-.-"},  {'L', ".-.."}, {'M', "--"},   {'N', "-."},   {'O', "---"},
    {'P', ".--."}, {'Q', "--.-"}, {'R', ".-."},  {'S', "..."},  {'T', "-"},
    {'U', "..-"},  {'V', "...-"}, {'W', ".--"},  {'X', "-..-"}, {'Y', "-.--"},
    {'Z', "--.."},
    {'0', "-----"},{'1', ".----"},{'2', "..---"},{'3', "...--"},{'4', "....-"},
    {'5', "....."},{'6', "-...."},{'7', "--..."},{'8', "---.."},{'9', "----."},
    {'.', ".-.-.-"},{',', "--..--"},{'?', "..--.."},{'/', "-..-."},
    {'=', "-...-"}, {'+', ".-.-."},
};
constexpr size_t TABLE_N = sizeof(TABLE) / sizeof(TABLE[0]);
} // namespace

const char* pattern(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (size_t i = 0; i < TABLE_N; i++)
        if (TABLE[i].c == c) return TABLE[i].p;
    return nullptr;
}

// ---- Timing ---------------------------------------------------------------

// Compute the element unit and the (Farnsworth-stretched) inter-character and
// inter-word gaps, all in ms, for an overall speed `wpm` and character speed
// `char_wpm`. Shared by the Player (to send) and the Decoder (to classify) so
// both agree on the same gap durations.
static void farns_gaps(uint8_t wpm, uint8_t char_wpm,
                       uint16_t& unit, uint16_t& ichar, uint16_t& iword) {
    if (char_wpm < wpm) char_wpm = wpm;        // never key slower than overall
    unit = unit_ms(char_wpm);                  // dit/dah/intra-char at char speed
    if (char_wpm == wpm) {                      // plain timing
        ichar = (uint16_t)(3 * unit);
        iword = (uint16_t)(7 * unit);
    } else {
        // ARRL Farnsworth: total padding delay (ms) added across one standard
        // word to bring the average rate from char_wpm down to wpm. A standard
        // word holds 19 units of gap (inter-char + inter-word), so distribute
        // ta over them: 3/19 to each inter-char gap, 7/19 to each inter-word.
        //   ta = (60*C - 37.2*S) / (C*S)  [seconds], C=char wpm, S=overall wpm
        float ta = (60.0f * char_wpm - 37.2f * wpm) / (char_wpm * (float)wpm);
        float ta_ms = ta * 1000.0f;
        ichar = (uint16_t)(3 * unit + ta_ms * 3.0f / 19.0f);
        iword = (uint16_t)(7 * unit + ta_ms * 7.0f / 19.0f);
    }
}

// ---- Player ---------------------------------------------------------------

void Player::begin(uint8_t wpm, uint8_t char_wpm) {
    farns_gaps(wpm, char_wpm, unit_, ichar_gap_, iword_gap_);
    finished_ = true;
    down_ = false;
}

void Player::start(const char* text) {
    segs_.clear();
    const uint16_t u = unit_;
    for (const char* s = text; *s; s++) {
        const char* p = pattern(*s);
        if (!p) {                       // space / unsupported -> word gap
            segs_.push_back({false, iword_gap_});
            continue;
        }
        for (const char* e = p; *e; e++) {
            segs_.push_back({true, (uint16_t)((*e == '-') ? 3 * u : u)});
            segs_.push_back({false, u});            // intra-char gap
        }
        if (!segs_.empty()) segs_.back().ms = ichar_gap_;        // -> char gap
    }
    idx_ = 0;
    started_at_ = millis();
    seg_start_  = started_at_;
    pos_ms_     = 0;
    total_ms_   = 0;
    for (const Seg& s : segs_) total_ms_ += s.ms;
    finished_ = segs_.empty();
    down_ = !finished_ && segs_[0].on;
}

void Player::update(uint32_t now_ms) {
    if (finished_) { down_ = false; pos_ms_ = total_ms_; return; }
    while (now_ms - seg_start_ >= segs_[idx_].ms) {
        seg_start_ += segs_[idx_].ms;
        idx_++;
        if (idx_ >= segs_.size()) { finished_ = true; down_ = false; pos_ms_ = total_ms_; return; }
    }
    down_ = segs_[idx_].on;
    pos_ms_ = now_ms - started_at_;
}

void Player::resync(uint32_t now_ms, uint32_t pos_ms) {
    if (segs_.empty()) return;             // nothing loaded -> no-op
    if (pos_ms >= total_ms_) {             // at/after the end -> finish the render
        idx_        = segs_.size();
        finished_   = true;
        down_       = false;
        pos_ms_     = total_ms_;
        started_at_ = now_ms - total_ms_;
        seg_start_  = now_ms;
        return;
    }
    // Walk the cumulative timeline to the segment that contains pos_ms.
    uint32_t cum = 0;
    size_t   i   = 0;
    while (i < segs_.size() && cum + segs_[i].ms <= pos_ms) {
        cum += segs_[i].ms;
        i++;
    }
    idx_        = i;
    started_at_ = now_ms - pos_ms;         // position 0 maps to this wall time
    seg_start_  = started_at_ + cum;       // wall time the active segment began
    pos_ms_     = pos_ms;
    finished_   = false;
    down_       = segs_[idx_].on;
}

uint16_t Player::elems_elapsed() const {
    // Count "on" segments whose key-down has started: those at index < idx_ have
    // fully sounded, and segs_[idx_] (the active one) is counted too if it is an
    // element currently sounding. When finished, idx_ == segs_.size() so every
    // element is counted (the total). Recomputed from idx_, so a resync() seek
    // yields the correct count immediately.
    uint16_t c = 0;
    size_t end = idx_ < segs_.size() ? idx_ + 1 : segs_.size();
    for (size_t i = 0; i < end; i++)
        if (segs_[i].on) c++;
    return c;
}

// ---- Decoder --------------------------------------------------------------

void Decoder::begin(uint8_t wpm, uint8_t char_wpm) {
    uint16_t unit, ichar, iword;
    farns_gaps(wpm, char_wpm, unit, ichar, iword);
    // Split each boundary at the midpoint of the two durations it separates:
    //   dit (1u) vs dah (3u)            -> 2u
    //   intra-char gap (1u) vs char gap -> (1u + ichar)/2
    //   char gap vs word gap            -> (ichar + iword)/2
    // With plain timing this reduces to the old 2u / 5u thresholds; with
    // Farnsworth the gap thresholds scale up with the stretched gaps.
    dah_ms_     = (uint16_t)(2 * unit);
    chargap_ms_ = (uint16_t)((unit + ichar) / 2);
    wordgap_ms_ = (uint16_t)((ichar + iword) / 2);
    last_down_ = false;
    edge_ms_   = 0;
    n_elems_   = 0;
    pending_   = false;       // reused as "char already emitted for this gap"
    have_edge_ = true;        // reused as "word already emitted for this gap"
}

char Decoder::classify() {
    elems_[n_elems_] = 0;
    for (size_t i = 0; i < TABLE_N; i++)
        if (strcmp(TABLE[i].p, elems_) == 0) return TABLE[i].c;
    return '?';
}

char Decoder::update(bool key_down, uint32_t now_ms) {
    if (key_down != last_down_) {
        uint32_t dur = now_ms - edge_ms_;
        if (last_down_) {                       // ON segment just ended
            if (n_elems_ < 7) {
                char e = (dur >= dah_ms_) ? '-' : '.';
                elems_[n_elems_++] = e;
                new_elem_ = e;                  // expose for live dit/dah scroll
            }
        } else {                                // gap just ended, new element
            pending_   = false;                 // new char in progress
            have_edge_ = false;
        }
        last_down_ = key_down;
        edge_ms_   = now_ms;
        return 0;
    }

    if (!key_down && n_elems_ > 0 && !pending_) {
        if (now_ms - edge_ms_ >= chargap_ms_) { // char-gap boundary
            elems_[n_elems_] = 0;
            char c = classify();
            n_elems_ = 0;
            pending_ = true;                     // char emitted for this gap
            return c;
        }
    }
    if (!key_down && pending_ && !have_edge_) {
        if (now_ms - edge_ms_ >= wordgap_ms_) {  // word-gap boundary
            have_edge_ = true;                   // word emitted
            return ' ';
        }
    }
    return 0;
}

// ---- Decoder: edge-mode (duration-fed) ------------------------------------

void Decoder::push_char(char c) {
    uint8_t nt = (uint8_t)((oq_tail_ + 1) % sizeof(outq_));
    if (nt == oq_head_) return;          // full: drop (caller should drain often)
    outq_[oq_tail_] = c;
    oq_tail_ = nt;
    last_out_ = c;
}

char Decoder::take_char() {
    if (oq_head_ == oq_tail_) return 0;
    char c = outq_[oq_head_];
    oq_head_ = (uint8_t)((oq_head_ + 1) % sizeof(outq_));
    return c;
}

void Decoder::flush() {
    // Abandon the in-progress character at a hard boundary (silence timeout /
    // resync). If it was damaged by a loss (poisoned) or only half-built, surface
    // a single '?' so a cut-off character isn't silently dropped. The decoded
    // output queue is intentionally NOT cleared (it's drained every loop) so that
    // '?' survives to be displayed.
    if (poisoned_ || n_elems_ > 0) push_char('?');
    poisoned_ = false;
    n_elems_  = 0;
    last_out_ = 0;
    new_elem_ = 0;
}

void Decoder::poison() {
    // Mark the current character undecodable; it resolves to one '?' when it
    // closes (feed_segment at a char-gap, or flush). Discard partial elements so
    // a truncated run can't classify as a wrong letter, and mark the dit/dah view.
    poisoned_ = true;
    n_elems_  = 0;
    new_elem_ = '?';
}

void Decoder::feed_segment(bool on, uint16_t dur_ms) {
    if (on) {                                    // a keyed element: dit or dah
        if (poisoned_) return;                   // damaged char: absorb, show one '?'
        if (n_elems_ < 7) {
            char e = (dur_ms >= dah_ms_) ? '-' : '.';
            elems_[n_elems_++] = e;
            new_elem_ = e;                       // live dit/dah scroll
        }
        return;
    }
    // a gap. A char-gap (or longer) closes the current character; a word-gap
    // also yields a space. Thresholds are the same midpoints begin() computed.
    if (dur_ms >= chargap_ms_) {
        if (poisoned_) {                         // damaged char resolves to one '?'
            push_char('?');
            poisoned_ = false;
            n_elems_ = 0;
        } else if (n_elems_ > 0) {
            elems_[n_elems_] = 0;
            push_char(classify());
            n_elems_ = 0;
        }
    }
    if (dur_ms >= wordgap_ms_ && last_out_ != ' ') {
        push_char(' ');
    }
}

} // namespace morse
