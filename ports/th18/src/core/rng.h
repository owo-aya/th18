#pragma once
#include <cstdint>

// 原版游戏随机数发生器（TH18 自带 16 位 xorshift 变体，非 MSVC rand、非 MT19937）。
// 状态全局：原版 DAT_004cf288 = { u16 state; u16 pad; u32 counter }。
// 原版函数：
//   FUN_00402740(state) -> u32 = (a<<16)|b        整数随机（ECL 特殊变量 -10000）
//   FUN_004027D0(state) -> float [0,1)            0..1 浮点（ECL 特殊变量 -10001）
//   FUN_00402810(state) -> float [-1,1)           -1..1 浮点（ECL 特殊变量 -9987）
// ECL 的随机类指令通过 GetVarInt(FUN_00437360) 读取这三个特殊变量，不直接调 RNG。

struct Rng {
    uint16_t state;
    uint16_t pad;
    uint32_t counter;
};

Rng& rng();             // 全局发生器（原版 DAT_004cf288）
// 卡池抽选用独立发生器（原版 DAT_004cf280）；FUN_00453640 用 timeGetTime() 低 16 位
// 先播种 card_rng，再把同一 seed 复制给 rng()。
Rng& card_rng();
void rng_reset(uint16_t seed);
uint32_t rng_next();    // FUN_00402740
uint32_t rng_next(Rng& random);
uint16_t rng_next16();  // FUN_004026d0，412随机朝向分支仅消耗一次16位步进
float rng_float01();    // FUN_004027D0
float rng_float_pm1();  // FUN_00402810
