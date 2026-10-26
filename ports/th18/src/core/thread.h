#pragma once
#include <cstdint>

// 原版线程对象（ThreadInf，大小 0x28，RTTI vtable 0x4b49c4，构造函数 FUN_00402af0）。
// A/B 队列按优先级升序；同优先级的新任务前插（FUN_00401180 / FUN_00401230）。
// A 队列（FUN_004012e0）每帧执行；B 队列（FUN_00401420）在呈现分支执行。
// 线程对象字段与反编译一一对应：
//   +0x00 priority      优先级（升序）
//   +0x04 flags         bit0=摘链时释放，bit1=暂停/特殊
//   +0x08 step          步进函数，参数为 ctx
//   +0x0c on_enqueue    入队前调用一次后清零
//   +0x10 on_paused     暂停态且管理器特殊标记非 0 时调用
//   +0x14 self          链表节点指向自身
//   +0x18 next / +0x1c prev
//   +0x20 ctx           执行时作为参数传入 step
enum ThreadStatus : int32_t {
    THREAD_DONE = 0,      // 完成：摘下链表，flags bit0 时释放
    THREAD_CONTINUE = 1,  // 继续
    THREAD_SPECIAL = 2,   // 特殊状态（暂停线程转暂停回调）
    THREAD_QUIT = 3,      // 主循环返回 1（退出程序）
    THREAD_STOP_ZERO = 4, // 主循环返回 0
    THREAD_STOP_NEG = 5,  // 主循环返回 -1
    THREAD_RESCAN = 6,    // 重新从头扫描队列
    THREAD_STOP_ZERO2 = 8,
};

struct Thread {
    int32_t priority;
    uint32_t flags;
    ThreadStatus (*step)(void* ctx);
    void (*on_enqueue)(void);
    void (*on_paused)(void);
    void* self;
    Thread* next;
    Thread* prev;
    void* ctx;
};

Thread* thread_create(ThreadStatus (*step)(void* ctx));
void thread_destroy(Thread* t);
void thread_set_priority(Thread* t, int32_t priority);
void thread_set_ctx(Thread* t, void* ctx);
void thread_set_paused(Thread* t, bool paused);
void thread_set_on_enqueue(Thread* t, void (*fn)(void));
void thread_enqueue_logic(Thread* t);   // A 队列
void thread_enqueue_render(Thread* t);  // B 队列

// 每帧执行；返回 0=正常、1=退出、-1=错误（与原版 FUN_004012e0 一致）。
int32_t thread_run_logic();
// 执行一次 B 队列；返回执行数量（原版 FUN_00401420）。
int32_t thread_run_render();

void thread_remove(Thread* t);      // 原版 FUN_004015a0
void thread_manager_init();
void thread_manager_set_special(int32_t on);  // 原版管理器 +0x54 特殊标记
