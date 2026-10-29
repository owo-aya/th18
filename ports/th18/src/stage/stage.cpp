#include "stage/stage.h"

#include <cstdlib>
#include <cstring>
#include <algorithm>

#include "core/app.h"
#include "storage/storage.h"
#include "presentation/msg.h"
#include "presentation/sound.h"
#include "gameplay/score_data.h"
#include "gameplay/shop.h"
#include "gameplay/spell.h"
#include "replay/replay.h"
#include "core/input.h"
#include "core/rng.h"
#include "platform/win32_app.h"
#include "platform/gfx.h"
#include "presentation/anm.h"
#include "presentation/text.h"
#include "player/player.h"
#include "player/bomb.h"
#include "cards/card_manager.h"
#include "cards/card_profile.h"
#include "presentation/trophy.h"
#include "core/game_session.h"
#include "projectiles/laser.h"
#include "resources/dat.h"
#include "stage/enemy.h"
#include "stage/session.h"
#include "stage/ecl_format.h"
#include "stage/ecl_vm.h"
#include "stage/hud.h"
#include "stage/background.h"
#include "storage/storage.h"

static void draw_clear_bonus();

namespace stage {
static uint32_t message_events;
static Shop* active_shop;
static Thread* shop_task;
bool shop_active() {return active_shop!=nullptr;}
static Thread* background_logic_task;
static Thread* background_upper_task;
static ThreadStatus background_logic(void*) {if(stage_session_started() || (stage_session_flags()&0x800))stage_background_update();return THREAD_CONTINUE;}
static ThreadStatus background_upper_render(void*) {stage_background_render_upper();return THREAD_CONTINUE;}

Stage* g_stage = nullptr;
float g_speed_scale = 1.0f;

// .data 0x4c93f0 资源名表：每关 std/ecl/msg a-d/logo.anm 这些文件名。
// 同区 0x4c9410 另有一张 9 项表（含旧版 anm 名 th18_03/th17_04），运行时是否使用未确认。
const StageResources& stage_resources(StageId id) {
    static const StageResources kStages[] = {
        {"st01.std", "st01.ecl", "st01a.msg", "st01b.msg", "st01c.msg", "st01d.msg", "st01logo.anm"},
        {"st02.std", "st02.ecl", "st02a.msg", "st02b.msg", "st02c.msg", "st02d.msg", "st02logo.anm"},
        {"st03.std", "st03.ecl", "st03a.msg", "st03b.msg", "st03c.msg", "st03d.msg", "st03logo.anm"},
        {"st04.std", "st04.ecl", "st04a.msg", "st04b.msg", "st04c.msg", "st04d.msg", "st04logo.anm"},
        {"st05.std", "st05.ecl", "st05a.msg", "st05b.msg", "st05c.msg", "st05d.msg", "st05logo.anm"},
        {"st06.std", "st06.ecl", "st06a.msg", "st06b.msg", "st06c.msg", "st06d.msg", "st06logo.anm"},
        {"st07.std", "st07.ecl", "st07a.msg", "st07b.msg", "st07c.msg", "st07d.msg", "st07logo.anm"},
    };
    return kStages[static_cast<int32_t>(id) - 1];
}

// 0x0048d9d0：解析单个 .ecl 文件数据。
//   文件头 0x24 字节；include 区长度 header.include_size 从 +0x24 起；
//   子程序偏移表紧随其后（u32 × sub_count，相对文件起点）；
//   之后是 sub_count 个 NUL 结尾的子程序名；名字按序对应偏移表。
//   每个子程序数据 = 文件起点 + offset（"ECLH" 头 +0x10 之后是指令流）。
int32_t parse_ecl_file(void* ecl_resource, const void* file_data) {
    auto* res = static_cast<ecl::EclResource*>(ecl_resource);
    const auto* data = static_cast<const uint8_t*>(file_data);
    const auto* header = static_cast<const ecl::FileHeader*>(file_data);

    const int32_t file_subs = static_cast<int32_t>(header->sub_count);
    const auto* offsets = reinterpret_cast<const uint32_t*>(data + ecl::kHeaderSize + header->include_size);
    const char* name = reinterpret_cast<const char*>(
        data + (header->sub_count + 9) * 4 + header->include_size);

    // 本文件的子程序表（名字已按文件内顺序给出，按名字插入到全局升序表）。
    auto* file_entries = new ecl::SubEntry[file_subs];
    for (int32_t i = 0; i < file_subs; ++i) {
        file_entries[i].name = name;
        file_entries[i].data = data + offsets[i];
        name += std::strlen(name) + 1;
    }

    const int32_t old_count = res->sub_count;
    auto* merged = new ecl::SubEntry[old_count + file_subs];
    int32_t old_i = 0;
    int32_t new_i = 0;
    int32_t out_i = 0;
    while (old_i < old_count && new_i < file_subs) {
        if (std::strcmp(res->subs[old_i].name, file_entries[new_i].name) < 0) {
            merged[out_i++] = res->subs[old_i++];
        } else {
            merged[out_i++] = file_entries[new_i++];
        }
    }
    while (old_i < old_count) merged[out_i++] = res->subs[old_i++];
    while (new_i < file_subs) merged[out_i++] = file_entries[new_i++];

    delete[] res->subs;
    delete[] file_entries;
    res->subs = merged;
    res->sub_count = old_count + file_subs;

    // +0xc 记录已加载文件的数据指针（同名/包含链的递归加载按文件序号追加）。
    auto** files = new void*[res->file_index + 1];
    for (int32_t i = 0; i < res->file_index; ++i) files[i] = res->files[i];
    files[res->file_index] = const_cast<void*>(file_data);
    delete[] res->files;
    res->files = files;
    ++res->file_index;

    load_ecl_includes(res, const_cast<uint8_t*>(data) + ecl::kHeaderSize);
    return 0;
}

// 0x0042da90：处理 include 区。布局见 ecl_format.h 的 AnimList/EclIncludeList。
// ANIM 项由 ANM 运行时加载（0x004867e0）；ECLI 项递归走 load_ecl_file（vtable[0]）。
void load_ecl_includes(void* ecl_resource, void* include_block) {
    auto* res = static_cast<ecl::EclResource*>(ecl_resource);
    auto* block = static_cast<uint8_t*>(include_block);
    auto* anim_list = reinterpret_cast<const ecl::AnimList*>(block);
    const char* name = reinterpret_cast<const char*>(block) + 8;
    for (uint32_t i = 0; i < anim_list->count; ++i) {
        res->animations[res->animation_count] = anm_get(120 + res->animation_count, name);
        ++res->animation_count;
        name += std::strlen(name) + 1;
    }

    uintptr_t ecl_list_addr = reinterpret_cast<uintptr_t>(name);
    ecl_list_addr = (ecl_list_addr + 3) & ~static_cast<uintptr_t>(3);
    auto* ecl_list = reinterpret_cast<const ecl::EclIncludeList*>(ecl_list_addr);
    const char* ecl_name = reinterpret_cast<const char*>(ecl_list_addr) + 8;
    for (uint32_t i = 0; i < ecl_list->count; ++i) {
        load_ecl_file(ecl_resource, ecl_name);
        ecl_name += std::strlen(ecl_name) + 1;
    }
}

// 0x0042d3e0：从 th18.dat 读入 .ecl 后解析。
int32_t load_ecl_file(void* ecl_resource, const char* name) {
    size_t size = 0;
    void* data = dat_read(name, &size);
    return parse_ecl_file(ecl_resource, data);
}

// 0x0042dc20 的端口组合版本：EclResourceInf 构造（0x1098 字节）+ 按名字加载。
void* ecl_resource_create(const char* name) {
    auto* res = new ecl::EclResource{};
    res->vtable = nullptr;
    res->file_index = 0;
    res->sub_count = 0;
    res->files = nullptr;
    res->subs = nullptr;
    load_ecl_file(res, name);
    return res;
}

// 0x0042d210 的端口版本：释放 EclResourceInf 里 parse 分配的数组与文件数据。
void ecl_resource_destroy(void* ecl_resource) {
    if (ecl_resource == nullptr) return;
    auto* res = static_cast<ecl::EclResource*>(ecl_resource);
    for(uint32_t i=0;i<res->animation_count;++i)anm_release_resource(120+i);
    for (int32_t i = 0; i < res->file_index; ++i) free(res->files[i]);
    delete[] res->files;
    delete[] res->subs;
    delete res;
}

// 0x0043a730：关卡数据载入。原版顺序：
//   1) ANM 0x00486880(6, DAT_004cf428[9]) → stage+0x160（st0Nlogo.anm，A 队 ANM 运行时）
//   2) 对话 .msg：DAT_004cf428[5 + (DAT_004cccf8 + DAT_004cccf4)] 读入 → stage+0x1b4
//   3) 清 stage+0x14c bit0；+0x140/+0x144/+0x148 = 0；+0x13c = -1；
//      +0x158 = DAT_004cccfc；+0x1b8 / +0x1c0 = -1。
// 端口：msg 名按角色编号在 msg_a..d 里选（原版用 DAT_004cccf4/cccf8 两个索引相加）。
int32_t stage_load_data(Stage& stage) {
    const StageResources& res = stage_resources(static_cast<StageId>(session().stage));
    stage.logo_anm = anm_get(6, res.logo_anm);
    stage.field13c = -1;
    stage.field140 = 0;
    stage.field144 = 0;
    stage.field148 = 0;
    stage.flags14c &= ~1u;
    stage.field158 = 0;
    stage.field1b8 = -1;
    stage.field1c0 = -1;

    size_t msg_size = 0;
    const char* msg_name = (&res.msg_a)[session().player];
    stage.msg_data = file_read(msg_name, &msg_size, false);
    return 0;
}

// 0x0043b800：关卡对象构造。原版顺序：分配 0x2cc + *flags0 |= 2 → 等资源 →
// 0x00486880(5, "fronttr.anm") → +0x2c0 → 0x0043a730 载入数据 → 挂三线程。
// 端口暂无 ANM 运行时（A 队 P1），fronttr.anm 句柄留空。
Stage* stage_create() {
    auto* stage = new Stage{};
    stage->flags0 = 2;
    g_stage = stage;
    stage_load_data(*stage);
    msg_load(stage->msg_data,static_cast<AnmResource*>(stage->logo_anm));

    stage->task_logic = thread_create(stage_logic);
    thread_set_priority(stage->task_logic, 0x21);
    thread_set_ctx(stage->task_logic, stage);
    thread_enqueue_logic(stage->task_logic);

    stage->task_bg = thread_create(stage_bg_task);
    thread_set_priority(stage->task_bg, 3);
    thread_enqueue_render(stage->task_bg);
    background_upper_task=thread_create(background_upper_render);
    thread_set_priority(background_upper_task,6);
    thread_enqueue_render(background_upper_task);
    background_logic_task=thread_create(background_logic);
    thread_set_priority(background_logic_task,0x12);
    thread_enqueue_logic(background_logic_task);

    stage->task_hud = thread_create(stage_hud_render);
    thread_set_priority(stage->task_hud, 0x31);
    thread_set_ctx(stage->task_hud, stage);
    thread_enqueue_render(stage->task_hud);
    return stage;
}

// 0x0043b560：关卡对象析构（线程 + ANM/表现句柄 + msg 数据）。
void stage_destroy(Stage* stage) {
    thread_remove(stage->task_logic);
    thread_remove(stage->task_bg);
    thread_remove(background_upper_task);
    thread_remove(background_logic_task);
    thread_remove(stage->task_hud);
    anm_release_resource(6);
    free(stage->msg_data);
    g_stage = nullptr;
    delete stage;
}

// 0x0043bb70：关卡逻辑线程（A 队 0x21）。已确认的每帧部分：
//   flags bit0x100 时清 +0x1a4；+0x198 = +0x19c（波次快照）；
//   波次按全局速度缩放 g_speed_scale（DAT_004ccbf0）推进：0.99 < s < 1.01 时
//   wave_current += 1 且 wave_frac += 1.0，否则 wave_frac += s、wave_current = (int)wave_frac；
//   flags bit0x1800 时再推进一次；达到 +0x1ac 的 wave_target 时切 0x800/0x1000 位并清计数。
// 场景计时器、BGM/音效、Boss 血条等其余分支待 P1 后续片。
ThreadStatus stage_logic(void* ctx) {
    if(!stage_session_started())return THREAD_CONTINUE;
    static int logic_calls = 0;
    if (logic_calls < 8) { storage_log("stage_logic #%d", logic_calls); ++logic_calls; }
    auto& stage = *static_cast<Stage*>(ctx);
    ++session().logic_frame;
    g_status.score=session().score;
    msg_update();
    stage.dialog_active=msg_active();
    message_events|=msg_take_events();
    if ((stage.flags & 0x100) != 0 && stage.field1a4 != 0) stage.field1a4 = 0;

    stage.wave_prev = stage.wave_current;
    const float speed = g_speed_scale;
    if (speed > 0.99f && speed < 1.01f) {
        stage.wave_current += 1;
        stage.wave_frac += 1.0f;
    } else {
        stage.wave_frac += speed;
        stage.wave_current = static_cast<int32_t>(stage.wave_frac);
    }

    if ((stage.flags & 0x1800) != 0) {
        stage.wave_prev = stage.wave_current;
        if (speed > 0.99f && speed < 1.01f) {
            stage.wave_current += 1;
            stage.wave_frac += 1.0f;
        } else {
            stage.wave_frac += speed;
            stage.wave_current = static_cast<int32_t>(stage.wave_frac);
        }
    }

    if ((stage.flags&0x1800)!=0 && stage.wave_target <= stage.wave_current) {
        stage.flags ^= 0x1800;
        stage.wave_current = 0;
        stage.wave_frac = 0.0f;
        stage.wave_prev = 0;
    }
    hud_update();
    return THREAD_CONTINUE;
}

// 0x0043cd00：关卡 HUD 绘制线程（B 队 0x31）。绘制本体需要 ANM 运行时与文字管理器
// （原版 ANM 句柄 stage+0x108/+0x110、文字管理器 DAT_004cf2ac），这些由 A 队 P1 提供；
// 本函数只维持线程，等接口接上后补 0x0043cd00 的绘制序列。
ThreadStatus stage_hud_render(void* ctx) {
    hud_render();
    draw_clear_bonus();
    return THREAD_CONTINUE;
}

// 0x0043d700：B 队 0x35 的空任务，恒返回 1。
ThreadStatus stage_bg_task(void* ctx) {
    stage_background_render();
    return THREAD_CONTINUE;
}

}  // namespace stage

