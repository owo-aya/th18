#include <cstdlib>
#include <cstring>

#include "core/app.h"
#include "core/game_session.h"
#include "core/input.h"
#include "presentation/title.h"
#include "presentation/ending.h"
#include "presentation/sound.h"
#include "presentation/trophy.h"
#include "presentation/anm.h"
#include "presentation/text.h"
#include "platform/gfx.h"
#include "stage/background.h"
#include "stage/session.h"
#include "storage/storage.h"

static AppContext g_app;
static GameData* g_game_data = nullptr;

namespace {
// 406ad0's payload contains five embedded VMs. They are drawn by 406d00,
// rather than being inserted into the ordinary ANM drawing queues.
struct LoadingTransition {
    AnmVm* panels[4];
    AnmVm* pattern;
    Thread* logic;
    Thread* render;
    bool closing;
    int frames;
};
LoadingTransition* g_loading_transition;

void release_transition_vms(LoadingTransition* transition) {
    for (AnmVm* vm : transition->panels) anm_vm_free(vm);
    anm_vm_free(transition->pattern);
}

ThreadStatus update_transition(void* context) {
    auto* transition = static_cast<LoadingTransition*>(context);
    int finished = 0;
    for (AnmVm* vm : transition->panels) {
        anm_vm_update(vm);
        finished += vm->script_cursor < 0 && !(vm->flags2 & 1);
    }
    if (finished < 4) anm_vm_update(transition->pattern);
    ++transition->frames;
    // 406c80: all four exit scripts end, or label 1 has run for 60 frames.
    if (finished == 4 || (transition->closing && transition->frames >= 60)) {
        thread_remove(transition->render);
        release_transition_vms(transition);
        g_loading_transition = nullptr;
        delete transition;
        return THREAD_DONE;
    }
    return THREAD_CONTINUE;
}

ThreadStatus draw_transition(void* context) {
    auto* transition = static_cast<LoadingTransition*>(context);
    IDirect3DDevice9* device = gfx_device();
    gfx_select_camera(2);
    // 406d00 clears destination alpha while preserving the existing RGB, then
    // the four sliding panels write the mask used by effect194's blend 7.
    struct Vertex { float x, y, z, rhw; uint32_t color; };
    const Vertex vertices[4] = {{0,0,0,1,0},{640,0,0,1,0},{0,480,0,1,0},{640,480,0,1,0}};
    device->SetTexture(0, nullptr);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
    device->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_ZERO);
    device->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex));
    for (AnmVm* vm : transition->panels) anm_draw_vm(vm);
    anm_apply_blend(transition->pattern);
    device->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_ONE);
    anm_draw_vm(transition->pattern);
    return THREAD_CONTINUE;
}

void destroy_transition() {
    if (g_loading_transition == nullptr) return;
    thread_remove(g_loading_transition->logic);
    thread_remove(g_loading_transition->render);
    release_transition_vms(g_loading_transition);
    delete g_loading_transition;
    g_loading_transition = nullptr;
}
}

void app_begin_stage_loading() {
    text_loading_start(480.0f, 392.0f);
    auto* transition = new LoadingTransition{};
    AnmResource* resource = anm_get(8, "effect.anm");
    for (int i = 0; i < 4; ++i) {
        transition->panels[i] = anm_vm_alloc(resource);
        anm_vm_set_script(transition->panels[i], resource, 3 + i);
        transition->panels[i]->extra_x = 320.0f;
        transition->panels[i]->extra_y = 240.0f;
    }
    transition->pattern = anm_vm_alloc(resource);
    anm_vm_set_script(transition->pattern, resource, 194);
    transition->logic = thread_create(update_transition);
    transition->logic->flags |= 2; // Native second-queue ANM A0x0b uses flags 3.
    thread_set_ctx(transition->logic, transition);
    thread_set_priority(transition->logic, 0x0b);
    thread_enqueue_logic(transition->logic);
    // Label 7 selects native second-queue layer 31, displayed in pass B0x57.
    transition->render = thread_create(draw_transition);
    thread_set_ctx(transition->render, transition);
    thread_set_priority(transition->render, 0x57);
    thread_enqueue_render(transition->render);
    g_loading_transition = transition;
    // Title A0x08 creates the native root before the existing A0x0b pass.
    // This newly inserted task lies before the manager's cached next task.
    update_transition(transition);
}

