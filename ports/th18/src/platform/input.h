#pragma once
#include <windows.h>
#include <cstdint>

// 采样物理设备，返回合成后的原始动作位（键盘 + 手柄 + 摇杆）。
// 原版：FUN_00401860（XInput）、FUN_00401c50（键盘）、FUN_00401650（DInput 摇杆）。
uint32_t input_sample_device();
