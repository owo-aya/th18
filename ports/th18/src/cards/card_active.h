#pragma once

struct AbilityCard;

void card_active_activate(AbilityCard* card);
void card_active_update(AbilityCard* card);
void card_active_on_death(AbilityCard* card);
void card_active_detach_player(AbilityCard* card);
void card_active_unload(AbilityCard* card);
void card_active_stage_start(AbilityCard* card);
