#ifdef DEVICE_CARDPUTER_ADV

#include "keyboard.h"
#include "pins.h"
#include "platform_cardputer.h"
#include <M5Unified.h>

// TCA8418 keypad controller driver for the Cardputer ADV.
//
// Register init, the 1..80 keynum -> (row,col) decode, and the 4x14 keymap are
// adapted from the working Cardputer-ADV reference firmware
// (go-go-golems/esp32-s3-m5, matrix_console.c / tca8418.c) and ultimately from
// M5's own M5Cardputer-UserDemo-ADV HAL. The physical matrix is 7 rows x 8
// columns; the TCA8418 reports a key *number* which we fold back into M5's
// "picture" coordinates (4 rows x 14 columns) to index the legend.
//
// We read over M5.In_I2C, so there is no second I2C master on GPIO8/9.
// cardputer_m5_begin() (re)begins that bus on GPIO8/9 after M5.begin(): without
// that the TCA8418 NACKs and the keypad is dead, even though M5 detects the
// board correctly as CardputerADV. See the note there.

namespace {

// TCA8418 registers (subset).
constexpr uint8_t REG_CFG        = 0x01;
constexpr uint8_t REG_KEY_LCK_EC = 0x03;   // low nibble = event count
constexpr uint8_t REG_KEY_EVENT_A= 0x04;   // FIFO read: bit7=press, 6..0=keynum
constexpr uint8_t REG_GPIO_INT_STAT_1 = 0x11;
constexpr uint8_t REG_GPIO_INT_EN_1   = 0x1A;
constexpr uint8_t REG_KP_GPIO_1  = 0x1D;
constexpr uint8_t REG_GPI_EM_1   = 0x20;
constexpr uint8_t REG_GPIO_DIR_1 = 0x23;
constexpr uint8_t REG_GPIO_INT_LVL_1 = 0x26;
constexpr uint8_t REG_INT_STAT   = 0x02;

constexpr uint8_t CFG_GPI_IEN = 0x02;
constexpr uint8_t CFG_KE_IEN  = 0x01;

constexpr uint8_t ADDR = I2C_ADDR_TCA8418;   // 0x34

// Hard cap on consecutive FIFO reads in one drain. The TCA8418 event-count
// nibble maxes at 10 (its FIFO depth), so a healthy drain finishes well under
// this. The bound exists only so a wedged I2C read that keeps returning a
// non-zero count can never spin loop() forever and trip the watchdog (a
// suspected cause of the rare type-time reboot — plan Risk B; that crash could
// not be reproduced after this bound landed and is no longer tracked).
constexpr uint8_t FIFO_DRAIN_MAX = 32;

// Legend in M5 "picture" coordinates. '\0' = modifier / non-emitting; control
// codes: '\b' del, '\r' enter, '\t' tab.
const char KEY_FIRST[4][14] = {
    {'`','1','2','3','4','5','6','7','8','9','0','-','=','\b'},
    {'\t','q','w','e','r','t','y','u','i','o','p','[',']','\\'},
    {0,  0,  'a','s','d','f','g','h','j','k','l',';','\'','\r'},
    {0,  0,  0,  'z','x','c','v','b','n','m',',','.','/',' '},
};
const char KEY_SECOND[4][14] = {
    {'~','!','@','#','$','%','^','&','*','(',')','_','+','\b'},
    {'\t','Q','W','E','R','T','Y','U','I','O','P','{','}','|'},
    {0,  0,  'A','S','D','F','G','H','J','K','L',':','"','\r'},
    {0,  0,  0,  'Z','X','C','V','B','N','M','<','>','?',' '},
};

bool s_shift = false;
bool s_caps  = false;

void wr(uint8_t reg, uint8_t val) { M5.In_I2C.writeRegister8(ADDR, reg, val, 400000); }
uint8_t rd(uint8_t reg)          { return M5.In_I2C.readRegister8(ADDR, reg, 400000); }

// Decode a raw KEY_EVENT byte into picture coords + press state. Returns false
// for an empty/out-of-range event. Mirrors the reference decode exactly.
bool decode(uint8_t evt, uint8_t& row, uint8_t& col, bool& pressed) {
    if (evt == 0) return false;
    pressed = (evt & 0x80) != 0;
    uint8_t keynum = (uint8_t)(evt & 0x7F);
    if (keynum == 0) return false;
    keynum--;                                   // 0-based
    uint8_t r = keynum / 10;                    // internal 10-wide matrix
    uint8_t c = keynum % 10;
    col = (uint8_t)(r * 2 + (c > 3 ? 1 : 0));   // -> picture col 0..13
    row = (uint8_t)(c % 4);                     // -> picture row 0..3
    return (row < 4 && col < 14);
}

} // namespace