// 关卡会话（原版全局 DAT_004cf2e4，0xd8 字节对象，FUN_004437b0 创建、0x4424e0 初始化）。
// 原版会话对象本身不存关卡/管理器指针（那些在 DAT_004cf2e0/0x2d0/0x3f4/0x40c 全局），
// 端口同样用全局，字段偏移按原版：+0x004 A 队任务、+0x008 B 队任务、+0x010 事件计时、
// +0xb0 位标志、+0xb4 结束倒计时、+0xb8 结束等待、+0xd0 创建参数、+0xd4 重试计数。
struct StageSession {
    uint32_t field000;       // +0x000
    Thread* task_logic;      // +0x004 A 队会话任务（0x00443d70 → 0x00443860）
    Thread* task_render;     // +0x008 B 队会话任务（0x00443d80）
    uint32_t field00c;       // +0x00c
    int32_t event_timer;     // +0x010 道具/事件计时（0/0x1e/5 分支）
    uint8_t pad014[0x24 - 0x14];
    int32_t wave;            // +0x024 ECL524
    uint8_t pad028[0xb0 - 0x28];
    uint32_t flags;          // +0xb0 位标志（4/8/0x10/0x4000/0x10000 等）
    int32_t ending_frame;    // +0xb4 结束序列帧计数
    int32_t ending_wait;     // +0xb8 结束等待帧数（创建时 0x3c）
    uint8_t pad0bc[0xd0 - 0xbc];
    uint32_t param;          // +0xd0 会话创建参数（0=实际游玩，1=录像播放）
    int32_t retry_counter;   // +0xd4
};
static_assert(sizeof(StageSession) == 0xd8);

