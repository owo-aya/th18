#include "cards/card_passive.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <list>

#include "cards/ability_card.h"
#include "cards/card_manager.h"
#include "core/app.h"
#include "core/game_session.h"
#include "core/rng.h"
#include "gameplay/spell.h"
#include "player/player.h"
#include "player/bomb.h"
#include "projectiles/laser.h"
#include "presentation/anm.h"
#include "presentation/sound.h"
#include "stage/session.h"
#include "stage/enemy.h"
#include "stage/stage.h"

namespace {

struct PassiveVisual {
    AbilityCard* owner;
    AnmVm* vm;
};
std::list<PassiveVisual> g_visuals;
int g_damage_events = 0;
constexpr float kPi = 3.1415927410125732f;

// 40a560 / 40da80 / 40cef0: previous, integer time, fractional time, reset, flags.
void advance_passive_timer(int32_t* timer) {
    timer[3]=0;
    timer[0]=timer[1];
    float& time=*reinterpret_cast<float*>(timer+2);
    const float speed=stage::g_speed_scale;
    if(speed>0.99f && speed<1.01f) {
        ++timer[1];time+=1.0f;
    } else {
        time+=speed;timer[1]=static_cast<int32_t>(time);
    }
}

float& player_float(int offset) {
    return *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(g_player) + offset);
}
int& total_money() { return *reinterpret_cast<int*>(g_status.unk_054); }

void add_bomb() { status_add_bomb(&g_status); }
void add_bomb_fragment() { status_add_bomb_fragment(&g_status); }
void add_life_fragment() { status_add_life_fragment(&g_status); }

AnmVm* create_visual(AbilityCard* owner, AnmResource* resource, int script, const float* position) {
    AnmVm* vm = anm_vm_alloc(resource);
    vm->extra_x = position[0];
    vm->extra_y = position[1];
    vm->extra_z = position[2];
    anm_vm_set_script(vm, resource, script);
    anm_queue_add(vm);
    g_visuals.push_back({owner, vm});
    anm_manage_vm(vm, &g_visuals.back().vm);
    return vm;
}

void remove_visuals(AbilityCard* card) {
    for (auto it = g_visuals.begin(); it != g_visuals.end();) {
        if (it->owner == card) {
            anm_vm_free(it->vm);
            it = g_visuals.erase(it);
        } else ++it;
    }
}

AnmVm* find_visual(uint32_t handle) {
    for (PassiveVisual& visual : g_visuals)
        if (visual.vm && visual.vm->handle == handle) return visual.vm;
    return nullptr;
}

void discard_visual(uint32_t handle) {
    for (auto it = g_visuals.begin(); it != g_visuals.end(); ++it) {
        if (it->vm && it->vm->handle == handle) {
            anm_vm_free(it->vm);
            g_visuals.erase(it);
            return;
        }
    }
}

AnmVm* create_option_shield(AbilityCard* card, const int32_t* option) {
    const float position[] = {option[0x17] / 128.0f, option[0x18] / 128.0f, 0.0f};
    AnmVm* vm = create_visual(card, card_manager_ability(), 27, position);
    vm->flags3 = (vm->flags3 & ~0x200000u) | 0x101000u;
    vm->draw_layer = 14;
    return vm;
}

const ShtInstruction* instruction_for(const AttackObject* shot) {
    const auto* sht = static_cast<const PlayerSht*>(g_player->sht);
    return reinterpret_cast<const ShtInstruction*>(sht->sub_offset[shot->command >> 8]) + (shot->command & 255);
}

void restore_live_state() {
    if ((g_player->flags_644 & 1) == 0) {
        g_player->flags_640 = 0;
        g_player->flags_644 |= 1;
    }
    g_player->timer_state = 60;
    g_player->timer_float = 60.0f;
    g_player->unk_634 = 59;
    g_player->state = 1;
}

