#pragma once
#include <cstdint>

#include "core/thread.h"
#include "player/player_sht.h"

struct AnmVm;
struct AnmResource;

// 自机对象：原版大小 0x479d0（FUN_0045af20 分配），全局当前自机 DAT_004cf410。
// 字段偏移与语义取自 FUN_0045af20 / FUN_0045a7a0 / FUN_0045a600 / FUN_0045b170 /
// FUN_0045be90 的反编译（toolchains/re/dump）。
// 子机与攻击/子弹槽位尚未逐字段展开，按原版数组形状保留。
constexpr int32_t kPlayerSize = 0x479d0;

// 运动学状态（共享模块 FUN_00402BF0 初始化 / FUN_00402CC0 推进；攻击对象、子弹、
// 道具都用这一形状）。type_flags 低 4 位 = 运动类型：0 直线、2 减速转向、3 圆周、4 向心。
struct Motion {
    float x, y, z;        // +0x00 当前位置
    float dx, dy, dz;     // +0x0c 每帧叠加的位移
    float speed;          // +0x18 速度
    float angle;          // +0x1c 朝向
    float angle_add;      // +0x20 累计旋转
    float angle_speed;    // +0x24 旋转速度
    float unk_028;        // +0x28
    float unk_02c;        // +0x2c
    float angle_base;     // +0x30 圆周/向心的基准角
    float vel_x;          // +0x34 初始化写出的每帧位移
    float vel_y;          // +0x38
    float unk_03c;        // +0x3c
    uint32_t type_flags;  // +0x40 低 4 位运动类型
};
static_assert(sizeof(Motion) == 0x44);

// 攻击对象（FUN_0045E320 创建 / FUN_0045EDB0 更新），玩家 +0x1570 起 0x200 个 ×0xF8。
// 每个攻击对象挂一个子弹（FUN_0045DFA0）和一个 ANM 表现对象。
constexpr int32_t kAttackObjectOffset = 0x1570;
constexpr int32_t kAttackObjectCount = 0x200;
constexpr int32_t kAttackObjectSize = 0xf8;

struct AttackObject {
    uint32_t flags;          // +0x00 bit0 = 已激活
    int32_t id;              // +0x04 生成序号 → 子弹 +0x90
    uint32_t anm_handle;     // +0x08 子弹 ANM 句柄（FUN_00488350 返回）
    int32_t life_prev;       // +0x0c 上一帧寿命（浮点）
    float life;              // +0x10 寿命（浮点，每帧 +1）
    float life_i;            // +0x14 寿命整数（浮点保存）
    uint32_t flags_018;      // +0x18 bit0 初始化标记
    int32_t unk_01c;         // +0x1c
    uint8_t unk_020[0x28];   // +0x20..+0x47
    Motion motion;           // +0x48 运动学（坐标/角度/速度）
    int32_t active;          // +0x8c 1 = 槽占用（0 = 空）
    int32_t unk_090;         // +0x90
    int32_t laser_hit;       // +0x94 本帧激光命中，45fc80 每帧清零
    int32_t laser_hit_anm;   // +0x98 激光当前处于命中动画
    int32_t damage;          // +0x9c ←指令 +0x02（子弹每帧同步到这里）
    float half_width;        // +0xa0 ←指令 +0x0c（子弹 OBB 全宽）
    float half_height;       // +0xa4 ←指令 +0x10
    uint8_t unk_0a8[4];      // +0xa8
    uint32_t command;        // +0xac 子程序<<8 | 指令序号
    int32_t bullet_index;    // +0xb0 FUN_0045DFA0 返回的池索引（1 起；0 = 无）
    AnmVm* anm_vm;           // +0xb4 子弹 ANM VM（原版按句柄反查，这里直接存）
    uint8_t unk_0b8[0x28];   // +0xb8..+0xdf
    void* anm_manager;       // +0xe0 用于创建子弹 ANM 的 ANM 运行时
    uint32_t param;          // +0xe4 子机对象指针（或 0）
    int32_t handler_fire;    // +0xe8 ←指令 +0x2c 发射时回调
    int32_t handler_update;  // +0xec ←指令 +0x30 逐帧回调（返回非 0 冻结）
    int32_t handler_c;       // +0xf0 ←指令 +0x34
    int32_t handler_d;       // +0xf4 ←指令 +0x38
};
static_assert(sizeof(AttackObject) == kAttackObjectSize);

