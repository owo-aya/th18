#include "player/bomb.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "cards/card_manager.h"
#include "cards/card_passive.h"
#include "gameplay/spell.h"
#include "stage/session.h"
#include "core/thread.h"
#include "player/player.h"
#include "projectiles/laser.h"
#include "presentation/anm.h"
#include "presentation/sound.h"
#include "stage/enemy.h"
#include "stage/stage.h"

namespace {
constexpr float kPi = 3.141592741f;
constexpr float kTau = 6.283185482f;
struct ReimuOrb {
    AnmVm* vm;
    PlayerBullet* hitbox;
    float pos[3];
    float center[2];
    float velocity[2];
    float angle;
    float radius;
    float radial_speed;
    float angular_speed;
    float speed;
    float time;
    int timer;
    int previous_timer;
    int index;
    bool active;
};
Thread* g_task;
AnmVm* g_primary;
AnmVm* g_secondary;
ReimuOrb g_orbs[24];
int g_player_id;
bool g_active;
bool g_card_active;
bool g_enhanced;
bool g_spell_penalty; // 原Bomb+68；构造清零，grace之后发动时置1。
float g_time;
int g_timer;
int g_previous_timer;
float g_pos[3];
float g_angle;

float wrap_angle(float a) {
    while (a > kPi) a -= kTau;
    while (a < -kPi) a += kTau;
    return a;
}

void position_vm(AnmVm* vm, const float* pos) {
    vm->extra_x = pos[0];
    vm->extra_y = pos[1];
    vm->extra_z = pos[2];
}

AnmVm* create_vm(int script, const float* pos, AnmVm** owner) {
    auto* resource = reinterpret_cast<AnmResource*>(g_player->anm_player);
    AnmVm* vm = anm_vm_alloc(resource);
    anm_vm_bind_script(vm, resource, script);
    vm->flags3 |= 0x1000;
    position_vm(vm, pos);
    anm_vm_update(vm);
    anm_queue_add(vm);
    // 488350 -> A22. An earlier Bomb's fading VM keeps its own lifetime when
    // its handle slot is reused; deleting it must not clear the replacement.
    if (owner && *owner) anm_manage_vm(*owner, nullptr);
    anm_manage_vm(vm, owner);
    return vm;
}

void invulnerable(int frames) {
    g_player->invuln = frames;
    g_player->invuln_value = static_cast<float>(frames);
    g_player->anim_id = frames - 1;
    g_player->flags_47780 = 0;
    g_player->flags_47784 |= 1;
}

// 420f80: world position through +5bc, without screen origin or coordinate scale.
void vm_position(AnmVm* vm, float* pos) {
    const float* offset = reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(vm) + 0x4e4);
    pos[0] = vm->pos_x + vm->extra_x + offset[0];
    pos[1] = vm->pos_y + vm->extra_y + offset[1];
    pos[2] = vm->pos_z + vm->extra_z + offset[2];
    if (vm->root && !(vm->flags3 & 0x40000)) {
        auto* parent = vm->root;
        if (vm->flags3 & 0x2000000) {
            const float c = std::cos(parent->rot_z), s = std::sin(parent->rot_z);
            const float x = pos[0];
            pos[0] = x * c - pos[1] * s;
            pos[1] = x * s + pos[1] * c;
        }
        float base[3];
        vm_position(parent, base);
        for (int i = 0; i < 3; ++i) pos[i] += base[i];
    }
}

void start_orb(int slot, int index, int damage, bool reverse) {
    ReimuOrb& orb = g_orbs[slot];
    orb = {};
    std::memcpy(orb.pos, &g_player->x, sizeof(orb.pos));
    orb.center[0] = g_player->x;
    orb.center[1] = g_player->y;
    orb.angle = wrap_angle(index * (kPi / 4.0f));
    orb.radial_speed = reverse ? -0.049087386f : 0.049087386f;
    orb.angular_speed = reverse ? -0.104719758f : 0.104719758f;
    create_vm(g_enhanced ? 23 : 22, orb.pos, &orb.vm);
    orb.hitbox = player_damage_circle(orb.pos, 56.0f, 0.0f, 9999, damage);
    orb.hitbox->flags |= 4;
    orb.hitbox->hit_interval = 3;
    orb.hitbox->damage_max = 400;
    orb.index = index;
    orb.previous_timer = -1;
    orb.active = true;
}

