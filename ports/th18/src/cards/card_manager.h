#pragma once
#include <cstdint>

#include "cards/ability_card.h"

// 卡牌管理器：全局 DAT_004cf298。创建入口 FUN_004082b0(0x4082b0)，
// 分配 0xd70 字节；同函数加载 ability.anm(资源号 0x1e)、abcard.anm(0x1f)、
// abmenu.anm(0x20)，并只在第一次创建时调用 FUN_004160b0 解析 ability.txt
// （文本块指针 DAT_004cf29c）。
struct CardList {
    int32_t unk_00;        // +0x00
    AbilityCard* first;    // +0x04
    int32_t unk_08;        // +0x08
    AbilityCard* last;     // +0x0c 尾部节点指针（FUN_00412e90 追加时更新）
};

struct CardManager {
    uint32_t flags_0;            // +0x00
    void* thread_a;              // +0x04 由 FUN_00401530(&LAB_00408a90)+FUN_00401180(0x16)
    void* thread_b;              // +0x08 由 FUN_00401530(&LAB_00408ab0)+FUN_00401230(0x33)
    int32_t anm_ability;         // +0x0c ability.anm 对象
    int32_t anm_abcard;          // +0x10 abcard.anm 对象
    int32_t anm_abmenu;          // +0x14 abmenu.anm 对象
    CardList list;               // +0x18
    int32_t count;               // +0x28 卡对象数（上限 0xff，工厂据此拒绝）
    int32_t count_kind8;         // +0x2c flags_50 & 8 的卡数
    int32_t count_kind40;        // +0x30 flags_50 & 0x40 的卡数
    int32_t count_other;         // +0x34 其余卡数
    AbilityCard* special;        // +0x38 flags_50 & 8 的唯一卡（MAINSHOT 类）
    uint8_t unk_3c[8];           // +0x3c..+0x43
    int32_t visual_previous;    // +0x44 显示计时器
    int32_t visual_frame;       // +0x48 发动闪烁相位
    float visual_time;          // +0x4c 随游戏速度推进
    uint32_t visual_latch;      // +0x50
    uint32_t visual_flags;      // +0x54
    int32_t slot_objects[256];   // +0x58 FUN_00408d00 遍历的 256 个槽
    uint8_t unk_458[0xc58 - 0x458]; // +0x458..+0xc57 未整理区
    float lily_multiplier;       // +0xc58 Lily 卡的道具价值倍率（FUN_0040fe70 读取）
    uint8_t unk_c5c[0xc84 - 0xc5c]; // +0xc5c..+0xc83
    int32_t owned[kCardIdCount]; // +0xc84 每卡 4 字节，工厂加入时置 1
    int32_t unk_d6c;             // +0xd6c
};

CardManager* card_manager_create(); // FUN_004082b0 @ 0x4082b0

// 创建并加入一张卡；返回加入后 count，失败返回 -1。
// 内部按 id 分配对应派生类对象、写 info 指针、按 flags 调用虚表槽 0/1，
// 一次性卡在该函数内销毁。被 0x407da0/0x417880/0x417cc0/0x445a80/0x462d20 等调用。
int32_t card_manager_add(CardManager* manager, CardId id, uint32_t flags); // FUN_00411460 @ 0x411460

// 把卡对象追加到 manager 内的链表（manager 偏移 +0x18 为列表头）。
void card_list_append(CardList* list, AbilityCard* node); // FUN_00412e90 @ 0x412e90

// 解析 ability.txt 到卡文本块：每卡 7 行 × 64 字节，行地址 = ((id * 7 + line) * 64)。
// 调用者仅 FUN_004082b0；文本块全局 DAT_004cf29c。
void parse_ability_text(uint8_t* text_block); // FUN_004160b0 @ 0x4160b0

// 卡对象/表现句柄刷新（遍历 256 个槽，DAT_0051f65c 对象管理器）；
// 随后按 manager 数值槽调用 FUN_00408de0 三次（参数槽 +0x2c/+0x30/+0x34）。
void card_manager_refresh(CardManager* manager, int32_t mode); // FUN_00408d00 @ 0x408d00

// 由 id 取卡表行（NULL 行兜底），等价 FUN_00407d70。
const CardInfo* card_info_by_id(int32_t id); // FUN_00407d70 @ 0x407d70

// 一局的装备与回调。初始卡在自机/SHT 创建后安装，销毁在自机之前。
void card_manager_begin_run(const int32_t* ids, int count, uint32_t flags = CardAdd_NoImmediate);
int32_t card_manager_acquire(CardId id, uint32_t flags);
void card_manager_clear_cards(); // 原407da0(0)：保留本局取得记录
void card_manager_end_run();
void card_manager_on_player_move();
void card_manager_handle_input(); // Player state1，Bomb之后、移动/射击之前
void card_manager_on_shoot(int timer_a, int timer_b);
void card_manager_on_hit();
uint32_t card_manager_on_death();
void card_manager_after_death_penalty();
bool card_manager_owned(CardId id);
bool card_manager_acquired(CardId id); // 原+c84本局取得记录，BLANK清卡后保留
int card_manager_equipped_count();
CardId card_manager_equipped_id(int index);
int card_manager_cooldown(CardId id);
bool card_manager_active(CardId id);
struct AnmResource;
AnmResource* card_manager_ability();
// 原411460按取得顺序保留独立实例；同ID的重复卡不合并成owned位。
int card_manager_card_count();
AbilityCard* card_manager_card_at(int index);
AbilityCard* card_manager_find(CardId id);
CardManager* card_manager();
struct AttackObject;
void card_manager_on_shot(AttackObject* shot);
void card_manager_player_reset();
void card_manager_shop_exit(); // 原408ad0/417880：离店后逐卡slot8
void card_manager_stage_start(); // 原4424e0：STD成功后，先13全卡，再0全卡
void card_manager_detach_player(); // 跨关保留实例/取得记录/values，卸旧Player的附属实体
void card_manager_options_reset();
void card_manager_player_update();
void card_manager_on_drops(const float* position, int* drop_counts);
int32_t card_manager_selected_id();
void card_manager_select_id(int32_t id);
int32_t card_manager_value(int32_t index);
void card_manager_set_value(int32_t index, int32_t value);
