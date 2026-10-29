#pragma once
#include <cstdint>

struct Shop;

// 004171b0 / 00417cc0。stage 是正在结算的关卡；商品使用独立 card_rng。
// 会话为商店注册独立 A0x0c 任务；ANM 按原队列推进，价格由 B0x51 绘制。
Shop* shop_create(int stage);
void shop_set_paused(Shop* shop, bool paused); // 00458680/00458e40: clear/reopen, retain goods.
bool shop_update(Shop* shop); // true：购买后的离店延时或 BLANK 确认已经结束
void shop_destroy(Shop* shop);

extern const int32_t g_card_price[14];
int32_t card_price(int32_t tier); // 00416dd0，按当前持有的 YAMAWARO 计算
