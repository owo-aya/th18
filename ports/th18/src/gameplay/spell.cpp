#include "spell.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

#include "core/game_session.h"
#include "core/thread.h"
#include "gameplay/score_data.h"
#include "player/bomb.h"
#include "player/player.h"
#include "presentation/anm.h"
#include "presentation/text.h"
#include "replay/replay.h"
#include "presentation/sound.h"
#include "stage/background.h"
#include "stage/enemy.h"
#include "stage/hud.h"
#include "stage/stage.h"

namespace {
SpellManager* g_spell;
std::vector<AnmVm*> g_roots;
AnmVm* g_result;

// The four spell-background groups of each original 0xd4-byte stage descriptor
// at 004c9410. Resource 7 is the STD ANM; 0/1 are bullet/effect; 2+ are ECL ANIMs.
struct BackgroundEntry { int resource, script, keep_std, extra_resource, extra_script; };
const BackgroundEntry kBackgrounds[7][4] = {
    {{3,10,0,3,18},{3,10,0,3,18},{0,0,0,0,0},{0,0,0,0,0}},
    {{3,11,0,3,18},{3,11,0,3,18},{0,0,0,0,0},{0,0,0,0,0}},
    {{3,10,0,3,17},{3,10,0,-1,-1},{0,0,0,0,0},{0,0,0,0,0}},
    {{3,12,0,3,19},{3,12,0,-1,-1},{0,0,0,0,0},{0,0,0,0,0}},
    {{3,9,0,3,18},{-1,-1,0,-1,-1},{0,0,0,0,0},{0,0,0,0,0}},
    {{3,13,0,3,21},{-1,-1,0,-1,-1},{-1,0,1,3,21},{0,0,0,0,0}},
    {{3,10,0,3,19},{-1,0,1,4,12},{5,12,1,5,19},{5,12,1,5,19}}
};

double seconds_now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

AnmResource* stage_resource(int id) {
    if (id == 7) return stage_background_resource();
    const int index = id == 0 ? 7 : id == 1 ? 8 : 120 + id - 2;
    return anm_manager()->resources[index];
}

AnmVm* create_vm(AnmResource* resource, int script) {
    AnmVm* vm = anm_vm_alloc(resource);
    vm->flags3 |= 0x1000;
    anm_vm_set_script(vm, resource, script);
    vm->flags3 &= ~0x30000u;
    anm_queue_add(vm);
    g_roots.push_back(vm);
    return vm;
}

void game_origin(AnmVm* vm) {
    // The port draws gameplay ANMs in its full-window 1280x960 projection.
    // Convert the native gameplay origin to the script's 640/1280 coordinate mode.
    const float factor = ((vm->flags3 >> 22) & 7) == 2 ? 2.0f : 1.0f;
    vm->extra_x = 224.0f * factor;
    vm->extra_y = 16.0f * factor;
}

void forget_vm(AnmVm* vm) {
    if (g_spell->background == vm) g_spell->background = nullptr;
    if (g_spell->caption == vm) g_spell->caption = nullptr;
    if (g_spell->name_vm == vm) g_spell->name_vm = nullptr;
    if (g_spell->counter == vm) g_spell->counter = nullptr;
    if (g_spell->circle == vm) g_spell->circle = nullptr;
    if (g_result == vm) g_result = nullptr;
}

void free_vm(AnmVm* vm) {
    if (vm == nullptr) return;
    g_roots.erase(std::find(g_roots.begin(), g_roots.end(), vm));
    forget_vm(vm);
    anm_vm_free(vm);
}

void update_vms() {
    for (auto it = g_roots.begin(); it != g_roots.end();) {
        AnmVm* vm = *it;
        anm_vm_update(vm);
        // A deleted parent still owns live descendants (notably effect20).
        if (vm->script_cursor < 0 && !(vm->flags2 & 1) && anm_find_child(vm, -1) == nullptr) {
            forget_vm(vm);
            anm_vm_free(vm);
            it = g_roots.erase(it);
        } else {
            ++it;
        }
    }
}

void display_label(int label) {
    if (g_spell->caption) anm_set_label(g_spell->caption, label);
    if (g_spell->name_vm) anm_set_label(g_spell->name_vm, label);
    if (g_spell->counter) anm_set_label(g_spell->counter, label);
}

void boss_position(float* position) {
    stage::Enemy* boss = stage::find_enemy(stage::g_enemy_manager->boss_id[0]);
    position[0] = boss->x;
    position[1] = boss->y;
    position[2] = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(boss) + 0x1278);
}

