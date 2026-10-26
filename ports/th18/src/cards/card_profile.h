#pragma once
#include "cards/card_id.h"

// Same ScoreData owner as rankings and progress. Init reads it once and loads
// ability text; later title entries keep the process's current cards and decks.
void card_profile_init();
bool card_profile_unlocked(CardId id);
bool card_profile_mark_acquired(CardId id); // true on first unlock; no disk write
// FUN_00444650's live full-game stage 6/7 clear branch. Caller excludes replay
// (session +0xd0), Stage/Spell Practice (mode &0x30), and Extra's early exit when
// DAT_004c5f8c >= 0. miss_count is DAT_004ccd18, incremented by actual death
// FUN_0045d090. Updates cards, deck 0 capacity, clear notices and achievements;
// returns newly unlocked CardId bits for the caller's card notifications.
// First achievements enter the trophy queue; this call does not write to disk.
uint64_t card_profile_record_clear(int player, int difficulty, bool extra,
                                   bool continued, bool has_blank, int miss_count);
int card_profile_deck_count(int bank);
CardId card_profile_deck_id(int bank, int slot);
void card_profile_set_deck_card(int bank, int slot, CardId id);
const CardId* card_profile_order(); // 0x4b3600 共57项；卡库取前56项，携带选择含末尾Null
bool card_profile_cleared(int player, int difficulty);
uint64_t card_profile_high_score(int player, int difficulty);

// ability.txt 原始 Shift-JIS；每卡第一行名称，随后六行说明。
const char* card_name(CardId id);
const char* card_description(CardId id, int line);
