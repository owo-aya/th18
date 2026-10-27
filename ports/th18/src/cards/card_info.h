#pragma once
#include <cstdint>

#include "cards/card_id.h"

// 原版卡牌定义表：0x4c53c0，58 项 × 0x34 字节（13 个 dword）。
// 查表函数 FUN_00407d70(0x407d70)：按行内 +0x04 的 id 线性匹配，
// 命中返回该行地址，未命中返回 NULL 行（0x4c5f20，id=0x38）。
struct CardInfo {
    const char* key;     // +0x00 名称字符串（.rdata，如 0x4b49c8 "BLANK"）
    int32_t id;          // +0x04 卡 id；ability.txt 文本块下标 = id * 7
    int32_t unk_08;      // +0x08 0/1
    int32_t category;    // +0x0c 0..4
    int32_t tier;        // +0x10 0..13；FUN_00416f50 用 min_tier <= tier <= max_tier 过滤
    int32_t weight;      // +0x14 0..6；FUN_00416f50 按该值重复塞入候选数组
    int32_t stage_rule;  // +0x18 取得关：0=任意关，1..5=指定关，
                         //      6=第1/2关+随机，7=1..3关，8=2..4关，9=3..5关，
                         //      10=第4/5关+随机，11/12=通关标记（读进度 0x5f588+id 字节）
    int32_t unk_1c;      // +0x1c 0/1
    int32_t unk_20;      // +0x20 0/1
    int32_t unk_24;      // +0x24 0/1
    int32_t unk_28;      // +0x28 0/1
    int32_t text_name;   // +0x2c abcard.anm 大图 sprite（*_max，通常 256x320）；不是文本 id
    int32_t text_desc;   // +0x30 abcard.anm 小图 sprite（*_min，64x80）；409310 掉落卡牌使用此项
};

extern const CardInfo g_card_info[kCardIdCount];

const CardInfo* card_info_by_id(int32_t id); // FUN_00407d70 @ 0x407d70
