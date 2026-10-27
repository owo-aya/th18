#include "cards/card_manager.h"
#include "cards/card_options.h"
#include "cards/card_passive.h"
#include "cards/card_profile.h"
#include "cards/card_active.h"

#include <cstdlib>
#include <cstring>

#include "core/input.h"
#include "core/thread.h"
#include "player/player.h"
#include "player/player_sht.h"
#include "presentation/anm.h"
#include "presentation/trophy.h"
#include "presentation/sound.h"
#include "stage/stage.h"
#include "stage/hud.h"
#include "stage/enemy.h"
#include "stage/session.h"

namespace {
AnmResource* g_ability;
Thread* g_task;
CardManager g_cards;
AbilityCard* g_instances[256];
int g_instance_count;
CardId g_equipped[256];
int g_count;
bool g_player_attached;

// 0x411460各构造分支的实际分配量；0..55是能力卡，56是装备空槽。
constexpr uint8_t kCardBytes[57] = {
    0x54,0x54,0x54,0x54,0x54,0x54,0x54,0x54,
    0x58,0x58,0x58,0x58,0x58,0x58,0x58,0x58,0x58,0x58,0x58,0x58,0x58,
    0x54,0x54,0x60,0x58,0x54,0x54,0x58,0x54,0x5c,0xb4,0x54,0x58,0x54,0x54,
    0x64,0x54,0x54,0x54,0x54,0x54,0x54,0x68,0x74,0xc4,0x68,0x68,0x6c,
    0x58,0x68,0x68,0x5c,0x54,0x54,0x5c,0x54,0x54
};
constexpr int kActiveLimit[13] = {999,2400,3600,2700,1800,1200,18000,7200,600,1500,0,1200,5400};

AbilityCard* install_instance(CardId id, uint32_t flags) {
    AbilityCard* card = static_cast<AbilityCard*>(std::calloc(1, kCardBytes[id]));
    card->id = id;
    card->slot_index = g_instance_count + 1;
    card->node_self = card;
    card->node_prev = g_cards.list.last;
    card->info = card_info_by_id(id);
    card->unk_20 = card->unk_34 = -1;
    card->flags_30 = card->flags_44 = 1;
    uint32_t kind = 4;
    if (card->info->category == 0) {
        kind = 8;
        card->param_48 = kActiveLimit[id - Warp];
    } else if (card->info->category == 1) {
        kind = 0x44;
    } else {
        if ((id >= Extend && id <= Mokou) || id == Narumi || id == Pache)
            kind = 2;
        if (id == PowerMax)
            kind = 6;
    }
    card->flags_50 = kind | (flags & 1u);
    if (card_passive_acquire(card, flags)) {
        std::free(card);
        return nullptr;
    }
    if (card->flags_50 & 8) { ++g_cards.count_kind8; g_cards.special = card; }
    else if (card->flags_50 & 0x40) ++g_cards.count_kind40;
    else ++g_cards.count_other;
    if (g_cards.list.last)
        g_cards.list.last->node_next = card;
    else
        g_cards.list.first = card;
    g_cards.list.last = card;
    g_instances[g_instance_count++] = card;
    g_cards.count = g_instance_count;
    g_cards.owned[id] = 1;
    if (id != Null) g_equipped[g_count++] = id;
    return card;
}

ThreadStatus update(void*) {
    if (!stage_session_started()) return THREAD_CONTINUE;
    if (!g_player_attached) return THREAD_CONTINUE;
    for (int i = 0; i < g_instance_count; ++i) {
        card_options_update(g_instances[i]);
        card_passive_update(g_instances[i]);
        card_active_update(g_instances[i]);
    }
    stage::hud_update_cards();
    // 408640 -> 405990: the flash clock belongs to the card manager and
    // advances after visual updates, including fractional game speed.
    g_cards.visual_previous=g_cards.visual_frame;
    g_cards.visual_latch=0;g_cards.visual_flags|=1;
    const float speed=stage::g_speed_scale;
    const bool unit=speed>0.99f && speed<1.01f;
    g_cards.visual_time+=unit?1.0f:speed;
    g_cards.visual_frame=unit?g_cards.visual_frame+1:int(g_cards.visual_time);
    return THREAD_CONTINUE;
}
} // namespace

