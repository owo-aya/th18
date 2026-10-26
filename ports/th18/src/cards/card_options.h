#pragma once

struct AbilityCard;

// 0045d5e0 rebuilds every option, then calls each card's slot 6 in list order.
// Reset every instance before recreating the list. The twelve slots live in Player.
void card_options_create(AbilityCard* card);
void card_options_reset(AbilityCard* card);

// After player movement/trail and option_a: option_b following, then slot 9 contact.
// The player path must not separately advance these options' ANM VMs.
void card_options_player_move(AbilityCard* card);
void card_options_shoot(AbilityCard* card, int timer_a, int timer_b);

// Advance owned visuals once per logic frame, including death/respawn frames.
void card_options_update(AbilityCard* card);
void card_options_unload(AbilityCard* card);
