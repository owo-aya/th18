#include "cards/card_menu.h"

#include <algorithm>
#include <d3d9.h>

#include "cards/card_profile.h"
#include "core/input.h"
#include "platform/gfx.h"
#include "presentation/anm.h"
#include "presentation/sound.h"
#include "presentation/text.h"

struct PauseCards {
    int state = 0;
    int timer = 0;
    int choice = 0;
    int owned_count = 0;
    int owned_selection = 0;
    int selection = 0;
    int scroll = 0;
    AnmResource* cards;
    AnmResource* menu;
    AnmResource* text;
    AnmVm* root = nullptr;
    AnmVm* small_cards[256]{};
    AnmVm* preview[256]{};
    AnmVm* tiles[56]{};
    CardId owned[256]{};
    AnmVm* words[7]{};
    IDirect3DTexture9* word_texture[7]{};
    AnmVm* category = nullptr;
};

namespace {
constexpr char kEmpty[] = "\x89\xbd\x82\xe0\x8e\x9d\x82\xc1\x82\xc4\x82\xa2\x82\xdc\x82\xb9\x82\xf1\x81I";
constexpr char kLocked[] = "\x82\xdc\x82\xbe\x8e\xe8\x82\xc9\x93\xfc\x82\xea\x82\xc4\x82\xc8\x82\xa2";

void create_vm(AnmVm*& slot, AnmResource* resource, int script,
               float x, float y, uint32_t queue) {
    slot = anm_vm_alloc(resource);
    slot->extra_x = x;
    slot->extra_y = y;
    slot->flags3 |= 0x1000u;
    slot->node_flags = queue;
    anm_vm_set_script(slot, resource, script);
    anm_queue_add(slot);
    anm_manage_vm(slot, &slot);
}

void create_card(AnmVm*& slot, PauseCards* page, int script,
                 CardId id, float x, float y) {
    create_vm(slot, page->cards, script, x, y, 6);
    // 00409030 / 004091a0 tick again before replacing the card face.
    anm_vm_update(slot);
    anm_set_sprite(slot, page->cards, card_info_by_id(id)->text_name);
}

void free_vm(AnmVm*& slot) {
    anm_vm_free(slot);
    slot = nullptr;
}

void set_visible(AnmVm* vm, bool visible) {
    if (visible) vm->flags2 |= 2u;
    else vm->flags2 &= ~2u;
    for (int queue = 0; queue < 2; ++queue)
        for (AnmVm* child = static_cast<AnmVm*>(queue == 0 ?
                 anm_manager()->queue_head : anm_manager()->queue_head2);
             child != nullptr; child = static_cast<AnmVm*>(child->node_next))
            if (child->parent == vm) set_visible(child, visible);
}

void clear_cards(PauseCards* page) {
    for (AnmVm*& vm : page->small_cards) free_vm(vm);
    for (AnmVm*& vm : page->preview) free_vm(vm);
    for (AnmVm*& vm : page->tiles) free_vm(vm);
}

void clear_words(PauseCards* page) {
    for (int i = 0; i < 7; ++i) {
        free_vm(page->words[i]);
        if (page->word_texture[i] != nullptr) {
            page->word_texture[i]->Release();
            page->word_texture[i] = nullptr;
        }
    }
    free_vm(page->category);
}

void create_words(PauseCards* page, bool description) {
    for (int i = 0; i < (description ? 7 : 1); ++i) {
        const int height = i == 0 ? 80 : 40;
        create_vm(page->words[i], page->text, 3 + i, 0, 0, 4);
        gfx_device()->CreateTexture(1024, height, 1, 0, D3DFMT_A4R4G4B4,
                                    D3DPOOL_MANAGED, &page->word_texture[i], nullptr);
        AnmVm* vm = page->words[i];
        vm->texture = page->word_texture[i];
        vm->sprite_w = 1024;
        vm->sprite_h = static_cast<float>(height);
        vm->uv[0] = vm->uv[1] = 0;
        vm->uv[2] = vm->uv[3] = 1;
    }
    create_vm(page->category, page->menu, 5, 448, 260, 4);
}

void describe(PauseCards* page, CardId id, bool description, bool owned) {
    const bool unlocked = card_profile_unlocked(id);
    const float origin_y = owned ? 192.0f : 100.0f;
    AnmVm* name = page->words[0];
    name->extra_x = 448;
    name->extra_y = origin_y + 310;
    text_write_texture_centered(name, unlocked ? 0xffffff : 0x808080,
                                 0, 10, 0, unlocked ? card_name(id) : kLocked);
    for (int i = 1; i < 7; ++i) {
        AnmVm* word = page->words[i];
        word->extra_x = 448;
        word->extra_y = origin_y + 498 + (i - 1) * 38;
        if (unlocked && description)
            text_write_texture_centered(word, 0xffffff, 0, 0, 0,
                                         card_description(id, i - 1));
        set_visible(word, unlocked && description);
    }
    page->category->extra_y = origin_y + 160;
    const int category = card_info_by_id(id)->category;
    set_visible(page->category, unlocked && description && category != 4);
    if (unlocked && description && category != 4) {
        anm_set_sprite(page->category, page->menu, category + 5);
        anm_set_label(page->category, 2);
    }
}

void set_state(PauseCards* page, int state) {
    page->state = state;
    page->timer = 0;
}

void build_small(PauseCards* page, bool expanded) {
    // 00415cb0 receives spacing=100 and maximum span=512 at both mode-0 sites.
    float spacing = 100;
    float span = (page->owned_count - 1) * spacing;
    if (span > 512) {
        span = 512;
        spacing = span / (page->owned_count - 1);
    }
    float x = 448 - span * 0.5f;
    for (int i = 0; i < page->owned_count; ++i) {
        free_vm(page->small_cards[i]);
        create_card(page->small_cards[i], page, 12, page->owned[i],
                     x, expanded ? 116.0f : 330.0f);
        if (expanded) anm_interrupt(page->small_cards[i], 7);
        x += spacing;
    }
}

void build_main(PauseCards* page) {
    free_vm(page->root);
    create_vm(page->root, page->menu, 4, 448, 100, 0);
    anm_set_label(page->root, page->choice + 7);
    page->owned_count = card_manager_card_count();
    // 00415af0 mode 0 reverses the acquisition list: newest card first.
    for (int i = 0; i < page->owned_count; ++i)
        page->owned[i] = card_id(card_manager_card_at(page->owned_count - 1 - i));
    build_small(page, false);
    set_state(page, 1);
}

void build_owned(PauseCards* page) {
    anm_set_label(page->root, 1);
    if (page->owned_count == 0) {
        create_words(page, false);
        page->words[0]->extra_x = 448;
        page->words[0]->extra_y = 470;
        text_write_texture_centered(page->words[0], 0xffffff, 0, 10, 0, kEmpty);
        set_visible(page->category, false);
        set_state(page, 5);
        return;
    }
    for (int i = 0; i < page->owned_count; ++i)
        create_card(page->preview[i], page, 13, page->owned[i], 448, 324);
    const int current = page->owned_selection;
    anm_interrupt(page->preview[current], 2);
    anm_interrupt(page->preview[current], 20);
    if (current + 1 < page->owned_count) anm_interrupt(page->preview[current + 1], 21);
    if (current > 0) anm_interrupt(page->preview[current - 1], 19);
    build_small(page, true);
    anm_interrupt(page->small_cards[current], 2);
    create_words(page, true);
    describe(page, page->owned[current], true, true);
    set_state(page, 4);
}

void move_owned(PauseCards* page, int direction) {
    const int previous = page->owned_selection;
    anm_set_label(page->small_cards[previous], 3);
    const int current = std::clamp(previous + direction, 0, page->owned_count - 1);
    page->owned_selection = current;
    anm_set_label(page->small_cards[current], 2);
    if (current == previous) return;
    if (direction > 0) {
        if (current + 1 < page->owned_count) anm_set_label(page->preview[current + 1], 17);
        anm_set_label(page->preview[current], 15);
        if (current > 0) anm_set_label(page->preview[current - 1], 11);
        if (current > 1) anm_set_label(page->preview[current - 2], 13);
    } else {
        if (current > 0) anm_set_label(page->preview[current - 1], 18);
        anm_set_label(page->preview[current], 16);
        if (current + 1 < page->owned_count) anm_set_label(page->preview[current + 1], 12);
        if (current + 2 < page->owned_count) anm_set_label(page->preview[current + 2], 14);
    }
    sound_play_se(10);
    describe(page, page->owned[current], true, true);
}

void build_library(PauseCards* page) {
    anm_set_label(page->root, 1);
    clear_cards(page);
    const CardId* order = card_profile_order();
    for (int i = 0; i < 56; ++i) {
        const CardId face = card_profile_unlocked(order[i]) ? order[i] : Back;
        create_card(page->preview[i], page, 13, face, 448, 250);
        create_card(page->tiles[i], page, 12, face,
                     181.0f + (i % 7) * 90, 600.0f + (i / 7) * 120);
        anm_interrupt(page->tiles[i], 8);
        anm_interrupt(page->tiles[i], 23);
    }
    page->scroll = std::clamp(page->selection / 7 - 1, 0, 6);
    for (int i = 0; i < 56; ++i)
        if (i / 7 < page->scroll) anm_interrupt(page->tiles[i], 16);
    for (int row = 0; row < page->scroll; ++row)
        for (int i = 0; i < 56; ++i) {
            anm_interrupt(page->tiles[i], 9);
            if (i / 7 == 0) anm_interrupt(page->tiles[i], 11);
            else if (i / 7 == page->scroll + 2) anm_interrupt(page->tiles[i], 12);
        }
    anm_interrupt(page->preview[page->selection], 2);
    anm_interrupt(page->preview[page->selection], 10);
    anm_interrupt(page->tiles[page->selection], 2);
    create_words(page, true);
    describe(page, order[page->selection], false, false);
    set_state(page, 6);
}

void scroll_library(PauseCards* page, bool detail) {
    int row = page->scroll;
    const int selected_row = page->selection / 7;
    if (selected_row < row + 1) row = std::max(0, selected_row - 1);
    else if (selected_row >= row + 2) row = std::min(selected_row - 1, 6);
    if (row == page->scroll) return;
    for (int i = 0; i < 56; ++i) {
        anm_interrupt(page->tiles[i], row > page->scroll ? 9 : 10);
        if (i / 7 == (row > page->scroll ? page->scroll : page->scroll + 2) &&
            (row > page->scroll || detail))
            anm_interrupt(page->tiles[i], 11);
        else if (i / 7 == (row > page->scroll ? row + 2 : row))
            anm_interrupt(page->tiles[i], 12);
    }
    page->scroll = row;
}

void move_library(PauseCards* page, int direction, bool detail) {
    const int previous = page->selection;
    int current = previous;
    if (detail) current = std::clamp(current + direction, 0, 55);
    else if (direction == -1) current += current % 7 == 0 ? 6 : -1;
    else if (direction == 1) current += current % 7 == 6 ? -6 : 1;
    else if (direction == -7 && current >= 7) current -= 7;
    else if (direction == 7 && current + 7 < 56) current += 7;
    if (current == previous) return;
    if (direction == 7) anm_set_label(page->preview[previous], 8);
    else anm_interrupt(page->preview[previous], direction < 0 ? 7 : 8);
    anm_interrupt(page->tiles[previous], 3);
    page->selection = current;
    if (direction == 7) anm_set_label(page->preview[current], 10);
    else anm_interrupt(page->preview[current], direction < 0 ? 9 : 10);
    anm_interrupt(page->tiles[current], 2);
    sound_play_se(10);
    describe(page, card_profile_order()[current], detail, false);
    scroll_library(page, detail);
}

void return_main(PauseCards* page) {
    clear_cards(page);
    clear_words(page);
    set_state(page, 0);
}
} // namespace