void start_cancel(AbilityCard* card) {
    AnmVm* visual = create_visual(card, card_manager_ability(), 41, &g_player->x);
    card->extra_58 = static_cast<int32_t>(visual->handle);
    PlayerBullet* bullet = player_damage_circle(&g_player->x, 8.0f, 0.0f, 40, 5);
    bullet->damage_max = 9999999;
    *reinterpret_cast<PlayerBullet**>(reinterpret_cast<uint8_t*>(card) + 0x5c) = bullet;
    card->extra_54 = 1;
    sound_play_se(0x2c);
    card->unk_20 = -1;
    card->unk_24 = 0;
    card->unk_28 = 0.0f;
    if ((card->flags_30 & 1) == 0) { card->unk_2c=0; card->flags_30|=1; }
    if ((g_player->flags_47784 & 1) == 0) {
        g_player->flags_47780=0;
        g_player->flags_47784|=1;
    }
    g_player->anim_id=59;
    g_player->invuln = 60;
    g_player->invuln_value = 60.0f;
    ++stage::g_enemy_manager->field40;
    stage::g_enemy_manager->field44=0;
    bomb_set_card_active(true);
    spell_on_sun_card(); // 40a2a0/40d840 use the same spell grace-period branch.
    restore_live_state();
}

bool immediate(AbilityCard* card) {
    const CardId id = card_id(card);
    const bool live = stage_session() != nullptr && (card->flags_50 & 2u) != 0;
    switch (id) {
    case Extend2:
        if (live) {
            g_status.lives_max = std::min(g_status.lives_max + 1, 7);
            add_life_fragment();
        }
        break;
    case Bomb2: if (live) add_bomb_fragment(); break;
    case Pendulum:
        if (live) { total_money() += 50; g_status.money += 50; }
        break;
    case Dango:
        if (live && status_add_power(&g_status, 50)) sound_play_se_at(13, g_player->x);
        break;
    default: return false;
    }
    if (live) card->flags_50 &= ~2u;
    return true;
}

bool equip(AbilityCard* card) {
    const bool live = stage_session() != nullptr && (card->flags_50 & 2u) != 0;
    switch (card_id(card)) {
    case Extend:
        if (live) { g_status.lives_max = std::min(g_status.lives_max + 1, 7); status_add_life(&g_status); }
        if (live) card->flags_50 &= ~2u;
        return true;
    case Bomb:
        if (live) { g_status.unk_088 = std::min(g_status.unk_088 + 1, 7); add_bomb(); }
        if (live) card->flags_50 &= ~2u;
        return true;
    case Mokou:
        if (live) { g_status.lives_max = 7; for (int i = 0; i < 3; ++i) status_add_life(&g_status); }
        break;
    case Narumi: if (live) status_add_life(&g_status); break;
    case PowerMax:
        if (live && status_add_power(&g_status, 100)) sound_play_se_at(13, g_player->x);
        break;
    }
    if (live) card->flags_50 &= ~2u;
    return false;
}

} // namespace

bool card_passive_acquire(AbilityCard* card, uint32_t flags) {
    // 00411460: slot0 and slot1 short-circuit independently of adding to the list.
    if (card_id(card) == Mukade) g_damage_events = 0;
    if ((flags & 1u) == 0 && immediate(card)) return true;
    return (flags & 2u) != 0 && equip(card);
}

uint32_t card_passive_on_death(AbilityCard* card, uint32_t handled) {
    switch (card_id(card)) {
    case AutoBomb:
        if (handled != 0) break;
        if (card_manager_owned(Rokumon) && g_status.money >= 200) break;
        if (bomb_available()) {
            start_cancel(card);
            g_status.unk_07c = std::min(std::max(0, g_status.unk_07c - 2), g_status.unk_088);
            return 1;
        }
        break;
    case Rokumon:
        if (g_status.money >= 200) {
            start_cancel(card);
            g_status.money -= 200;
            return 1;
        }
        break;
    case DeadBombExtend: card->extra_54 = g_status.money; break;
    case Koishi:
    case PowerMax: card->extra_54 = g_status.power; break;
    }
    return 0;
}

void card_passive_on_hit(AbilityCard* card) {
    if (card_id(card) == DeadBombExtend) g_player->respawn_wait = 15;
}

void card_passive_after_death_penalty(AbilityCard* card) {
    switch (card_id(card)) {
    case DeadBombExtend:
        total_money() = g_status.money = card->extra_54;
        break;
    case Koishi:
        card->extra_54 = std::max(card->extra_54 - 50, 100);
        g_status.power = std::clamp(card->extra_54, g_status.power_step, g_status.power_max);
        break;
    case PowerMax:
        card->extra_54 = std::min(card->extra_54, g_status.power_step * 3);
        g_status.power = std::clamp(card->extra_54, g_status.power_step, g_status.power_max);
        break;
    case DeadSpell:
        stage::spawn_item(7, &g_player->x, -kPi * 0.5f, 3.2f, 3);
        break;
    }
}