static StageSession* g_stage_session = nullptr;
static Thread* g_session_dispatcher;
static Thread* g_input_dispatcher;
static bool g_stage_transfer;
static bool g_stage_loaded,g_stage_started;
static AnmVm* g_stage_clear_vm;
static uint32_t g_stage_clear_bonus;
static Thread* g_clear_fade_logic;
static Thread* g_clear_fade_render;
static float g_clear_fade_time;
static uint32_t g_clear_fade_alpha;

struct ScreenShake {
    ScreenShake* next;
    Thread* task;
    float time;
    int duration;
    int from;
    int to;
};
static ScreenShake* g_screen_shakes;

// 475c70: A14, two 402740 calls on 4cf280; camera1's negative-X case stays positive.
static ThreadStatus screen_shake_update(void* context) {
    auto* shake = static_cast<ScreenShake*>(context);
    const float speed = stage::g_speed_scale;
    shake->time += speed > 0.99f && speed < 1.01f ? 1.0f : speed;
    if (static_cast<int>(shake->time) < shake->duration) {
        const float amplitude = (shake->to - shake->from) * shake->time / shake->duration + shake->from;
        const uint32_t x = rng_next(card_rng()) % 3;
        const uint32_t y = rng_next(card_rng()) % 3;
        const float offset_x = x == 0 ? 0.0f : x == 1 ? amplitude : -amplitude;
        const float offset_y = y == 0 ? 0.0f : y == 1 ? amplitude : -amplitude;
        gfx_set_camera_shake(1, x == 0 ? 0.0f : amplitude, offset_y);
        gfx_set_camera_shake(3, offset_x, offset_y);
        return THREAD_CONTINUE;
    }
    ScreenShake** link = &g_screen_shakes;
    while (*link != shake) link = &(*link)->next;
    *link = shake->next;
    delete shake;
    return THREAD_DONE;
}

