#pragma once
#include <cstdint>

// 每逻辑帧输入，使用原版动作位。位定义见 core/input.h。
// 实机输入与录像输入进入同一处理路径。
struct FrameInput {
    uint32_t held = 0;      // 本帧按住
    uint32_t pressed = 0;   // 本帧按下沿
    uint32_t released = 0;  // 本帧释放沿
    uint32_t repeat = 0;    // 按住超过 25 帧后每 8 帧一次
};