void explode_orb(ReimuOrb& orb) {
    // 0x4212a0/0x422176：圆从64每帧扩8，11帧100伤害；旧追踪圈退出。
    sound_play_se_at(27, orb.pos[0]);
    stage::cancel_ordinary_bullets_circle(orb.pos[0], orb.pos[1], 128.0f, 0);
    laser::cancel_circle(orb.pos[0], orb.pos[1], 128.0f, 1, 1);
    PlayerBullet* hit = player_damage_circle(orb.pos, 64.0f, 8.0f, 11, 100);
    hit->flags |= 4;
    anm_set_label(orb.vm, 1);
    orb.hitbox->flags &= ~1u;
    orb.hitbox = nullptr;
    orb.active = false;
}

stage::Enemy* nearest_enemy(float x, float y) {
    stage::Enemy* target = nullptr;
    // 421e10 passes a 512-pixel search radius to 438cb0.
    float distance = 512.0f * 512.0f;
    for (auto* node = stage::g_enemy_manager->enemy_head; node; node = node->next) {
        stage::Enemy* enemy = node->self;
        if (enemy->flag & 0xc000021) continue;
        const float dx = enemy->x - x, dy = enemy->y - y;
        const float d = dx * dx + dy * dy;
        if (d < distance) { distance = d; target = enemy; }
    }
    return target;
}

void update_reimu() {
    invulnerable(40);
    if (g_secondary) position_vm(g_secondary, &g_player->x);
    if (g_timer != g_previous_timer && g_timer == 0)
        for (int i = 0; i < 8; ++i) start_orb(i, i, 15, false);
    if (g_timer != g_previous_timer && g_enhanced && g_timer == 80) {
        sound_play_se_at(44, g_player->x);
        for (int i = 0; i < 8; ++i) start_orb(8 + i, i, 7, true);
    }
    if (g_timer == (g_enhanced ? 320 : 200)) {
        for (ReimuOrb& orb : g_orbs)
            if (orb.active) explode_orb(orb);
        if (g_secondary) anm_set_label(g_secondary, 1);
        stage_screen_shake(8,6,6);
        return;
    }
    bool alive = false;
    for (int i = 0; i < 24; ++i) {
        ReimuOrb& orb = g_orbs[i];
        alive |= orb.vm != nullptr;
        if (!orb.active) continue;
        const float previous_x = orb.pos[0], previous_y = orb.pos[1];
        const bool changed = orb.timer != orb.previous_timer;
        const int launch = (orb.index + 9) * 10;
        if (orb.timer < launch) {
            if (changed) {
                orb.center[0] = g_player->x;
                orb.center[1] = g_player->y;
                if (orb.timer < 90) orb.radius += 1.5f;
                orb.angle = wrap_angle(orb.angle + orb.angular_speed);
            }
            orb.radius += orb.radial_speed * stage::g_speed_scale;
            const float x = orb.center[0] + std::cos(orb.angle) * orb.radius;
            const float y = orb.center[1] + std::sin(orb.angle) * orb.radius;
            orb.pos[0] = x;
            orb.pos[1] = y;
        } else {
            if (changed && orb.timer == launch) {
                orb.angle = std::atan2(orb.velocity[1], orb.velocity[0]);
                orb.speed = std::hypot(orb.velocity[0], orb.velocity[1]);
            } else if (changed) {
              if (stage::Enemy* target = nearest_enemy(orb.pos[0], orb.pos[1])) {
                const float target_angle = std::atan2(target->y - orb.pos[1], target->x - orb.pos[0]);
                const float turn = wrap_angle(target_angle - orb.angle);
                if (std::fabs(turn) < kPi / 4.0f) {
                    if (std::fabs(turn) < 0.261799395f) orb.speed = std::min(8.0f, orb.speed + 0.2f);
                } else orb.speed = std::max(1.0f, orb.speed - 0.7f);
                // 0x421f46/404bc0：朝目标的最短角差每帧取十分之一。
                orb.angle = wrap_angle(orb.angle + turn * 0.1f);
              } else if (orb.pos[0] < -160 || orb.pos[0] > 160 || orb.pos[1] < 32 || orb.pos[1] > 416) {
                orb.speed *= 0.9f;
              }
            }
            orb.pos[0] += std::cos(orb.angle) * orb.speed * stage::g_speed_scale;
            orb.pos[1] += std::sin(orb.angle) * orb.speed * stage::g_speed_scale;
        }
        orb.pos[0] = std::floor(orb.pos[0] * 100.0f) / 100.0f;
        orb.pos[1] = std::floor(orb.pos[1] * 100.0f) / 100.0f;
        orb.velocity[0] = orb.pos[0] - previous_x;
        orb.velocity[1] = orb.pos[1] - previous_y;
        position_vm(orb.vm, orb.pos);
        std::memcpy(orb.hitbox->motion, orb.pos, sizeof(orb.pos));
        orb.previous_timer = orb.timer;
        orb.time += stage::g_speed_scale;
        orb.timer = static_cast<int>(orb.time);
        if (orb.hitbox->damage_accum >= 400) {
            explode_orb(orb);
            // 422255 issues a second explosion SE before its A14 shake task.
            sound_play_se_at(27,orb.pos[0]);
            stage_screen_shake(8,6,6);
        }
    }
    // 4223a0 runs after all orb movement and impact explosions.
    for (int i=0;i<24;++i) {
        ReimuOrb& orb=g_orbs[i];
        if(orb.active && (g_timer&7)==(i&7)) {
            stage::cancel_ordinary_bullets_circle(orb.pos[0],orb.pos[1],64.0f,0);
            laser::cancel_circle(orb.pos[0],orb.pos[1],64.0f,0,1);
        }
    }
    if (g_timer > 119 && !alive) {
        if (g_secondary) anm_set_label(g_secondary, 1);
        g_active = false;
    }
}

