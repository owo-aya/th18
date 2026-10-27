#pragma once

void bomb_begin_run(int player);
void bomb_end_run();
// 0x420420 的库存、对话、敌机与正在发动状态判定。
bool bomb_available();
bool bomb_try_activate();
bool bomb_activate_from_card(); // 原410e60：不检查/不扣Bomb库存，仍受正在发动/+a0排它限制
bool bomb_active();
int bomb_elapsed_frames(); // Original Bomb+0x38, item auto-collect window.
// 0x420360 的 +0xa0：自动Bomb/六文钱之庇护占用同一发动窗口。
void bomb_set_card_active(bool active);
