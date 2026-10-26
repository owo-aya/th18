#pragma once
#include <cstdint>

// TH18 碰撞/命中判定。
//
// 已定位的相关全局：
//   DAT_004cf410  Player*（自机）
//   DAT_004cf2d0  EnemyManager*（敌机链表 +0x18c）
//   DAT_004cf2b4  bullet::Manager*（敌弹数组 +0x3450）
//   DAT_004cf3f4  laser::Manager*（激光链表 +0x14）
//   DAT_004cccdc  GameState 本体；+0x24 = 难度 0..3
//
// 已定位的判定辅助：
//   0x00404080  把坐标约束到给定矩形（0,0,224,384/448 之类），返回是否在界内；
//               参数 (out3, out2, pos3, f, f, f, f, f)，0x00439xxx 用它做激光出界判定。
//   0x0043a2b0  由角度（float）算方向向量，参数 (out3, angle, scale)。
//   0x00439020  遍历激光链表：对出界/越界的激光在交点处生成敌机脚本 "BossCard6_et"。
//   0x004391a0  同族遍历（读 DAT_004cf3f4+0x14 链表，见 0x0043916d/0x004392ed）。
//
// 未定位（P1 继续）：敌弹 vs 自机命中、擦弹、自机攻击 vs 敌机、Bomb、卡牌区域。
namespace collision {

// 点是否在矩形内/求矩形最近点：0x00404080。
int32_t clamp_to_rect(float* out3, float* out2, const float* pos3,
                      float left, float right, float top, float bottom);

// 角度 → 方向向量：0x0043a2b0。
void angle_vector(float* out3, float angle, float scale);

// 激光越界检查 + 生成 "BossCard6_et"：0x00439020。
void laser_boundary_check();

// 同族激光检查：0x004391a0。
void laser_boundary_check2();

}  // namespace collision
