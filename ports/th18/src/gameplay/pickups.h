#pragma once

#include <cstdint>

namespace stage {
// ItemManager 445820/445920 and its pickup-score PopupManager.
void pickups_create();
void pickups_destroy();
void pickup_popup(const float* position,int value,uint32_t color);
}
