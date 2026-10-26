#pragma once
#include <cstdint>

// 自机 SHT 格式（pl00..pl03.sht）。载入函数 FUN_0045a7a0(0x45a7a0)。
// 资源名表在 .rdata：
//   0x4b7088: pl00.sht / pl01.sht / pl02.sht / pl03.sht（4 项）
//   0x4b7098: pl00.anm / pl01.anm / pl02.anm / pl03.anm（4 项）
// 索引 = DAT_004cccf4 + DAT_004cccf8（两个全局的用途 P1 由选人流程确认）。
//
// 载入过程（从 FUN_0045a7a0 反编译）：
//   1. FUN_00402060 把 SHT 读入内存，指针存 player+0x47940；
//      若 DAT_00570920 有缓存则直接复用（Replay）。
//   2. 子程序入口重定位：文件偏移 0x00e0 起，每个 dword 为相对偏移，
//      改写为 sht_base + 值 + 0x180；子程序数 = 文件偏移 0x0002 的 uint16。
//   3. 指令区：每条 0x5c 字节，按首字节 opcode 查四张表：
//        +0x2c <- 0x4b4230[opcode]   +0x30 <- 0x4b4210[opcode]
//        +0x34 <- 0x4cf414[opcode]   +0x38 <- 0x4b41f0[opcode]
//   4. player+0x24 = 100（指令区内偏移 0x24 的整数写 100）
//   5. DAT_004ccd3c = sht[0x24] * sht[0x20]；DAT_004ccd40 = sht[0x24]
//   6. +0x477a4 + i*4（i = 0x10..0x1c）：sht[i] * DAT_004b9310（浮点换算表）
//
// 线程：FUN_00401530(&LAB_0045caa0)+FUN_00401180(0x17) 写 player+0x04；
//       FUN_00401530(&LAB_0045cac0)+FUN_00401230(0x1d) 写 player+0x08。
constexpr int32_t kShtInstructionSize = 0x5c;
constexpr uint32_t kShtSubroutineTable = 0x00e0;
constexpr uint32_t kShtRelocateBias = 0x180;
constexpr uint32_t kShtLoadBias = 0x180;

struct PlayerSht {
    uint8_t unknown_000[2];   // +0x000
    uint16_t sub_count;       // +0x002 子程序数
    uint8_t unknown_004[0xdc];// +0x004..+0x0df
    uint32_t sub_offset[1];   // +0x0e0 子程序相对偏移（载入时改为绝对地址）
};

struct ShtInstruction {
    uint8_t timer_count;       // +0x00 周期（>0 时按 timer % count == timer_slot 触发；0 = 每帧触发；<0 = 列表结束）
    uint8_t timer_slot;        // +0x01 触发的计时器余数
    int16_t damage;            // +0x02→攻击对象 +0x9c，溅射伤害基数
    float offset_x;            // +0x04 位置偏移（相对自机/子机）
    float offset_y;            // +0x08
    float unk_00c;             // +0x0c →攻击对象 +0xa0
    float unk_010;             // +0x10 →攻击对象 +0xa4
    float angle;               // +0x14 子弹朝向
    float speed;               // +0x18 子弹速度
    uint8_t unk_01c[4];        // +0x1c
    uint8_t option_index;      // +0x20 低 4 位 = 子机序号（0 = 自机），高 4 位 = 同调组序号
    uint8_t kind;              // +0x21 2 = 同调组限制；4/5/6 = 绝对坐标偏移
    int16_t anm_script;        // +0x22 子弹 ANM 脚本号
    int16_t se_id;             // +0x24 发射音效号（<0 = 无）
    uint8_t unk_026[4];        // +0x26
    uint8_t timer_b_count;     // +0x2a 第二计时器周期（非 0 时用 timerB % count == timer_b_slot）
    uint8_t timer_b_slot;      // +0x2b
    int32_t handler_a;         // +0x2c <- 0x4b4230[值]（发射时调用）
    int32_t handler_b;         // +0x30 <- 0x4b4210[值]
    int32_t handler_c;         // +0x34 <- 0x4cf414[值]
    int32_t handler_d;         // +0x38 <- 0x4b41f0[值]
    uint8_t unknown_03c[0x20]; // +0x3c..+0x5b
};
static_assert(sizeof(ShtInstruction) == kShtInstructionSize);

// 四张 opcode 分派表（每 opcode 一项）：0x4b4230（主）、0x4b4210、0x4cf414、0x4b41f0。
// 0x4b4230 前 8 项：0, 0x45f7b0, 0x45fc20, 0x460b60, 0x460da0, 0x4612d0, 0x461330, 0x4613b0。

// SHT 载入写出的 Power 参数在状态块 0x4cccdc（见 player.h）：
//   +0x60 power_max = sht[0x24] * sht[0x20]，+0x64 power_step = sht[0x24]。
