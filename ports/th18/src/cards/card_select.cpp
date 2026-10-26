#include "cards/card_select.h"

#include <d3d9.h>
#include <algorithm>

#include "cards/card_info.h"
#include "cards/card_profile.h"
#include "core/input.h"
#include "platform/gfx.h"
#include "presentation/anm.h"
#include "presentation/text.h"
#include "presentation/sound.h"

struct CardSelect {
    int bank;
    int state;
    int timer;
    int slot;
    int count;
    int selected;
    int scroll;
    bool focused;
    AnmResource* cards;
    AnmResource* menu;
    AnmResource* text;
    AnmVm* heading;
    AnmVm* slots[16];
    AnmVm* preview[57];
    AnmVm* tiles[57];
    CardId ids[57];
    AnmVm* words[7];
    IDirect3DTexture9* word_texture[7];
    AnmVm* category;
};

namespace {

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

AnmVm* create_card(CardSelect* selector, int script, CardId id, float x, float y)
{
    AnmVm* vm = create_vm(selector->cards, script, x, y, 6);
    anm_set_sprite(vm, selector->cards, card_info_by_id(id)->text_name);
    return vm;
}

void free_vm(AnmVm*& vm)
{
    anm_vm_free(vm);
    vm = nullptr;
}

void update_vm(AnmVm* vm)
{
    if (vm != nullptr)
        anm_vm_update(vm);
}

void clear_words(CardSelect* selector)
{
    for (int i = 0; i < 7; ++i) {
        free_vm(selector->words[i]);
        if (selector->word_texture[i] != nullptr) {
            selector->word_texture[i]->Release();
            selector->word_texture[i] = nullptr;
        }
    }
    free_vm(selector->category);
}

void describe_card(CardSelect* selector, CardId id, bool description)
{
    // 00416540: the name is at origin.y + 92 + 310; body begins 188px later.
    for (int i = 0; i < 7; ++i) {
        const int height = i == 0 ? 80 : 40;
        if (selector->words[i] == nullptr) {
            selector->words[i] = create_vm(selector->text, 3 + i, 640.0f,
                                           i == 0 ? 472.0f : 660.0f + (i - 1) * 38.0f, 4);
            gfx_device()->CreateTexture(1024, height, 1, 0, D3DFMT_A4R4G4B4,
                                        D3DPOOL_MANAGED, &selector->word_texture[i], nullptr);
            AnmVm* vm = selector->words[i];
            vm->texture = selector->word_texture[i];
            vm->sprite_w = 1024.0f;
            vm->sprite_h = static_cast<float>(height);
            vm->uv[0] = vm->uv[1] = 0.0f;
            vm->uv[2] = vm->uv[3] = 1.0f;
        }
        text_write_texture_centered(selector->words[i], 0x00ffffff, 0, i == 0 ? 10 : 0, 0,
                                    i == 0 ? card_name(id) : card_description(id, i - 1));
        if (i == 0 || description)
            selector->words[i]->flags2 |= 2u;
        else
            selector->words[i]->flags2 &= ~2u;
    }
    if (selector->category == nullptr)
        selector->category = create_vm(selector->menu, 5, 640.0f, 322.0f, 4);
    const int category = card_info_by_id(id)->category;
    if (category == 4)
        selector->category->flags2 &= ~2u;
    else {
        anm_set_sprite(selector->category, selector->menu, category + 5);
        selector->category->flags2 |= 2u;
        anm_set_label(selector->category, 2);
    }
}

void clear_cards(CardSelect* selector)
{
    for (AnmVm*& slot : selector->slots)
        free_vm(slot);
    for (AnmVm*& preview : selector->preview)
        free_vm(preview);
    for (AnmVm*& tile : selector->tiles)
        free_vm(tile);
}

void build_small_slots(CardSelect* selector, bool expanded)
{
    const int count = card_profile_deck_count(selector->bank);
    float spacing = expanded ? 100.0f : 80.0f;
    float span = spacing * (count - 1);
    if (span > 512.0f) {
        span = 512.0f;
        spacing = span / (count - 1);
    }
    float x = (expanded ? 640.0f : 980.0f) - span * 0.5f;
    for (int slot = 0; slot < count; ++slot) {
        free_vm(selector->slots[slot]);
        selector->slots[slot] = create_card(selector, 12,
            card_profile_deck_id(selector->bank, slot), x, expanded ? 86.0f : 760.0f);
        if (expanded) {
            anm_interrupt(selector->slots[slot], 7);
            anm_interrupt(selector->slots[slot], 22);
        }
        x += spacing;
    }
}

void build_slot_view(CardSelect* selector)
{
    clear_cards(selector);
    const int count = card_profile_deck_count(selector->bank);
    for (int slot = 0; slot < count; ++slot)
        selector->preview[slot] = create_card(selector, 13,
            card_profile_deck_id(selector->bank, slot), 640.0f, 294.0f);
    anm_interrupt(selector->preview[selector->slot], 2);
    anm_interrupt(selector->preview[selector->slot], 20);
    if (selector->slot + 1 < count)
        anm_interrupt(selector->preview[selector->slot + 1], 21);
    if (selector->slot > 0)
        anm_interrupt(selector->preview[selector->slot - 1], 19);
    build_small_slots(selector, true);
    anm_interrupt(selector->slots[selector->slot], 2);
    describe_card(selector, card_profile_deck_id(selector->bank, selector->slot), true);
    selector->state = 8;
    selector->timer = 0;
}

void build_grid(CardSelect* selector)
{
    clear_cards(selector);
    selector->count = 0;
    const CardId* order = card_profile_order();
    for (int i = 0; i < 57; ++i) {
        const CardId id = order[i];
        if (card_info_by_id(id)->unk_20 != 0 && card_profile_unlocked(id))
            selector->ids[selector->count++] = id;
    }
    selector->selected = 0;
    for (int i = 0; i < selector->count; ++i) {
        const CardId id = selector->ids[i];
        selector->preview[i] = create_card(selector, 13, id, 640.0f, 294.0f);
        selector->tiles[i] = create_card(selector, 12, id,
                                       238.0f + (i % 10) * 90.0f,
                                       634.0f + (i / 10) * 120.0f);
        anm_interrupt(selector->tiles[i], 8);
        anm_interrupt(selector->tiles[i], 22);
        if (id == card_profile_deck_id(selector->bank, selector->slot))
            selector->selected = i;
    }
    // 00415b70 positions a retained selection in the middle visible row.
    // Repeated label9 adjusts the script's cached destination before it ticks.
    int row = selector->selected / 10 - 1;
    if (row < 0)
        row = 0;
    else
        row = std::min(row, selector->count / 10 - 2);
    for (int i = 0; i < selector->count; ++i)
        if (i / 10 < row)
            anm_interrupt(selector->tiles[i], 16);
    for (int n = 0; n < row; ++n) {
        for (int i = 0; i < selector->count; ++i) {
            anm_interrupt(selector->tiles[i], 9);
            if (i / 10 == 0)
                anm_interrupt(selector->tiles[i], 11);
            else if (i / 10 == row + 2)
                anm_interrupt(selector->tiles[i], 12);
        }
    }
    selector->scroll = row;
    anm_interrupt(selector->preview[selector->selected], 2);
    anm_interrupt(selector->tiles[selector->selected], 2);
    describe_card(selector, selector->ids[selector->selected], false);
    selector->state = 9;
    selector->timer = 0;
}

void move_slot(CardSelect* selector, int direction)
{
    const int previous = selector->slot;
    const int count = card_profile_deck_count(selector->bank);
    selector->slot = std::clamp(previous + direction, 0, count - 1);
    if (selector->slot == previous)
        return;
    sound_play_se(10);
    anm_set_label(selector->slots[previous], 3);
    anm_set_label(selector->slots[selector->slot], 2);
    const int current = selector->slot;
    if (direction > 0) {
        if (current + 1 < count) anm_set_label(selector->preview[current + 1], 17);
        anm_set_label(selector->preview[current], 15);
        if (current > 0) anm_set_label(selector->preview[current - 1], 11);
        if (current > 1) anm_set_label(selector->preview[current - 2], 13);
    } else {
        if (current > 0) anm_set_label(selector->preview[current - 1], 18);
        anm_set_label(selector->preview[current], 16);
        if (current + 1 < count) anm_set_label(selector->preview[current + 1], 12);
        if (current + 2 < count) anm_set_label(selector->preview[current + 2], 14);
    }
    describe_card(selector, card_profile_deck_id(selector->bank, current), true);
}

void move_grid(CardSelect* selector, int direction)
{
    const int previous = selector->selected;
    int current = previous;
    if (direction == -1) {
        if (current % 10 == 0)
            current = current / 10 < selector->count / 10 ? current + 9 : selector->count - 1;
        else
            --current;
    } else if (direction == 1) {
        if (current == selector->count - 1)
            current = selector->count / 10 * 10;
        else
            current += current % 10 == 9 ? -9 : 1;
    } else if (direction == -10 && current >= 10)
        current -= 10;
    else if (direction == 10 && current + 10 < selector->count)
        current += 10;
    if (previous == current)
        return;
    sound_play_se(10);
    if (direction == 10)
        anm_set_label(selector->preview[previous], 8);
    else
        anm_interrupt(selector->preview[previous], direction < 0 ? 7 : 8);
    anm_interrupt(selector->tiles[previous], 3);
    selector->selected = current;
    if (direction == 10)
        anm_set_label(selector->preview[current], 10);
    else
        anm_interrupt(selector->preview[current], direction < 0 ? 9 : 10);
    anm_interrupt(selector->tiles[current], 2);
    describe_card(selector, selector->ids[current], false);
    int row = selector->scroll;
    const int selected_row = current / 10;
    if (selected_row < row + 1)
        row = std::max(0, selected_row - 1);
    else if (selected_row >= row + 2)
        row = std::min(selected_row - 1, selector->count / 10 - 2);
    if (row != selector->scroll) {
        for (int i = 0; i < selector->count; ++i) {
            anm_interrupt(selector->tiles[i], row > selector->scroll ? 9 : 10);
            if (i / 10 == (row > selector->scroll ? selector->scroll : selector->scroll + 2))
                anm_interrupt(selector->tiles[i], 11);
            else if (i / 10 == (row > selector->scroll ? row + 2 : row))
                anm_interrupt(selector->tiles[i], 12);
        }
        selector->scroll = row;
    }
}

} // namespace

