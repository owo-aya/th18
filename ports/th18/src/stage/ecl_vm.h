#pragma once
#include <cstdint>
#include <vector>

#include "stage/ecl_format.h"
#include "stage/ecl_opcodes.h"

// TH18 ECL 虚拟机（敌机侧）。
// 与关卡脚本 VM 不同：本 VM 只执行 id >= 300 的指令；id < 300 直接返回（0x00436417 分支）。
//
// 原版实现：
//   0x00430d30  vm_exec     EnemyInf vtable[0]，入口 0x00430d40（thunk: ADD ECX,0x122c; JMP）
//   0x0048dbe0  thread_current  取当前指令指针
//   0x0048dc20  thread_start    按子程序名开始执行（0x48d920 查名字，然后清 instr/offset）
//   0x0048d920  sub_find        在 ECL 资源子程序表里二分查名字
//   0x00437360  var_get_int     ECL 变量取值（含 -10000.. 特殊变量）
//   0x00437b20  var_get_ptr     ECL 变量取地址
//   0x00438aa0 / 0x00438ad0 / 0x00438b10 / 0x00438bc0   取第 i 个 int / float / float(默认) / float
//   0x0048d4f0  float 参数取值（task 版本，返回 XMM0）
// 全局：DAT_004cf2d0 = EnemyManager*，DAT_004cccdc = GameState 本体（+0x24 难度 0..3）。
namespace stage {

struct Enemy;

// ECL 线程。构造 0x0042e100 时 enemy+0x0c 指向它；0x0048d920 写 sub_index，
// 0x0048dc20 再把 instr/offset 清零。0x0048dbe0 反过来经 owner 取 ECL 资源表。
struct EclThread {
    float time;          // +0x0000 时间累加值（VM 每次调用 += delta，指令 time <= 它时执行）
    int32_t sub_index;   // +0x0004 子程序索引，-1 表示没找到
    int32_t offset;      // +0x0008 子程序内指令字节偏移，-1 表示未开始
    uint8_t stack[0x1000]; // +0x000c 表达式栈区
    int32_t stack_ptr;   // +0x100c 栈顶（0x0048b3a0 的 param_1[0x403]）
    int32_t stack_ret;   // +0x1010 暂存的上一栈顶（param_1[0x404]）
    float id;            // +0x1014 线程编号（opcode 17/18/19/20 按它找线程）
    Enemy* owner;        // +0x1018 所属敌机（构造写 this）
    float field101c;     // +0x101c opcode 0x14 写
    uint8_t rank_mask;   // +0x1020 本线程难度位掩码（0x0048b3a0 的 param_1+0x408）
    uint8_t pad1021[3];
    // +0x1024: eight scalar interpolators for ECL91/92. Each records its
    // original stack frame and setup instruction for later destination writes.
    uint8_t task_slots[0x1024 - 0x1024 + 0x180];
    uint32_t task_a[8];  // +0x11a4
    int32_t task_instruction[8][2]; // +0x11c4 {sub_index, offset}
    uint32_t flags1204;  // +0x1204 opcode 0x12/0x13 置/清 bit0
    // 端口补充字段：原版 EclThread 没有它，ECL 资源是从 owner 的 +0x1218 取的；
    // 关卡脚本宿主（非敌机）没有 +0x1218，所以把资源指针缓存在线程里。
    void* ecl_res;
    // 端口补充字段：同步子程序调用（opcode 11）的返回地址栈。原版把返回地址压在表达式栈里，
    // 端口单独开一个数组，避免与 0x28/0x29 的栈分配冲突。
    int32_t call_sub[16];
    int32_t call_off[16];
    int32_t call_sp;
    float call_time[16];
    int32_t call_stack[16];
    int32_t call_base[16];
    std::vector<EclThread*> children;
};

// 脚本 VM（原版 0x0048b3a0）：按时间/难度执行 id < 0x5f 的指令，遇到 id >= 0x5f 时
// 转调 owner 的 vtable[0]（敌机为 0x00430d30 经 adjustor thunk，ECX = owner+0x122c）。
// 返回 0 = 正常继续；-1 = 线程结束（sub_index/offset 已置 -1）。
int32_t script_vm_run(EclThread* thread, float delta);
// 难度位（enemy+0x1030 = 1 << difficulty）。
enum Difficulty : int32_t {
    kEasy = 0,
    kNormal = 1,
    kHard = 2,
    kLunatic = 3,
};

// ECL 指令 rank 掩码位。
enum RankMask : uint8_t {
    kRankEasy = 1,
    kRankNormal = 2,
    kRankHard = 4,
    kRankLunatic = 8,
};

// ECL 变量 id（var_get_int / var_get_ptr）。负数 id 是内建状态，其余是敌机自身字段或
// 变量 A..H（-9957.. 等）。只列出已确认的；完整映射见 0x00437360 的 switch。
enum EclVar : int32_t {
    kVarRandInt = -10000,       // 随机 int（0x00402740 结果 & 0x7fffffff）
    kVarRandFloat = -10001,     // 随机 [0,1)
    kVarRandFloatPM = -9987,    // 随机 [-1,1)
    kVarX = -9985,              // enemy+0x1270
    kVarY = -9984,              // enemy+0x1274
    kVarSpeed = -9983,          // enemy+0x12b4
    kVarAngle = -9982,          // enemy+0x12b8
    kVarPlayerX = -9981,        // player+0x620
    kVarPlayerY = -9980,        // player+0x624
    kVarPlayerDistance = -9960, // sqrt((x-px)^2+(y-py)^2)
    kVarRankEasy = -9953,       // difficulty == 0
    kVarRankNormal = -9952,     // difficulty == 1
    kVarRankHard = -9951,       // difficulty == 2
    kVarRankLunatic = -9950,    // difficulty == 3
    kVarDiff = -9943,           // GameState+0x24 难度原值
    kVarBossHp = -9965,         // manager+0x3c
    kVarBossHp2 = -9964,        // manager+0x40
    kVarBossHp3 = -9963,        // manager+0x44
    kVarEnemyCount = -9946,     // GameState+0x1b0 计数
    kVarId = -9922,             // enemy+0x14b8（变量 A）
    // …其余 id 见 GetVarInt 原版 switch。
};

// 取 ECL 变量：0x00437360。未命中的 id 返回 0。
int32_t var_get_int(Enemy& self, int32_t var_id);

// 取 ECL 变量地址：0x00437b20。未命中的 id 返回 nullptr。
int32_t* var_get_ptr(Enemy& self, int32_t var_id);

// 第 index 个参数（1 起）取 int：0x00438aa0。
int32_t arg_int(Enemy& self, int32_t index);

// 第 index 个参数（1 起）取 float：0x00438ad0。
float arg_float(Enemy& self, int32_t index);

// 第 index 个参数（1 起）取 float，args 不足时用 def：0x00438b10。
float arg_float_default(Enemy& self, int32_t index, float def);

// 第 index 个参数（0 起）取 float：0x00438bc0（默认值走 XMM2，通常直接给 0）。
float arg_float_raw(Enemy& self, int32_t index);

// 每帧执行一条 ECL 指令：0x00430d30（返回 0 = 子程序结束/无操作，1 = 继续）。
// 参数是敌机 VM 子对象（&enemy+0x122c）。
int32_t vm_exec(void* vm);

// 让敌机从某个子程序重新开始：0x0048dc20。
void thread_start(EclThread* thread, void* ecl_resource, const char* sub_name);

// 取当前指令：0x0048dbe0。sub/offset 无效时返回 nullptr。
const ecl::InsHeader* thread_current(EclThread* thread);

// 在 ECL 资源里按名字二分查子程序：0x0048d920。返回索引，-1 = 不存在。
int32_t sub_find(void* ecl_resource, const char* name);

// ECL 宿主虚表。原版敌机 EnemyInf 的 vtable @0x4b654c 前五项：
//   [0] 0x00430d30 每帧执行 ECL；[1] 0x00437360 取 int 变量；[2] 0x00437b20 取变量地址；
//   [3] 0x00437c90 取 float 变量（0x0048d5a0/0x0048b3a0 的变量分支调用）；
//   [4] 0x004387e0 取 float 变量地址（0x0048d7c0 调用）。
// 端口里这些函数由我们自己的 C++ 实现填充，敌机与关卡脚本宿主各有一张表。
struct EclHostTable {
    int32_t (*exec)(void* host);                          // [0] 0x00430d30
    int32_t (*var_int)(void* host, int32_t var_id);       // [1] 0x00437360
    int32_t* (*var_ptr)(void* host, int32_t var_id);      // [2] 0x00437b20
    float (*var_float)(void* host, int32_t var_id);       // [3] 0x00437c90
    float* (*var_float_ptr)(void* host, int32_t var_id);  // [4] 0x004387e0
};

// 关卡脚本宿主：原版关卡脚本跑在关卡对象（stage 0x2cc）的某个脚本宿主上，
// 具体类未定位。端口里用一个独立小对象承载：vtable + 当前线程 + 变量 A..H + 自身坐标。
// 敌机的 op_create_enemy 用 enemy+0x122c 的宿主区，关卡脚本没有敌机字段，
// 所以关卡的创建敌机由 stage_exec 直接按指令参数构造 SpawnArgs。
void script_tree_run(EclThread* thread, float delta);
void script_tree_destroy(EclThread* thread);

}  // namespace stage