void update_marisa() {
    invulnerable(40);
    if (!g_primary) {
        if (g_secondary) anm_set_label(g_secondary, 1);
        g_active = false;
        return;
    }
    const int end = g_enhanced ? 450 : 300;
    if (g_timer > end) return;
    if (g_timer == end) {
        anm_set_label(g_primary, 1);
        anm_set_label(g_secondary, 1);
        g_player->flags_4779c &= ~4u;
    }
    g_primary->rot_z = g_angle;
    if (g_player->vel_x < 0) g_angle -= 0.002617994f;
    else if (g_player->vel_x > 0) g_angle += 0.002617994f;
    g_player->move_scale = 0.5f;
    std::memcpy(g_pos, &g_player->x, sizeof(g_pos));
    position_vm(g_primary, g_pos);
    position_vm(g_secondary, g_pos);
    if (g_timer != g_previous_timer && g_timer % 3 == 0) {
        const float distances[] = {208, 240, 304};
        const float heights[] = {32, 128, 256};
        const int damage[] = {50, 15, g_enhanced ? 40 : 15};
        for (int i = 0; i < 3; ++i) {
            const float pos[] = {g_pos[0] + std::cos(g_angle) * distances[i],
                                 g_pos[1] + std::sin(g_angle) * distances[i], g_pos[2]};
            PlayerBullet* hit = player_damage_rectangle(pos, g_angle, 512, heights[i], 0, damage[i]);
            hit->flags |= 4;
            if (g_enhanced && i == 2) hit->kind = 3;
        }
    }
    for (int i = 0; AnmVm* beam = anm_find_child(g_primary, g_enhanced ? 35 : 28, i); ++i) {
        float pos[3];
        vm_position(beam, pos);
        stage::cancel_ordinary_bullets_rectangle(pos[0], pos[1],
                                             beam->scale_x * 48, beam->scale_y * 160, g_angle, 0);
        laser::cancel_rectangle(pos[0], pos[1], beam->scale_x * 48, beam->scale_y * 160, g_angle, 1);
    }
}

void update_sakuya() {
    if (!g_primary) { g_active = false; return; }
    if (g_enhanced && (g_timer == 50 || g_timer == 100 || g_timer == 150)) {
        sound_play_se(51);
        create_vm(24, &g_player->x, &g_secondary);
        player_damage_circle(&g_player->x, 8, 10, 40, 7);
        player_damage_circle(g_pos, 640, 0, 20, 50);
    }
    const int stop = g_enhanced ? 230 : 110;
    if (g_timer < stop) stage::set_enemy_bullets_frozen(true);
    else if (g_timer == stop) {
        sound_play_se(27);
        sound_play_se(41);
        stage::set_enemy_bullets_frozen(false);
        player_damage_circle(g_pos, 640, 0, 40, 400);
        stage::cancel_enemy_bullets_circle(g_pos[0], g_pos[1], 600, 3);
    }
}

void update_sanae() {
    invulnerable(40);
    if (!g_primary) {
        if (g_secondary) anm_set_label(g_secondary, 1);
        g_active = false;
        return;
    }
    if (g_timer == 0) player_damage_circle(g_pos, 16, 0.355555564f, 180, 13);
    else if (g_timer == 180 || (g_enhanced && (g_timer == 240 || g_timer == 300)))
        player_damage_circle(g_pos, 80, 20, 100, g_enhanced ? 30 : 80);
    if (g_timer == 180 || (g_enhanced && (g_timer == 240 || g_timer == 300))) sound_play_se(6);
    if (g_timer >= (g_enhanced ? 460 : 400)) {
        anm_set_label(g_primary, 1);
        anm_set_label(g_secondary, 1);
        return;
    }
    const float radius = g_timer < 180 ? g_time * 48 / 180 + 16 :
                         g_timer < 200 ? (g_time - 180) * 416 / 20 + 64 : 480;
    stage::cancel_enemy_bullets_circle(g_pos[0], g_pos[1], radius, 1);
}