void card_passive_player_reset(AbilityCard* card) {
    switch (card_id(card)) {
    case ItemCatch:
        player_float(0x47988) = 10.0f;
        player_float(0x4798c) = 30.0f;
        player_float(0x47990) = player_float(0x47994) = 110.0f;
        break;
    case ItemLine:
        player_float(0x47998) = 224.0f;
        if (stage::g_stage != nullptr) stage::g_stage->flags |= 0x2000u;
        break;
    case SpeedQueen:
        player_float(0x4799c) = 1.0f;
        player_float(0x479a4) = player_float(0x479a8) = 0.5f;
        card->extra_54 = card->extra_58 = 0;
        break;
    case OptionBR: {
        auto* raw = reinterpret_cast<uint8_t*>(card);
        card->param_48 = 360;
        card->unk_20 = -1;
        card->unk_24 = 0;
        card->unk_28 = 0.0f;
        for (int option = 0; option < 4; ++option) {
            auto* timer = reinterpret_cast<int32_t*>(raw + 0x54 + option * 0x14);
            timer[0] = 359;
            timer[1] = 360;
            reinterpret_cast<float*>(timer)[2] = 360.0f;
            timer[3] = 0;
            timer[4] |= 1u;
        }
        break;
    }
    }
}

void card_passive_options_reset(AbilityCard* card) {
    if (card_id(card) != OptionBR) return;
    auto* raw = reinterpret_cast<uint8_t*>(card);
    for (int slot = 0; slot < 4; ++slot) {
        auto* handle = reinterpret_cast<uint32_t*>(raw + 0xa4 + slot * 4);
        discard_visual(*handle);
        *handle = 0;
        const auto* option = reinterpret_cast<const int32_t*>(g_player->option_a + slot * 0xf0);
        const int elapsed = *reinterpret_cast<const int32_t*>(raw + 0x58 + slot * 0x14);
        if (option[0] != 0 && elapsed >= card->param_48)
            *handle = create_option_shield(card, option)->handle;
    }
}

void card_passive_player_update(AbilityCard* card) {
    switch (card_id(card)) {
    case SpeedQueen: {
        const auto* sht = reinterpret_cast<const uint8_t*>(g_player->sht);
        if (g_player->flags_4779c & 0x200u) {
            g_player->speed_normal = static_cast<int>(*reinterpret_cast<const float*>(sht + 0x10) * 128.0f);
            g_player->speed_diag = static_cast<int>(*reinterpret_cast<const float*>(sht + 0x18) * 128.0f);
            card->extra_54 = 1;
            card->extra_58 = 0;
            break;
        }
        constexpr int scripts[4] = {17, 19, 14, 14};
        auto* resource = reinterpret_cast<AnmResource*>(g_player->anm_player);
        AnmVm* visual = create_visual(card, resource, scripts[g_player_character], &g_player->x);
        const auto* player_vm = reinterpret_cast<const AnmVm*>(g_player->vm_player);
        anm_set_sprite(visual, resource, player_vm->sprite_index);
        visual->color = 0xffff0000u;
        g_player->speed_normal = 1024;
        g_player->speed_diag = 724;
        if (card->extra_58 != g_player->direction && card->extra_58 == 0 && g_player->focus_mode == 0) {
            // 40ca74: advance this timer by four scaled ticks, preserving its previous value.
            g_player->flags_47780 = 0;
            g_player->anim_id = g_player->invuln;
            const float speed = stage::g_speed_scale;
            g_player->invuln_value += (speed > 0.99f && speed < 1.01f) ? 4.0f : speed * 4.0f;
            g_player->invuln = static_cast<int>(g_player->invuln_value);
        }
        card->extra_58 = g_player->direction;
        break;
    }
    case OptionBR: {
        auto* raw = reinterpret_cast<uint8_t*>(card);
        for (int slot = 0; slot < 4; ++slot) {
            const auto* option = reinterpret_cast<const int32_t*>(g_player->option_a + slot * 0xf0);
            auto* timer = reinterpret_cast<int32_t*>(raw + 0x54 + slot * 0x14);
            auto* handle = reinterpret_cast<uint32_t*>(raw + 0xa4 + slot * 4);
            if (option[0] == 2) {
                const float x = option[0x17] / 128.0f, y = option[0x18] / 128.0f;
                if (timer[1] >= card->param_48) {
                    if (*handle == 0) *handle = create_option_shield(card, option)->handle;
                    if (stage::cancel_one_enemy_bullet(x, y, 8.0f, 0)) {
                        timer[0] = -1;
                        timer[1] = 0;
                        reinterpret_cast<float*>(timer)[2] = 0.0f;
                        timer[3] = 0;
                        timer[4] |= 1u;
                        anm_set_label(find_visual(*handle), 1);
                        *handle = 0;
                    }
                }
                if (*handle != 0) {
                    AnmVm* vm = find_visual(*handle);
                    vm->extra_x = x;
                    vm->extra_y = y;
                }
                advance_passive_timer(timer);
            } else if (*handle != 0) {
                anm_set_label(find_visual(*handle), 1);
                *handle = 0;
            }
        }
        advance_passive_timer(&card->unk_20);
        break;
    }
    case AutoBomb:
    case Rokumon:
        if (card->extra_54 != 0) {
            auto* bullet = *reinterpret_cast<PlayerBullet**>(reinterpret_cast<uint8_t*>(card) + 0x5c);
            // 0040a59d reads the zero-initialized global 004ccdbc, not this card's
            // timer. The original therefore keeps this cancel radius at 200.
            bullet->hit_radius = 200.0f;
            const auto* position = reinterpret_cast<const float*>(bullet->motion);
            // 40a560 / 40da80: ordinary bullets first, then lasers with immunity respected.
            stage::cancel_ordinary_bullets_circle(position[0], position[1], bullet->hit_radius, 0);
            laser::cancel_circle(position[0], position[1], bullet->hit_radius, 0, 1);
            if (card->unk_24 >= 60) {
                card->extra_54 = 0;
                bomb_set_card_active(false);
            }
        }
        advance_passive_timer(&card->unk_20);
        break;
    }
}

