#pragma once
#include <cstdint>
#include "player/player.h"

#include "projectiles/bullet.h"

struct Thread;
struct AnmVm;
enum ThreadStatus : int32_t;

// TH18 敌机（EnemyInf）与敌机管理器。
//
// EnemyInf：RTTI type_info @0x4ce5e4，vftable @0x4b654c
//   [0] 0x00430d30  每帧执行 ECL（trampoline：ADD ECX,0x122c; JMP 0x00430d40）
//   [1] 0x00437360  GetVarInt(this, varId)
//   [2] 0x00437b20  GetVarPtr(this, varId)
//   [3] 0x00437c90
//   [4] 0x004387e0
//   [5] 0x0042d220  scalar deleting destructor
//   构造 0x0042e100（先调基类 0x0042ce10），对象 0x683c 字节。
//
// 创建链路：ECL 300/301/304/305/321 → 0x004369e0 填参数块 → 0x0042d7d0 CreateEnemy。
// 按 ID 查：0x0042d500。每帧表现刷新（淡入淡出/特效槽）：0x0042fe80。
// 全局：DAT_004cf2d0 = EnemyManager*，DAT_004cf410 = Player*，DAT_004cccdc = GameState。
namespace stage {

struct EclThread;

// ECL 创建敌机时传入的参数块（0x004369e0 在栈上填，0x0042d7d0 消费）。
// 偏移已验证：
//   0x00 float x, 0x04 float y          → enemy+0x12b4/+0x12b8
//   0x08 int32                          → enemy+0x12bc
//   0x0c int32                          → enemy+0x621c
//   0x10 int32                          → enemy+0x623c（0x0042d110 初始化的子对象）
//   0x14 int32    >999 时 enemy 获得 0x40000000 标志（Boss/大敌机）
//                                      → 同时写入 enemy+0x6220/+0x6224/+0x6228
//   0x18 位0 → enemy 标志 0x80000（相对坐标 / 镜像）
//   0x1c 位0 → enemy 标志 0x4000000
//   0x20..0x4c 12 个 dword → enemy+0x14b8..+0x14e4（ECL 变量 A..H 及其余变量区）
//   0x50 int32  父敌机 ID             → enemy+0x6834
struct SpawnArgs {
    float x;
    float y;
    int32_t arg2;
    int32_t score;
    int32_t drop_kind;
    int32_t health;  // >999 → 大敌机生命标志
    uint32_t flags;
    uint32_t flags2;
    int32_t vars[12];
    int32_t parent_id;
};

struct Enemy;
struct EnemyInterrupt {
    int32_t health=-1;
    int32_t time;
    const char* health_sub;
    const char* time_sub;
};

// 敌机链表节点（内嵌在 Enemy+0x1514，manager+0x18c/+0x190 指向节点）。
// 0x0042d7d0 插入：node[0]=this，node[1]=next，node[2]=prev，尾节点 +0xc 更新为最新节点。
// 0x0042df50 遍历：next = node[1]，enemy = node[0]。
struct EnemyListLink {
    Enemy* self;           // +0x00 = Enemy*
    EnemyListLink* next;   // +0x04
    EnemyListLink* prev;   // +0x08
    EnemyListLink* field0c;  // +0x0c 尾节点更新为最新节点
};

// 敌机对象。只列出已确认字段；中间空隙用显式 padding 保持偏移。
struct Enemy {
    void** vtable;  // +0x0000
    uint8_t pad004[0x0c - 0x04];
    EclThread* thread;  // +0x000c ECL 线程对象（0x0048d920/0x0048dc20 写 thread+4/+8）
    uint8_t pad010[0x1018 - 0x10];
    void* ecl_set;  // +0x1018 ECL 文件集（ECL 线程持有，解析子程序偏移用）
    uint8_t pad101c[0x1218 - 0x101c];
    void* ecl_res;  // +0x1218 当前关卡 ECL 资源（构造时取 manager+0x188）
    uint8_t pad121c[0x1270 - 0x121c];
    float x;  // +0x1270
    float y;  // +0x1274