CardSelect* card_select_create(int bank)
{
    CardSelect* selector = new CardSelect{};
    selector->bank = bank;
    selector->state = 11;
    selector->cards = anm_get(0x1f, "abcard.anm");
    selector->menu = anm_get(0x20, "abmenu.anm");
    selector->text = anm_get(0, "text.anm");
    return selector;
}

void card_select_destroy(CardSelect* selector)
{
    clear_cards(selector);
    clear_words(selector);
    free_vm(selector->heading);
    delete selector;
}

void card_select_focus(CardSelect* selector, bool focused)
{
    selector->focused = focused;
    if (selector->heading != nullptr) {
        anm_set_label(selector->heading, focused ? 2 : 3);
        for (int i = 0; i < card_profile_deck_count(selector->bank); ++i)
            if (selector->slots[i] != nullptr)
                anm_set_label(selector->slots[i], focused ? 4 : 5);
    }
}

void card_select_open(CardSelect* selector)
{
    // 00466740 forces 00413af0 state 3's timer to 31, bypassing the generic
    // card-menu choices and entering the selected starting-deck bank directly.
    anm_set_label(selector->heading, 1);
    selector->state = 3;
    selector->timer = 31;
}

void card_select_stage_select(CardSelect* selector)
{
    // 467320: move the collapsed deck heading into its stage-select position.
    anm_set_label(selector->heading, 27);
}