struct Player {
    uint32_t unknown_000;        // +0x000
    void* thread_logic;          // +0x004 SHT 主线程（原版 LAB_0045caa0，priority 0x17）
    void* thread_render;         // +0x008 渲染线程（LAB_0045cac0，priority 0x1d）
    int32_t anm_player;          // +0x00c plXX.anm 资源对象（A 的 anm_get）
    int32_t anm_player2;         // +0x010 同一资源的第二个引用
    uint8_t vm_player[0x60c];    // +0x014 自机 ANM VM（AnmVm 大小 0x60c，FUN_00478580 初始化）
    float x;                     // +0x620 世界坐标
    float y;                     // +0x624
    float z;                     // +0x628
    int32_t x_sub;               // +0x62c 定点坐标（1 像素 = 128）
    int32_t y_sub;               // +0x630
    int32_t unk_634;             // +0x634 上一帧 0x638（-1 表示首帧）
    int32_t timer_state;         // +0x638 当前状态计时（入场/死亡/复活共用）
    float timer_float;           // +0x63c 与 timer_state 同步的浮点计时
    uint32_t flags_640;          // +0x640 一次性标记（处理完置 0）
    uint32_t flags_644;          // +0x644 bit0 = 计时器已初始化
    int32_t unk_648;             // +0x648 上一帧 0x64c
    int32_t shoot_cooldown;      // +0x64c 连续计数：<4 时禁止聚焦；>0x13 才触发射击 VM
    int32_t unk_650;             // +0x650 0x64c 的浮点累计
    int32_t unk_654;             // +0x654 一次性标记
    uint32_t flags_658;          // +0x658 bit0 = 计时器已初始化
    int32_t unk_65c;             // +0x65c 上一帧 0x660
    int32_t shot_timer;          // +0x660 射击计时：>0x13 后每帧调用 SHT 射击 VM
    int32_t unk_664;             // +0x664 0x660 的浮点累计
    int32_t unk_668;             // +0x668 一次性标记
    uint32_t flags_66c;          // +0x66c bit0 = 计时器已初始化
    uint8_t option_a[0x3c0];     // +0x670 4 个 0xF0 子机对象（FUN_0045bc90 更新）
    uint8_t option_b[0xb40];     // +0x0a30 12 个 0xF0 子机对象（同上）
    // +0x1570..+0x476ab 攻击对象与自机子弹池，原版形状：
    //   0x1570 起攻击对象（0x200×0xF8，FUN_0045edb0 遍历；初始化循环按 0x400×0xF8 写索引）
    //   0x20570 子弹池游标，0x20574 起 0x400×0x9C 自机子弹（FUN_0045de40 分配，FUN_0045be90 更新）
    // 两者在原版内存上重叠，按原版直接按偏移访问，不展开成独立成员。
    uint8_t shot_area[0x4613c];  // +0x1570..+0x476ab
    int32_t state;               // +0x476ac 0 入场 / 1 通常 / 2 死亡 / 3 消弹 / 4 决死窗口
    int32_t focus_handle;        // +0x476b0 聚焦高亮 ANM 句柄
    int32_t handle_476b4;        // +0x476b4 拖尾/受击表现句柄
    int32_t handle_476b8;        // +0x476b8 上一次 0x476bc
    int32_t effect_timer;        // +0x476bc 受击表现剩余帧
    int32_t effect_value;        // +0x476c0 表现计数（浮点累计）
    int32_t unk_476c4;           // +0x476c4 一次性标记
    uint32_t flags_476c8;        // +0x476c8 bit0
    int32_t focus_mode;          // +0x476cc 本帧聚焦标记
    int32_t script_a;            // +0x476d0 当前射击脚本索引（-2 = 未运行）
    int32_t script_a_timer;      // +0x476d4
    float script_a_value;        // +0x476d8
    int32_t unk_476dc;           // +0x476dc
    uint32_t flags_476e0;        // +0x476e0 bit0
    int32_t script_b;            // +0x476e4 第二个射击脚本索引（-2 = 未运行）
    int32_t script_b_timer;      // +0x476e8
    float script_b_value;        // +0x476ec
    int32_t unk_476f0;           // +0x476f0
    uint32_t flags_476f4;        // +0x476f4 bit0
    int32_t unk_476f8;           // +0x476f8
    uint8_t unk_476fc[0x78];     // +0x476fc..+0x47773
    int32_t anim_id;             // +0x47774 无敌计时器上一帧值
    int32_t invuln;              // +0x47778 无敌剩余帧
    float invuln_value;          // +0x4777c 无敌闪烁计数（初始 1.0）
    uint32_t flags_47780;        // +0x47780 一次性标记
    uint32_t flags_47784;        // +0x47784 bit0 = 无敌计时已初始化
    int32_t anim_id2;            // +0x47788 第二个动画编号（-1 = 无）
    int32_t alpha;               // +0x4778c 透明度/闪烁值
    int32_t effect_timer2;       // +0x47790
    uint32_t flags_47794;        // +0x47794 bit0
    uint32_t flags_47798;        // +0x47798 bit0
    uint32_t flags_4779c;        // +0x4779c bit1 死亡/Bomb 中, bit4 闪烁, bit8/9 脚本移动, bit10 射击
    int32_t script_offset;       // +0x477a0 上一帧脚本计时
    int32_t script_timer;        // +0x477a4 脚本移动计时
    float script_value;          // +0x477a8 脚本移动浮点计时
    int32_t unk_477ac;           // +0x477ac
    int32_t unk_477b0;           // +0x477b0
    int32_t speed_normal;        // +0x477b4 通常移动速度（像素×128，SHT[0x10]）
    int32_t speed_focus;         // +0x477b8 聚焦移动速度（SHT[0x14]）
    int32_t speed_diag;          // +0x477bc 斜向移动速度（SHT[0x18]）
    int32_t speed_diag_focus;    // +0x477c0 斜向聚焦速度（SHT[0x1c]）
    float vel_x;                 // +0x477c4 本帧速度（像素/帧）
    float vel_y;                 // +0x477c8
    float vel_z;                 // +0x477cc
    float vel_x_prev;            // +0x477d0 上一帧速度（渲染插值用）
    float vel_y_prev;            // +0x477d4
    float vel_z_prev;            // +0x477d8
    int32_t vel_x_int_dc;        // +0x477dc 本帧速度取整
    int32_t vel_y_int_e0;        // +0x477e0
    int32_t follow_factor;       // +0x477e4 子机跟随系数（0x1e = 立即跟随）
    int32_t follow_stun;         // +0x477e8 受击后子机跟随恢复计数
    float move_scale;            // +0x477ec 移动倍率（初始 1.0）
    float shake_x;               // +0x477f0 移动附加量，每帧归零（DAT_005217dc）
    float shake_y;               // +0x477f4
    float shake_z;               // +0x477f8
    uint8_t unk_477fc[4];        // +0x477fc
    int32_t trail[33][2];        // +0x47800 历史位置（[0] 为当前位置，移动时整体后移）
    int32_t respawn_wait;        // +0x47908 决死窗口帧数（8；延长卡改为15）
    float shot_angle;            // +0x4790c 射击角度（斜向时 ±3π/4，FUN_0045ea00 写入）
    float shot_offset;           // +0x47910 射击 y 偏移（上下方向输入微调）
    uint8_t unk_47914[0x10];     // +0x47914..+0x47923
    int32_t unk_47924;           // +0x47924
    float respawn_x;             // +0x47928 死亡位置，复活入场消弹的中心
    float respawn_y;             // +0x4792c
    float respawn_z;             // +0x47930
    int32_t unk_47934;           // +0x47934 上一帧 dx（动画切换判定）
    int32_t unk_47938;           // +0x47938 上一帧 dy
    int32_t direction;           // +0x4793c 0 静止 1 上 2 下 3 左 4 右 5-8 斜向
    void* sht;                   // +0x47940 载入的 plXX.sht
    int32_t unk_47944;           // +0x47944 射击脚本当前子程序（复位为 0）
    uint8_t flag_47948;          // +0x47948 复位为 0
    uint8_t unk_47949[0x27];     // +0x47949..+0x4796f
    uint32_t flags_47970;        // +0x47970 bit0 初始化清除
    uint8_t unk_47974[8];        // +0x47974..+0x4797b
    float invuln_alpha;          // +0x4797c 无敌闪烁倍率（初始 1.0）
    uint8_t unk_47980[0x50];     // +0x47980..+0x479cf
};