namespace keyboard {

void begin() {
    cardputer_m5_begin();                       // ensure M5 (and In_I2C) is up

    // GPIO defaults, all pins in keypad event mode, falling-edge int enables.
    for (uint8_t i = 0; i < 3; i++) {
        wr(REG_GPIO_DIR_1 + i, 0x00);
        wr(REG_GPI_EM_1   + i, 0xFF);
        wr(REG_GPIO_INT_LVL_1 + i, 0x00);
        wr(REG_GPIO_INT_EN_1  + i, 0xFF);
    }
    // Matrix size: 7 rows x 8 cols (low pins). row_mask=0x7F, col_mask=0xFF.
    wr(REG_KP_GPIO_1 + 0, 0x7F);                // rows
    wr(REG_KP_GPIO_1 + 1, 0xFF);                // cols 0..7
    wr(REG_KP_GPIO_1 + 2, 0x00);

    // Drain any stale events and clear interrupt status, then enable.
    for (uint8_t i = 0; i < FIFO_DRAIN_MAX && (rd(REG_KEY_LCK_EC) & 0x0F) != 0; i++) {
        if (rd(REG_KEY_EVENT_A) == 0) break;
    }
    (void)rd(REG_GPIO_INT_STAT_1);
    (void)rd(REG_GPIO_INT_STAT_1 + 1);
    (void)rd(REG_GPIO_INT_STAT_1 + 2);
    wr(REG_INT_STAT, 0x03);
    wr(REG_CFG, (uint8_t)(CFG_GPI_IEN | CFG_KE_IEN));

    // INT line is informational here (we poll the event count); park it as an
    // input so the open-drain line isn't left floating.
    pinMode(PIN_KBD_INT, INPUT_PULLUP);
}

bool read_char(char& out) {
    // Drain events until one yields a printable char (or the FIFO empties).
    // Press/release of modifiers are folded in and never returned. The drain is
    // bounded (FIFO_DRAIN_MAX) so a wedged event-count read can't spin forever.
    for (uint8_t i = 0; i < FIFO_DRAIN_MAX && (rd(REG_KEY_LCK_EC) & 0x0F) != 0; i++) {
        uint8_t evt = rd(REG_KEY_EVENT_A);
        uint8_t row, col; bool pressed;
        if (!decode(evt, row, col, pressed)) continue;

        if (row == 2 && col == 0) { s_shift = pressed; continue; }      // shift
        if (row == 2 && col == 1) { if (pressed) s_caps = !s_caps; continue; } // capslock
        if (!pressed) continue;                                          // emit on press only

        char base = KEY_FIRST[row][col];
        if (base == 0) continue;                                         // ctrl/opt/alt
        bool letter = (base >= 'a' && base <= 'z');
        bool shifted = letter ? (s_shift ^ s_caps) : s_shift;
        char c = shifted ? KEY_SECOND[row][col] : KEY_FIRST[row][col];
        if (c == 0) continue;
        out = c;
        return true;
    }
    return false;
}

} // namespace keyboard

#endif // DEVICE_CARDPUTER_ADV