bool card_select_closed(const CardSelect* selector) { return selector->state > 10; }

void card_select_update(CardSelect* selector)
{
    update_vm(selector->heading);
    for (AnmVm* vm : selector->slots) update_vm(vm);
    for (AnmVm* vm : selector->preview) update_vm(vm);
    for (AnmVm* vm : selector->tiles) update_vm(vm);
    for (AnmVm* vm : selector->words) update_vm(vm);
    update_vm(selector->category);
    const FrameInput input = input_frame();
    const uint32_t movement = input.pressed | input.repeat;
    const bool confirm = (input.pressed & (ACTION_SHOT | ACTION_MENU_OK)) != 0;
    const bool cancel = (input.pressed & (ACTION_BOMB | ACTION_MENU_CANCEL | ACTION_CARD)) != 0;
    switch (selector->state) {
    case 11:
        free_vm(selector->heading);
        selector->heading = create_vm(selector->menu, 15, 0.0f, 0.0f);
        build_small_slots(selector, false);
        if (selector->focused) {
            card_select_focus(selector, true);
            for (int i = 0; i < 10; ++i) {
                anm_vm_update(selector->heading);
                for (AnmVm* vm : selector->slots) update_vm(vm);
            }
        }
        selector->state = 12;
        selector->timer = 0;
        break;
    case 12:
        if (selector->timer >= 7)
            selector->state = 13;
        break;
    case 3:
        if (selector->timer >= 31)
            build_slot_view(selector);
        break;
    case 8:
        if ((movement & ACTION_RIGHT) != 0) move_slot(selector, 1);
        if ((movement & ACTION_LEFT) != 0) move_slot(selector, -1);
        if (confirm) {
            sound_play_se(7);
            build_grid(selector);
        } else if (cancel) {
            sound_play_se(9);
            clear_cards(selector);
            clear_words(selector);
            selector->state = 14;
            selector->timer = 0;
        }
        break;
    case 9:
        if ((movement & ACTION_LEFT) != 0) move_grid(selector, -1);
        if ((movement & ACTION_RIGHT) != 0) move_grid(selector, 1);
        if ((movement & ACTION_UP) != 0) move_grid(selector, -10);
        if ((movement & ACTION_DOWN) != 0) move_grid(selector, 10);
        if (confirm) {
            const CardId id = selector->ids[selector->selected];
            bool duplicate = false;
            for (int i = 0; i < card_profile_deck_count(selector->bank); ++i)
                if (i != selector->slot && id != Null && card_profile_deck_id(selector->bank, i) == id)
                    duplicate = true;
            if (!duplicate) {
                sound_play_se(7);
                card_profile_set_deck_card(selector->bank, selector->slot, id);
                build_slot_view(selector);
            } else sound_play_se(16);
        } else if (cancel) {
            sound_play_se(9);
            build_slot_view(selector);
        }
        break;
    case 14:
        if (selector->timer >= 10) {
            selector->state = 11;
            selector->timer = 0;
        }
        break;
    }
    ++selector->timer;
}

