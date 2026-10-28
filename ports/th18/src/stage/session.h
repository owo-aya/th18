#pragma once
#include <cstdint>

// 关卡会话（原版全局 DAT_004cf2e4，0xd8 字节对象，FUN_004437b0 创建，
// 初始化线程体 0x4424e0）：创建玩家/录像/卡牌初始装備/关卡 ANM/场景对象/
// 敌机管理器/子弹管理器/激光管理器/得分对象/BGM/HUD，并注册各自 A/B 队列任务。
// 实现：stage.cpp（P1 与关卡对象/敌机管理器同文件，不新增构建项）。

struct StageSession;

// 创建会话并完成原版 0x4424e0 的初始化。
// stage：原版关卡编号（1..6，7=Extra）；flags：0=实际游玩，1=录像播放。
// 失败返回 nullptr（原版置 session+0xb0 |= 8）。
StageSession* stage_session_create(uint32_t stage, uint32_t flags);
void stage_session_load(StageSession* session);
void stage_session_start(StageSession* session);
bool stage_session_started();
bool stage_session_can_pause();
void stage_session_set_paused(bool paused);
void stage_session_set_wave(int wave);
void stage_screen_shake(int duration, int from, int to);
void stage_master_spark_shake(bool enhanced);

// 销毁会话（原版 FUN_004432c0）：停止任务、释放各管理器、重载成绩、停 BGM。
void stage_session_destroy(StageSession* session);

// 会话全局访问器（原版全局 DAT_004cf2e4）：create 赋值、destroy 清空；
// 其他模块要用会话数据时调这个，不要各自存副本。
StageSession* stage_session();
uint32_t stage_session_flags();