void stage_screen_shake(int duration, int from, int to) {
    auto* shake = new ScreenShake{g_screen_shakes, thread_create(screen_shake_update), 0, duration, from, to};
    g_screen_shakes = shake;
    thread_set_ctx(shake->task, shake);
    thread_set_priority(shake->task, 0x14);
    thread_set_paused(shake->task, true);
    thread_enqueue_logic(shake->task);
}

struct BombShake {
    BombShake* next;
    Thread* task;
    float time;
    int amplitude, rise, hold, fall;
};
static BombShake* g_bomb_shakes;

// 475e40: shared rise/hold/fall shake, drawing from gameplay RNG (4cf288).
static ThreadStatus bomb_shake_update(void* context) {
    auto* shake = static_cast<BombShake*>(context);
    if (stage_session() == nullptr || (stage_session_flags() & 0x77u) != 0)
        return THREAD_CONTINUE;
    const float speed = stage::g_speed_scale;
    shake->time += speed > 0.99f && speed < 1.01f ? 1.0f : speed;
    const int tick = static_cast<int>(shake->time);
    const int fall_start = shake->rise + shake->hold;
    const int end = fall_start + shake->fall;
    if (tick < end) {
        const float fraction = tick < shake->rise ? shake->time / shake->rise :
            tick < fall_start ? 1.0f : (end - shake->time) / shake->fall;
        const float amplitude = shake->amplitude * fraction;
        const uint32_t x = rng_next() % 3;
        const uint32_t y = rng_next() % 3;
        const float offset_x = x == 0 ? 0.0f : x == 1 ? amplitude : -amplitude;
        const float offset_y = y == 0 ? 0.0f : y == 1 ? amplitude : -amplitude;
        gfx_set_camera_shake(1, offset_x, offset_y);
        gfx_set_camera_shake(3, offset_x, offset_y);
        return THREAD_CONTINUE;
    }
    BombShake** link = &g_bomb_shakes;
    while (*link != shake) link = &(*link)->next;
    *link = shake->next;
    delete shake;
    return THREAD_DONE;
}

