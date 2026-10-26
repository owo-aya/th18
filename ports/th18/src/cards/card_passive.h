#pragma once
#include <cstdint>

struct AbilityCard;
struct AttackObject;

bool card_passive_acquire(AbilityCard* card, uint32_t flags);
uint32_t card_passive_on_death(AbilityCard* card, uint32_t handled);
void card_passive_on_hit(AbilityCard* card);
void card_passive_after_death_penalty(AbilityCard* card);
void card_passive_player_reset(AbilityCard* card);
void card_passive_options_reset(AbilityCard* card);
void card_passive_player_update(AbilityCard* card);
void card_passive_on_shot(AbilityCard* card, AttackObject* shot);
void card_passive_update(AbilityCard* card);
void card_passive_on_drops(AbilityCard* card, const float* position, int* drop_counts);
void card_passive_reset(AbilityCard* card);
void card_passive_stage_start(AbilityCard* card);
void card_passive_unload(AbilityCard* card);
void card_passive_detach_player(AbilityCard* card);

// Original 0042ed40 increments this once for each nonzero enemy damage event.
void card_passive_damage_event();
// MUKADE's original virtual slots 14/15 store the shared damage-event counter.
void card_passive_set_value(AbilityCard* card, int32_t value);
int32_t card_passive_value(AbilityCard* card);
// SHT hit callback installed by MAINSHOT_SP (0040c440 -> 0040c4f0).
int card_passive_shot_hit(AttackObject* shot);