extern Player* g_player;              // DAT_004cf410
extern int32_t g_player_character;    // DAT_004cccf4 角色 0..3（plXX）
extern int32_t g_player_shot_type;    // DAT_004cccf8 与角色相加得 SHT 索引
void player_refresh_power_options();
void player_set_shop_active(bool active);
void player_start_warp(int side);

// 自机子弹池：Player+0x20574 起 0x400 个，步长 0x9C（FUN_0045dfa0 分配、
// FUN_0045be90 尾部推进、FUN_0045f0f0 命中判定）。槽 0 单独在 Player+0x204D8。
constexpr int32_t kPlayerBulletPoolOffset = 0x20574;
constexpr int32_t kPlayerBulletCapacity = 0x400;
constexpr int32_t kPlayerBulletSize = 0x9c;

struct PlayerBullet {
    uint32_t flags;          // +0x00 bit0 有效、bit1 激光/圆判定、bit2 贯穿提示
    float hit_radius;        // +0x04 激光型（flags bit1）的子弹半径
    float unk_008;           // +0x08
    float angle;             // +0x0c 朝向（OBB 旋转角）
    float unk_010;           // +0x10
    float half_width;        // +0x14 OBB 半宽（判定时 ×0.5）
    float half_height;       // +0x18 OBB 半高（判定时 ×0.5）
    uint8_t motion[0x44];    // +0x1c 运动学状态（前 12 字节 = x,y,z float）
    int32_t life_prev;       // +0x60 上一帧寿命（hitstop 时相等）
    int32_t life;            // +0x64 寿命（帧序号，兼作命中间隔计数）
    float life_f;            // +0x68 寿命浮点（每帧 -1，到 0 消弹）
    int32_t unk_06c;         // +0x6c
    uint32_t flags_070;      // +0x70 bit0
    int32_t damage;          // +0x74 单次伤害（SHT 指令值）
    int32_t damage_accum;    // +0x78 累计伤害（贯穿/多段判定）
    int32_t damage_max;      // +0x7c 累计伤害上限（>=9999999 表示不消失）
    int32_t hit_interval;    // +0x80 命中间隔（寿命取模）
    int32_t last_hit_enemy;  // +0x84 已命中的敌机 id（同目标只结算一次）
    int32_t hit_cooldown;    // +0x88 命中冷却
    int32_t hit_type;        // +0x8c 多段命中组 1..4（0 = 普通）
    int32_t attack_id;       // +0x90 生成它的攻击对象序号
    int32_t unk_094;         // +0x94
    int32_t kind;            // +0x98 特殊伤害处理序号（0 = 普通）
};
static_assert(sizeof(PlayerBullet) == kPlayerBulletSize);