void stage_bomb_shake(int amplitude, int rise, int hold, int fall) {
    auto* shake = new BombShake{g_bomb_shakes,
        thread_create(bomb_shake_update), 0, amplitude, rise, hold, fall};
    g_bomb_shakes = shake;
    thread_set_ctx(shake->task, shake);
    thread_set_priority(shake->task, 0x14);
    thread_set_paused(shake->task, true);
    thread_enqueue_logic(shake->task);
}

// 43cd00: the award follows front111's alpha at the original (224,200).
static void draw_clear_bonus() {
    if (g_stage_clear_vm == nullptr) return;
    text_set_group(2);
    text_set_style(4);
    text_set_life(0);
    text_set_size(1, 1);
    text_set_alignment(0, 0);
    text_set_color((g_stage_clear_vm->color & 0xff000000u) | 0xffffffu);
    const TextPos pos{448, 400, 0};
    text_draw_number(text_manager(), &pos, g_stage_clear_bonus);
    text_set_color(0xffffffffu);
    text_set_alignment(1, 1);
    text_set_style(0);
    text_set_group(0);
}

// 475820 / 475750, effect kind 5: 200 scaled frames from clear tick 180.
static ThreadStatus clear_fade_update(void*) {
    g_clear_fade_alpha = g_clear_fade_time < 200.0f ?
        static_cast<uint32_t>(g_clear_fade_time * 255.0f / 200.0f) : 255u;
    g_clear_fade_time += stage::g_speed_scale;
    return THREAD_CONTINUE;
}

static ThreadStatus clear_fade_render(void*) {
    gfx_select_camera(2);
    auto* device = gfx_device();
    struct Vertex { float x, y, z, rhw; uint32_t color; };
    const uint32_t color = g_clear_fade_alpha << 24;
    const Vertex vertices[] = {{0,0,0,1,color}, {640,0,0,1,color},
                              {0,480,0,1,color}, {640,480,0,1,color}};
    device->SetTexture(0, nullptr);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex));
    return THREAD_CONTINUE;
}

StageSession* stage_session() {
    return g_stage_session;
}
uint32_t stage_session_flags(){return g_stage_session->flags;}
bool stage_session_started(){return g_stage_started;}
bool stage_session_can_pause() {
    return (g_stage_session->task_logic->flags&2)!=0 && (g_stage_session->flags&0x10000)==0 && g_stage_session->event_timer>29;
}
void stage_session_set_paused(bool paused) {
    if(paused)g_stage_session->flags|=0x10;else g_stage_session->flags&=~0x10u;
    if(stage::active_shop)shop_set_paused(stage::active_shop,paused);
}

void stage_session_set_wave(int wave) {
    g_status.unk_008 = wave;
    g_stage_session->wave = wave;
}

// 0x00443d80：B 队会话任务。清 game_data 的 +0xc0/+0xc4/+0xc8/+0xcc（本帧色调/闪烁状态），
// 返回 THREAD_CONTINUE。
ThreadStatus session_render(void* ctx) {
    auto* data = reinterpret_cast<uint8_t*>(game_data());
    *reinterpret_cast<uint32_t*>(data + 0xc0) = 0;
    *reinterpret_cast<uint32_t*>(data + 0xc4) = 0;
    *reinterpret_cast<uint32_t*>(data + 0xc8) = 0;
    *reinterpret_cast<uint32_t*>(data + 0xcc) = 0;
    return THREAD_CONTINUE;
}

// 00444650: shared MSG/ECL stage completion, including the final-stage award.
static void finish_stage(StageSession* s) {
    auto& st = *stage::g_stage;
    const int stage = session().stage;
    const int difficulty = static_cast<int>(session().difficulty);
    const bool playback = replay_mode() == ReplayMode::Playback;
    const bool spell_practice = session().mode == GameMode::SpellPractice ||
        (playback && (replay_info().mode_flags & 2) != 0);
    const bool practice = session().mode == GameMode::Practice ||
        (playback && (replay_info().mode_flags & 1) != 0);
    if (playback && (stage == 7 || !replay_has_stage(stage + 1))) {
        laser::open_replay_end();
        return;
    }
    if (!spell_practice) {
        st.clear_bonus = stage * 1000000u;
        g_stage_clear_bonus = st.clear_bonus;
        session().score = std::min(session().score + st.clear_bonus / 10, 999999999u);
        g_status.score = session().score;
        g_stage_clear_vm = anm_vm_alloc(anm_get(5, "front.anm"));
        g_stage_clear_vm->flags3 |= 0x1000;
        anm_vm_set_script(g_stage_clear_vm, anm_get(5, "front.anm"), 0x6f);
        anm_queue_add(g_stage_clear_vm);
        st.flags |= 0x100;
        st.wave_prev = -1;
        st.wave_current = 0;
        st.wave_frac = 0;
    }
    player_set_shop_active(true);
    if (practice || spell_practice) {
        if (playback) {
            laser::open_replay_end();
        } else {
            if (spell_practice)
                score_record_spell_practice(session().player, g_status.spell_id, g_status.score);
            else
                score_record_practice(session().player, difficulty, stage, g_status.score, true);
            laser::open_practice_clear();
        }
        return;
    }
    if (stage < 6) {
        if (!playback)
            score_data().character[session().player][0x12ef4 + (difficulty * 9 + stage) * 8] = 1;
        g_stage_transfer = true;
        app_advance_stage(stage + 1);
        return;
    }
    st.flags |= 0x10;
    // 444650: the achievement's Extra ending returns before clear awards.
    if(app_context()->extra_ending_view>=0) {
        app_context()->extra_ending_view=-1;
        app_context()->next_state=4;
        return;
    }
    s->flags |= 0x4000;
    s->retry_counter = 0;
    if (playback) return;

    const uint32_t remaining_bonus = (g_status.unk_07c + g_status.lives * 5) * 1000000u;
    session().score = std::min(session().score + remaining_bonus / 10, 999999999u);
    g_status.score = session().score;
    st.clear_bonus += remaining_bonus;
    g_stage_clear_bonus = st.clear_bonus;
    const bool continued = g_status.continues != 0;
    const bool has_blank = card_manager_owned(Blank);
    const uint64_t acquired = card_profile_record_clear(session().player, difficulty,
        stage == 7, continued, has_blank, g_status.miss);
    for (int id = 0; id < 56; ++id)
        if ((acquired & (uint64_t{1} << id)) != 0)
            trophy_notify(static_cast<CardId>(id), 1);
    score_record_clear(session().player, difficulty, continued);
    app_context()->ending_index = continued ? session().player + 8 :
        session().player * 2 + (has_blank ? 0 : 1);
    app_context()->run_cleared = true;
    replay_finish_run(true, replay_info().slow_rate);
}