void move_circle() {
    if (g_spell->circle) {
        g_spell->circle->extra_x = g_spell->position[0] + 224.0f;
        g_spell->circle->extra_y = g_spell->position[1] + 16.0f;
        g_spell->circle->extra_z = g_spell->position[2];
    }
}

// 004728a0 samples after the gameplay callbacks. The replay stores this packed
// real-time value separately from the deterministic gameplay-frame count.
void sample_time() {
    SpellManager& spell = *g_spell;
    if ((spell.flags & 1) && !(spell.flags & 0x40)) {
        spell.flags |= 0x40;
        spell.started_seconds = seconds_now();
    } else if (!(spell.flags & 1) && (spell.flags & 0x40)) {
        spell.last_frames = spell.frame_count;
        const double elapsed = seconds_now() - spell.started_seconds - spell.excluded_seconds;
        const int seconds = (std::min)(static_cast<int>(elapsed), 999);
        const int centiseconds = static_cast<int>((elapsed - static_cast<int>(elapsed)) * 100.0);
        const uint32_t encoded = ((seconds + 22 + centiseconds) * 1000 + (seconds + 66) % 1000) * 100
                               + (centiseconds + 33) % 100;
        spell.recorded_time = replay_spell_time(spell.timing_index++, encoded);
        spell.excluded_seconds = 0.0;
        spell.flags &= ~0x40u;
    }
}

ThreadStatus update(void*) {
    update_vms();
    SpellManager& spell = *g_spell;
    if (spell.flags & 1) {
        ++spell.frame_count;
        if (spell.timer > 59 && !(spell.flags & 0x200)) stage_background_set_visible(false);
        if (spell.timer > 299 && !(spell.flags & 8))
            spell.bonus = ((spell.bonus - (spell.initial_bonus - spell.initial_bonus / 3) /
                          (spell.time_limit - 300)) / 10) * 10;
        spell.timer_reset = 0;
        spell.timer_previous = spell.timer;
        if (stage::g_speed_scale > .99f && stage::g_speed_scale < 1.01f) {
            ++spell.timer;
            spell.timer_fraction += 1.0f;
        } else {
            spell.timer_fraction += stage::g_speed_scale;
            spell.timer = static_cast<int>(spell.timer_fraction);
        }
        if (spell.timer >= 120) {
            const bool near = (spell.flags & 0x100) ? g_player->y > 352.0f : g_player->y < 96.0f;
            const bool far = (spell.flags & 0x100) ? g_player->y < 320.0f : g_player->y > 128.0f;
            if (!(spell.flags & 4) && near) { display_label(3); spell.flags |= 4; }
            else if ((spell.flags & 4) && far) { display_label(2); spell.flags &= ~4u; }
        }
        // 429eb0 keeps the last position after the boss slot has been cleared.
        if (stage::g_enemy_manager->boss_id[0] != 0) {
            if (stage::Enemy* boss = stage::find_enemy(stage::g_enemy_manager->boss_id[0])) {
                const float position[3] = {boss->x, boss->y,
                    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(boss) + 0x1278)};
                for (int i = 0; i < 3; ++i) spell.position[i] += (position[i] - spell.position[i]) * .05f;
            }
        }
        move_circle();
        if ((spell.flags & 0x20) && !bomb_active()) spell.flags &= ~0x20u;
    }
    sample_time();
    return THREAD_CONTINUE;
}

