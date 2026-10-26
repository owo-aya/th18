#pragma once
#include <cstdint>

namespace stage {
void hud_create(int difficulty, int player);
void hud_refresh_cards();
void hud_select_card();
void hud_spell_start();
void hud_spell_end();
void hud_boss_segment(int boss,int index,float ratio,uint32_t color);
void hud_boss_spells(int count);
void hud_update();
void hud_render();
void hud_destroy();
}
