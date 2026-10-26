#pragma once
#include <cstdint>

#include "core/frame_input.h"

// 逻辑帧率，与原版一致。
constexpr int LOGIC_FPS = 60;

// 推进一个原版逻辑帧：输入 → 场景更新 → 各系统更新。
// 绘制在外部循环中消费当前状态，不推进逻辑。
void frame_update(const FrameInput& input);
uint32_t frame_count();
