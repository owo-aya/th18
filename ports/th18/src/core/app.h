#pragma once
#include <cstdint>

#include "core/thread.h"

// 全局游戏上下文（原版 &DAT_004ccdf0）：
// FUN_004543a0 读取 th18.cfg 并应用启动参数（+0x224 位标志），
// FUN_00453460 作为 A 队列优先级 1 的 app 主线程每帧推进，
// 状态机在 FUN_00455040（按 +0x7f4 当前状态 / +0x7f8 请求状态切换）。
// 状态值（原版 switch）：0、1、3、4、7、0xa、0xb、0xc、0xd、0xe、0xf、0x10、0x11、0x13。
struct AppContext {
    // +0x114 启动参数/显示相关（FUN_004543a0 非 0 时写日志）
    uint32_t display_flag = 0;

    // +0x224 启动参数位标志，FUN_004543a0 逐位检查：
    // 0x2 窗口/全屏相关、0x4、0x8、0x10、0x20（DAT_004cd610=1）、0x40
    uint32_t startup_flags = 0;

    // +0x7f4 当前状态（模式/场景），每帧由 FUN_00455040 推进。
    uint32_t state = 0;
    // +0x7f8 请求切换到的状态，下一帧生效；与原版相同：等于 state 时状态机直接返回。
    uint32_t next_state = 0;
    // +0x7fc 记录切换前的 state。
    uint32_t prev_state = 0;
    // +0x804 进入游戏/关卡相关标记（case 7/0xa/0xb/0x13 置 1，case 0xc 置 0）。
    uint32_t game_flag = 0;
    // +0x808 0xa/0x13 置 0，0xc/0x10 中另有赋值（含义待 P1 确认）。
    uint32_t extra_flag = 0;
    // +0x820 FUN_004543a0 清零。
    uint32_t field_820 = 0;
    // +0xb40 由 FUN_004531b0() 返回的对象（标题场景），失败则切到状态 3。
    void* title_obj = nullptr;
    // +0xb60 附近：标题菜单对象（原版全局 DAT_004cf2cc，FUN_00464cb0 创建，状态 4 时创建）。
    void* title_menu = nullptr;
    // 当前关卡会话（原版全局 DAT_004cf2e4，状态 7/0xa/0xb/0xc/0xd/0xe/0x13 创建与销毁）。
    void* stage_session = nullptr;
    // 开始游戏的关卡编号（原版 DAT_004ccce0，菜单选择后写入）。
    uint32_t start_stage = 0;
    // 当前关卡编号（原版 DAT_004cccdc）。
    uint32_t current_stage = 0;
    // 会话创建参数（原版 FUN_004437b0 的参数，写入会话 +0xd0）。
    uint32_t stage_param = 0;
    // DAT_004ccd00 / DAT_004cccf4, selected before state 7 creates the player.
    int32_t difficulty = 1;
    int32_t selected_player = 0;
    int32_t selected_spell = -1; // DAT_004ccd14, selected global spell number.
    int32_t practice_lives = 0;  // DAT_004ccccc: 0=default, 1..9 held at confirmation.
    // DAT_004ccde8 and the clear-return branch of DAT_004cf438.
    int32_t ending_index = 0;
    bool run_cleared = false;
    // score +0x5f608 stores each starting-deck bank in 16 bytes.
    int32_t initial_card_count = 0;
    int32_t initial_cards[16] = {};
    // +0xb58 淡入淡出颜色，状态机每次进入时置 0xff000000。
    uint32_t fade_color = 0;
    // +0xacc 帧更新控制：0=正常推进 FUN_00455040，2=进入退出流程。
    uint32_t update_mode = 0;
};

AppContext* app_context();
void app_context_init();
void app_context_shutdown();
// 00444650: continue the same run through state 0xc, retaining session flags.
void app_advance_stage(int32_t next_stage);
// 466740 -> 42b0a0/406ad0: the normal character-confirm loading transition.
void app_begin_stage_loading();
// 443e60, called at the A0x10 session start point (original label 1).
void app_finish_stage_loading();
ThreadStatus app_context_step(void* ctx);  // 原版 FUN_00453460

// 游戏数据对象（原版 DAT_0051f65c）：malloc 0x39724b8（约 60.2 MB）后由
// FUN_00483820 构造，FUN_00484eb0 析构。对象内首先是 ThreadInf 头
// （vftable + 4 个 dword），随后是敌机槽数组（+0x1c0 起，0x624 字节/个 × 0x8000 个）
// 与另一组同规模数组（+0xc481fc 区）；数组元素大小 0x60c 的辅助数组另行初始化。
struct GameData;

GameData* game_data();
void game_data_init();
void game_data_shutdown();