void app_finish_stage_loading() {
    if (g_loading_transition != nullptr) {
        AnmResource* resource = anm_get(8, "effect.anm");
        for (int i = 0; i < 4; ++i) {
            AnmVm* vm = g_loading_transition->panels[i];
            anm_vm_reset(vm);
            anm_vm_set_script(vm, resource, 7 + i);
            vm->extra_x = 320.0f;
            vm->extra_y = 240.0f;
        }
        g_loading_transition->closing = true;
        g_loading_transition->frames = 0;
    }
}

AppContext* app_context() {
    return &g_app;
}

void app_context_init() {
    g_app = AppContext{};
    // 原版 FUN_00453db0 启动时把当前状态置 0xfffffffe，首帧请求状态 0 触发 case 0 建标题。
    g_app.state = 0xfffffffeu;
}

static void destroy_stage_session(AppContext* app) {
    if (app->stage_session != nullptr) {
        // 432c0 starts ASCII17 before replacing a session or returning to title.
        if (app->next_state == 0xa || app->next_state == 0xb || app->next_state == 0xc ||
            app->next_state == 0xe || app->next_state == 4 || app->next_state == 0x10)
            text_loading_start(480.0f, 392.0f);
        stage_session_destroy(static_cast<StageSession*>(app->stage_session));
        app->stage_session = nullptr;
    }
}

static void create_stage_session(AppContext* app, uint32_t stage, uint32_t param) {
    app->stage_param = param;
    app->stage_session = stage_session_create(stage, param);
}

static void destroy_title_menu(AppContext* app) {
    if (app->title_menu != nullptr) {
        title_menu_destroy(static_cast<TitleMenu*>(app->title_menu));
        app->title_menu = nullptr;
    }
}

static void destroy_title(AppContext* app) {
    destroy_title_menu(app);
    if (app->title_obj != nullptr) {
        title_scene_destroy(static_cast<TitleScene*>(app->title_obj));
        app->title_obj = nullptr;
    }
}

void app_context_shutdown() {
    g_app.next_state = 0x11;
    if (g_app.state == 0xf) ending_destroy();
    destroy_stage_session(&g_app);
    destroy_title(&g_app);
    ending_finish_fade_shutdown();
    destroy_transition();
    trophy_shutdown();
    g_app = AppContext{};
}

void app_advance_stage(int32_t next_stage) {
    g_app.current_stage = static_cast<uint32_t>(next_stage);
    g_app.next_state = 0xc;
}