void queue_time(int frames, int seconds, int centiseconds, uint32_t alpha) {
    TextPos pos{448.0f, 288.0f, 0};
    text_set_style(4);
    text_set_size(2.0f, 2.0f);
    text_set_color(alpha | 0x00ffffff);
    text_draw_fmt(text_manager(), &pos, "%3d.", (std::min)(frames / 60, 999));
    pos = {536.0f, 300.0f, 0};
    text_set_size(1.2f, 1.2f);
    text_draw_fmt(text_manager(), &pos, "%.2ds", (frames % 60) * 100 / 60);
    text_set_color(alpha | 0x00808080);
    pos = {448.0f, 320.0f, 0};
    text_set_size(2.0f, 2.0f);
    text_draw_fmt(text_manager(), &pos, "%3d.", seconds);
    pos = {536.0f, 332.0f, 0};
    text_set_size(1.2f, 1.2f);
    text_draw_fmt(text_manager(), &pos, "%.2ds", centiseconds);
}

ThreadStatus render(void*) {
    SpellManager& spell = *g_spell;
    text_set_life(0);
    *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(text_manager()) + 0x19248) = 2;
    if ((spell.flags & 1) && spell.counter) {
        text_set_style(2);
        text_set_size(2.0f, 2.0f);
        text_set_color((spell.counter->color & 0xff000000) | 0x00ffffff);
        TextPos pos{532.0f, 70.0f, 0};
        if (spell.flags & 2) text_draw_fmt(text_manager(), &pos, "%8d", spell.bonus);
        else { pos.x = 564.0f; text_draw_fmt(text_manager(), &pos, "$"); }
        const int column = session().mode == GameMode::SpellPractice ||
            (replay_mode() == ReplayMode::Playback && (replay_info().mode_flags & 2) != 0) ? 4 : 0;
        const uint8_t* record = score_data().character[session().player] + 0x990 + spell.id * 0xdc + column;
        const int captures = *reinterpret_cast<const int32_t*>(record);
        const int attempts = *reinterpret_cast<const int32_t*>(record + 8);
        pos = {720.0f, 70.0f, 0};
        if (captures >= 100) text_draw_fmt(text_manager(), &pos, "MASTER");
        else if (attempts >= 100) text_draw_fmt(text_manager(), &pos, "%.2d/99+", captures);
        else text_draw_fmt(text_manager(), &pos, "%.2d/%.2d", captures, attempts);
    }
    if (g_result) {
        const int seconds = (spell.recorded_time / 100 % 1000 + 934) % 1000;
        const int centiseconds = (spell.recorded_time % 100 + 67) % 100;
        queue_time(spell.last_frames, seconds, centiseconds, g_result->color & 0xff000000);
    }
    text_set_style(0);
    text_set_color(0xffffffff);
    text_set_size(2.0f, 2.0f);
    *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(text_manager()) + 0x19248) = 0;
    return THREAD_CONTINUE;
}

void capture_display(int bonus) {
    AnmResource* front = anm_get(5, "front.anm");
    game_origin(create_vm(front, 0x31));
    int divisor = 10000000;
    bool leading_zero = true;
    AnmResource* ascii = anm_get(2, "ascii.anm");
    for (int i = 0; i < 8; ++i) {
        AnmVm* digit = create_vm(ascii, 4 + i);
        game_origin(digit);
        const int value = bonus / divisor % 10;
        anm_set_sprite(digit, ascii, 239 + value);
        leading_zero = leading_zero && value == 0;
        if (leading_zero) digit->flags2 &= ~2u;
        divisor /= 10;
    }
    for (int i = 0; i < 2; ++i) {
        AnmVm* comma = create_vm(ascii, 12 + i);
        game_origin(comma);
        anm_set_sprite(comma, ascii, 0xfd);
        if (bonus <= (i == 0 ? 999999 : 999)) comma->flags2 &= ~2u;
    }
}
}

SpellManager* spell_manager() { return g_spell; }

void spell_create() {
    g_spell = new SpellManager{};
    g_spell->flags0 = 2;
    g_spell->logic = thread_create(update);
    thread_set_priority(g_spell->logic, 0x1f);
    thread_enqueue_logic(g_spell->logic);
    g_spell->render = thread_create(render);
    thread_set_priority(g_spell->render, 0xc);
    thread_enqueue_render(g_spell->render);
}