PauseCards* pause_cards_create() {
    auto* page = new PauseCards;
    page->cards = anm_get(0x1f, "abcard.anm");
    page->menu = anm_get(0x20, "abmenu.anm");
    page->text = anm_get(0, "text.anm");
    return page;
}

void pause_cards_destroy(PauseCards* page) {
    free_vm(page->root);
    clear_cards(page);
    clear_words(page);
    delete page;
}

bool pause_cards_update(PauseCards* page) {
    const FrameInput input = input_frame();
    const uint32_t movement = input.pressed | input.repeat;
    const bool confirm = (input.pressed & (ACTION_SHOT | ACTION_MENU_OK)) != 0;
    const bool cancel = (input.pressed & (ACTION_BOMB | ACTION_MENU_CANCEL)) != 0;
    switch (page->state) {
    case 0:
        build_main(page);
        break;
    case 1:
        if (page->timer >= 7) set_state(page, 2);
        break;
    case 2: {
        const int previous = page->choice;
        if (movement & ACTION_UP) page->choice = (page->choice + 2) % 3;
        if (movement & ACTION_DOWN) page->choice = (page->choice + 1) % 3;
        if (previous != page->choice) {
            anm_set_label(page->root, page->choice + 7);
            sound_play_se(10);
        }
        if (cancel || (confirm && page->choice == 2)) {
            sound_play_se(9);
            set_state(page, 10);
        } else if (confirm) {
            sound_play_se(7);
            anm_set_label(anm_find_child(page->root, page->choice + 1), 6);
            set_state(page, 3);
        }
        break;
    }
    case 3:
        if (page->timer >= 31) {
            if (page->choice == 0) build_owned(page);
            else build_library(page);
        }
        break;
    case 4:
        if (movement & ACTION_RIGHT) move_owned(page, 1);
        if (movement & ACTION_LEFT) move_owned(page, -1);
        if (cancel) {
            sound_play_se(9);
            return_main(page);
        }
        break;
    case 5:
        if (cancel || confirm) {
            sound_play_se(9);
            return_main(page);
        }
        break;
    case 6:
        if (movement & ACTION_LEFT) move_library(page, -1, false);
        if (movement & ACTION_RIGHT) move_library(page, 1, false);
        if (movement & ACTION_UP) move_library(page, -7, false);
        if (movement & ACTION_DOWN) move_library(page, 7, false);
        if (confirm) {
            sound_play_se(7);
            for (AnmVm* vm : page->tiles) set_visible(vm, false);
            describe(page, card_profile_order()[page->selection], true, false);
            set_state(page, 7);
        }
        if (cancel) {
            sound_play_se(9);
            return_main(page);
        }
        break;
    case 7:
        if (movement & ACTION_LEFT) move_library(page, -1, true);
        if (movement & ACTION_RIGHT) move_library(page, 1, true);
        if (cancel) {
            sound_play_se(9);
            for (AnmVm* vm : page->tiles) set_visible(vm, true);
            describe(page, card_profile_order()[page->selection], false, false);
            set_state(page, 6);
        }
        break;
    case 10:
        if (page->timer >= 10) return true;
        break;
    }
    ++page->timer;
    return false;
}