// 子弹池遍历（原版固定 0x400 槽，用 flags bit0 判断有效槽）。
int32_t player_bullet_count();
const PlayerBullet* player_bullet_at(int32_t index);
PlayerBullet* player_damage_circle(const float*pos,float radius,float radius_per_frame,int life,int damage);
PlayerBullet* player_damage_rectangle(const float*pos,float angle,float width,float height,int life,int damage);

// 自机子弹对敌机判定：移植 FUN_0045f0f0 @ 0x45f0f0。
//   pos   敌机世界坐标（原版敌人 +0x44）
//   size  敌机 OBB 尺寸指针（原版敌人 +0x110；nullptr 时用 radius 圆判定）
//   radius 敌机圆半径（size 为 nullptr 时使用；原版 = 敌人 +0x110 的一半）
//   out_hit 命中标记输出（非 0 表示有命中，调用方据此生成 0x430240 命中特效）
//   out_pos 命中点输出（3 个 float，原版敌人 +0x280）
//   mode  0 = 结算伤害并执行命中处理，1 = 只查询相交伤害
//   enemy_id 敌机 id（同一帧同一子弹对同一敌机只结算一次；0 表示不限制）
//   enemy_angle 敌机 OBB 旋转（size 非空时使用）
// 返回：本次判定结算的伤害合计（每目标被 player+0x47984 = SHT[0x28] 上限夹住）。
int32_t player_bullet_hit_test(const float* pos, const float* size, float radius,
                               uint32_t* out_hit, float* out_pos, int32_t mode,
                               float enemy_id, float enemy_angle);

// 射击脚本 VM：
//   FUN_0045EA00 = 按 Power 选子程序、扫指令表、按计时器触发；
//   FUN_0045E320 = 生成攻击对象（分配 ANM + 子弹）；
//   FUN_0045EDB0 = 逐帧推进攻击对象运动并把位置同步到子弹。
void player_shoot_update(Player* p);
void player_attack_update(Player* p);
AttackObject* player_attack_pool(Player* p);
int32_t player_attack_spawn(Player* p, const ShtInstruction* instr, int32_t command,
                            float timer_a, const float* base_pos, void* option,
                            AnmResource* resource = nullptr);
