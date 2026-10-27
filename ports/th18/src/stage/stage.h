#pragma once
#include <cstdint>

#include "core/thread.h"
namespace bullet {struct Slot;}
struct AnmVm;

// TH18 关卡流程与关卡数据加载。
//
// 资源：每关在 th18.dat 里有 {st0N.std, st0N.ecl, st0N.ecl 旧名(th18_03 等), st0Na..st0Nd.msg,
// st0Nlogo.anm} 这些文件名（.rdata 0x4b7210 起，st01..st07 连续排布，st07 是 Extra）。
// .data 0x4c93f0 起是每关 9 个 const char* 的资源名表；0x4c94e8 起是每关 {std, ecl} 对。
//
// 关卡对象：0x0043b800 构造（0x2cc 字节，全局 DAT_004cf2e0），0x0043a730 载入数据，
// 0x0043b560 析构。构造时挂三个线程（入口都是 thunk）：
//   A 队优先级 0x21 → 0x0043d6f0（PUSH ECX; CALL 0x0043bb70; POP ECX; RET）= 关卡逻辑
//   B 队优先级 0x35 → 0x0043d700（MOV EAX,1; RET，恒返回 THREAD_CONTINUE 的空任务）
//   B 队优先级 0x31 → 0x0043d710（PUSH ECX; CALL 0x0043cd00; POP ECX; RET）= 关卡 HUD 绘制
//
// ECL 加载：EclResourceInf（0x1098 字节，0x0042dc20 构造）按文件名读 th18.dat 并调
// 0x0048d9d0 解析；include 区（ANIM + ECLI）由 0x0042da90 处理，ECLI 项递归加载。
//
// 关卡脚本：ECL 里 id < 300 的指令由另一个 VM 执行（敌机 VM 0x00436417 分支直接返回，
// 不处理这些 id）。该关卡 VM 入口尚未定位（P1）。
namespace stage {
bool shop_active();
int32_t enemy_count();
void spawn_item(int32_t kind,float x,float y,float angle,float speed,int32_t delay);
int cancel_enemy_bullets_circle(float x,float y,float radius,int kind);
// 429250: unlike 429370, this also cancels bullets with a nonzero delay.
void cancel_delayed_enemy_bullets_circle(float x,float y,float radius,int kind);
bool cancel_one_enemy_bullet(float x,float y,float radius,int kind);
void cancel_enemy_bullets_rectangle(float x,float y,float width,float height,float angle,int kind);
void set_enemy_bullets_frozen(bool frozen);
void psyco_reflect_bullets(float x,float y,int timer);
void miko_flash_cancel();
int kaname_cancel_bullets(float x,float y);
void cancel_bullet_point(float x,float y,int kind);
void laser_cancel_effect(float x,float y,int kind,int type,int color,bool rectangle);
void player_graze(float x,float y);
void fire_enemy_bullet_slot(const bullet::Slot& slot,float x,float y);
int bullet_sprite_for_vm(const AnmVm* vm,int logical);
float bullet_scale_for_vm(const AnmVm* vm);

enum class StageId : int32_t {
    kStage1 = 1,
    kStage2,
    kStage3,
    kStage4,
    kStage5,
    kStage6,
    kExtra = 7,
};

// 每关的资源文件名（.data 0x4c93f0 资源名表的内容）。
struct StageResources {
    const char* std;       // st0N.std
    const char* ecl;       // st0N.ecl（工作用）
    const char* msg_a;     // st0Na.msg
    const char* msg_b;
    const char* msg_c;
    const char* msg_d;
    const char* logo_anm;  // st0Nlogo.anm
};

// 关卡资源名表。
const StageResources& stage_resources(StageId id);

// 关卡对象（0x0043b800 创建，0x2cc 字节；原版全局 DAT_004cf2e0）。
struct Stage {
    uint32_t flags0;        // +0x000 构造写 2
    Thread* task_logic;     // +0x004 A 队 0x21 → 0x0043bb70
    Thread* task_bg;        // +0x008 B 队 0x35 → 0x0043d700（空任务）
    uint8_t pad00c[0x108 - 0x00c];
    void* anm_hud_a;        // +0x108 HUD 数字用 ANM 句柄
    void* anm_hud_b;        // +0x10c
    void* effect_handle;    // +0x114 0x00488be0 的表现对象句柄
    uint8_t pad114[0x11c - 0x114];
    int32_t timer;          // +0x11c 场景计时
    uint8_t pad120[0x134 - 0x120];
    int32_t hi_score_shown;  // +0x134 非 0 时 HUD 画 hi-score
    uint8_t pad138[0x13c - 0x138];
    int32_t field13c;  // +0x13c 载入时 -1
    int32_t field140;  // +0x140 载入时 0
    int32_t field144;  // +0x144 载入时 0
    int32_t field148;  // +0x148 载入时 0
    uint32_t flags14c;  // +0x14c 载入时清 bit0
    Thread* task_hud;   // +0x150 B 队 0x31 → 0x0043cd00
    uint8_t pad154[0x158 - 0x154];
    int32_t field158;   // +0x158 载入时 = DAT_004cccfc
    uint8_t pad15c[0x160 - 0x15c];
    void* logo_anm;  // +0x160 st0Nlogo.anm（0x00486880(6, 名字)）
    uint8_t pad164[0x194 - 0x164];
    uint32_t flags;       // +0x194 场景状态位：0x100/0x800/0x1000/0x1800
    int32_t wave_prev;    // +0x198
    int32_t wave_current;  // +0x19c 每帧按速度缩放推进
    float wave_frac;      // +0x1a0
    int32_t field1a4;     // +0x1a4 flags bit0x100 时清零
    uint8_t pad1a8[0x1ac - 0x1a8];
    int32_t wave_target;  // +0x1ac 达到后切 flags bit 0x800/0x1000
    int32_t dialog_active; // +0x1b0 MSG scene exists
    void* msg_data;   // +0x1b4 对话 .msg 文件数据（0x0043a730 读入）
    int32_t field1b8;  // +0x1b8 载入时 -1
    uint8_t pad1bc[0x1c0 - 0x1bc];
    int32_t field1c0;  // +0x1c0 载入时 -1
    uint8_t hud_blocks[0x54 * 3];  // +0x1c4 三个目标的血条/HUD 块（步长 0x54）
    void* front_anm;  // +0x2c0 fronttr.anm 句柄（0x0043b800 里 0x00486880(5, ...)）
    uint32_t clear_bonus; // +0x2c4 original displayed stage-clear award
    uint8_t pad2c8[4];
};
static_assert(sizeof(Stage) == 0x2cc);

// 原版 DAT_004cf2e0：当前关卡对象。
extern Stage* g_stage;

// 原版 DAT_004ccbf0：全局速度缩放（1.0 为常速；波次计数按它步进）。
extern float g_speed_scale;

// EclResourceInf 按名字加载一个 .ecl：0x0042d3e0（this = ECL 资源对象）。
// 返回 0 成功、-1 失败（原版返回 (int)(ret > -1) - 1）。
int32_t load_ecl_file(void* ecl_resource, const char* name);

// 处理已加载文件的 include 区（ANM 列表 + ECLI 递归加载）：0x0042da90。
void load_ecl_includes(void* ecl_resource, void* include_block);

// 解析单个 .ecl 文件数据：0x0048d9d0（this, fileData）。
int32_t parse_ecl_file(void* ecl_resource, const void* file_data);

// 新建 ECL 资源对象（0x0042dc20 构造 0x1098 字节）并按名字加载：端口侧组合函数。
void* ecl_resource_create(const char* name);
void ecl_resource_destroy(void* ecl_resource);

// 关卡对象：构造 0x0043b800 / 析构 0x0043b560 / 数据载入 0x0043a730。
Stage* stage_create();
void stage_destroy(Stage* stage);
int32_t stage_load_data(Stage& stage);

// 关卡逻辑线程：0x0043bb70（A 队 0x21）。
ThreadStatus stage_logic(void* ctx);
// 关卡 HUD 绘制线程：0x0043cd00（B 队 0x31）。
ThreadStatus stage_hud_render(void* ctx);
// 空任务：0x0043d700（B 队 0x35），恒返回 THREAD_CONTINUE。
ThreadStatus stage_bg_task(void* ctx);
void clear_enemy_bullets(float x, float y, float radius, bool turn_to_money);
void damage_enemies_in_circle(float x, float y, float radius, int32_t damage);

}  // namespace stage

void stage_dispatchers_init();
void stage_dispatchers_shutdown();
