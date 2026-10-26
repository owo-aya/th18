#pragma once
#include <cstdint>

#include "core/thread.h"
struct AnmVm;

// TH18 激光/效果对象。原版有 5 个类（RTTI 名 → vftable 地址）：
//   LaserDataInf     0x4b68ec
//   LaserLineInf     0x4b687c
//   LaserCurveInf    0x4b672c   [0]=0x448500 构造 0x448920
//   LaserBeamInf     0x4b679c   [0]=0x4485d0
//   LaserInfiniteInf 0x4b680c   [0]=0x448570
// 各类 vtable 中 [2]=0x447fe0、[14..25]=0x4480b0..0x448160 等是本类共用的基类方法；
// [10]（vtable+0x28）是收 (int,int) 的开关方法，消弹时被 FUN_00449090 调用。
//
// 管理器对象（DAT_004cf3f4，0x7c4 字节；会话创建 0x004424e0 里分配）：
//   +0x004 = A 队任务对象（0x00448870，prio 0x1c）
//   +0x008 = B 队任务对象（0x004488e0，prio 0x24）
//   +0x00c = 哨兵节点（LaserDataInf 基对象：+0x00 vtable、+0x04 prev、+0x08 next）
//   +0x014 = 活动对象链表头 = 哨兵.next
//   +0x794 = 链表尾（构造时 = 哨兵地址 this+0xc）
//   +0x798 = 对象数
//   +0x79c = 0x10000（构造时写）
//   +0x7b8 = bullet.anm 资源（FUN_00486880(7, "bullet.anm")）
// 链表节点即对象本身：+0x04 prev、+0x08 next、+0x0c 状态位、+0x10 ==1 时直接销毁、
// +0x18/+0x1c/+0x20 波次三元组、+0x24 计数。每帧更新 0x00448760。状态位 6（bit1|bit2）
// 是双缓冲切换：((flags+2)^flags)&6^flags；切换后 bit1|bit2 >= 4 时销毁对象。
// 消弹：0x00449090（遍历链表，对状态 !=1 的对象调用 vtable+0x28(1,0)）
//   调用点：ECL opcode 518（解释器 0x434d5d 附近）与 613（0x434e16 附近）、0x0045be90。
// 创建（曲线激光）：ECL opcode 702/703/711/713 → 0x00448920 构造 → 0x00449e00 分发器。
namespace laser {

enum class Type : int32_t {
    Line = 0,
    Infinite = 1,
    Curve = 2,
    Beam = 3,
};

struct Extra {float f[4];int32_t i[4];uint32_t type;int32_t async;uint32_t sub;};
struct LineArgs {
    float pos[3],angle,max_length,start_length,end_travel,width,speed;
    int32_t type,color;float offset;int32_t cursor;uint32_t flags;
    Extra extras[24];int32_t se,transform_se;
};
struct InfiniteArgs {
    float origin[3],position_step[3],angle,angle_step,max_length,start_length,max_width,growth;
    int32_t delay,expand,hold,shrink,se,transform_se,label;
    float axis_offset;int32_t cursor,type,color;uint32_t flags;Extra extras[24];
};
struct CurveArgs {
    float pos[3],angle,width,speed;int32_t style,color,node_count;float offset;uint32_t flags;
    Extra extras[24];int32_t se,transform_se,cursor;void* path_clone;float start_time;
};

// 对象公共头（链表节点部分，其余字段按各类具体布局）。
struct Object {
    void** vtable;   // +0x00
    Object* prev;    // +0x04
    Object* next;    // +0x08
    uint32_t flags;  // +0x0c 状态位（bit0 更新过、bit1/2 双缓冲、bit3 停用）
    int32_t state;   // +0x10 ==1 时本帧销毁
    int32_t field14; // +0x14
    int32_t wave0;   // +0x18 波次计数
    int32_t wave1;   // +0x1c 波次计数
    float wave_frac; // +0x20
    int32_t field24; // +0x24
    uint8_t pad028[0x54-0x28];
    float x,y,z,vx,vy,vz,angle,length,width,speed,traveled;
    int32_t label;
};

// 管理器（DAT_004cf3f4 指向，0x7c4 字节）。
struct Manager {
    uint32_t field000;      // +0x000
    Thread* task_logic;     // +0x004 A 队任务（0x00448870）
    Thread* task_render;    // +0x008 B 队任务（0x004488e0）
    void** root_vtable;     // +0x00c 哨兵节点 vtable（LaserDataInf）
    Object* root_prev;      // +0x010 哨兵 prev
    Object* head;           // +0x014 哨兵.next = 活动对象链表头
    uint8_t pad018[0x794 - 0x18];
    Object* tail;           // +0x794
    int32_t count;          // +0x798
    uint32_t field79c;      // +0x79c
    uint8_t pad7a0[0x7b8 - 0x7a0];
    void* bullet_anm;       // +0x7b8 "bullet.anm" 资源
    uint8_t pad7bc[0x7c4 - 0x7bc];
};
static_assert(sizeof(Manager) == 0x7c4);

inline Manager* manager = nullptr;  // 原版 DAT_004cf3f4

// 管理器构造（0x004424e0 内联）：分配 0x7c4、清 0、+0x79c=0x10000、
// 载入 "bullet.anm"、注册 A 队 0x00448870(prio 0x1c) 与 B 队 0x004488e0(prio 0x24)、
// 尾指针指向 +0xc 哨兵。
Manager* create_manager();

// 管理器析构（0x00448650）：停 A/B 队任务、DAT_004cf3f4 清 0、释放。
void destroy_manager(Manager* mgr);

// A 队任务（0x00448870）：会话 +0xb0 的 bit0/bit1/bit2/bit0x400 全清且练习对象
// （DAT_004cf2a4）为空时调 0x00448760；bit1（暂停）时速度缩放暂存为 0。
ThreadStatus update_task(void* ctx);

// B 队任务（0x004488e0）：会话 +0xb0 bit2 置位时跳过；遍历链表，对 state != 1 的
// 对象调 vtable[5]（渲染）。
ThreadStatus render_task(void* ctx);

// 曲线激光创建：0x00448920(this) 构造，vtable 写 0x4b672c。
void create_curve_laser(void* self);

// 激光创建分发：0x00449e00（按类型调 0x00448920 等构造）。ECL 702/703/711/713 走这里。
void create_laser(int32_t type,const void* args);
Object* find(int32_t id);
int sprite_for_vm(const AnmVm* vm,int script_sprite);
void set_origin(int32_t id,float x,float y);
void set_position_step(int32_t id,float x,float y);
void set_angle_step(int32_t id,float value);
void clear_tag(int32_t label);
void set_beam_flag(int32_t id,int32_t value);
void spawn_extra_border_enemies(int effect);
int cancel_circle(float x,float y,float radius,int32_t kind,int respect_immune=0);
int cancel_rectangle(float x,float y,float width,float height,float angle,int respect_immune=0);
void freeze(bool value);

// 消弹：0x00449090。参数非 0 时对所有活动对象调用 vtable+0x28(arg, 0)。
void clear_lasers(int32_t arg);
void clear_phase_lasers();

// 每帧更新：0x00448760（A 队 0x1c 经 0x00448870 调用）。返回 1。
int32_t update(Manager* manager);

// 激光管理器（原版 DAT_004cf40c，0x3f8 字节；会话创建 0x004424e0 内联分配）：
//   +0x000 标志（构造 *puVar9 |= 2）
//   +0x004 A 队任务（0x00457be0 = JMP 0x00457a60，prio 0x0c）
//   +0x008 B 队任务（0x00458470 = JMP 0x00458090，prio 0x51；符卡名/首领名 HUD 任务）
// 其余字段按 0x004424e0 的写值保留偏移：+0x00c=-1、+0x01c|=1、+0x020=-1、+0x030|=1、
// +0x03c=999、+0x104=1、+0x114=999、+0x1dc=1、+0x1ec 结束状态（1..3 走 0x00458e40）。
struct LaserManager {
    uint32_t flags;         // +0x000
    Thread* task_logic;     // +0x004
    Thread* task_render;    // +0x008
    int32_t frame_a;        // +0x00c 每帧 = +0x010
    int32_t wave_a;         // +0x010 波次计数
    float frac_a;           // +0x014
    int32_t field018;       // +0x018
    uint32_t flags01c;      // +0x01c
    int32_t frame_b;        // +0x020
    int32_t wave_b;         // +0x024
    float frac_b;           // +0x028
    int32_t field02c;       // +0x02c
    uint32_t flags030;      // +0x030
    int32_t field034;       // +0x034
    uint8_t pad038[0x03c - 0x038];
    int32_t field03c;       // +0x03c = 999
    uint8_t pad040[0x104 - 0x040];
    int32_t field104;       // +0x104 = 1
    int32_t field108;       // +0x108
    int32_t field10c;       // +0x10c
    uint8_t pad110[0x114 - 0x110];
    int32_t field114;       // +0x114 = 999
    uint8_t pad118[0x198 - 0x118];
    int32_t field198;       // +0x198
    uint8_t pad19c[0x1dc - 0x19c];
    int32_t field1dc;       // +0x1dc = 1
    int32_t field1e0;       // +0x1e0
    int32_t field1e4;       // +0x1e4
    int32_t field1e8;       // +0x1e8 句柄（B 队任务用）
    int32_t sequence_mode;  // +0x1ec 非 0 时按 1..3 调 0x00458e40
    uint8_t pad1f0[0x3f8 - 0x1f0];
};
static_assert(sizeof(LaserManager) == 0x3f8);

inline LaserManager* laser_manager = nullptr;  // 原版 DAT_004cf40c

// 构造（0x004424e0 内联）：分配 0x3f8、按上表初始化、DAT_004cf40c 赋值、
// 注册 A 队 0x00457be0(prio 0x0c) 与 B 队 0x00458470(prio 0x51)。
LaserManager* create_laser_manager();

// 析构：停 A/B 队任务、释放。
void destroy_laser_manager(LaserManager* mgr);

// A 队任务（0x00457a60）：结束状态分支 + 录像/暂停门控 + 两组波次计数推进。
ThreadStatus laser_update_task(void* ctx);

// B 队任务（0x00458090）：符卡名/首领名 HUD 绘制，用 text 管理器（DAT_004cf2ac）与
// 目标实体查表；等 text/score 接口接上后补绘制。
ThreadStatus laser_render_task(void* ctx);
// 45be90 -> 458a30: the live-player no-lives path opens the existing controller.
void open_game_over();
// 45be90 end branches: replay playback finishing -> ReplayEnd page,
// practice/short run clear -> PracticeClear page. Both reuse open_menu
// (pause set immediately, then IngameMenu A0x0c/B0x51).
void open_replay_end();
void open_practice_clear();

}  // namespace laser
