#include "cards/card_options.h"

#include <cmath>
#include <cstdint>

#include "cards/ability_card.h"
#include "cards/card_manager.h"
#include "player/player.h"
#include "presentation/anm.h"
#include "stage/enemy.h"
#include "stage/stage.h"

namespace {
constexpr float kPi = 3.1415927410125732f;
constexpr float kTwoPi = 6.2831854820251465f;
constexpr float kOrbitStep = 0.03490658476948738f;

// Player+0xa30, twelve 0xf0-byte slots. Card+0x54/+0x58 own their slots.
struct CardOption {
    int active;                       // +0x00
    uint8_t unknown_004[0x50];
    int target_x, target_y;            // +0x54
    int x, y;                         // +0x5c, 1/128 pixel
    int normal_x, normal_y;            // +0x64
    int focus_x, focus_y;              // +0x6c
    uint8_t unknown_074[0x34];
    float angle, radius;              // +0xa8
    AnmVm* vm;                        // +0xb0, port holds the VM directly
    AnmVm* full_vm;                   // +0xb4
    uint8_t unknown_0b8[0x14];
    int previous_focus;               // +0xcc
    int owner_number;                 // +0xd0
    int snap;                         // +0xd4
    uint32_t flags;                   // +0xd8, bit 1: own target position
    int death_notified;                // +0xdc
    float side;                       // +0xe0
    int target_enemy;                 // +0xe4
    uint32_t motion_callback;          // +0xe8
    uint32_t unknown_0ec;
};

bool has_options(const AbilityCard* card) {
    return (card->id >= ReimuOp && card->id <= NueOp) || card->id == Magatama;
}

CardOption* first_option(const AbilityCard* card) {
    return reinterpret_cast<CardOption*>(static_cast<uintptr_t>(static_cast<uint32_t>(card->extra_54)));
}

CardOption* second_option(const AbilityCard* card) {
    return reinterpret_cast<CardOption*>(static_cast<uintptr_t>(static_cast<uint32_t>(card->extra_58)));
}

float wrap_angle(float angle) {
    while (angle > kPi) angle -= kTwoPi;
    while (angle < -kPi) angle += kTwoPi;
    return angle;
}

// 0040a790: first free slot, fixed normal/focused offsets, ability.anm layer 14.
CardOption* create_option(AbilityCard* card, int y, int script) {
    auto* slots = reinterpret_cast<CardOption*>(g_player->option_b);
    CardOption* option = nullptr;
    for (int i = 0; i < 12; ++i) {
        if (slots[i].active == 0) {
            option = &slots[i];
            break;
        }
    }
    if (option == nullptr) return nullptr;

    option->active = 2;
    if (option->vm != nullptr) anm_vm_free(option->vm);
    option->owner_number = card->slot_index;
    option->normal_x = option->focus_x = 0;
    option->normal_y = option->focus_y = y * 128;
    option->target_x = option->x = g_player->x_sub;
    option->target_y = option->y = g_player->y_sub + y * 128;
    option->target_enemy = 0;
    option->death_notified = 0;
    option->flags &= ~2u;
    option->snap = 1;

    AnmResource* ability = card_manager_ability();
    option->vm = anm_vm_alloc(ability);
    anm_vm_bind_script(option->vm, ability, script);
    option->vm->draw_layer = 14;
    option->vm->flags3 = (option->vm->flags3 & ~0x200000u) | 0x101000u;
    anm_vm_update(option->vm);
    option->vm->extra_x = 0.0f;
    option->vm->extra_y = -1000.0f;
    option->vm->extra_z = 0.0f;
    card->unk_1c = static_cast<int32_t>(anm_queue_add(option->vm));
    anm_manage_vm(option->vm, &option->vm);
    return option;
}

void release_option(CardOption* option) {
    if (option == nullptr) return;
    if (option->vm != nullptr) anm_vm_free(option->vm);
    option->vm = nullptr;
    option->active = 0;
    option->flags &= ~2u;
    option->death_notified = 0;
    option->snap = 1;
}

// 0040b6e0: retain the selected enemy until it disappears or becomes untargetable.
void alice_follow(CardOption* option) {
    if (stage::g_enemy_manager == nullptr) {
        option->target_enemy = 0;
        return;
    }
    stage::Enemy* target = nullptr;
    const float x = option->x / 128.0f;
    const float y = option->y / 128.0f;
    if (option->target_enemy == 0) {
        float nearest = 512.0f * 512.0f;
        for (auto* node = stage::g_enemy_manager->enemy_head; node; node = node->next) {
            stage::Enemy* enemy = node->self;
            if ((enemy->flag & 0xc000021u) != 0) continue;
            const float dx = enemy->x - x;
            const float dy = enemy->y - y;
            const float distance = dx * dx + dy * dy;
            if (distance < nearest) {
                nearest = distance;
                target = enemy;
            }
        }
        if (target == nullptr) return;
        option->target_enemy = target->id;
    } else {
        target = stage::find_enemy(option->target_enemy);
        if (target == nullptr || (target->flag & 0xc000021u) != 0) {
            option->target_enemy = 0;
            return;
        }
    }

    const float dx = target->x - x;
    const float dy = target->y - y;
    const float distance = std::sqrt(dx * dx + dy * dy);
    float speed;
    if (distance >= 64.0f) speed = distance < 128.0f ? 2.0f : 5.0f;
    else if (distance < 48.0f) return;
    else speed = -2.0f;
    const float angle = std::atan2(dy, dx);
    option->target_x = static_cast<int>((x + std::cos(angle) * speed) * 128.0f);
    option->target_y = static_cast<int>((y + std::sin(angle) * speed) * 128.0f);
    option->normal_x = option->focus_x = option->target_x;
    option->normal_y = option->focus_y = option->target_y;
}

void death_animation(CardOption* option) {
    if ((g_player->flags_4779c & 2u) != 0) {
        if (option->death_notified == 0) {
            option->death_notified = 1;
            if (option->vm != nullptr) anm_set_label(option->vm, 3);
        }
    } else {
        option->death_notified = 0;
    }
}

// 0045bc90, with the callbacks installed by each card's slot 6.
void move_option(CardOption* option, CardId id) {
    if (option == nullptr || option->active == 0) return;
    death_animation(option);
    if ((g_player->flags_4779c & 2u) != 0) {
        option->target_x = g_player->x_sub;
        option->target_y = g_player->y_sub;
        if (g_player->follow_stun > 29) {
            option->active = 0;
            if (option->vm != nullptr) anm_set_label(option->vm, 1);
            return;
        }
    } else {
        if ((option->flags & 2u) == 0) {
            option->target_x = g_player->x_sub + (g_player->focus_mode ? option->focus_x : option->normal_x);
            option->target_y = g_player->y_sub + (g_player->focus_mode ? option->focus_y : option->normal_y);
        }
        switch (id) {
        case SakuyaOp: {
            const float angle = wrap_angle(g_player->shot_angle + option->angle);
            option->target_x = g_player->x_sub + static_cast<int>(std::cos(angle) * option->radius * 128.0f);
            option->target_y = g_player->y_sub + static_cast<int>(std::sin(angle) * option->radius * 128.0f);
            break;
        }
        case YoumuOp:
            if (g_player->focus_mode == 0) {
                option->normal_x = g_player->trail[16][0] - g_player->x_sub;
                option->normal_y = g_player->trail[16][1] - g_player->y_sub;
            }
            option->target_x = g_player->x_sub + option->normal_x;
            option->target_y = g_player->y_sub + option->normal_y;
            option->previous_focus = g_player->focus_mode;
            break;
        case AliceOp: alice_follow(option); break;
        case NueOp:
            option->target_x = g_player->x_sub + static_cast<int>(std::cos(option->angle) * 96.0f * 128.0f);
            option->target_y = g_player->y_sub + static_cast<int>(std::sin(option->angle) * 96.0f * 128.0f);
            option->angle = wrap_angle(option->angle + kOrbitStep);
            break;
        case Magatama:
            option->target_x = g_player->x_sub + static_cast<int>(option->side * 128.0f);
            option->target_y = g_player->y_sub + static_cast<int>(std::sin(option->angle) * 8.0f * 128.0f);
            option->angle = wrap_angle(option->angle + kOrbitStep);
            break;
        }
    }

    if (option->snap != 0) {
        option->snap = 0;
        option->x = option->target_x;
        option->y = option->target_y;
    } else if (g_player->follow_factor > 29) {
        const int dx = (option->target_x - option->x) * g_player->follow_factor / 100;
        const int dy = (option->target_y - option->y) * g_player->follow_factor / 100;
        if (dx == 0 && dy == 0) {
            option->x = option->target_x;
            option->y = option->target_y;
        } else {
            option->x += dx;
            option->y += dy;
        }
    }
    if (option->vm != nullptr) {
        option->vm->extra_x = option->x / 128.0f;
        option->vm->extra_y = option->y / 128.0f;
        option->vm->extra_z = 0.0f;
    }
}

void advance_visual(CardOption* option) {
    if (option == nullptr || option->vm == nullptr) return;
    death_animation(option);
}

// 0040a9c0: card shots use the current character's SHT and ability.anm.
void fire_option(CardOption* option, int script, int timer_a, int timer_b) {
    if (option == nullptr) return;
    const auto* sht = static_cast<const PlayerSht*>(g_player->sht);
    const auto* instruction = reinterpret_cast<const ShtInstruction*>(sht->sub_offset[script]);
    const float position[3] = {option->x / 128.0f, option->y / 128.0f, 0.0f};
    for (int index = 0; static_cast<int8_t>(instruction->timer_count) >= 0; ++index, ++instruction) {
        const bool fire = instruction->timer_b_count == 0
            ? timer_a % instruction->timer_count == instruction->timer_slot
            : timer_b % instruction->timer_b_count == instruction->timer_b_slot;
        if (fire)
            player_attack_spawn(g_player, instruction, (script << 8) | index,
                                static_cast<float>(timer_a), position, option, card_manager_ability());
    }
}
} // namespace

