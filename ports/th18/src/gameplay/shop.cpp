#include "gameplay/shop.h"

#include <d3d9.h>
#include <algorithm>

#include "cards/card_info.h"
#include "cards/card_manager.h"
#include "cards/card_profile.h"
#include "core/game_session.h"
#include "core/input.h"
#include "core/rng.h"
#include "platform/gfx.h"
#include "player/player.h"
#include "presentation/anm.h"
#include "presentation/sound.h"
#include "presentation/text.h"

const int32_t g_card_price[14] = {
    0, 50, 80, 100, 100, 140, 180, 200, 240, 280, 300, 350, 400, 450
};

struct Shop {
    int stage;
    int state;
    int timer;
    int count;
    int selected;
    int answer;
    bool blank;
    const CardInfo* goods[56];
    AnmResource* cards;
    AnmResource* menu;
    AnmResource* text;
    AnmVm* heading;
    AnmVm* preview[56];
    AnmVm* new_mark[56];
    AnmVm* words[7];
    IDirect3DTexture9* word_texture[7];
    AnmVm* category;
};

namespace {

uint32_t shop_random()
{
    Rng& random = card_rng();
    const uint32_t a = ((random.state ^ 0x9630u) + 0x9aadu) & 0xffffu;
    const uint32_t b = ((((a >> 14) + a * 4) ^ 0x9630u) + 0x9aadu) & 0xffffu;
    random.state = static_cast<uint16_t>(((b >> 14) + b * 4) & 0xffffu);
    random.counter += 2;
    return (a << 16) | b;
}

// 00416e10 returns 0 (absent), 1 (random/common), or 2 (stage's fixed card).
int availability(const CardInfo& info, int stage)
{
    switch (info.stage_rule) {
    case 0: return 1;
    case 1: case 2: case 3: case 4: case 5:
        return stage == info.stage_rule ? 2 : card_profile_unlocked(static_cast<CardId>(info.id));
    case 6:
        return stage == 1 || stage == 2 || shop_random() % 5 == 0;
    case 7:
        return (stage >= 1 && stage <= 3) || shop_random() % 5 == 0;
    case 8:
        return (stage >= 2 && stage <= 4) || shop_random() % 5 == 0;
    case 9:
        return (stage >= 3 && stage <= 5) || shop_random() % 5 == 0;
    case 10:
        return stage == 4 || stage == 5 || shop_random() % 5 == 0;
    case 11:
        return stage >= 1 && stage <= 5 && card_profile_unlocked(static_cast<CardId>(info.id));
    case 12:
        return card_profile_unlocked(static_cast<CardId>(info.id));
    }
    return 0;
}

void pick_random(Shop* shop, int min_tier, int max_tier)
{
    const CardInfo* candidates[560];
    int count = 0;
    for (int id = 0; id < 56; ++id) {
        const CardInfo& info = g_card_info[id];
        if (card_manager_acquired(static_cast<CardId>(id)) && info.unk_1c == 0)
            continue;
        // The availability call precedes price/weight filters in 00416f50.
        if (availability(info, shop->stage) != 1 || info.tier < min_tier ||
            info.tier > max_tier || info.weight == 0 || info.weight == 6)
            continue;
        int index = 0;
        while (index < shop->count && shop->goods[index]->id != id)
            ++index;
        if (index < shop->count)
            continue;
        const int weight = info.weight + (card_profile_unlocked(static_cast<CardId>(id)) ? 0 : 5);
        for (int n = 0; n < weight; ++n)
            candidates[count++] = &info;
    }
    if (count != 0)
        shop->goods[shop->count++] = candidates[shop_random() % count];
}

void build_goods(Shop* shop)
{
    const int tiers[3][2] = {{10, 14}, {7, 9}, {1, 6}};
    for (const auto& tier : tiers) {
        pick_random(shop, tier[0], tier[1]);
        if (card_manager_owned(Maneki))
            pick_random(shop, 1, 14);
    }
    // 004171b0 scans all card IDs twice: common zero-weight cards, then fixed cards.
    for (int pass = 1; pass <= 2; ++pass) {
        for (int id = 0; id < 56; ++id) {
            const CardInfo& info = g_card_info[id];
            if (card_manager_acquired(static_cast<CardId>(id)) && info.unk_1c == 0)
                continue;
            if (availability(info, shop->stage) == pass && (pass == 2 || info.weight == 0))
                shop->goods[shop->count++] = &info;
        }
    }
    shop->blank = card_manager_owned(Blank);
}

AnmVm* create_vm(AnmResource* resource, int script, float x, float y, uint32_t queue = 0)
{
    AnmVm* vm = anm_vm_alloc(resource);
    vm->extra_x = x;
    vm->extra_y = y;
    vm->node_flags = queue;
    anm_vm_set_script(vm, resource, script);
    anm_queue_add(vm);
    return vm;
}

void set_state(Shop* shop, int state)
{
    shop->state = state;
    shop->timer = 0;
}

void show_words(Shop* shop, bool visible)
{
    for (AnmVm* vm : shop->words) {
        if (visible) vm->flags2 |= 2u;
        else vm->flags2 &= ~2u;
    }
    if (visible && shop->goods[shop->selected]->category != 4)
        shop->category->flags2 |= 2u;
    else
        shop->category->flags2 &= ~2u;
}

void describe(Shop* shop)
{
    const CardInfo& info = *shop->goods[shop->selected];
    for (int line = 0; line < 7; ++line) {
        const int height = line == 0 ? 80 : 40;
        if (shop->words[line] == nullptr) {
            shop->words[line] = create_vm(shop->text, 3 + line, 448.0f,
                line == 0 ? 482.0f : 670.0f + (line - 1) * 38.0f, 4);
            gfx_device()->CreateTexture(1024, height, 1, 0, D3DFMT_A4R4G4B4,
                D3DPOOL_MANAGED, &shop->word_texture[line], nullptr);
            AnmVm* vm = shop->words[line];
            vm->texture = shop->word_texture[line];
            vm->sprite_w = 1024.0f;
            vm->sprite_h = static_cast<float>(height);
            vm->uv[0] = vm->uv[1] = 0.0f;
            vm->uv[2] = vm->uv[3] = 1.0f;
        }
        const CardId id = static_cast<CardId>(info.id);
        text_write_texture_centered(shop->words[line], 0xffffff, 0, line == 0 ? 10 : 0, 0,
            line == 0 ? card_name(id) : card_description(id, line - 1));
    }
    if (shop->category == nullptr)
        shop->category = create_vm(shop->menu, 5, 448.0f, 332.0f, 4);
    if (info.category != 4) {
        anm_set_sprite(shop->category, shop->menu, info.category + 5);
        anm_set_label(shop->category, 2);
    }
    show_words(shop, true);
}

void show_goods(Shop* shop)
{
    for (int i = 0; i < shop->count; ++i) {
        const CardInfo& info = *shop->goods[i];
        if (!card_profile_unlocked(static_cast<CardId>(info.id)))
            shop->new_mark[i] = create_vm(shop->menu, 14, 448.0f, 312.0f);
        shop->preview[i] = create_vm(shop->cards, 13, 448.0f, 312.0f, 6);
        anm_set_sprite(shop->preview[i], shop->cards, info.text_name);
        if (g_status.money < card_price(info.tier))
            anm_interrupt(shop->preview[i], 3);
    }
    if (shop->count > 1)
        anm_set_label(shop->preview[1], 21);
    anm_set_label(shop->preview[0], 20);
    if (!shop->blank)
        describe(shop);
}

void move_selection(Shop* shop, int direction)
{
    const int previous = shop->selected;
    const int current = std::clamp(previous + direction, 0, shop->count - 1);
    if (current == previous)
        return;
    shop->selected = current;
    if (direction > 0) {
        if (current + 1 < shop->count) anm_set_label(shop->preview[current + 1], 17);
        anm_set_label(shop->preview[current], 15);
        if (current > 0) anm_set_label(shop->preview[current - 1], 11);
        if (current > 1) anm_set_label(shop->preview[current - 2], 13);
    } else {
        if (current > 0) anm_set_label(shop->preview[current - 1], 18);
        anm_set_label(shop->preview[current], 16);
        if (current + 1 < shop->count) anm_set_label(shop->preview[current + 1], 12);
        if (current + 2 < shop->count) anm_set_label(shop->preview[current + 2], 14);
    }
}

void buy(Shop* shop)
{
    sound_play_se(7);
    set_state(shop, 5);
    anm_set_label(anm_find_child(shop->heading, 6), 6);
    const CardInfo& info = *shop->goods[shop->selected];
    // 00417cc0 applies acquisition before pricing: YAMAWARO discounts itself.
    card_manager_acquire(static_cast<CardId>(info.id), CardAdd_CheckEquip);
    const int price = card_price(info.tier);
    if (price <= g_status.money)
        g_status.money -= price;
    else {
        if (status_sub_power(&g_status, price - g_status.money))
            player_refresh_power_options();
        g_status.money = 0;
    }
}

void draw_prices(Shop* shop)
{
    if (shop->state != 2 && shop->state != 6 && shop->state != 7 && shop->state != 8)
        return;
    static const uint32_t colors[14] = {
        0xffffffff, 0xfff0a080, 0xfff0a080, 0xfff0a080, 0xffffb000, 0xffffb000,
        0xffffb000, 0xffffd000, 0xffffd000, 0xffffd000, 0xffffff50, 0xffffff50,
        0xffffff50, 0xffffff50
    };
    text_set_life(0);
    text_set_size(1.0f, 1.0f);
    text_set_style(6);
    text_set_alignment(0, 1);
    text_set_color(0xffd0d0d0);
    const TextPos money_pos{224.0f, 59.0f, 0.0f};
    text_draw_fmt(text_manager(), &money_pos, "Money %d  (+%d)", g_status.money, g_status.power - 100);
    const int tier = shop->goods[shop->selected]->tier;
    const int price = card_price(tier);
    text_set_color(g_status.money < price ? 0xff606060 : colors[tier]);
    const TextPos price_pos{224.0f, 222.0f, 0.0f};
    text_draw_fmt(text_manager(), &price_pos, "Cost %d", price);
    text_set_color(0xffffffff);
    text_set_style(0);
    text_set_alignment(1, 1);
}

void update_visuals(Shop* shop)
{
    if (shop->heading) anm_vm_update(shop->heading);
    for (int i = 0; i < shop->count; ++i) {
        if (shop->preview[i]) anm_vm_update(shop->preview[i]);
        if (shop->new_mark[i]) {
            AnmVm* mark = shop->new_mark[i];
            AnmVm* card = shop->preview[i];
            anm_vm_update(mark);
            mark->extra_x = card->pos_x + card->extra_x + (card->scale_x - 1.0f) * mark->pos_x;
            mark->extra_y = card->pos_y + card->extra_y + (card->scale_x - 1.0f) * mark->pos_y;
            mark->rot_z = card->rot_z - 0.34906584f;
            mark->scale_x = card->scale_x;
            mark->scale_y = card->scale_y;
            if (i < shop->selected - 1 || i > shop->selected + 1) {
                mark->extra_x = -900.0f;
                mark->extra_y = 900.0f;
            }
        }
    }
    for (AnmVm* vm : shop->words)
        if (vm) anm_vm_update(vm);
    if (shop->category) anm_vm_update(shop->category);
}

} // namespace