    // 0x0050 起的大块区域：ECL VM 与战斗状态（构造 memset 0x5600 字节，到 0x682c）。
    // 其中 0x12a8/0x12b4/0x12b8/0x12cc/0x12d0/0x12d4/0x12f8/0x12fc/0x1310/0x1314/0x1318
    // 是 GetVarInt 能直接读的浮点状态；0x133c..0x1348 是 4 个 24.0f 初始值；
    // 0x1350 起是 16 个表现对象句柄（0x0042fe80 按 enemy+0x1510 的淡出值刷新 alpha）。
    float z; // +0x1278
    uint8_t pad127c[0x12b4-0x127c];
    Motion absolute_motion; // +0x12b4
    Motion relative_motion; // +0x12f8
    float hurtbox[2]; // +0x133c
    float hitbox[2]; // +0x1344
    uint8_t pad134c[4];
    uint32_t effect[16];  // +0x1350 表现对象句柄，每槽 4 字节
    float animation_offset[16][3]; // +0x1390 ECL320
    int32_t animation_parent[16]; // +0x1450 ECL322，原版初值 -1
    int32_t anm_index;  // +0x1490 当前 ECL 动画资源选择
    int32_t primary_anm_index; // +0x1494 槽 0 创建时的资源选择
    int32_t primary_anm_script; // +0x1498 槽 0 当前移动姿态组
    int32_t primary_base_script; // +0x149c 槽 0 基准脚本
    int32_t primary_anim_state; // +0x14a0
    uint8_t pad14a4[4];
    int32_t animation_layer; // +0x14a8 ECL552，实际绘制层加 7
    float previous_x; // +0x14ac 上次世界位置，供死亡方向与移动朝向
    float previous_y; // +0x14b0
    float previous_z; // +0x14b4
    int32_t var[12];  // +0x14b8..+0x14e4 ECL 变量 A..H 区（SpawnArgs.vars 拷贝）
    uint8_t pad14e8[0x1510 - 0x14e8];
    float fade_time;  // +0x1510 >0 时 0x0042fe80 把表现对象 alpha 从 1 拉到 0
    EnemyListLink link;  // +0x1514 敌机链表节点（manager+0x18c head / +0x190 tail）
    uint8_t pad1524[0x17c4 - 0x1524];

