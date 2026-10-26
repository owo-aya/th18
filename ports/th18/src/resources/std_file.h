#pragma once
#include <cstdint>

// STD 背景场景文件（.std，TH18 用 thstd 的 option_version=2 布局）。
// 格式来自 thtk/thstd/thstd.h；运行时解析与播放见 stage 模块，本文件只放文件结构。
//
// 布局：
//   StdHeader                      文件头（0x90 字节）
//   uint32 offsets[nb_objects]     每对象头部偏移
//   各对象：ObjectHeader + Quad[]  （Quad 序列以 dword 0x0004FFFF 结束）
//   Face[]                         （以 object_id==0xFFFF 结束，再跟 4 个 0xFFFFFFFF）
//   ScriptInstr[]                  （以 size==0xFFFF 结束，再跟 5 个 0xFFFFFFFF）

struct StdHeader {
    uint16_t nb_objects;   // +0x00
    uint16_t nb_faces;     // +0x02
    uint32_t faces_offset; // +0x04 相对文件起点
    uint32_t script_offset;// +0x08
    uint32_t unknown;      // +0x0c 实测 0
    char anm_name[128];    // +0x10  背景用 ANM 名（如 "st01wl.anm"）
};

// 每个背景对象（屏幕上的大 quad 组合）。
struct StdObjectHeader {
    uint16_t id;      // +0x00
    uint16_t unknown; // +0x02
    float x, y, z;    // +0x04 位置
    float width;      // +0x10
    float height;     // +0x14
    float depth;      // +0x18
};

// 对象内部的 quad：一块可单独用脚本控制的贴图区域。
struct StdQuad {
    uint16_t unknown;      // +0x00 类型
    uint16_t size;         // +0x02 写侧固定 0x1c
    uint16_t script_index; // +0x04 引用 script 表
    uint16_t _padding;     // +0x06
    float x, y, z;         // +0x08 相对对象的位置
    float width;           // +0x14
    float height;          // +0x18
};

// 面（faces 段）：每个 quad 的一个实例，决定绘制顺序与偏移。
struct StdFace {
    uint16_t object_id; // +0x00  0xFFFF 表示结束
    uint16_t unknown1;  // +0x02
    float x, y, z;      // +0x04
};

// 脚本指令：time 为绝对时间，type 为指令号，size 为整条指令字节数（含 8 字节头）。
struct StdInstr {
    uint32_t time; // +0x00
    uint16_t type; // +0x04
    uint16_t size; // +0x06  0xFFFF = 结束
};

// STD 指令参数格式（thstd formats_v2，'S'/'C' = 整数/颜色，'f' = float）。
// 原版指令语义到动作的映射在 stage 模块补齐（摄像机/雾/对象可见性等）。

// 【关卡描述表】原版在一张静态表里按关卡索引取所有关卡资源名：
//   基址 0x4c9410，条目步长 0xd4，当前条目全局 DAT_004cf428，
//   选择代码 `DAT_004cf428 = &DAT_004c9410 + stage * 0xd4`（sub_444650/sub_455040 等）。
// 条目字段（实测 st01/st02 条目）：
//   +0x00 int32  关卡编号
//   +0x04 char*  STD 文件名（"st01.std"）
//   +0x08 char*  ECL 文件名（"st01.ecl"）
//   +0x0c char*  关卡 BGM 名（"th18_03"）
//   +0x10 char*  Boss BGM 名（"th18_04"）
//   +0x14..+0x20 char* 对话文件 stNNa.msg..stNNd.msg（按机体+shot 选）
//   +0x24 char*  关卡 logo ANM（"st01logo.anm"）
//   +0x28 int32  关卡 BGM 曲目 id（FUN_004546a0(0, id) 播放）
//   +0x2c int32  Boss/BGM 曲目 id（FUN_004546a0(1, id) 播放）
//   +0x34/+0x38 按 param*0x28 取的子表（路径/编号，sub_42a320 用）
//   +0x48/+0x4c 按 param*0x28 取的子表（sub_43e550 用）
//   +0x50/+0x54 按 param*0x28 取的子表（sub_43e550 用）
//   +0x58、+0x78、+0x7c、+0x80 单值（sub_4422c0/sub_43e550 用）
struct StageDescriptor {
    int32_t stage_id;         // +0x00
    const char* std_name;     // +0x04
    const char* ecl_name;     // +0x08
    const char* bgm_stage;    // +0x0c
    const char* bgm_boss;     // +0x10
    const char* msg_name[4];  // +0x14
    const char* logo_anm;     // +0x24
    int32_t bgm_stage_id;     // +0x28
    int32_t bgm_boss_id;      // +0x2c
    uint8_t rest[0xd4 - 0x30];
};

// 场景/舞台对象：全局 DAT_004cf2e0，由 sub_43b800 创建（0x2cc 字节）：
//   +0x004 task（背景更新，优先级 0x21）
//   +0x2c0 fronttr.anm 的 ANM 句柄（前景转场）
//   +0x4c 之后为对话/立绘 VM 指针数组（见 msg_file.h 的 sub_43a8b0）
// 游戏逻辑通过 DAT_004cf2e0 读写舞台状态（敌机/Boss/背景共用）。
struct StageObject;
enum StdInstrId : uint16_t {
    STD_INS_00 = 0,  // ""
    STD_INS_01 = 1,  // "SS"
    STD_INS_02 = 2,  // "fff"
    STD_INS_03 = 3,  // "SSfff"
    STD_INS_04 = 4,  // "fff"
    STD_INS_05 = 5,  // "SSfff"
    STD_INS_06 = 6,  // "fff"
    STD_INS_07 = 7,  // "f"
    STD_INS_08 = 8,  // "Cff"
    STD_INS_09 = 9,  // "SSCff"
    STD_INS_10 = 10, // "SSfffffffff"
    STD_INS_12 = 12, // "S"
    STD_INS_13 = 13, // "S"
    STD_INS_14 = 14, // "SSS"
    STD_INS_16 = 16, // "S"
    STD_INS_17 = 17, // "S"
    STD_INS_18 = 18, // "SSfff"
    STD_INS_19 = 19, // "S"
    STD_INS_21 = 21, // "SSf"
};