// 00418c20 / 00417880: Shop runs before the pause controller and Session.
static ThreadStatus shop_dispatch(void*) {
    if (!shop_update(stage::active_shop)) return THREAD_CONTINUE;
    replay_shop_result();
    card_manager_shop_exit();
    player_set_shop_active(false);
    player_refresh_power_options();player_refresh_power_options();
    stage::hud_refresh_lives();stage::hud_refresh_bombs();
    shop_destroy(stage::active_shop);
    stage::active_shop = nullptr;
    stage::shop_task = nullptr;
    return THREAD_DONE;
}

// 00443860: session A0x10, including the 120/180/380-frame clear sequence.
ThreadStatus session_update(void* ctx) {
    auto* s = static_cast<StageSession*>(ctx);
    stage::Stage* st = stage::g_stage;
    if((stage::message_events&MSG_STAGE_END)!=0) {
        stage::message_events&=~MSG_STAGE_END;
        finish_stage(s);
    }
    if((stage::message_events&MSG_BGM_FADE)!=0){stage::message_events&=~MSG_BGM_FADE;bgm_fade(msg_bgm_fade_seconds());}
    if((stage::message_events&MSG_SHOP)!=0) {
        stage::message_events&=~MSG_SHOP;
        stage::active_shop=shop_create(session().stage);
        player_set_shop_active(true);
        stage::shop_task = thread_create(shop_dispatch);
        stage::shop_task->flags |= 2;
        thread_set_priority(stage::shop_task, 0x0c);
        thread_enqueue_logic(stage::shop_task);
    }

    if ((s->flags & 0x4000) != 0) {
        if ((s->flags & 0x10) != 0) return THREAD_QUIT;
        ++s->retry_counter;
        if (s->param != 0) {
            if (s->retry_counter == 120) laser::open_replay_end();
        } else {
            if (s->retry_counter == 180) {
                g_clear_fade_time = 0;
                g_clear_fade_alpha = 0;
                g_clear_fade_logic = thread_create(clear_fade_update);
                g_clear_fade_logic->flags |= 2;
                thread_set_priority(g_clear_fade_logic, 0x14);
                thread_enqueue_logic(g_clear_fade_logic);
                g_clear_fade_render = thread_create(clear_fade_render);
                thread_set_priority(g_clear_fade_render, 0x59);
                thread_enqueue_render(g_clear_fade_render);
            }
            if (s->retry_counter > 379)
                app_context()->next_state = session().stage == 7 ? 0x10 : 0xf;
        }
    }

    // 原版：stage+0x194 bit0x100（Boss 已击破的收尾）且 wave 计数 < 0x78 时先不推进。
    if ((st->flags & 0x100) != 0 && st->wave_current < 0x78 && (s->flags & 0x10) == 0) {
        return THREAD_CONTINUE;
    }

    // 0x10 计时分支：0 帧与 0x1e 帧各触发一次道具/练习对象调度（未移植，调用点保留在注释）。
    // if (s->event_timer == 0) FUN_00443e60(s);
    // else if (s->event_timer == 0x1e) FUN_004443c0(s);

    // 退出流程：bit4..6 任一置位 → 原版返回 3（THREAD_QUIT）结束会话任务。
    if ((s->flags & 0x70) != 0) return THREAD_QUIT;

    // bit2（重开/退出中的淡出）→ 置 bit7 后继续跑一帧。
    if ((s->flags & 4) != 0) {
        s->flags |= 0x80;
        return THREAD_CONTINUE;
    }

    // 结束等待（+0xb0 bit0x10000）：+0xb4 帧计数递增，到 +0xb8 后原版播 BGM/清标志。
    if ((s->flags & 0x10000) != 0) {
        if (s->ending_frame == 0) {
            // 原版此处置 BGM/停止音效，端口等 sound 接口。
        }
        ++s->ending_frame;
        if (s->ending_frame >= s->ending_wait) {
            s->flags &= ~0x10000u;
            s->ending_frame = 0;
        }
    }
    ++g_status.counter_00c;++g_status.counter_010;++g_status.counter_014;
    *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(g_player)+0x47984)=*reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(g_player->sht)+0x28);
    s->field00c=s->event_timer;
    float& event_time=*reinterpret_cast<float*>(s->pad014);event_time+=stage::g_speed_scale;s->event_timer=int32_t(event_time);
    return THREAD_CONTINUE;
}

