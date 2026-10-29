#include "cards/card_active.h"

#include <algorithm>
#include <cstring>
#include <list>

#include "cards/card_manager.h"
#include "core/input.h"
#include "gameplay/spell.h"
#include "player/bomb.h"
#include "player/player.h"
#include "presentation/anm.h"
#include "presentation/sound.h"
#include "stage/enemy.h"
#include "stage/stage.h"

namespace {
struct ActiveVisual { AbilityCard* card; AnmVm* vm; uint32_t handle; };
std::list<ActiveVisual> g_visuals;

float* effect_position(AbilityCard* card) {
    return reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(card) + 0x58);
}

void move_visual(AnmVm* vm, const float* position) {
    vm->extra_x = position[0];
    vm->extra_y = position[1];
    vm->extra_z = position[2];
}

AnmVm* create_visual(AbilityCard* card, int script, const float* position) {
    AnmResource* ability = card_manager_ability();
    AnmVm* vm = anm_vm_alloc(ability);
    // 原416a10先给初始层13，再执行脚本；脚本的layer命令可覆盖它。
    vm->draw_layer = 13;
    vm->flags3 |= 0x1000;
    move_visual(vm, position);
    anm_vm_set_script(vm, ability, script);
    anm_queue_add(vm);
    card->unk_1c = static_cast<int32_t>(vm->handle);
    g_visuals.push_back({card, vm, vm->handle});
    // Original effects keep their ANM lifetime while the shop suspends card logic.
    anm_manage_vm(vm, &g_visuals.back().vm);
    return vm;
}

void free_visuals(AbilityCard* card) {
    for (auto it = g_visuals.begin(); it != g_visuals.end();) {
        if (it->card == card) {
            anm_vm_free(it->vm);
            it = g_visuals.erase(it);
        } else ++it;
    }
    card->unk_1c = 0;
}

AnmVm* visual_for(AbilityCard* card) {
    for (const ActiveVisual& visual : g_visuals)
        if (visual.card == card && visual.vm) return visual.vm;
    return nullptr;
}

void reset_state_time(AbilityCard* card) {
    card->unk_20 = -1;
    card->unk_24 = 0;
    card->unk_28 = 0;
    card->unk_2c = 0;
    card->flags_30 |= 1;
}

void advance_state_time(AbilityCard* card) {
    card->unk_20 = card->unk_24;
    if (stage::g_speed_scale > .99f && stage::g_speed_scale < 1.01f) {
        card->unk_28 += 1;
        ++card->unk_24;
    } else {
        card->unk_28 += stage::g_speed_scale;
        card->unk_24 = static_cast<int>(card->unk_28);
    }
    card->unk_2c = 0;
}

bool combat_clock() {
    return stage::g_stage &&
        *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(stage::g_stage) + 0x1b0) == 0 &&
        stage::g_enemy_manager && stage::g_enemy_manager->count != 0 && !stage::shop_active();
}

void cooldown(AbilityCard* card) {
    if (combat_clock() && card->count > 0) {
        card->unk_34 = card->count;
        card->count_f -= stage::g_speed_scale;
        card->count = static_cast<int>(card->count_f);
        card->flags_40 = 0;
    }
}

void start_cooldown(AbilityCard* card) {
    card->count_f = card->param_48 * card_manager()->lily_multiplier;
    card->count = static_cast<int>(card->count_f);
    card->unk_34 = card->count - 1;
    card->flags_40 = 0;
    card->flags_44 |= 1;
}

void stop_area(AbilityCard* card) {
    card->extra_54 = 0;
    reset_state_time(card);
    free_visuals(card);
}

void sun_invulnerability() {
    // 4099d0(2), used by both 40fb60 activation and 40f920 upkeep.
    if ((g_player->flags_47784 & 1) == 0) {
        g_player->flags_47780 = 0;
        g_player->flags_47784 |= 1;
    }
    g_player->anim_id = 1;
    g_player->invuln = 2;
    g_player->invuln_value = 2.0f;
}

void add_bomb_fragment() {
    if (g_status.unk_07c >= g_status.unk_088) { g_status.unk_080 = 0; return; }
    if (++g_status.unk_080 > 2) {
        g_status.unk_080 = 0;
        ++g_status.unk_07c;
        sound_play_se(46);
    }
}
} // namespace