void card_options_create(AbilityCard* card) {
    CardOption* option;
    switch (card_id(card)) {
    case ReimuOp: option = create_option(card, 48, 2); break;
    case ReimuOp2: option = create_option(card, -24, 2); break;
    case MarisaOp: option = create_option(card, 16, 6); break;
    case MarisaOp2: option = create_option(card, 32, 6); break;
    case SakuyaOp:
        option = create_option(card, 16, 7);
        option->angle = 0.0f;
        option->radius = 20.0f;
        break;
    case SakuyaOp2: option = create_option(card, 32, 7); break;
    case SanaeOp: option = create_option(card, 28, 14); break;
    case SanaeOp2: option = create_option(card, -20, 14); break;
    case YoumuOp:
        option = create_option(card, 28, 23);
        option->owner_number = 2;
        break;
    case AliceOp:
        option = create_option(card, 28, 16);
        option->flags |= 2u;
        break;
    case CirnoOp: option = create_option(card, -60, 20); break;
    case OkinaOp: option = create_option(card, 36, 18); break;
    case NueOp:
        option = create_option(card, -60, 22);
        option->flags |= 2u;
        break;
    case Magatama:
        option = create_option(card, -60, 64);
        option->flags |= 2u;
        option->side = -60.0f;
        card->extra_54 = static_cast<int32_t>(reinterpret_cast<uintptr_t>(option));
        option = create_option(card, 60, 65);
        option->flags |= 2u;
        option->side = 60.0f;
        card->extra_58 = static_cast<int32_t>(reinterpret_cast<uintptr_t>(option));
        return;
    // MAGATAMA2 has the base vtable and no option; its EX acquisition is external.
    default: return;
    }
    card->extra_54 = static_cast<int32_t>(reinterpret_cast<uintptr_t>(option));
}