    bullet::Slot bullets[bullet::kSlotCount];  // +0x17c4 每槽 0x488 字节
    uint32_t fire_anm[bullet::kSlotCount];     // +0x6044 每槽发射用 ANM id
    float fire_offset[bullet::kSlotCount][3];  // +0x6084 发射基点
    float fire_target[bullet::kSlotCount][3];  // +0x6144 发射定位参数（0x004369e0 中 601 的分支判据在 0x614c）
    float subpixel_x;  // +0x6204
    float subpixel_y;  // +0x6208
    uint8_t pad620c[0x621c - 0x620c];
    int32_t score;  // +0x621c SpawnArgs.score
    int32_t health; // +0x6220 当前生命
    int32_t health_max;  // +0x6224
    int32_t health_previous; // +0x6228
    int32_t health_scaled; // +0x622c 符卡累计扣血精度为普通 HP 的七倍
    int32_t phase_health_threshold; // +0x6230 当前阶段下界
    int32_t damage_accum; // +0x6234
    uint32_t flags6238;  // +0x6238 位1 由构造清除
    int32_t drop_kind; // +0x623c primary item
    uint8_t pad6240[0x6304 - 0x6240];
    int32_t death_se;        // +0x6304 = (enemy_id & 1) + 3
    int32_t death_script;    // +0x6308 默认由首轮 ECL 的槽 0 动画决定，ECL323 可覆盖
    int32_t death_resource;  // +0x630c 默认 effect.anm，ECL323 可覆盖
    int32_t damage_flash; // +0x6310 命中闪色后的四帧间隔
    uint8_t pad6314[4];
    int32_t state6318;  // +0x6318 = -1
    int32_t state631c;  // +0x631c 构造 = -1；CreateEnemy 覆盖为 1
    int32_t state6320;  // +0x6320 构造 = 0；CreateEnemy 覆盖为 2
    uint32_t field6324;  // +0x6324 CreateEnemy 写 0x40000000
    int32_t state6328;  // +0x6328 构造 = 0
    uint32_t flags632c;  // +0x632c CreateEnemy |= 1
    uint8_t pad6330[0x6358 - 0x6330];
    float alpha;   // +0x6358 Bomb 伤害倍率，构造 = 1.0f
    uint32_t flag; // +0x635c 敌机标志字（0x20/0x20000/0x80000/0x4000000/0x40000000 等）
    uint8_t pad6360[0x636c - 0x6360];
    int32_t boss_index; // +0x636c 由 ECL512 设置，flag 的 0x800000 位表示已登记
    uint8_t pad6370[0x67b4 - 0x6370];
    Enemy* self;  // +0x67b4 构造写 this，ECL VM 由此回指敌人
    uint8_t pad67b8[0x6824 - 0x67b8];
    int32_t field6824;  // +0x6824 构造取 DAT_004ccce4
    int32_t field6828;  // +0x6828 opcode 500 用：0 时置 1 并调 0x00439ec0
    int32_t field682c;  // +0x682c
    int32_t id;         // +0x6830 敌机唯一 ID（manager+0x90 递增分配）
    int32_t parent_id;  // +0x6834 创建者 ID
    int32_t field6838;  // +0x6838
    AnmVm* visual[16];
    const char* death_sub;
    int32_t bullet_extra_cursor[16];
    EnemyInterrupt interrupts[8];
    int32_t phase_time;
    float phase_fraction;
    const char* pending_sub;
    int32_t special_callback;
};

// 敌机管理器（DAT_004cf2d0 指向它；0x0042dc20 构造，0x1a0 字节）。
struct EnemyManager {
    uint32_t flags;       // +0x00 构造写 2
    Thread* task_logic;   // +0x04 A 队优先级 0x1b → 0x0042df50
    Thread* task_render;  // +0x08 B 队优先级 0x17 → 空任务（0x0042e0b0 恒返回 1）
    int32_t score_c;  // +0x0c..+0x38 由 GetVarInt -9934..-9927 读取的统计值
    int32_t field10;
    int32_t field14;
    int32_t field18;
    int32_t field1c;
    int32_t field20;
    int32_t field24;
    int32_t field28;
    int32_t field2c;
    int32_t field30;
    int32_t field34;
    int32_t field38;
    int32_t field3c;  // +0x3c ECL -9949：脚本重置后累计的 miss 数
    int32_t field40;  // +0x40 ECL -9948：脚本重置后累计的 Bomb/相应能力使用数
    int32_t field44;  // +0x44 ECL -9947：脚本置 1，miss/Bomb 将其清零
    int32_t boss_id[3]; // +0x48 / +0x4c / +0x50
    uint8_t pad054[0x88-0x54];
    uint32_t flags88; // +0x88 ECL512 清位 0（0042d4d0）
    int32_t max_enemies;  // +0x8c 同时存在的敌机上限（构造 99999）
    int32_t next_id;      // +0x90 下一个敌机 ID（跳过 0）
    int32_t last_id;      // +0x94
    int32_t field98;      // +0x98 构造 = -1；每次更新先写入旧波次索引
    int32_t wave_index;   // +0x9c 波次索引（按速度缩放推进）
    float wave_frac;      // +0xa0 波次小数部分
    int32_t field_a4;     // +0xa4 更新末尾清零
    int32_t flags_a8;     // +0xa8 构造 |= 1
    int32_t field_ac;     // +0xac 每帧清零
    int32_t field_b0;     // +0xb0 每帧清零
    uint8_t pad0b4[0x168 - 0xb4];
    void* targets[8];  // +0x168.. 目标实体指针数组（opcode 的 target 参数用）
    void* ecl_resource;  // +0x188 当前关卡 ECL 资源（0x0042e100 里拷给 enemy+0x1218）
    EnemyListLink* enemy_head;  // +0x18c 链表头（节点在 Enemy+0x1514）
    EnemyListLink* enemy_tail;  // +0x190 链表尾
    int32_t field194;   // +0x194
    int32_t count;      // +0x198 当前敌机数（0x004369e0 用 0x8c 比较它）
    int32_t field19c;   // +0x19c
};
static_assert(sizeof(EnemyManager) == 0x1a0);

// 敌机创建：0x0042d7d0（EnemyManager*, ECL 子程序名, SpawnArgs*）→ Enemy*
// 先 0x0048dc71(0x683c) 分配，再 0x0042e100 构造，写 SpawnArgs 各字段，
// 最后把 Enemy+0x1514 节点挂到 manager+0x18c head / +0x190 tail 并 ++count。
Enemy* create_enemy(const char* sub_name, const SpawnArgs& args);

// 按 ID 查敌机：0x0042d500（0 表示未找到）。
Enemy* find_enemy(int32_t id);

// ECL 300/301/304/305/321 的敌机创建处理器：0x004369e0（参数在 ECL 线程里）。
void op_create_enemy(Enemy& self);

// 每帧表现刷新：0x0042fe80（按 self.fade_time 更新 16 个表现对象 alpha）。
int32_t enemy_update_presentation(Enemy& self);

// 删除：0x0042d220（scalar deleting destructor，从链表摘除并释放）。
void delete_enemy(Enemy* enemy);

// 敌机管理器构造：0x0042dc20（建 0x1a0 管理器 + 0x1098 EclResourceInf 加载 ecl_name，
// 注册 A 队 0x1b 更新线程与 B 队 0x17 空任务线程）。
extern EnemyManager* g_enemy_manager;  // 原版 DAT_004cf2d0
extern int32_t g_difficulty;         // 原版 DAT_004cccdc+0x24

EnemyManager* create_enemy_manager(const char* ecl_name);

// 敌机管理器销毁：0x0042d210（停两个任务线程，释放全部敌机与 ECL 资源）。
void destroy_enemy_manager(EnemyManager* manager);

// 敌机管理器每帧更新：0x0042df50（A 队 0x1b）。
// 遍历 manager+0x18c 链表：非 0x2000000 标志的敌机更新表现，0x2000000 的删除；
// 末尾清 +0xac/+0xb0/+0xa4，并推进波次计数（+0x98/+0x9c/+0xa0，按全局速度缩放）。
ThreadStatus enemy_manager_update(void* ctx);
void clear_enemy_bullets(float x, float y, float radius, bool turn_to_money);
void damage_enemies_in_circle(float x, float y, float radius, int32_t damage);

}  // namespace stage