void card_active_activate(AbilityCard* card) {
    switch (card_id(card)) {
    case Warp:
        if ((g_player->flags_4779c & 0x180) == 0) {
            if (g_player->x_sub <= -0x5c00 && (input_battle_state().held & ACTION_LEFT)) player_start_warp(1);
            else if (g_player->x_sub >= 0x5c00 && (input_battle_state().held & ACTION_RIGHT)) player_start_warp(2);
        }
        break;
    case Lily: {
        if (card->count > 0 || stage::g_enemy_manager == nullptr) break;
        const int n = card->extra_54;
        stage::SpawnArgs args{};
        args.y = -32;
        args.drop_kind = n % 3 == 2 ? 4 : 6;
        args.health = n < 10 ? n * 300 + 4000 : 7000;
        args.vars[0] = n < 8 ? n / 2 : 4;
        stage::create_enemy(n % 3 == 2 ? "ItemCarryLily2" : "ItemCarryLily", args);
        start_cooldown(card);
        ++card->extra_54;
        card->flags_50 |= 0x20;
        break;
    }
    case Kozuchi:
    case Kaname:
    case Moon:
    case MikoFlash:
    case Vampire:
    case Sun: {
        if (card->extra_54 != 0 || card->count > 0) break;
        uint8_t* raw = reinterpret_cast<uint8_t*>(card);
        float* pos = effect_position(card);
        std::memcpy(pos, &g_player->x, 12);
        if (card->id == Kaname) {
            pos[1] -= 100;
            *reinterpret_cast<int32_t*>(raw + 0x70) = 0;
        } else if (card->id == Moon) {
            uint8_t* block = raw + 0x64;
            std::memset(block, 0, 0x58);
            float* f = reinterpret_cast<float*>(block);
            int32_t* i = reinterpret_cast<int32_t*>(block);
            std::memcpy(f, pos, 12);
            std::memcpy(f + 3, pos, 12);
            f[4] -= 400;
            i[15] = -1;
            i[19] = 1;
            i[20] = 120;
            i[21] = 1;
            *reinterpret_cast<PlayerBullet**>(raw + 0xbc) = player_damage_circle(pos, 10, 0, 120, 45);
            *reinterpret_cast<float*>(raw + 0xc0) = 0;
        } else if (card->id == MikoFlash) {
            *reinterpret_cast<PlayerBullet**>(raw + 0x64) = player_damage_circle(pos, 600, 0, 120, 10);
        } else if (card->id == Sun) {
            spell_on_sun_card();
            sun_invulnerability();
            *reinterpret_cast<PlayerBullet**>(raw + 0x68) = player_damage_circle(pos, 10, 0, 600, 80);
        }
        const int script = card->id == Kozuchi ? 30 : card->id == Kaname ? 28 : card->id == Moon ? 45 :
                           card->id == MikoFlash ? 47 : card->id == Vampire ? 38 : 50;
        create_visual(card, script, pos);
        card->extra_54 = 1;
        reset_state_time(card);
        start_cooldown(card);
        if (card->id == Kaname) card->flags_50 |= 0x20;
        sound_play_se(card->id == Moon ? 53 : (card->id == Vampire || card->id == Kozuchi) ? 30 : card->id == Sun ? 49 : 77);
        break;
    }
    case Bassdrum:
    case Psyco:
        if (card->extra_54 != 0 || card->count > 0) break;
        std::memcpy(effect_position(card), &g_player->x, 12);
        create_visual(card, card->id == Bassdrum ? 58 : 60, effect_position(card));
        card->extra_54 = 1;
        *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(card) + 0x64) = 0;
        reset_state_time(card);
        start_cooldown(card);
        sound_play_se(51);
        break;
    case Cylinder:
        if (card->count > 0) break;
        if (g_status.power < g_status.power_step * 2) { sound_play_se(16); break; }
        if (bomb_activate_from_card()) {
            status_sub_power(&g_status, g_status.power_step);
            player_refresh_power_options();
            start_cooldown(card);
        }
        break;
    case Riceball:
        if (card->count > 0) break;
        if (g_status.power < g_status.power_max) {
            if (status_add_power(&g_status, 100)) player_refresh_power_options();
            sound_play_se(13);
        } else add_bomb_fragment();
        start_cooldown(card);
        create_visual(card, 66, &g_player->x);
        break;
    default: break;
    }
}

