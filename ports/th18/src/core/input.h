#pragma once
#include <cstdint>

#include "core/frame_input.h"

// 动作位。由原版 FUN_00401860（XInput）、FUN_00401c50（键盘）、FUN_00401650（DInput）
// 合成原始位后经键位配置表映射得到；方向位 0x10/0x20/0x40/0x80 在各设备下一致。
enum : uint32_t {
    ACTION_SHOT = 0x0001,   // Z
    ACTION_BOMB = 0x0002,   // X
    ACTION_SLOW = 0x0008,   // Shift
    ACTION_UP = 0x0010,
    ACTION_DOWN = 0x0020,
    ACTION_LEFT = 0x0040,
    ACTION_RIGHT = 0x0080,
    ACTION_MENU_CANCEL = 0x0100,
    ACTION_CARD = 0x0400,   // C
    ACTION_CARD_CYCLE = 0x0800, // D
    ACTION_MENU_OK = 0x80000,
};

// 输入状态机（原版 DAT_004ca210 起，FUN_00404e90 推进）。
// held/prev 由设备采样写入，其余由状态机计算。
struct InputState {
    uint32_t held = 0;      // +0x000 当前按住位
    uint32_t prev = 0;      // +0x004 上一帧按住位
    uint32_t repeat = 0;    // +0x008 按住超过 25 帧后的重复触发位
    uint32_t pressed = 0;   // +0x00c 按下沿
    uint32_t released = 0;  // +0x010 释放沿
    int32_t hold[32] = {};  // +0x014 每个动作的按住帧数
    int32_t extra[32] = {}; // +0x114 每个动作的第二计数（原版同增，用途待确认）
    uint32_t first_repeat = 0; // +0x22c 按住超过 7 帧的位
};

void input_reset();
// 对应 FUN_00404e90：写入本帧原始位并计算沿与重复触发。
void input_update(uint32_t raw_bits);
const InputState& input_state();
FrameInput input_frame();
void input_battle_update(uint32_t raw_bits);
void input_battle_reset();
void input_battle_clear();
const InputState& input_battle_state();
FrameInput input_battle_frame();