// 原版 FUN_00455040：按 +0x7f4（当前状态）与 +0x7f8（请求状态）切换场景。
// 返回值：1=继续（同一帧内不切换时），4=结束（app 线程以 STOP_ZERO 返回），
// 5=退出（app 线程以 STOP_NEG 返回）。
// 状态含义（取自反编译 + 各 case 行为）：
//   0  = 创建标题场景 FUN_004531b0，成功后请求状态 1
//   1  = 标题加载中（标题 A 队任务把请求状态置 4）
//   3  = 清理标题并结束
//   4  = 创建标题菜单 FUN_00464cb0（仅从标题 1/2 或游戏 7/0xf 进入时创建）
//   7  = 直接创建关卡会话 FUN_004437b0(0)
//   0xa = 开始普通游戏（关卡取 DAT_004ccce0），请求 7
//   0xb = 重开录像（会话参数 1），请求 7
//   0xc = 转入 current_stage（保留会话 +0xd0；444650 递增关卡后使用），请求 7
//   0xd = 进入录像播放（会话参数 1），请求 7
//   0xe = 直接以参数 0 创建会话，请求 7
//   0xf = 结局流程（FUN_0042bab0）
//   0x10 = 返回标题：销毁会话、请求 4
//   0x11 = 退出游戏
//   0x13 = 从菜单进入 Extra（会话参数 0、extra 标记）
static uint32_t app_state_machine(AppContext* app) {
    if (app->state == app->next_state) {
        return 1;
    }

    const uint32_t prev = app->state;
    app->prev_state = prev;
    app->fade_color = 0xff000000;

    switch (app->next_state) {
    case 0:
        app->next_state = 1;
        app->title_obj = title_scene_create();
        storage_log("state 0: title scene=%p", app->title_obj);
        if (app->title_obj == nullptr) {
            app->next_state = 3;
            goto case_3;
        }
        break;

    case 3:
    case_3:
        destroy_stage_session(app);
        destroy_title(app);
        trophy_shutdown();
        input_reset();
        return 4;

    case 4:
        if (prev == 1 || prev == 2) {
            app->title_menu = title_menu_create();
            break;
        }
        if (prev == 7) {
            destroy_stage_session(app);
        } else if (prev == 0xf) {
            ending_destroy();
        }
        app->title_menu = title_menu_create();
        break;

    case 7:
        if (prev == 4) {
            // 00464d20: the completed selection UI leaves before the session.
            destroy_title_menu(app);
        }
        app->game_flag = 1;
        if (session().mode == GameMode::Full) app->selected_spell = -1;
        create_stage_session(app, app->start_stage, 0);
        break;

    case 0xa:
        destroy_stage_session(app);
        app->game_flag = 1;
        app->extra_flag = 0;
        app->next_state = 7;
        app->current_stage = app->start_stage;
        app->stage_param = 0;
        create_stage_session(app, app->current_stage, 0);
        break;

    case 0xb:
        destroy_stage_session(app);
        app->game_flag = 1;
        app->extra_flag = 0;
        app->next_state = 7;
        app->current_stage = app->start_stage;
        app->stage_param = 1;
        create_stage_session(app, app->current_stage, 1);
        break;

    case 0xc:
        app->game_flag = 0;
        if (prev == 7) {
            destroy_stage_session(app);
        }
        app->next_state = 7;
        create_stage_session(app, app->current_stage, app->stage_param);
        break;

    case 0xd:
        if (prev == 4) destroy_title_menu(app);
        app->next_state = 7;
        app->game_flag = 1;
        app->stage_param = 1;
        create_stage_session(app, app->current_stage, 1);
        break;

    case 0xe:
        destroy_stage_session(app);
        app->game_flag = 1;
        app->next_state = 7;
        app->stage_param = 0;
        create_stage_session(app, app->current_stage, 0);
        break;

    case 0xf:
        if (prev == 7) {
            destroy_stage_session(app);
        }
        ending_create(app->ending_index);
        break;

    case 0x10:
        destroy_stage_session(app);
        if (prev == 0xf) ending_destroy();
        app->next_state = 4;
        app->title_menu = title_menu_create();
        break;

    case 0x11:
        destroy_stage_session(app);
        if (prev == 0xf) ending_destroy();
        destroy_title(app);
        trophy_shutdown();
        input_reset();
        app->state = 0x11;
        return 5;

    case 0x13:
        destroy_stage_session(app);
        app->selected_spell = -1;
        app->game_flag = 1;
        app->extra_flag = 1;
        app->next_state = 7;
        app->current_stage = app->start_stage;
        app->stage_param = 0;
        create_stage_session(app, app->current_stage, 0);
        break;

    default:
        break;
    }

    app->state = app->next_state;
    storage_log("state %u -> %u", prev, app->state);
    return 1;
}

// 原版 FUN_00453460：A 队列优先级 1 的 app 主线程。
// 这里只保留每帧必须的部分：状态机推进（声音淡入淡出/场景槽/输入采样分别随
// presentation/sound、stage 场景栈、platform 输入模块接入）。
ThreadStatus app_context_step(void* ctx) {
    // 00453460 starts with 00476d20, consuming the previous frame's SE requests.
    sound_update();
    AppContext* app = static_cast<AppContext*>(ctx);
    if (app->update_mode == 2) {
        return THREAD_STOP_ZERO;
    }
    if (app->update_mode == 0) {
        const bool unchanged = app->state == app->next_state;
        const uint32_t rc = app_state_machine(app);
        if (rc == 4) {
            return THREAD_STOP_ZERO;
        }
        if (rc == 5) {
            return THREAD_STOP_NEG;
        }
        // 437b0 first publishes the loading session. Resource construction is
        // separate from A0x10's start point; no Player/ECL tick runs under bit4.
        if (unchanged && app->state == 7 && (stage_session_flags() & 4))
            stage_session_load(static_cast<StageSession*>(app->stage_session));
    }
    title_finish_ambient_step();
    return THREAD_CONTINUE;
}

GameData* game_data() {
    return g_game_data;
}

void game_data_init() {
    // 原版 FUN_00471270：malloc 0x39724b8（约 60.2 MB）后由 FUN_00483820 构造
    // （ThreadInf 头 + 敌机槽数组 + 辅助数组）。P1 先按原版尺寸分配并清零，
    // 构造函数的字段初始化等各模块用到时再补。
    void* mem = std::malloc(0x39724b8u);
    if (mem != nullptr) {
        std::memset(mem, 0, 0x39724b8u);
    }
    g_game_data = static_cast<GameData*>(mem);
}

void game_data_shutdown() {
    trophy_shutdown();
    // 原版 FUN_00484eb0 析构 + free。
    std::free(g_game_data);
    g_game_data = nullptr;
}
