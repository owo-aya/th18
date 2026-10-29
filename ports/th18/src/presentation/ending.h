#pragma once

// 42bab0/42b650: e01..e12 MSG controller, A0x24; script-created VMs use A0x22.
// Index is DAT_004ccde8. Normal endings chain to staff1..staff4 by difficulty.
void ending_create(int ending_index, bool viewing = false);
bool ending_active();
bool ending_finished(); // 42bb30 flags+1c bit8, consumed by the title menu.
void ending_destroy(); // 42bb10/42b8b0
void ending_finish_fade_shutdown();