void spell_destroy() {
    if (g_spell == nullptr) return;
    thread_remove(g_spell->logic);
    thread_remove(g_spell->render);
    for (AnmVm* vm : g_roots) anm_vm_free(vm);
    g_roots.clear();
    g_result = nullptr;
    delete g_spell;
    g_spell = nullptr;
}

void spell_start(int id, const char* sjis_name, int time_limit, int background_index) {
    SpellManager& spell = *g_spell;
    spell.timer_previous = -1;
    spell.timer = 0;
    spell.timer_fraction = 0.0f;
    spell.timer_reset = 0;
    spell.timer_flags |= 1;
    spell.id = id;
    std::strcpy(spell.name, sjis_name);
    spell.flags = (spell.flags & ~0x98u) | 3;
    if (replay_mode() != ReplayMode::Playback)
        score_spell_start(session().player, id, session().mode == GameMode::SpellPractice, sjis_name);
    stage::hud_spell_start();
    spell.frame_count = 1;
    AnmResource* ascii = anm_get(2, "ascii.anm");
    spell.caption = create_vm(ascii, 0);
    spell.name_vm = create_vm(anm_get(0, "text.anm"), 2);
    text_write_texture(spell.name_vm, 0xffffff, 0, 0, 0, 0, sjis_name);
    spell.counter = create_vm(ascii, 1);
    sound_play_se(0x21);
    spell.circle = create_vm(anm_get(8, "effect.anm"), 13);
    boss_position(spell.position);
    move_circle();
    for (int script : {11,12})
        *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(anm_find_child(spell.circle, script)) + 0x4ac) = time_limit;
    constexpr int bonuses[5] = {500000,1000000,1500000,2000000,1000000};
    spell.bonus = bonuses[static_cast<int>(session().difficulty)] * session().stage;
    spell.initial_bonus = (std::min)(spell.bonus, 999999999);
    spell.time_limit = time_limit;
    game_origin(create_vm(anm_get(8, "effect.anm"), 20));
    const BackgroundEntry& background = kBackgrounds[session().stage - 1][background_index];
    if (background.resource >= 0) {
        spell.background = create_vm(stage_resource(background.resource), background.script);
        game_origin(spell.background);
    }
    spell.flags = (spell.flags & ~0x200u) | (background.keep_std << 9);
    if (background.extra_resource != -1)
        game_origin(create_vm(stage_resource(background.extra_resource), background.extra_script));
    spell.flags &= ~0x60u;
    *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(&g_status) + 0xf8) &= ~0x800u;
}

void spell_end() {
    SpellManager& spell = *g_spell;
    if (!(spell.flags & 1)) return;
    stage_background_set_visible(true);
    display_label(1);
    spell.flags &= ~1u;
    free_vm(spell.background);
    free_vm(spell.circle);
    spell.flags &= ~0x20u;
    stage::hud_spell_end();
    if (!(spell.flags & 2)) {
        game_origin(create_vm(anm_get(5, "front.anm"), 0x32));
    } else {
        session().score = static_cast<uint32_t>((std::min)(static_cast<int64_t>(session().score) + spell.bonus / 10, int64_t{999999999}));
        g_status.score = session().score;
        capture_display(spell.bonus);
        if (replay_mode() != ReplayMode::Playback)
            score_spell_capture(session().player, spell.id, session().mode == GameMode::SpellPractice);
        sound_play_se(0x2e);
    }
    g_result = create_vm(anm_get(5, "front.anm"), 0x54);
    game_origin(g_result);
    if (spell.flags & 0x80) sound_play_se(0x45);
}

void spell_on_sun_card() {
    if (g_spell && (g_spell->flags & 1)) {
        if (g_spell->timer >= 60) { g_spell->bonus = 0; g_spell->flags &= ~0x22u; }
        else if (bomb_active()) g_spell->flags |= 0x20;
    }
}

bool spell_active_after_grace() { return g_spell && (g_spell->flags & 1) && g_spell->timer >= 60; }

void spell_disable_circle() {
    g_spell->flags |= 0x10;
    free_vm(g_spell->circle);
}
