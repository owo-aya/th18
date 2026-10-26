#pragma once
#include <cstdint>

#include "cards/card_info.h"
#include "cards/card_id.h"

// 以原版虚表槽号组织卡牌效果调用。对应 AbilityCardInf::vftable = 0x4b6010，
// 21 槽：0..19 虚函数，20 为 scalar deleting destructor（MSVC 布局）。
//
// 已知槽语义（来自各派生类覆写函数与 FUN_00411460 的调用点）：
//   0  取得时即时效果（flags bit0==0 时调用；返回非 0 则该卡消耗、不加入链表）
//   1  装備时效果（flags bit1==1 时调用；返回非 0 同上消耗）
//   2  生成/发动（Lily 0x40fe70、Sun 0x40fb60、Kaname 0x40ebf0 等覆写）
//   13 复位（基类空操作）
//   14 set_value(int)：写 +0x38、+0x34(-1)、+0x3c(float)、清 +0x40 bit0/+0x44 bit0
//   15 value()：读 +0x38
//   16 标记位：返回 (+0x50 >> 5) & 1
//   17/18 基类返回 0
//   19 卸载/结束（基类空操作；Extend 0x409b70 置 +0x50 bit1）
//   20 析构
//
// 该结构保存原版对象头（0x54 字节，带扩展数据的卡为 0x58/0x5c）。
struct AbilityCard {
    void* vtable;            // +0x00 AbilityCardInf::vftable 或派生类虚表
    int32_t id;              // +0x04 卡 id（CardId）
    int32_t slot_index;      // +0x08 加入序号 = manager.count + 1
    AbilityCard* node_self;  // +0x0c 链表节点自指（原版 *(+0xc) = this）
    AbilityCard* node_prev;  // +0x10
    AbilityCard* node_next;  // +0x14
    int32_t unk_18;          // +0x18 工厂清零
    int32_t unk_1c;          // +0x1c 工厂清零
    int32_t unk_20;          // +0x20
    int32_t unk_24;          // +0x24
    float unk_28;            // +0x28 状态计时浮点值（对应+0x24整数帧）
    int32_t unk_2c;          // +0x2c
    uint32_t flags_30;       // +0x30 bit0
    int32_t unk_34;          // +0x34
    int32_t count;           // +0x38 槽 14/15 读写的数量值
    float count_f;           // +0x3c 槽 14 写入的浮点（= count）
    uint32_t flags_40;       // +0x40
    uint32_t flags_44;       // +0x44 bit0/bit1
    int32_t param_48;        // +0x48 整数参数（Lily 卡用作基数）
    const CardInfo* info;    // +0x4c 卡表行指针（工厂内 card_info_by_id 写入）
    uint32_t flags_50;       // +0x50 bit0 装備/生效, bit1 消耗, bit5 槽 16 标记
    int32_t extra_54;        // +0x54 Lily 累计计数 / Mukade 等扩展字段
    int32_t extra_58;        // +0x58 扩展字段（仅 0x58/0x5c 分配时存在）
};

// 工厂 flags（FUN_00411460 第三参）：
//   bit0 = 1 时跳过槽 0，仅装備
//   bit1 = 1 时额外调用槽 1，返回非 0 则该卡被销毁（一次性卡）
enum CardAddFlags : uint32_t {
    CardAdd_NoImmediate = 1,
    CardAdd_CheckEquip = 2,
};

inline CardId card_id(const AbilityCard* card) { return static_cast<CardId>(card->id); }