ThreadStatus update(void*) {
    if (!stage_session_started()) return THREAD_CONTINUE;
    if (stage::shop_active()) return THREAD_CONTINUE; // 00420040
    if (g_active) {
        spell_on_sun_card();
        switch (g_player_id) {
        case 0: update_reimu(); break;
        case 1: update_marisa(); break;
        case 2: update_sakuya(); break;
        case 3: update_sanae(); break;
        }
        g_previous_timer = g_timer;
        g_time += stage::g_speed_scale;
        g_timer = static_cast<int>(g_time);
    }
    return THREAD_CONTINUE;
}
} // namespace

void bomb_begin_run(int player) {
    g_player_id = player;
    g_active = g_card_active = false;
    g_spell_penalty = false;
    g_primary = g_secondary = nullptr;
    std::memset(g_orbs, 0, sizeof(g_orbs));
    g_task = thread_create(update);
    g_task->flags |= 2; // 原443e60：Bomb A19属于暂停时停步的游戏任务。
    thread_set_priority(g_task, 0x19);
    thread_enqueue_logic(g_task);
}

void bomb_end_run() {
    if (g_task) { thread_remove(g_task); g_task = nullptr; }
    if (g_primary) { anm_vm_free(g_primary); g_primary = nullptr; }
    if (g_secondary) { anm_vm_free(g_secondary); g_secondary = nullptr; }
    for (ReimuOrb& orb : g_orbs) {
        if (orb.vm) anm_vm_free(orb.vm);
        if (orb.hitbox) orb.hitbox->flags &= ~1u;
        orb = {};
    }
    // Player resources are cached here; release their detached effects at stage exit.
    anm_delete_independent(40 + g_player_id);
    if (g_player_id == 2) stage::set_enemy_bullets_frozen(false);
    g_active = g_card_active = false;
}

bool bomb_available() {
    return g_task && g_status.unk_07c >= 1 && !g_active && stage::g_stage &&
           *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(stage::g_stage) + 0x1b0) == 0 &&
           stage::g_enemy_manager && stage::g_enemy_manager->count != 0;
}

static void start_bomb() {
    g_active = true;
    if (spell_active_after_grace()) g_spell_penalty = true;
    spell_on_sun_card();
    g_enhanced = card_manager_owned(MagicScroll);
    card_passive_clear_damage_events();
    ++stage::g_enemy_manager->field40;
    stage::g_enemy_manager->field44 = 0;
    g_time = 0;
    g_timer = 0;
    g_previous_timer = -1;
    g_angle = -kPi / 2;
    std::memcpy(g_pos, &g_player->x, sizeof(g_pos));
    sound_play_se_at(44, g_player->x);
    switch (g_player_id) {
    case 0:
        sound_play_se(49);
        create_vm(37, g_pos, &g_secondary);
        break;
    case 1:
        sound_play_se(49);
        create_vm(g_enhanced ? 29 : 22, g_pos, &g_primary);
        stage_master_spark_shake(g_enhanced);
        create_vm(36, g_pos, &g_secondary);
        g_player->flags_4779c |= 4;
        invulnerable(120);
        break;
    case 2: {
        sound_play_se(51);
        const float pos[] = {0, 0, 0};
        create_vm(g_enhanced ? 25 : 22, pos, &g_primary);
        create_vm(g_enhanced ? 24 : 21, g_pos, &g_secondary);
        invulnerable(g_enhanced ? 300 : 180);
        break;
    }
    case 3:
        sound_play_se(30);
        create_vm(g_enhanced ? 21 : 17, g_pos, &g_primary);
        create_vm(25, g_pos, &g_secondary);
        invulnerable(110);
        break;
    }
}

bool bomb_try_activate() {
    if (!bomb_available() || g_card_active) return false;
    g_status.unk_07c = std::min(g_status.unk_07c - 1, g_status.unk_088);
    start_bomb();
    return true;
}

bool bomb_activate_from_card() {
    if (!g_task || g_active || g_card_active) return false;
    start_bomb();
    return true;
}

bool bomb_active() { return g_active; }

void bomb_marisa_hit(const float* pos) {
    AnmVm* effect = create_vm(20, pos, nullptr);
    float* velocity = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(effect) + 0x4c4);
    velocity[0] = std::cos(g_angle) * 6.5f;
    velocity[1] = std::sin(g_angle) * 6.5f;
}
int bomb_elapsed_frames() { return g_timer; }
void bomb_set_card_active(bool active) { g_card_active = active; }
