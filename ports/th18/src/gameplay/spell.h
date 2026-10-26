#pragma once

#include <cstdint>

struct Thread;
struct AnmVm;

// DAT_004cf2c0, 0xc0 bytes (004424e0). Spell tasks are A0x1f/B0xc;
// DAT_004cf40c's A0xc/B0x51 object is the separate result/replay controller.
struct SpellManager {
    uint32_t flags0;
    Thread* logic;
    Thread* render;
    AnmVm* background;       // +0x0c
    AnmVm* caption;          // +0x10 ascii0
    AnmVm* name_vm;          // +0x14 text2
    AnmVm* counter;          // +0x18 ascii1
    AnmVm* circle;           // +0x1c effect13
    int32_t timer_previous;  // +0x20
    int32_t timer;           // +0x24, scaled gameplay frames
    float timer_fraction;
    int32_t timer_reset;
    uint32_t timer_flags;
    char name[64];          // +0x34 original SJIS
    int32_t id;             // +0x74
    uint32_t flags;         // +0x78: 1 active, 2 capture valid, 8 no bonus decay
    int32_t bonus;          // +0x7c
    int32_t initial_bonus;  // +0x80
    int32_t time_limit;     // +0x84
    int32_t timing_index;   // +0x88 per-stage replay spell timing entry
    int32_t frame_count;    // +0x8c unscaled frames
    int32_t last_frames;    // +0x90
    int32_t field94;
    double started_seconds;
    double excluded_seconds;
    int32_t recorded_time;  // +0xa8 original packed seconds/centiseconds
    float position[3];     // +0xac eased boss position
    uint8_t pad0b8[8];
};

SpellManager* spell_manager();
void spell_create();
void spell_destroy();
// 0042a320, ECL522/537/538/539. background_index selects the stage descriptor.
void spell_start(int id, const char* sjis_name, int time_limit, int background_index);
void spell_end();          // 0042a780
// 00409ad0 and the identical Bomb/Miss grace-period branch.
void spell_on_sun_card();
bool spell_active_after_grace();
void spell_disable_circle(); // 0042d670
