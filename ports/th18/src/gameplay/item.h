#pragma once
#include <cstdint>

// 道具：TH18 的道具是 ANM 脚本驱动的通用对象（大小 0x683c），
// 由 FUN_0042d7d0(0x42d7d0) 按 ANM 名生成。
// 全局对象管理器 DAT_004cf2d0：链表头 +0x18c、尾 +0x190、数量 +0x198。
// 对象初始化 FUN_0042e100(0x42e100) 按 ANM 名建对象。
// 道具名（.rdata）："ItemCarryLily" 0x4b4c5c、"ItemCarryLily2" 0x4b4c4c
//   （LILY 卡 id 48 生成：计数每 3 个中的第 3 个用 ItemCarryLily2）。
// 拾取音效：se_item00.wav 0x4b8a10、se_item01.wav 0x4b8cd8。
constexpr int32_t kAnmObjectSize = 0x683c;
constexpr int32_t kItemSpawnParamsSize = 0x54;

// FUN_0042d7d0 把参数块逐字段抄进对象：
//   +0x00..+0x07 -> obj+0x12b4（x, y 浮点）
//   +0x08 -> obj+0x13bc        +0x0c -> obj+0x621c
//   +0x10 -> obj+0x623c        +0x14 -> obj+0x6220 / +0x6224 / +0x6228（三份）
//   +0x1c bit0 -> obj+0x761c 的 bit26；+0x14 > 999 -> obj 标志 bit30
//   +0x20..+0x4c -> obj+0x14b8.. 共 12 个 dword（交错写 +0x52e..）
//   +0x50 -> obj+0x6834
// LILY 卡（FUN_0040fe70）构造的实例：+0x04 = -32.0f（速度）、
//   +0x10 = (count%3!=2)*2+4（6/4）、+0x14 = count<10 ? count*300+4000 : 7000（价值）、
//   +0x20 = count<8 ? count/2 : 4（种类/等级）。
struct ItemSpawnParams {
    float x;         // +0x00
    float y;         // +0x04
    int32_t unk_08;  // +0x08
    int32_t unk_0c;  // +0x0c
    int32_t kind;    // +0x10
    int32_t value;   // +0x14 >999 时为大值道具
    int32_t unk_18;  // +0x18
    uint32_t flags;  // +0x1c bit0
    int32_t vec[12]; // +0x20..+0x4c
    int32_t unk_50;  // +0x50
};

extern void* g_anm_object_manager; // DAT_004cf2d0

void* object_create_from_anm(void* obj, const char* anm_name); // FUN_0042e100 @ 0x42e100

// 生成一个道具对象，返回对象指针（0 失败）。manager 通常为 DAT_004cf2d0。
void* item_spawn(void* manager, const char* anm_name, const ItemSpawnParams* params);
// FUN_0042d7d0 @ 0x42d7d0