void card_passive_on_shot(AbilityCard* card, AttackObject* shot) {
    const ShtInstruction* instruction = instruction_for(shot);
    switch (card_id(card)) {
    case MainShotPU:
        if (instruction->kind == 7) {
            shot->anm_vm->scale_x = shot->anm_vm->scale_y = 1.5f;
            shot->damage = static_cast<int>(instruction->damage * 1.4f);
            shot->half_width = instruction->unk_00c * 1.5f;
            shot->half_height = instruction->unk_010 * 1.5f;
        }
        break;
    case MainShotSP:
        if (instruction->kind == 7 && (rng_next() & 7u) == 0) shot->handler_d = 0x40c4f0;
        break;
    case Mukade:
        card->extra_54 = g_damage_events;
        shot->damage = static_cast<int>(instruction->damage * std::min(1.8f, 1.0f + g_damage_events / 20000.0f));
        break;
    }
}

int card_passive_shot_hit(AttackObject* shot) {
    sound_play_se_at(0x41, shot->motion.x);
    PlayerBullet* splash = player_damage_circle(&shot->motion.x, 8.0f, 2.4f, 20, 6);
    splash->hit_interval = 3;
    splash->damage_max = 9999999;
    shot->active = 2;
    shot->motion.speed = 2.0f;
    std::memcpy(splash->motion, &shot->motion, sizeof(Motion));
    if (shot->bullet_index != 0) const_cast<PlayerBullet*>(player_bullet_at(shot->bullet_index - 1))->flags &= ~1u;
    shot->bullet_index = 0;
    anm_vm_free(shot->anm_vm);
    AnmVm* visual = anm_vm_alloc(card_manager_ability());
    // 40c4f0: template copy, initial tick, then ordinary A22 queue ownership.
    anm_vm_bind_script(visual, card_manager_ability(), 26);
    visual->flags3 |= 0x1000u;
    anm_vm_update(visual);
    visual->extra_x = shot->motion.x;
    visual->extra_y = shot->motion.y;
    visual->extra_z = shot->motion.z;
    shot->anm_handle = anm_queue_add(visual);
    anm_manage_vm(visual, &shot->anm_vm);
    return shot->damage;
}

void card_passive_update(AbilityCard* card) {
    // A22 advances and retires the VMs independently of 00408a90's shop gate.
    for (auto it = g_visuals.begin(); it != g_visuals.end();) {
        if (it->owner == card && it->vm == nullptr) it = g_visuals.erase(it);
        else ++it;
    }
}