void card_manager_begin_run(const int32_t* ids, int count, uint32_t flags) {
    std::memset(&g_cards, 0, sizeof(g_cards));
    g_cards.lily_multiplier = 1.0f;
    g_instance_count = 0;
    g_count = 0;
    g_ability = anm_get(0x1e, "ability.anm");
    g_cards.anm_ability = static_cast<int32_t>(reinterpret_cast<uintptr_t>(g_ability));
    for (int i = 0; i < count; ++i) {
        const CardId id = static_cast<CardId>(ids[i]);
        // 0x407f10从人物选择进入游戏时先移除NULL空槽。
        if (id == Null)
            continue;
        card_manager_acquire(id, flags);
    }
    card_manager_player_reset();
    card_manager_options_reset();
    g_task = thread_create(update);
    g_task->flags |= 2; // 原407f10：局内Card任务受ThreadManager特殊/暂停态控制。
    thread_set_priority(g_task, 0x16);
    thread_enqueue_logic(g_task);
}

void card_manager_end_run() {
    thread_remove(g_task);
    g_task = nullptr;
    card_manager_clear_cards();
    anm_delete_independent(0x1e);
    std::memset(&g_cards, 0, sizeof(g_cards));
    g_ability = nullptr;
    g_player_attached = false;
    g_count = 0;
}

void card_manager_on_player_move() {
    for (int i = 0; i < g_instance_count; ++i)
        card_options_player_move(g_instances[i]);
}

void card_manager_handle_input() {
    const InputState& input = input_battle_state();
    const bool combat = stage::g_stage && stage::g_enemy_manager && stage::g_enemy_manager->count != 0 &&
        *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(stage::g_stage) + 0x1b0) == 0;
    if (g_cards.special && combat && (input.pressed & ACTION_CARD)) {
        card_active_activate(g_cards.special);
    }
    if (input.pressed & ACTION_CARD_CYCLE) {
        int selected = g_instance_count;
        for (int i = 0; i < g_instance_count; ++i)
            if (g_instances[i] == g_cards.special) selected = i;
        for (int n = 0; n < g_instance_count; ++n) {
            selected = selected == 0 ? g_instance_count - 1 : selected - 1;
            AbilityCard* card = g_instances[selected];
            if ((card->flags_50 & 8) == 0) continue;
            if (card == g_cards.special) break;
            g_cards.special = card;
            stage::hud_select_card();
            sound_play_se(78);
            break;
        }
    }
}

void card_manager_on_shoot(int timer_a, int timer_b) {
    for (int i = 0; i < g_instance_count; ++i)
        card_options_shoot(g_instances[i], timer_a, timer_b);
}

void card_manager_on_hit() {
    for (int i = 0; i < g_instance_count; ++i)
        card_passive_on_hit(g_instances[i]);
}

uint32_t card_manager_on_death() {
    uint32_t handled = 0;
    for (int i = 0; i < g_instance_count; ++i) {
        handled |= card_passive_on_death(g_instances[i], handled);
        card_active_on_death(g_instances[i]);
    }
    return handled;
}

void card_manager_after_death_penalty() {
    for (int i = 0; i < g_instance_count; ++i)
        card_passive_after_death_penalty(g_instances[i]);
}

bool card_manager_owned(CardId id) { return card_manager_find(id) != nullptr; }
bool card_manager_acquired(CardId id) { return g_cards.owned[id] != 0; }
int card_manager_equipped_count() { return g_count; }
CardId card_manager_equipped_id(int index) { return g_equipped[index]; }
int card_manager_cooldown(CardId id) {
    AbilityCard* card = card_manager_find(id);
    return card ? card->count : 0;
}
bool card_manager_active(CardId id) {
    AbilityCard* card = card_manager_find(id);
    return card && (card->flags_50 & 0x20u) != 0;
}
AnmResource* card_manager_ability() { return g_ability; }
int card_manager_card_count() { return g_instance_count; }
AbilityCard* card_manager_card_at(int index) { return g_instances[index]; }
AbilityCard* card_manager_find(CardId id) {
    for (int i = 0; i < g_instance_count; ++i)
        if (g_instances[i]->id == id)
            return g_instances[i];
    return nullptr;
}

CardManager* card_manager() { return &g_cards; }