int32_t card_price(int32_t tier)
{
    const int price = g_card_price[tier];
    return card_manager_owned(Yamawaro) ? price * 5 / 10 : price;
}

Shop* shop_create(int stage)
{
    Shop* shop = new Shop{};
    shop->stage = stage;
    shop->cards = anm_get(0x1f, "abcard.anm");
    shop->menu = anm_get(0x20, "abmenu.anm");
    shop->text = anm_get(0, "text.anm");
    build_goods(shop);
    return shop;
}

bool shop_update(Shop* shop)
{
    // 00417cc0 bypasses the menu at timer 30; replay restores the saved purchase
    // state in the caller's leave-shop path (00417880), not from shopping inputs.
    if (shop->timer == 30 &&
        (session().mode == GameMode::Replay || session().mode == GameMode::Practice))
        return true;
    update_visuals(shop);
    const FrameInput input = input_frame();
    const uint32_t movement = input.pressed | input.repeat;
    const bool confirm = (input.pressed & (ACTION_SHOT | ACTION_MENU_OK)) != 0;
    const bool cancel = (input.pressed & (ACTION_BOMB | ACTION_MENU_CANCEL)) != 0;
    switch (shop->state) {
    case 0:
        set_state(shop, 1);
        if (session().mode != GameMode::Practice) {
            shop->heading = create_vm(shop->menu, shop->blank ? 18 : 13, 448.0f, 32.0f);
            if (shop->blank) shop->timer = 60;
            else anm_set_label(shop->heading, 27);
        }
        sound_play_se(15);
        break;
    case 1:
        if (shop->timer > 60) {
            set_state(shop, shop->blank ? 3 : 2);
            show_goods(shop);
        }
        break;
    case 2: {
        const int previous = shop->selected;
        if (movement & ACTION_RIGHT) move_selection(shop, 1);
        if (movement & ACTION_LEFT) move_selection(shop, -1);
        if (previous != shop->selected) {
            sound_play_se(10);
            describe(shop);
        }
        if (confirm) {
            const int price = card_price(shop->goods[shop->selected]->tier);
            if (g_status.money >= price) {
                shop->answer = 0;
                set_state(shop, 6);
                anm_set_label(shop->heading, 7);
                sound_play_se(7);
            } else if (g_status.power - 100 + g_status.money >= price) {
                shop->answer = 1;
                set_state(shop, 7);
                anm_set_label(shop->heading, 18);
                sound_play_se(16);
            } else {
                set_state(shop, 8);
                anm_set_label(shop->heading, 37);
                sound_play_se(16);
            }
            show_words(shop, false);
        }
        break;
    }
    case 3:
        if (confirm || (input.pressed & ACTION_BOMB)) {
            sound_play_se(7);
            anm_set_label(shop->heading, 7);
            set_state(shop, 4);
            card_manager_clear_cards();
            for (int i = 0; i < shop->count; ++i)
                if (shop->goods[i]->unk_1c == 0)
                    card_manager_acquire(static_cast<CardId>(shop->goods[i]->id), CardAdd_CheckEquip);
            g_status.money = 0;
        }
        break;
    case 4:
        if (confirm || (input.pressed & ACTION_BOMB))
            return true;
        break;
    case 5:
        if (shop->timer > 30)
            return true;
        break;
    case 6: case 7: {
        const int previous = shop->answer;
        if (movement & ACTION_UP) shop->answer ^= 1;
        if (movement & ACTION_DOWN) shop->answer ^= 1;
        if (shop->answer != previous) {
            sound_play_se(10);
            anm_set_label(shop->heading, shop->answer + (shop->state == 7 ? 17 : 7));
        }
        if (cancel || (confirm && shop->answer == 1)) {
            sound_play_se(9);
            set_state(shop, 2);
            anm_set_label(shop->heading, 27);
            show_words(shop, true);
        } else if (confirm)
            buy(shop);
        break;
    }
    case 8:
        if (input.pressed & (ACTION_SHOT | ACTION_BOMB | ACTION_LEFT | ACTION_RIGHT |
                             ACTION_MENU_OK | ACTION_MENU_CANCEL)) {
            sound_play_se(9);
            set_state(shop, 2);
            anm_set_label(shop->heading, 27);
            show_words(shop, true);
        }
        break;
    case 9:
        if (shop->timer > 9)
            return true;
        break;
    }
    draw_prices(shop);
    ++shop->timer;
    return false;
}

void shop_destroy(Shop* shop)
{
    anm_vm_free(shop->heading);
    for (int i = 0; i < shop->count; ++i) {
        anm_vm_free(shop->preview[i]);
        anm_vm_free(shop->new_mark[i]);
    }
    for (int i = 0; i < 7; ++i) {
        anm_vm_free(shop->words[i]);
        if (shop->word_texture[i]) shop->word_texture[i]->Release();
    }
    anm_vm_free(shop->category);
    delete shop;
}