void card_select_render(CardSelect* selector)
{
    if (selector->state != 8 && selector->state != 9)
        return;
    uint8_t* fields = reinterpret_cast<uint8_t*>(text_manager());
    *reinterpret_cast<int32_t*>(fields + 0x19250) = 0;
    text_set_life(0);
    // 00415e00 labels every nonempty equipped card in the carry grid.
    if (selector->state == 9) {
        *reinterpret_cast<int32_t*>(fields + 0x19254) = 0;
        text_set_size(0.5f, 0.5f);
        for (int slot = 0; slot < card_profile_deck_count(selector->bank); ++slot) {
            const CardId id = card_profile_deck_id(selector->bank, slot);
            if (id == Null)
                continue;
            int index = 0;
            for (int i = 0; i < selector->count; ++i)
                if (selector->ids[i] == id)
                    index = i;
            AnmVm* tile = selector->tiles[index];
            const uint32_t alpha = tile->color & 0xff000000u;
            TextPos label{tile->pos_x + tile->extra_x,
                          tile->pos_y + tile->extra_y + 36.0f, 0.0f};
            text_set_style(8);
            text_set_color(alpha);
            text_draw_fmt(text_manager(), &label, "Slot %d", slot + 1);
            text_set_style(6);
            text_set_color(alpha | 0xffff00u);
            text_draw_fmt(text_manager(), &label, "Slot %d", slot + 1);
        }
    }
    *reinterpret_cast<int32_t*>(fields + 0x19254) = 1;
    text_set_size(1.0f, 1.0f);
    TextPos pos{640.0f, 430.0f, 0.0f};
    text_set_style(8);
    text_set_color(0xff000000);
    text_draw_fmt(text_manager(), &pos, "Slot %d", selector->slot + 1);
    text_set_style(6);
    text_set_color(0xffffff00);
    text_draw_fmt(text_manager(), &pos, "Slot %d", selector->slot + 1);
    *reinterpret_cast<int32_t*>(fields + 0x19250) = 1;
    *reinterpret_cast<int32_t*>(fields + 0x19254) = 1;
    text_set_style(0);
    text_set_color(0xffffffff);
}
