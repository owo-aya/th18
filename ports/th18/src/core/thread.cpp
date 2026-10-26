#include "core/thread.h"

#include <cstdlib>

// 线程管理器（原版 DAT_004cf294，malloc 0x58）。两个升序链表：
//   A 队列（+0x14/+0x18）：主循环每帧执行，游戏逻辑线程。
//   B 队列（+0x3c/+0x40）：呈现分支执行，渲染/后台线程。
struct ThreadManager {
    Thread* logic_head;
    Thread* render_head;
    Thread* next; // 原版+0x50；回调摘掉下一个任务时同步推进。
    int32_t special;
};

static ThreadManager g_tm;

static void link_sorted(Thread** head, Thread* t) {
    Thread* prev = nullptr;
    Thread* cur = *head;
    while (cur && cur->priority < t->priority) {
        prev = cur;
        cur = cur->next;
    }
    t->prev = prev;
    t->next = cur;
    if (prev) {
        prev->next = t;
    } else {
        *head = t;
    }
    if (cur) {
        cur->prev = t;
    }
}

static void unlink(Thread** head, Thread* t) {
    if (g_tm.next == t) {
        g_tm.next = t->next;
    }
    if (t->prev) {
        t->prev->next = t->next;
    } else {
        *head = t->next;
    }
    if (t->next) {
        t->next->prev = t->prev;
    }
    t->prev = nullptr;
    t->next = nullptr;
}

Thread* thread_create(ThreadStatus (*step)(void* ctx)) {
    Thread* t = static_cast<Thread*>(std::malloc(sizeof(Thread)));
    t->priority = 0;
    t->flags = 1;  // 摘链时释放
    t->step = step;
    t->on_enqueue = nullptr;
    t->on_paused = nullptr;
    t->self = t;
    t->next = nullptr;
    t->prev = nullptr;
    t->ctx = nullptr;
    return t;
}

void thread_destroy(Thread* t) {
    std::free(t);
}

void thread_set_priority(Thread* t, int32_t priority) {
    t->priority = priority;
}

void thread_set_ctx(Thread* t, void* ctx) {
    t->ctx = ctx;
}

void thread_set_paused(Thread* t, bool paused) {
    if (paused) {
        t->flags |= 2;
    } else {
        t->flags &= ~2u;
    }
}

void thread_set_on_enqueue(Thread* t, void (*fn)(void)) {
    t->on_enqueue = fn;
}

void thread_enqueue_logic(Thread* t) {
    if (t->on_enqueue) {
        t->on_enqueue();
        t->on_enqueue = nullptr;
    }
    link_sorted(&g_tm.logic_head, t);
}

void thread_enqueue_render(Thread* t) {
    if (t->on_enqueue) {
        t->on_enqueue();
        t->on_enqueue = nullptr;
    }
    link_sorted(&g_tm.render_head, t);
}

int32_t thread_run_logic() {
    // 原版 FUN_004012e0 的返回值：正常执行返回执行过的线程数（>=1），
    // 0 表示有线程返回 STOP_ZERO（主循环据此退出），-1 表示 STOP_NEG（重启）。
    int32_t executed = 0;
    Thread* t = g_tm.logic_head;
    while (t) {
        g_tm.next = t->next;
        if (!t->step) {
            t = g_tm.next;
            continue;
        }
        if ((t->flags & 2) && g_tm.special != 0) {
            if (t->on_paused) {
                t->on_paused();
            }
            t = g_tm.next;
            continue;
        }
        ThreadStatus st = t->step(t->ctx);
        switch (st) {
        case THREAD_DONE:
            unlink(&g_tm.logic_head, t);
            if (t->flags & 1) {
                std::free(t);
            }
            ++executed;
            break;
        case THREAD_QUIT:
            return 1;
        case THREAD_STOP_ZERO:
        case THREAD_STOP_ZERO2:
            return 0;
        case THREAD_STOP_NEG:
            return -1;
        case THREAD_RESCAN:
            t = g_tm.logic_head;
            continue;
        case THREAD_SPECIAL:
            if (t->flags & 2) {
                if (t->on_paused) {
                    t->on_paused();
                }
            }
            ++executed;
            break;
        default:
            ++executed;
            break;
        }
        t = g_tm.next;
    }
    return executed;
}

int32_t thread_run_render() {
    int32_t count = 0;
    Thread* t = g_tm.render_head;
    while (t) {
        g_tm.next = t->next;
        if (!t->step) {
            t = g_tm.next;
            continue;
        }
        ThreadStatus st = t->step(t->ctx);
        switch (st) {
        case THREAD_DONE:
            unlink(&g_tm.render_head, t);
            if (t->flags & 1) {
                std::free(t);
            }
            break;
        case THREAD_QUIT:
            return 1;
        case THREAD_STOP_ZERO:
        case THREAD_STOP_ZERO2:
            return 0;
        case THREAD_STOP_NEG:
            return -1;
        case THREAD_RESCAN:
            t = g_tm.render_head;
            continue;
        default:
            ++count;
            break;
        }
        t = g_tm.next;
    }
    return count;
}

void thread_remove(Thread* t) {
    Thread** head = &g_tm.logic_head;
    Thread* current = g_tm.logic_head;
    while (current != nullptr && current != t) {
        current = current->next;
    }
    if (current == nullptr) {
        head = &g_tm.render_head;
    }
    unlink(head, t);
    if (t->flags & 1) {
        std::free(t);
    }
}

void thread_manager_init() {
    g_tm.logic_head = nullptr;
    g_tm.render_head = nullptr;
    g_tm.next = nullptr;
    g_tm.special = 0;
}

void thread_manager_set_special(int32_t on) {
    g_tm.special = on;
}