void card_options_reset(AbilityCard* card) {
    if (!has_options(card)) return;
    release_option(first_option(card));
    card->extra_54 = 0;
    if (card_id(card) == Magatama) {
        release_option(second_option(card));
        card->extra_58 = 0;
    }
    card->unk_1c = 0;
    card->flags_50 &= ~2u;
}

void card_options_player_move(AbilityCard* card) {
    if (!has_options(card)) return;
    const CardId id = card_id(card);
    CardOption* option = first_option(card);
    move_option(option, id);
    if (id == Magatama) move_option(second_option(card), id);
    if (option == nullptr) return;
    const float position[3] = {option->x / 128.0f, option->y / 128.0f, 0.0f};
    switch (id) {
    case OkinaOp:
        stage::cancel_enemy_bullets_rectangle(position[0], position[1], 20.0f, 6.0f, 0.0f, 0);
        player_damage_rectangle(position, 0.0f, 20.0f, 6.0f, 2, 4);
        break;
    case NueOp:
        stage::cancel_enemy_bullets_circle(position[0], position[1], 6.0f, 0);
        break;
    case Magatama:
        stage::cancel_one_enemy_bullet(position[0], position[1], 3.0f, 0);
        option = second_option(card);
        if (option != nullptr)
            stage::cancel_one_enemy_bullet(option->x / 128.0f, option->y / 128.0f, 3.0f, 0);
        break;
    }
}

void card_options_shoot(AbilityCard* card, int timer_a, int timer_b) {
    if (!has_options(card)) return;
    CardOption* option = first_option(card);
    int script;
    switch (card_id(card)) {
    case ReimuOp: script = 10; break;
    case ReimuOp2: script = 18; break;
    case MarisaOp: script = 11; break;
    case MarisaOp2: script = 19; break;
    case SakuyaOp: script = 12 + (g_player->focus_mode != 0); break;
    case SakuyaOp2: script = 20; break;
    case SanaeOp: script = 14; break;
    case SanaeOp2: script = 21; break;
    case YoumuOp: script = 17; break;
    case AliceOp: {
        if (option->target_enemy == 0) return;
        stage::Enemy* target = stage::find_enemy(option->target_enemy);
        if (target == nullptr || (target->flag & 0xc000021u) != 0) {
            option->target_enemy = 0;
            return;
        }
        const float dx = target->x - option->x / 128.0f;
        const float dy = target->y - option->y / 128.0f;
        if (std::sqrt(dx * dx + dy * dy) > 128.0f) return;
        *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(g_player) + 0x479cc) = std::atan2(dy, dx);
        script = 15;
        break;
    }
    case CirnoOp: script = 16; break;
    default: return;
    }
    fire_option(option, script, timer_a, timer_b);
}

void card_options_update(AbilityCard* card) {
    if (!has_options(card)) return;
    advance_visual(first_option(card));
    if (card_id(card) == Magatama) advance_visual(second_option(card));
}

void card_options_unload(AbilityCard* card) {
    card_options_reset(card);
}
