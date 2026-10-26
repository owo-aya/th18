#pragma once
#include <cstdint>

#include "cards/card_manager.h"

// 00413810 / 00413af0 mode 0 at (448,100,0), used by the pause menu.
struct PauseCards;
PauseCards* pause_cards_create();
void pause_cards_destroy(PauseCards* page);
bool pause_cards_update(PauseCards* page);

// 卡牌菜单 / 商店相关的原版入口（P0 只定位入口与状态归属，具体状态机在 P2 展开）。
// 资源：ability.anm(0x1e)、abcard.anm(0x1f)、abmenu.anm(0x20)，均由
// FUN_004082b0(0x4082b0) 载入卡牌管理器 +0x0c/+0x10/+0x14。

// 卡牌菜单状态机：FUN_00413af0(0x413af0)，8121 字节，switch 状态 0..0xe。
// 使用卡表（0x407d70）、卡对象（0x407da0）、卡池选择（0x416540/0x4168a0/0x416940/
// 0x416b70/0x416ba0）、菜单对象（0x412f20/0x413180/0x4132b0/0x413390/0x4133d0/
// 0x413470/0x4134d0/0x413950）。由匿名块 0x416090 调用。
//        购卡成功音效 se_cardget.wav(0x4b8a90)，切换音效 se_changeitem.wav(0x4b8cf8)。
void card_menu_update(void* menu); // FUN_00413af0 @ 0x413af0

// 卡当前关是否可出现：读卡表 +0x18 stage_rule，按当前关 DAT_004cccdc 与
// 进度标记 DAT_004cf41c + 0x5f588 + id 判断。返回 bool。
bool card_available(const CardInfo* info); // FUN_00416e10 @ 0x416e10

// 从候选卡中按权重抽一张：param2..param3 限定 tier；跳过已持有、tier 为 0 或 6、
// 已在数组中的卡；weight 与未解锁卡的 5 倍权重都不入参；结果写入 *out。
// 返回 false 表示无候选。
bool card_pick_random(const CardInfo** out, int32_t min_tier, int32_t max_tier,
                      const CardInfo** exclude, int32_t exclude_count, int32_t stage);
// FUN_00416f50 @ 0x416f50

// 构建卡牌菜单/选卡列表：复制输入参数到菜单 +0x1d0，写 +0x1bc/+0x1d8 等；
// 依次尝试 MANEKI(0x26) 的额外抽取，再收集未持有且可得的卡，最后把数量写入
// +0xa2c、数组写入 +0xa30；持有 BLANK(0) 时 +0xe34 = 1。
void card_menu_build(void* menu, const uint8_t* params8); // FUN_004171b0 @ 0x4171b0

// 单卡菜单/装備切换：调用工厂 0x411460、卡对象 0x407da0、0x416d50、0x418ce0 等。
// 被 0x417cc0（商店列表）与 0x445a80（购卡流程）调用。
void card_slot_menu(void* menu); // FUN_00417880 @ 0x417880

// 商店状态机与关卡衔接见 gameplay/shop.h（0x417cc0 / 0x443860）。

// 初始装備持ち込み：0x407da0 + 0x411460 + 0x418ce0。
void card_equip_initial(void* ctx); // FUN_00462d20 @ 0x462d20

// 卡牌解锁/进度标记读写见 gameplay/score_data.h（card_unlock_flag）。