static ThreadStatus session_dispatch(void*) {
    if(g_stage_started && (g_stage_session->flags&0x70)!=0)return THREAD_QUIT;
    if(g_stage_clear_vm) {
        anm_vm_update(g_stage_clear_vm);
        if(g_stage_clear_vm->script_cursor<0 && (g_stage_clear_vm->flags2&1)==0){anm_vm_free(g_stage_clear_vm);g_stage_clear_vm=nullptr;}
    }
    if(g_stage_loaded && !g_stage_started) {
        auto* s=g_stage_session;
        if((s->flags&0x800)==0 && stage_background_has_previous()) {stage_background_begin_transition();s->flags|=0x800;}
        if((s->flags&0x800)!=0 && s->event_timer<30) {
            s->field00c=s->event_timer;
            float& time=*reinterpret_cast<float*>(s->pad014);time+=stage::g_speed_scale;s->event_timer=int32_t(time);
            return THREAD_CONTINUE;
        }
        stage_session_start(s);
    }
    return g_stage_started?session_update(g_stage_session):THREAD_CONTINUE;
}
static ThreadStatus battle_input_dispatch(void*) {
    if(!g_stage_started)return THREAD_CONTINUE;
    if(replay_mode()==ReplayMode::Playback) {
        if(stage::active_shop)return THREAD_CONTINUE;
        uint32_t held;
        if(replay_next_input(held))input_battle_update(held);
        else {
            input_battle_clear();
            // 462b07..462b2a: the explicit FFFF frame opens 4588f0 at A0x11.
            if (replay_finished()) laser::open_replay_end();
        }
    } else {
        input_battle_update(input_state().held&0xffffu);
        if(!stage::active_shop)replay_record_input(input_battle_frame(),platform_frame_fps());
    }
    return THREAD_CONTINUE;
}
void stage_dispatchers_init() {
    g_session_dispatcher=thread_create(session_dispatch);thread_set_priority(g_session_dispatcher,0x10);thread_enqueue_logic(g_session_dispatcher);
    g_input_dispatcher=thread_create(battle_input_dispatch);thread_set_priority(g_input_dispatcher,0x11);thread_enqueue_logic(g_input_dispatcher);
}
void stage_dispatchers_shutdown() {thread_remove(g_session_dispatcher);thread_remove(g_input_dispatcher);}

