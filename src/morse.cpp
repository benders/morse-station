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

// ---- Player ---------------------------------------------------------------

void Player::begin(uint8_t wpm) {
    unit_ = unit_ms(wpm);
    finished_ = true;
    down_ = false;
}

void Player::start(const char* text) {
    segs_.clear();
    const uint16_t u = unit_;
    for (const char* s = text; *s; s++) {
        const char* p = pattern(*s);
        if (!p) {                       // space / unsupported -> word gap
            segs_.push_back({false, (uint16_t)(7 * u)});
            continue;
        }
        for (const char* e = p; *e; e++) {
            segs_.push_back({true, (uint16_t)((*e == '-') ? 3 * u : u)});
            segs_.push_back({false, u});            // intra-char gap
        }
        if (!segs_.empty()) segs_.back().ms = (uint16_t)(3 * u);  // -> char gap
    }
    idx_ = 0;
    seg_start_ = millis();
    finished_ = segs_.empty();
    down_ = !finished_ && segs_[0].on;
}

void Player::update(uint32_t now_ms) {
    if (finished_) { down_ = false; return; }
    while (now_ms - seg_start_ >= segs_[idx_].ms) {
        seg_start_ += segs_[idx_].ms;
        idx_++;
        if (idx_ >= segs_.size()) { finished_ = true; down_ = false; return; }
    }
    down_ = segs_[idx_].on;
}

// ---- Decoder --------------------------------------------------------------

void Decoder::begin(uint8_t wpm) {
    unit_ = unit_ms(wpm);
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
            if (n_elems_ < 7)
                elems_[n_elems_++] = (dur >= 2u * unit_) ? '-' : '.';
        } else {                                // gap just ended, new element
            pending_   = false;                 // new char in progress
            have_edge_ = false;
        }
        last_down_ = key_down;
        edge_ms_   = now_ms;
        return 0;
    }

    if (!key_down && n_elems_ > 0 && !pending_) {
        if (now_ms - edge_ms_ >= 2u * unit_) {  // char-gap boundary
            char c = classify();
            n_elems_ = 0;
            pending_ = true;                     // char emitted for this gap
            return c;
        }
    }
    if (!key_down && pending_ && !have_edge_) {
        if (now_ms - edge_ms_ >= 5u * unit_) {   // word-gap boundary
            have_edge_ = true;                   // word emitted
            return ' ';
        }
    }
    return 0;
}

} // namespace morse
