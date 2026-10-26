#pragma once

struct SpellPracticeMenu;

// 46bca0/46c1d0/46cd50: stage, spell group, then difficulty.
SpellPracticeMenu* spell_practice_create(int player, bool returning_from_game);
bool spell_practice_update(SpellPracticeMenu* menu); // true after returning to title
void spell_practice_render(const SpellPracticeMenu* menu);
int spell_practice_player(const SpellPracticeMenu* menu);
void spell_practice_destroy(SpellPracticeMenu* menu);