void card_passive_on_drops(AbilityCard* card, const float* position, int* drop_counts) {
    if (card_id(card) != Money) return;
    int count = drop_counts[1] / 3;
    if (count == 0) count = 1;
    float angle = rng_float_pm1() * kPi;
    for (int i = 0; i < count; ++i) {
        const float factor = 1.0f + rng_float01() * 0.5f;
        const float x = position[0] + std::cos(angle) * 32.0f * factor;
        const float y = position[1] + std::sin(angle) * 32.0f * factor;
        const float drop_position[3]={x,y,position[2]};
        stage::spawn_item(2, drop_position, -kPi * 0.5f, 2.2f, 30);
        angle = angle + kPi * 0.5f + rng_float_pm1() * kPi * 0.25f;
        if (angle > kPi) angle -= kPi * 2.0f;
        if (angle < -kPi) angle += kPi * 2.0f;
    }
}

void card_passive_reset(AbilityCard* card) {
    switch (card_id(card)) {
    case Narumi:
        // 00409e90 reads app+804: a new run grants a life; a following
        // stage grants a fragment. 004424e0 invokes slot13 after STD creation.
        if (app_context()->game_flag != 0) status_add_life(&g_status);
        else add_life_fragment();
        break;
    case Pache: add_bomb(); break;
    case PowerMax:
        // 0040d380 invokes slot1 only for app+804's new-run branch.
        if (app_context()->game_flag != 0) equip(card);
        break;
    case AutoBomb:
    case Rokumon:
        remove_visuals(card);
        card->extra_54 = card->extra_58 = 0;
        *reinterpret_cast<PlayerBullet**>(reinterpret_cast<uint8_t*>(card) + 0x5c) = nullptr;
        card->flags_50 &= ~2u;
        break;
    case Mukade:
        card->extra_54 = 0;
        card->flags_50 &= ~2u;
        break;
    case ItemCatch: case ItemLine: case MainShotPU: case MagicScroll:
    case Koishi: case MainShotSP: case SpeedQueen: case OptionBR:
    case DeadSpell: case Yuyuko: case Money:
        // These original slot13 entries tail-call slot19's flag change.
        card->flags_50 &= ~2u;
        break;
    }
}

void card_passive_stage_start(AbilityCard* card) { immediate(card); }

void card_passive_unload(AbilityCard* card) {
    remove_visuals(card);
    if (card_id(card) == Extend || card_id(card) == Bomb || card_id(card) == Extend2 ||
        card_id(card) == Bomb2 || card_id(card) == Pendulum || card_id(card) == Dango ||
        card_id(card) == Mokou || card_id(card) == Narumi || card_id(card) == Pache || card_id(card) == PowerMax)
        card->flags_50 |= 2u;
    else if ((card_id(card) >= ItemCatch && card_id(card) <= Money) || card_id(card) == Rokumon || card_id(card) == Mukade)
        card->flags_50 &= ~2u;
    if (card_id(card) == AutoBomb || card_id(card) == Rokumon) {
        card->extra_54 = card->extra_58 = 0;
        *reinterpret_cast<PlayerBullet**>(reinterpret_cast<uint8_t*>(card) + 0x5c) = nullptr;
    }
    if (card_id(card) == Mukade) card->extra_54 = 0;
}

void card_passive_damage_event() { ++g_damage_events; }
void card_passive_clear_damage_events() { g_damage_events=0; }

void card_passive_detach_player(AbilityCard* card) {
    remove_visuals(card);
    if (card_id(card) == OptionBR) {
        auto* handles = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(card) + 0xa4);
        for (int i = 0; i < 4; ++i) handles[i] = 0;
    } else if (card_id(card) == AutoBomb || card_id(card) == Rokumon) {
        if (card->extra_54 != 0) bomb_set_card_active(false);
        card->extra_54 = card->extra_58 = 0;
        *reinterpret_cast<PlayerBullet**>(reinterpret_cast<uint8_t*>(card) + 0x5c) = nullptr;
    }
}

void card_passive_set_value(AbilityCard* card, int32_t value) {
    g_damage_events = value;
    card->extra_54 = value;
}
int32_t card_passive_value(AbilityCard*) { return g_damage_events; }