void card_manager_clear_cards() {
    for (int i = 0; i < g_instance_count; ++i) {
        card_options_unload(g_instances[i]);
        card_passive_unload(g_instances[i]);
        card_active_unload(g_instances[i]);
        std::free(g_instances[i]);
    }
    g_instance_count = g_count = 0;
    g_cards.list = {};
    g_cards.count = g_cards.count_kind8 = g_cards.count_kind40 = g_cards.count_other = 0;
    g_cards.special = nullptr;
    g_cards.lily_multiplier = 1.0f;
    // 原407da0(0)不清+c84的本局取得记录；只有新一局创建才清。
}

int32_t card_manager_acquire(CardId id, uint32_t flags) {
    if (g_instance_count > 255) return -1; // 原411460的实际持卡上限。
    if (id == Blank) card_manager_clear_cards();
    install_instance(id, flags);
    if ((flags & CardAdd_NoImmediate) == 0 && card_profile_mark_acquired(id)) trophy_notify(id);
    if (g_task) stage::hud_refresh_cards();
    return g_instance_count;
}

void card_manager_on_shot(AttackObject* shot) {
    for (int i = 0; i < g_instance_count; ++i)
        card_passive_on_shot(g_instances[i], shot);
}

void card_manager_player_reset() {
    g_player_attached = true;
    for (int i = 0; i < g_instance_count; ++i) {
        card_options_reset(g_instances[i]);
        card_passive_player_reset(g_instances[i]);
    }
}

void card_manager_detach_player() {
    g_player_attached = false;
    for (int i = 0; i < g_instance_count; ++i) {
        card_options_unload(g_instances[i]);
        card_passive_detach_player(g_instances[i]);
        card_active_detach_player(g_instances[i]);
    }
    anm_delete_independent(0x1e);
}

void card_manager_stage_start() {
    g_cards.lily_multiplier = 1.0f;
    for (int i = 0; i < g_instance_count; ++i) {
        AbilityCard* card = g_instances[i];
        card_active_stage_start(card);
        card_passive_reset(card);
        if (card->flags_50 & 0x40) card->flags_50 &= ~2u;
        if (card->id == MagicScroll) g_cards.lily_multiplier = 0.8f;
    }
    for (int i = 0; i < g_instance_count; ++i)
        card_passive_stage_start(g_instances[i]);
}

void card_manager_shop_exit() {
    // 原408ad0只调slot8；装备卡该槽是基类空操作，不能销毁仍需label2恢复的option_b。
    for (int i = 0; i < g_instance_count; ++i)
        card_passive_player_reset(g_instances[i]);
}

void card_manager_options_reset() {
    for (int i = 0; i < g_instance_count; ++i) card_options_reset(g_instances[i]);
    for (int i = 0; i < g_instance_count; ++i) {
        card_options_create(g_instances[i]);
        card_passive_options_reset(g_instances[i]);
    }
}

void card_manager_player_update() {
    for (int i = 0; i < g_instance_count; ++i)
        card_passive_player_update(g_instances[i]);
}

void card_manager_on_drops(const float* position, int* drop_counts) {
    for (int i = 0; i < g_instance_count; ++i)
        card_passive_on_drops(g_instances[i], position, drop_counts);
}

int32_t card_manager_selected_id() { return g_cards.special ? g_cards.special->id : -1; }

void card_manager_select_id(int32_t id) {
    for (int i = 0; i < g_instance_count; ++i) {
        if (g_instances[i]->id == id && (g_instances[i]->flags_50 & 8)) {
            g_cards.special = g_instances[i];
            if (g_task) stage::hud_select_card();
            return;
        }
    }
}

int32_t card_manager_value(int32_t index) {
    AbilityCard* card = g_instances[index];
    if (card->id == Mukade) return card_passive_value(card);
    if (card->id == Lily) return card->extra_54 << 16 | (card->count & 0xffff);
    return card->count;
}

void card_manager_set_value(int32_t index, int32_t value) {
    AbilityCard* card = g_instances[index];
    if (card->id == Mukade) { card_passive_set_value(card, value); return; }
    if (card->id == Lily) { card->extra_54 = value >> 16; value &= 0xffff; }
    card->count = value;
    card->count_f = static_cast<float>(value);
    card->unk_34 = -1;
    card->flags_40 &= ~1u;
    card->flags_44 &= ~1u;
}
