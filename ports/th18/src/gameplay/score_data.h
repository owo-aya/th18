#pragma once
#include <cstdint>

constexpr uint32_t kScoreCharacterSize = 0x130f0;
constexpr uint32_t kScoreStateSize = 0x3d0;

// On disk: TH81 header (0x18), encrypted LZSS, five CR v3 chunks and one ST v6
// chunk. Keep complete chunks, including fields not yet shown by a menu. The
// original 0xbf158 object also held pointers and a previous-save copy.
struct ScoreData {
    uint8_t character[5][kScoreCharacterSize];
    uint8_t state[kScoreStateSize];
};

// FUN_004637d0 initializes once. FUN_00413510 writes previous data to bak,
// writes current data to dat, then remembers current data for the next save.
void score_init();
ScoreData& score_data();
bool score_save();
// FUN_0046e490: persist the first achievement and queue its presentation once.
void score_mark_achievement(int id);

// FUN_00457870: insert at the first score <= the new score; return 0..9 or -1.
// score_div10 is the internal score, matching GameSession::score.
int score_insert_ranking(int player, int difficulty, uint32_t score_div10,
                         int stage, int continues, int64_t timestamp,
                         uint32_t slowdown, const char* name);
// FUN_00444650 / FUN_00458e40 / FUN_00443dc0. Call for live play, not replay.
void score_record_clear(int player, int difficulty, bool continued);
void score_record_practice(int player, int difficulty, int stage,
                           uint32_t score_div10, bool cleared);
void score_add_play_time(int player, uint64_t centiseconds);

// FUN_0042a320 starts one spell attempt, FUN_0042a780 counts a capture. Both
// update the selected character and CR[4]'s aggregate spell record. Stage
// Practice uses the normal counters; spell_practice means original mode 0x20.
// Do not call either function for replay. A failed spell needs no second write:
// its attempt was already counted by score_spell_start.
void score_spell_start(int player, int spell, bool spell_practice, const char* name);
void score_spell_capture(int player, int spell, bool spell_practice);
// FUN_00444650 updates this best score when a live Spell Practice finishes.
void score_record_spell_practice(int player, int spell, uint32_t score_div10);