void card_active_update(AbilityCard* card) {
    switch (card_id(card)) {
    case Warp:
        card->flags_50 = (card->flags_50 & ~0x20u) | ((g_player->flags_4779c & 0x180) != 0 ? 0x20u : 0u);
        break;
    case Kozuchi: {
        AnmVm* vm = visual_for(card);
        switch (card->extra_54) {
        case 0:
            card->flags_50 &= ~0x20u;
            cooldown(card);
            break;
        case 1: {
            card->flags_50 |= 0x20;
            float* pos = effect_position(card);
            std::memcpy(pos, &g_player->x, 12);
            pos[1] -= 16;
            if (vm) move_visual(vm, pos);
            if (card->unk_24 > 20) { card->extra_54 = 2; reset_state_time(card); }
            break;
        }
        case 2:
            card->flags_50 |= 0x20;
            if (card->unk_24 > 10) {
                card->extra_54 = 3;
                sound_stop_se(30);
                sound_play_se(41);
                reset_state_time(card);
                effect_position(card)[1] -= 84;
            }
            break;
        case 3:
            card->flags_50 |= 0x20;
            if (card->unk_24 < 10) {
                const float* pos = effect_position(card);
                const float radius = 30 + 4 * card->unk_28;
                stage::cancel_enemy_bullets_circle(pos[0], pos[1], radius, 4);
            }
            if (card->unk_24 > 40) stop_area(card);
            break;
        }
        advance_state_time(card);
        break;
    }
    case Kaname: {
        AnmVm* vm = visual_for(card);
        if (card->extra_54 == 0) {
            card->flags_50 &= ~0x20u;
            cooldown(card);
        } else if (card->extra_54 == 1) {
            if (card->unk_24 > 29) card->flags_50 &= ~0x20u;
            float* pos = effect_position(card);
            float* target = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(card) + 0x64);
            std::memcpy(target, &g_player->x, 12);
            target[1] -= 80;
            for (int i = 0; i < 3; ++i) pos[i] += (target[i] - pos[i]) * .04f;
            if (vm) move_visual(vm, pos);
            if (card->unk_24 > 1800) {
                if (vm) anm_set_label(vm, 1);
                card->extra_54 = 2;
                reset_state_time(card);
                sound_play_se(41);
            }
        } else {
            card->flags_50 &= ~0x20u;
            if (card->unk_24 > 8) stop_area(card);
        }
        if (card->extra_54 == 1 && vm) {
            const float* pos = effect_position(card);
            const int cancelled = stage::kaname_cancel_bullets(pos[0], pos[1]);
            vm->color = cancelled ? 0xff0080ff : 0xffffffff;
            int32_t& total = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(card) + 0x70);
            total += cancelled;
            if (total >= 250) {
                anm_set_label(vm, 1);
                card->extra_54 = 2;
                reset_state_time(card);
                sound_play_se(41);
            }
        }
        if (combat_clock()) advance_state_time(card);
        break;
    }
    case Moon: {
        if (card->extra_54 == 0) {
            card->flags_50 &= ~0x20u;
            cooldown(card);
        } else {
            card->flags_50 |= 0x20;
            uint8_t* raw = reinterpret_cast<uint8_t*>(card);
            float* pos = effect_position(card);
            interp_eval3(raw + 0x64, pos);
            pos[1] += *reinterpret_cast<float*>(raw + 0xc0);
            if (AnmVm* vm = visual_for(card)) move_visual(vm, pos);
            if (card->extra_54 == 1 && card->unk_24 > 80) {
                card->extra_54 = 2;
                if (AnmVm* vm = visual_for(card)) anm_set_label(vm, 1);
                reset_state_time(card);
            } else if (card->extra_54 == 2 && card->unk_24 > 40) stop_area(card);
            if (card->extra_54 != 0) {
                if (AnmVm* vm = visual_for(card)) {
                    const float radius = vm->scale_x * 50;
                    stage::cancel_enemy_bullets_circle(pos[0], pos[1], radius, 0);
                    PlayerBullet* circle = *reinterpret_cast<PlayerBullet**>(raw + 0xbc);
                    std::memcpy(circle->motion, pos, 12);
                    circle->hit_radius = radius;
                    vm->color = 0xffffffff;
                    if (circle->damage_accum != 0) {
                        *reinterpret_cast<float*>(raw + 0xc0) += 3.5f;
                        if ((card->unk_24 & 1) == 0) vm->color = 0xff8080ff;
                    }
                    circle->damage_accum = 0;
                }
            }
        }
        advance_state_time(card);
        break;
    }
    case MikoFlash:
        if (card->extra_54 == 0) {
            card->flags_50 &= ~0x20u;
            cooldown(card);
        } else {
            card->flags_50 |= 0x20;
            std::memcpy(effect_position(card), &g_player->x, 12);
            if (AnmVm* vm = visual_for(card)) move_visual(vm, &g_player->x);
            if (card->unk_24 > 130) stop_area(card);
            if (card->unk_24 >= 40 && card->unk_24 < 120 && card->unk_24 % 8 == 0)
                stage::miko_flash_cancel();
        }
        advance_state_time(card);
        break;
    case Vampire:
        if (card->extra_54 == 0) {
            card->flags_50 &= ~0x20u;
            cooldown(card);
        } else {
            card->flags_50 |= 0x20;
            std::memcpy(effect_position(card), &g_player->x, 12);
            if (AnmVm* vm = visual_for(card)) move_visual(vm, &g_player->x);
            if (card->extra_54 == 1) {
                if (card->unk_24 > 30) {
                    card->extra_54 = 2;
                    reset_state_time(card);
                    sound_play_se(27);
                }
            } else {
                if (card->unk_24 < 50) {
                    const float height = card->unk_24 < 10 ? card->unk_28 * 9 : 90;
                    float pos[3] = {g_player->x, g_player->y - height * .5f, g_player->z};
                    *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(g_player) + 0x47984) = 300;
                    player_damage_rectangle(pos, 0, 32, height, 2, 200);
                    stage::cancel_enemy_bullets_rectangle(pos[0], pos[1], 32, height, 0, 0);
                }
                if (card->unk_24 > 60) stop_area(card);
            }
        }
        advance_state_time(card);
        break;
    case Sun:
        if (card->extra_54 == 0) {
            card->flags_50 &= ~0x20u;
            cooldown(card);
        } else if (card->extra_54 == 1) {
            card->flags_50 |= 0x20;
            AnmVm* vm = visual_for(card);
            spell_on_sun_card();
            if (*reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(stage::g_stage) + 0x1b0) != 0) {
                card->extra_54 = 2;
                reset_state_time(card);
                anm_set_label(vm, 1);
            }
            sun_invulnerability();
            const float* pos = effect_position(card);
            const float radius = vm->scale_x * 96;
            stage::cancel_enemy_bullets_circle(pos[0], pos[1], radius, 0);
            (*reinterpret_cast<PlayerBullet**>(reinterpret_cast<uint8_t*>(card) + 0x68))->hit_radius = radius;
            if (card->unk_24 > 600) {
                card->extra_54 = 2;
                reset_state_time(card);
                sound_play_se(27);
            }
            if (card->unk_24 % 6 == 0) {
                if (status_sub_power(&g_status, 1)) player_refresh_power_options();
            }
        } else {
            card->flags_50 |= 0x20;
            if (card->unk_24 > 60) stop_area(card);
        }
        advance_state_time(card);
        break;
    case Lily:
    case Cylinder:
    case Riceball:
        cooldown(card);
        if (card->count < card->param_48 - 8) card->flags_50 &= ~0x20u;
        break;
    case Bassdrum:
    case Psyco:
        if (card->extra_54 == 0) {
            card->flags_50 &= ~0x20u;
            cooldown(card);
        } else {
            card->flags_50 |= 0x20;
            const float* pos = effect_position(card);
            if (card->id == Bassdrum) {
                stage::cancel_enemy_bullets_circle(pos[0], pos[1], 34 + card->unk_28, 0);
            } else stage::psyco_reflect_bullets(pos[0], pos[1], card->unk_24);
            if (card->unk_24 >= (card->id == Bassdrum ? 30 : 120)) stop_area(card);
        }
        advance_state_time(card);
        break;
    default: break;
    }
    for (auto it = g_visuals.begin(); it != g_visuals.end();) {
        ActiveVisual& visual = *it;
        if (visual.card != card) { ++it; continue; }
        if (visual.vm == nullptr) {
            if (static_cast<uint32_t>(card->unk_1c) == visual.handle) card->unk_1c = 0;
            it = g_visuals.erase(it);
            continue;
        }
        if (card->id == Riceball) {
            move_visual(visual.vm, &g_player->x);
            stage::cancel_enemy_bullets_circle(g_player->x, g_player->y, 48, 0);
        }
        ++it;
    }
}

void card_active_detach_player(AbilityCard* card) {
    free_visuals(card);
    if ((card->id >= Kozuchi && card->id <= Sun) || card->id == Bassdrum || card->id == Psyco) stop_area(card);
    card->flags_50 &= ~0x20u;
}

void card_active_unload(AbilityCard* card) {
    card_active_detach_player(card);
}

void card_active_on_death(AbilityCard* card) {
    // 原slot3只有Kozuchi/Vampire/Sun停止实体；Moon/Miko/Kaname仍按原寿命运行。
    if (card->id == Kozuchi || card->id == Vampire || card->id == Sun) {
        card->extra_54 = 0;
        free_visuals(card);
    }
}

void card_active_stage_start(AbilityCard* card) {
    switch (card_id(card)) {
    case Kozuchi: case Kaname: case Moon: case MikoFlash: case Vampire: case Sun:
    case Bassdrum: case Psyco: stop_area(card); break;
    default: break;
    }
    if (card->flags_50 & 8) card->flags_50 &= ~2u;
}