void player_hit();
void player_motion_init(Motion* m); // FUN_00402BF0
void player_motion_step(Motion* m); // FUN_00402CC0

// 生命周期（FUN_0045af20 / FUN_0045a7a0 / FUN_0045b020）。
Player* player_create();
int32_t player_init_from_sht(Player* p); // 非 0 = 失败
void player_destroy(Player* p);
void player_reset(Player* p);            // FUN_0045a600
void player_shot_area_init(Player* p);   // FUN_0045a690（参数为 p+0x620 的内部数组）

// 会话初始化/结束（FUN_004442e0 行 100–203 的顺序）：
//   difficulty = DAT_004ccd00，stage = DAT_004cccdc；角色/机种由菜单设置
//   g_player_character / g_player_shot_type。
void player_system_init(uint32_t difficulty, uint32_t stage,bool new_run);
void player_system_shutdown();

// 帧更新。
void player_update(Player* p);        // FUN_0045be90 主状态机
void player_shot_update(Player* p);   // FUN_0045b170 移动 + 子机 + 拖尾
void player_render_update(Player* p); // LAB_0045cac0：把 +0x14 的 ANM VM 画到自机坐标
ThreadStatus player_logic_task(void* ctx);  // 逻辑线程入口（0x45A7A0 注册，prio 0x17）
ThreadStatus player_render_task(void* ctx); // 渲染线程入口（0x45A7A0 注册，prio 0x1D）

// 自机状态块：固定地址 0x4cccdc，原版所有增减调用都传 &DAT_004cccdc。
//   +0x58 金钱（商店），+0x5c Power，+0x60 Power 上限，+0x64 Power 事件步长，
//   +0x6c 残机，+0x78 残机上限（Extend 卡最高推到 7）。
// Power 上限/步长在 SHT 载入时由脚本值写入：DAT_004ccd3c = sht[0x24]*sht[0x20]，
// DAT_004ccd40 = sht[0x24]（=100）。
struct GameStatusBlock {
    int32_t current_stage;
    int32_t start_stage;
    uint32_t unk_008;
    uint32_t counter_00c;
    uint32_t counter_010;
    uint32_t counter_014;
    int32_t player;
    int32_t shot;
    uint32_t score;
    int32_t difficulty;
    int32_t continues;
    uint8_t unk_02c[0x0c];
    int32_t spell_id;
    int32_t miss;
    uint8_t unk_040[8];
    int32_t extend_score_1; // +0x48 DAT_004ccd24 分数续关阈值（0x4424e0 行 138）
    int32_t extend_score_2; // +0x4c DAT_004ccd28
    int32_t extend_score_3; // +0x50 DAT_004ccd2c
    uint8_t unk_054[4];
    int32_t money;      // +0x58 DAT_004ccd34
    int32_t power;      // +0x5c DAT_004ccd38
    int32_t power_max;  // +0x60 DAT_004ccd3c
    int32_t power_step; // +0x64 DAT_004ccd40
    uint8_t unk_068[4];
    int32_t lives;      // +0x6c DAT_004ccd48
    int32_t unk_070;    // +0x70 DAT_004ccd4c
    int32_t unk_074;    // +0x74 DAT_004ccd50
    int32_t lives_max;  // +0x78 DAT_004ccd54
    int32_t unk_07c;    // +0x7c DAT_004ccd58
    int32_t unk_080;    // +0x80 DAT_004ccd5c
    int32_t unk_084;    // +0x84 DAT_004ccd60
    int32_t unk_088;    // +0x88 DAT_004ccd64
    uint8_t unk_08c[0xfc-0x8c];
};

extern GameStatusBlock g_status; // 0x4cccdc 固定地址

bool status_add_power(GameStatusBlock* s, int32_t value); // FUN_004573f0 @ 0x4573f0
bool status_sub_power(GameStatusBlock* s, int32_t value); // FUN_00457480 @ 0x457480
void status_add_life(GameStatusBlock* s);                // FUN_004575f0 @ 0x4575f0
void status_add_life_fragment(GameStatusBlock* s);
void status_add_bomb(GameStatusBlock* s);
void status_add_bomb_fragment(GameStatusBlock* s);
