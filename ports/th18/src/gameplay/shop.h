#pragma once
#include <cstdint>

struct Shop;

// 004171b0 / 00417cc0。stage 是正在结算的关卡；商品使用独立 card_rng。
// 调用者负责关卡暂停与离店后的关卡切换，商店推进自己的 ANM 和输入。
Shop* shop_create(int stage);
bool shop_update(Shop* shop); // true：购买后的离店延时或 BLANK 确认已经结束
void shop_destroy(Shop* shop);

extern const int32_t g_card_price[14];
int32_t card_price(int32_t tier); // 00416dd0，按当前持有的 YAMAWARO 计算
