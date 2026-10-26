#pragma once
#include <cstdint>

// TH18 Boss。Boss 不是独立类，而是带 0x40000000 标志的敌机：
// 0x0042d7d0（CreateEnemy）在 SpawnArgs.size > 999 时给 enemy->flag 置 0x40000000。
//
// 已确认：
//   - 创建：ECL 300/301/304/305/321（0x004369e0）传入子程序名（关卡脚本里是 "Boss"/"MBoss"），
//     子程序名在子程序数据 +0x14。
//   - 血量/计时：ECL 变量 -9965/-9964/-9963 读 EnemyManager +0x3c/+0x40/+0x44（0x00437360）。
//   - 击破/退场：敌机脚本最后走 "BossDead" / "MBossDead" / "BossEscape" / "MBossBDead"
//     子程序（.rdata 0x4b64a4 / 0x4b64b0 / 0x4b64b8 / 0x4b64c8；"BossDead" 在 0x00434c52
//     附近被引用）。
//   - 符卡 6 的边界激光生成敌机脚本 "BossCard6_et"/"BossCard6_et2"（0x00439020）。
//   - enemy.cpp:3702 的断言串 "m_bossRMesh" 在解释器 0x00430d30 内 0x00436079 引用。
//
// 未定位（P1 继续）：阶段切换、计时器字段与推进、符卡编号/数量、超时、Boss 血条与阶段标记。
namespace stage {

struct Enemy;

// Boss 判定（flag & 0x40000000）。
bool is_boss(const Enemy& enemy);

// Boss 血量槽（EnemyManager +0x3c/+0x40/+0x44，对应 ECL 变量 -9965/-9964/-9963）。
int32_t boss_hp0();
int32_t boss_hp1();
int32_t boss_hp2();

}  // namespace stage
