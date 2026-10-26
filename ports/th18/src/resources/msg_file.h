#pragma once
#include <cstdint>

// TH18: u32 entry_count followed by entry_count pairs, then instruction streams.
// Entries point into the same file; their second dword is not an instruction.
struct MsgEntry {
    int32_t offset;
    int32_t extra;
};

struct MsgInstr {
    uint16_t time;
    uint8_t type;
    uint8_t length;
    // length bytes follow; integers and floats are little-endian 32-bit values.
};

// 0043e550, with parameter layouts from thtk's TH18 version chain.
enum MsgInstrId : uint8_t {
    MSG_END = 0,
    MSG_LEFT_CREATE = 1,       // int slot
    MSG_RIGHT_CREATE = 2,      // int slot
    MSG_LEFT_REMOVE = 4,       // int slot
    MSG_RIGHT_REMOVE = 5,      // int slot
    MSG_TEXT_REMOVE = 6,
    MSG_LEFT_SPEAK = 7,        // int slot
    MSG_RIGHT_SPEAK = 8,       // int slot
    MSG_CENTER_SPEAK = 9,
    MSG_ALLOW_SKIP = 10,       // int enabled
    MSG_WAIT = 11,             // int frames
    MSG_ECL_SIGNAL = 12,
    MSG_LEFT_FACE = 13,        // int expression, int slot
    MSG_RIGHT_FACE = 14,       // int expression, int slot
    MSG_FIRST_LINE = 15,
    MSG_SECOND_LINE = 16,      // 15/16/17: encrypted zero-terminated Shift-JIS
    MSG_TEXT = 17,
    MSG_TEXT_HIDE = 18,
    MSG_BOSS_MUSIC = 19,
    MSG_BOSS_NAME = 20,        // int slot
    MSG_STAGE_FINISH = 21,
    MSG_MUSIC_FADE = 22,
    MSG_LEFT_EXIT = 23,
    MSG_RIGHT_EXIT = 24,
    MSG_TEXT_Y = 25,           // int y
    MSG_ALT_FONT = 26,
    MSG_MUSIC_FADE_TIME = 27,  // float seconds
    MSG_TEXT_POSITION = 28,    // float x, float y
    MSG_BUBBLE_STYLE = 29,     // int style
    MSG_SECOND_BOSS_CREATE = 31,
    MSG_SPEAKER = 32,          // int side
    MSG_PORTRAIT_DIM = 33,     // int side, int slot in the original dispatcher
    MSG_PORTRAIT_LIGHT = 34,   // int side, int slot
    MSG_STAGE_OVERLAY = 35,
    MSG_OPEN_SHOP = 36,
};

// 004419f0: each string restarts key=0x77, step=7; after each byte:
// key += step; step += 0x10. ASCII underscores become spaces after decoding.
// Actual scene: stage+0x1b0 -> 0x1d8-byte MSG object (0043d720/0043e550).
// 0043a8b0 initializes the HUD, not dialogue portraits.