// 0x004437b0 / 0x4424e0：创建会话，按顺序建关卡对象、敌机管理器、对象管理器（0x7c4），
// 最后注册 A 队会话任务（0x443d70 prio 0x10）与 B 队会话任务（0x443d80 prio 2）。
// stage_index 是 0 起的关卡编号（6 = Extra，对应资源 st07）。
StageSession* stage_session_create(uint32_t stage_index,uint32_t flags) {
    auto* s=new StageSession{};g_stage_session=s;g_stage_loaded=false;g_stage_started=false;
    session().stage=int32_t(stage_index);s->param=flags;s->flags=4;s->ending_wait=0x3c;
    s->task_logic=g_session_dispatcher;
    s->task_logic->flags&=~2u;
    s->task_render=thread_create(session_render);thread_set_priority(s->task_render,2);thread_set_ctx(s->task_render,s);thread_enqueue_render(s->task_render);
    return s;
}
void stage_session_load(StageSession* s) {
    const uint32_t stage_index=uint32_t(session().stage);
    stage::g_speed_scale=1.0f;
    const bool new_run=!g_stage_transfer;
    if (new_run) app_context()->run_cleared = false;

    session().stage = static_cast<int32_t>(stage_index);
    session().difficulty = static_cast<::Difficulty>(app_context()->difficulty);
    session().player = app_context()->selected_player;
    session().logic_frame = 0;
    session().score = 0;
    if(replay_mode()==ReplayMode::Playback)session().mode=GameMode::Replay;
    if(replay_mode()!=ReplayMode::Playback && new_run)replay_begin_record(storage_config());
    if (s->param == 0)
        score_data().character[session().player][0x12ef5 + (app_context()->difficulty * 9 + stage_index) * 8] = 1;
    stage::g_difficulty = app_context()->difficulty;
    player_system_init(app_context()->difficulty,stage_index,new_run);
    session().score=g_status.score;
    replay_begin_stage(stage_index);
    g_status.spell_id = replay_mode() == ReplayMode::Playback ?
        replay_info().spell_id : app_context()->selected_spell;
    if(new_run) {
        if(replay_mode()==ReplayMode::Playback)card_manager_begin_run(replay_stage_cards(),replay_stage_card_count(),1);
        else card_manager_begin_run(app_context()->initial_cards,app_context()->initial_card_count);
        // 4424e0: Extra's default Magatama2 is added before the replay card
        // checkpoint. A deck or playback that already contains it gets one copy.
        if (stage_index == 7 && !card_manager_owned(Magatama2))
            card_manager_acquire(Magatama2, 0);
    }
    replay_prepare_stage_cards();
    bomb_begin_run(app_context()->selected_player);
    storage_log("session create: stage_create");
    storage_log("session: stage_create begin");
    stage::stage_create();
    stage::hud_create(app_context()->difficulty,app_context()->selected_player);
    storage_log("session: stage_create ok");
    storage_log("session create: resources");
    const stage::StageResources& res = stage::stage_resources(static_cast<stage::StageId>(stage_index));
    stage_background_create(res.std);
    card_manager_stage_start();
    storage_log("session create: enemy manager");
    storage_log("session: enemy mgr begin ecl=%s", res.ecl);
    stage::create_enemy_manager(res.ecl);
    spell_create();
    storage_log("session: enemy mgr ok");
    // 0x4424e0 里敌机管理器之后是 0x7c4 对象管理器（DAT_004cf3f4）与激光管理器
    // （DAT_004cf40c，0x3f8）；后者构造尚未反编译，先建 0x7c4。
    storage_log("session create: laser manager");
    storage_log("session: obj mgr begin");
    laser::create_manager();
    storage_log("session: obj mgr ok");
    laser::create_laser_manager();
    storage_log("session: laser mgr ok");
    storage_log("session create: threads");

    g_stage_transfer=false;g_stage_loaded=true;s->flags&=~4u;
}
void stage_session_start(StageSession* s) {
    player_stage_start();
    replay_start_stage();stage::SpawnArgs main_args{};stage::create_enemy("main",main_args);
    // 443e60 still enters "main"; ECL -9907 routes to the chosen spell.
    if(g_status.spell_id < 0)bgm_play(session().stage==7?15:(session().stage-1)*2+1);
    g_stage_started=true;
    s->task_logic->flags|=2;static_cast<Thread*>(g_player->thread_logic)->flags|=2;
    stage::g_enemy_manager->task_logic->flags|=2;stage::g_stage->task_logic->flags|=2;
    stage::background_logic_task->flags|=2;
    stage::hud_start_cards(); // 443e60/4443c0: player -> replay -> main -> HUD (407f10).
    s->flags&=~0x800u;s->event_timer=0;*reinterpret_cast<float*>(s->pad014)=0;
    text_loading_stop();app_finish_stage_loading();
}

// 0x004432c0 的端口子集：停会话任务、销毁对象管理器/敌机管理器/关卡对象与会话对象。
void stage_session_destroy(StageSession* s) {
    if (s == nullptr) return;
    while (g_bomb_shakes != nullptr) {
        BombShake* shake = g_bomb_shakes;
        g_bomb_shakes = shake->next;
        thread_remove(shake->task);
        delete shake;
    }
    while (g_screen_shakes != nullptr) {
        ScreenShake* shake = g_screen_shakes;
        g_screen_shakes = shake->next;
        thread_remove(shake->task);
        delete shake;
    }
    gfx_set_camera_shake(1, 0, 0);
    gfx_set_camera_shake(3, 0, 0);
    if (g_clear_fade_logic != nullptr) {
        thread_remove(g_clear_fade_logic);
        thread_remove(g_clear_fade_render);
        g_clear_fade_logic = g_clear_fade_render = nullptr;
    }
    if(!g_stage_loaded) {
        card_manager_end_run();g_stage_transfer=false;
        stage_background_destroy();
        if(g_stage_clear_vm){anm_vm_free(g_stage_clear_vm);g_stage_clear_vm=nullptr;}
        thread_remove(s->task_render);g_stage_session=nullptr;g_stage_started=false;delete s;return;
    }
    if(stage::active_shop) {
        thread_remove(stage::shop_task);stage::shop_task=nullptr;
        shop_destroy(stage::active_shop);stage::active_shop=nullptr;
    }
    msg_unload();
    score_save();
    const bool carry=g_stage_transfer && app_context()->next_state==0xc;
    stage::hud_destroy();
    if(carry)card_manager_detach_player();
    else {
        g_stage_transfer=false;
        if(g_stage_clear_vm){anm_vm_free(g_stage_clear_vm);g_stage_clear_vm=nullptr;}
        card_manager_end_run();
    }
    bomb_end_run();
    spell_destroy();
    player_system_shutdown();
    thread_remove(s->task_render);
    laser::destroy_laser_manager(laser::laser_manager);
    laser::destroy_manager(laser::manager);
    stage::destroy_enemy_manager(stage::g_enemy_manager);
    if(carry)stage_background_retain_current();else stage_background_destroy();
    stage::stage_destroy(stage::g_stage);
    if (g_stage_session == s) g_stage_session = nullptr;
    delete s;g_stage_loaded=g_stage_started=false;
    if(!carry)stage::g_speed_scale=1.0f;
}
