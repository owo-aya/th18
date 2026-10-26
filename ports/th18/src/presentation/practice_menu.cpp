#include "presentation/practice_menu.h"

#include <cstdio>
#include <cstring>

#include "cards/card_profile.h"
#include "core/app.h"
#include "core/game_session.h"
#include "core/input.h"
#include "gameplay/score_data.h"
#include "presentation/anm.h"
#include "presentation/sound.h"
#include "presentation/text.h"
#include "replay/replay.h"

struct SpellPracticeMenu {
    AnmResource* resource;
    AnmVm* character = nullptr;
    AnmVm* stages = nullptr;
    AnmVm* groups = nullptr;
    AnmVm* heading = nullptr;
    AnmVm* rows[5]{};
    AnmVm* ranks[5]{};
    int player;
    int stage = 0;
    int group = 0;
    int selection = 0;
    int page = 0;
    int state = 0;
    int timer = 0;
    int spells[5]{};
};

namespace {
// 4b3710's 7x13x5 table: four consecutive difficulty IDs per main-game
// group, one Extra ID per group. The 97 entries at 4b3f30 have no Overdrive.
constexpr int kGroupCounts[] = {2, 3, 3, 3, 4, 6, 13};
constexpr int kFirstSpell[] = {0, 8, 20, 32, 44, 60, 84};

const uint8_t* record(int player, int spell) {
    return score_data().character[player] + 0x8d0 + spell * 0xdc;
}

int number(const uint8_t* bytes, int offset) {
    return *reinterpret_cast<const int32_t*>(bytes + offset);
}

int row_count(const SpellPracticeMenu* menu) { return menu->stage == 6 ? 1 : 4; }

int spell_at(const SpellPracticeMenu* menu, int row) {
    return kFirstSpell[menu->stage] + (menu->stage == 6 ? menu->group : menu->group * 4 + row);
}

bool encountered(int spell) { return number(record(4, spell), 0xc8) != 0; }

void create_vm(SpellPracticeMenu* menu, AnmVm** slot, int script) {
    AnmVm* vm = anm_vm_alloc(menu->resource);
    vm->flags3 |= 0x1000;
    anm_vm_set_script(vm, menu->resource, script);
    anm_queue_add(vm);
    anm_manage_vm(vm, slot);
}

void leave_vm(AnmVm*& vm) {
    if (vm != nullptr) {
        anm_set_label(vm, 1);
        anm_manage_vm(vm, nullptr);
    }
}

void select_row(SpellPracticeMenu* menu) {
    for (int row = 0; row < row_count(menu); ++row) {
        anm_set_label(menu->rows[row], row == menu->selection ? 2 : 3);
        anm_interrupt(menu->ranks[menu->stage == 6 ? 4 : row], row == menu->selection ? 2 : 3);
    }
}

// 46c680: the aggregate record unlocks a spell, while the selected character's
// spell-practice captures determine its name color and displayed results.
void show_spells(SpellPracticeMenu* menu) {
    for (AnmVm*& rank : menu->ranks) anm_vm_free(rank);
    for (int row = 0; row < 5; ++row) {
        menu->spells[row] = -2;
        if (row >= row_count(menu)) {
            anm_vm_free(menu->rows[row]);
            continue;
        }
        if (menu->rows[row] == nullptr) create_vm(menu, &menu->rows[row], 100 + row);
        const int id = spell_at(menu, row);
        const int rank = menu->stage == 6 ? 4 : row;
        create_vm(menu, &menu->ranks[rank], 160 + rank);
        if (menu->stage != 6) anm_interrupt(menu->ranks[rank], 3);
        char line[256];
        uint32_t color;
        if (encountered(id)) {
            menu->spells[row] = id;
            char name[193];
            std::strcpy(name, reinterpret_cast<const char*>(record(4, id)));
            const size_t length = std::strlen(name);
            if (length < 42) {
                std::memset(name + length, ' ', 42 - length);
                name[42] = '\0';
            }
            std::snprintf(line, sizeof(line), " No.%3d  %s", id + 1, name);
            color = number(record(menu->player, id), 0xc4) != 0 ? 0xffff80 : 0xefefef;
        } else {
            menu->spells[row] = -1;
            std::snprintf(line, sizeof(line),
                " No.%3d  \x81\x48\x81\x48\x81\x48\x81\x48\x81\x48\x81\x48\x81\x48\x81\x48\x81\x48\x81\x48\x81\x48", id + 1);
            color = 0xb0b0b0;
        }
        text_write_texture(menu->rows[row], color, 0, 0, 0, 0, line);
        anm_set_label(menu->rows[row], 3);
    }
}

void set_state(SpellPracticeMenu* menu, int state) {
    menu->state = state;
    menu->timer = 0;
}

void enter_stages(SpellPracticeMenu* menu) {
    create_vm(menu, &menu->stages, 118);
    create_vm(menu, &menu->heading, 45);
    menu->page = 0;
    set_state(menu, 1);
}

void enter_groups(SpellPracticeMenu* menu) {
    create_vm(menu, &menu->heading, 39);
    create_vm(menu, &menu->groups, 119);
    for (int i = 0; i < 13 - kGroupCounts[menu->stage]; ++i)
        anm_interrupt(menu->groups, 39 - i);
    show_spells(menu);
    menu->page = 1;
    set_state(menu, 1);
}

void enter_difficulties(SpellPracticeMenu* menu, int selection) {
    menu->selection = selection;
    while (menu->spells[menu->selection] < 0)
        menu->selection = (menu->selection + 1) % 5;
    select_row(menu);
    menu->page = 2;
    set_state(menu, 1);
}

void change_player(SpellPracticeMenu* menu, uint32_t movement) {
    if (movement & ACTION_LEFT) {
        sound_play_se(10);
        anm_interrupt(menu->character, menu->player + 37);
        menu->player = (menu->player + 3) % 4;
        anm_interrupt(menu->character, menu->player + 17);
    }
    if (movement & ACTION_RIGHT) {
        sound_play_se(10);
        anm_interrupt(menu->character, menu->player + 27);
        menu->player = (menu->player + 1) % 4;
        anm_interrupt(menu->character, menu->player + 7);
    }
    if ((movement & (ACTION_LEFT | ACTION_RIGHT)) && menu->page != 0) {
        show_spells(menu);
        if (menu->page == 2) select_row(menu);
    }
}
} // namespace

SpellPracticeMenu* spell_practice_create(int player, bool returning_from_game) {
    auto* menu = new SpellPracticeMenu;
    menu->resource = anm_get(0x10, "title.anm");
    menu->player = player;
    create_vm(menu, &menu->character, 171);
    if (returning_from_game) {
        menu->stage = static_cast<int>(app_context()->start_stage) - 1;
        const int local_spell = app_context()->selected_spell - kFirstSpell[menu->stage];
        menu->group = menu->stage == 6 ? local_spell : local_spell / 4;
        create_vm(menu, &menu->stages, 118);
        anm_interrupt(menu->stages, 3);
        anm_interrupt(menu->stages, menu->stage + 7);
        anm_interrupt(menu->stages, 6);
        anm_interrupt(menu->character, 3);
        anm_set_label(menu->character, player + 7);
        enter_groups(menu);
        anm_interrupt(menu->groups, 3);
        anm_interrupt(menu->groups, menu->group + 7);
        anm_interrupt(menu->groups, 6);
        leave_vm(menu->heading);
        enter_difficulties(menu, menu->stage == 6 ? 0 : local_spell % 4);
    } else enter_stages(menu);
    return menu;
}

bool spell_practice_update(SpellPracticeMenu* menu) {
    const FrameInput input = input_frame();
    const uint32_t movement = input.pressed | input.repeat;
    switch (menu->state) {
    case 1:
        if (menu->timer > 10) {
            set_state(menu, 2);
            if (menu->page != 2) {
                AnmVm* root = menu->page == 0 ? menu->stages : menu->groups;
                anm_interrupt(root, 3);
                anm_set_label(root, (menu->page == 0 ? menu->stage : menu->group) + 7);
            }
            if (menu->page == 0) {
                anm_interrupt(menu->character, 3);
                anm_set_label(menu->character, menu->player + 7);
            }
        }
        break;
    case 2: {
        int& selection = menu->page == 0 ? menu->stage : menu->page == 1 ? menu->group : menu->selection;
        const int count = menu->page == 0 ? 7 : menu->page == 1 ? kGroupCounts[menu->stage] : 5;
        const int previous = selection;
        if (movement & ACTION_UP) {
            do { selection = (selection + count - 1) % count; }
            while (menu->page == 2 && menu->spells[selection] < 0);
        }
        if (movement & ACTION_DOWN) {
            do { selection = (selection + 1) % count; }
            while (menu->page == 2 && menu->spells[selection] < 0);
        }
        if (previous != selection) {
            sound_play_se(10);
            if (menu->page == 2) select_row(menu);
            else {
                AnmVm* root = menu->page == 0 ? menu->stages : menu->groups;
                anm_interrupt(root, 3);
                anm_set_label(root, selection + 7);
                if (menu->page == 1) show_spells(menu);
            }
        }
        change_player(menu, movement);
        if (input.pressed & (ACTION_BOMB | ACTION_MENU_CANCEL)) {
            sound_play_se(9);
            set_state(menu, 4);
        } else if (input.pressed & (ACTION_SHOT | ACTION_MENU_OK)) {
            if (menu->page == 1) {
                bool available = false;
                for (int spell : menu->spells) available |= spell >= 0;
                if (!available) {
                    sound_play_se(16);
                    break;
                }
            }
            anm_set_label(menu->page == 0 ? menu->stages :
                          menu->page == 1 ? menu->groups : menu->rows[menu->selection], 6);
            sound_play_se(7);
            set_state(menu, 3);
            if (menu->page == 2) {
                bgm_fade(0.05f); // 46cf26 loads 4b90c4.
                sound_play_se(50);
            }
        }
        break;
    }
    case 3:
        if (menu->page == 0 && menu->timer > 19) {
            leave_vm(menu->heading);
            menu->group = 0;
            enter_groups(menu);
        } else if (menu->page == 1 && menu->timer >= 14) {
            leave_vm(menu->heading);
            enter_difficulties(menu, 0);
        } else if (menu->page == 2) {
            if (menu->timer == 10) app_begin_stage_loading();
            if (menu->timer > 39) {
                AppContext* app = app_context();
                app->selected_player = menu->player;
                app->selected_spell = menu->spells[menu->selection];
                app->difficulty = menu->stage == 6 ? 4 : menu->selection;
                app->start_stage = app->current_stage = menu->stage + 1;
                app->initial_card_count = card_profile_deck_count(3);
                for (int i = 0; i < app->initial_card_count; ++i)
                    app->initial_cards[i] = card_profile_deck_id(3, i);
                replay_close();
                session().mode = GameMode::SpellPractice;
                app->next_state = 7;
                menu->state = 5;
            }
        }
        break;
    case 4:
        if (menu->timer > 5) {
            if (menu->page == 0) {
                leave_vm(menu->heading);
                leave_vm(menu->stages);
                leave_vm(menu->character);
                return true;
            }
            if (menu->page == 1) {
                for (AnmVm*& row : menu->rows) leave_vm(row);
                for (AnmVm*& rank : menu->ranks) anm_vm_free(rank);
                leave_vm(menu->heading);
                leave_vm(menu->groups);
                leave_vm(menu->stages);
                enter_stages(menu);
            } else {
                leave_vm(menu->groups);
                enter_groups(menu);
            }
        }
        break;
    }
    ++menu->timer;
    return false;
}

void spell_practice_render(const SpellPracticeMenu* menu) {
    if (menu->page == 0 || menu->state > 3 || (menu->page == 2 && menu->state == 3)) return;
    text_set_group(0);
    text_set_life(0);
    text_set_size(1, 1);
    text_set_alignment(1, 1);
    text_set_style(2);
    for (int row = 0; row < row_count(menu); ++row) {
        const int id = spell_at(menu, row);
        const uint8_t* stats = record(menu->player, id);
        const bool selected = menu->page == 2 && menu->selection == row;
        text_set_color(number(stats, 0xc4) != 0 ? (selected ? 0xff90d0ff : 0xff60a0c0) :
                                               (selected ? 0xffb0b0b0 : 0xff404040));
        TextPos pos{660, 382.0f + row * 88.0f, 0};
        if (!encountered(id) && number(record(4, id), 0xcc) == 0) {
            text_draw_fmt(text_manager(), &pos, "SCORE        00  ----/----");
        } else {
            text_draw_fmt(text_manager(), &pos, "SCORE %8d0  %4d/%4d",
                          number(stats, 0xd8), number(stats, 0xc4), number(stats, 0xcc));
            pos.y += 20;
            text_set_color(number(stats, 0xc0) != 0 ? 0xff206060 : 0xff404040);
            text_draw_fmt(text_manager(), &pos, "GAME MODE        %4d/%4d",
                          number(stats, 0xc0), number(stats, 0xc8));
        }
    }
    text_set_style(0);
    text_set_color(0xffffffff);
}

int spell_practice_player(const SpellPracticeMenu* menu) { return menu->player; }

void spell_practice_destroy(SpellPracticeMenu* menu) {
    anm_vm_free(menu->character);
    anm_vm_free(menu->stages);
    anm_vm_free(menu->groups);
    anm_vm_free(menu->heading);
    for (AnmVm* vm : menu->rows) anm_vm_free(vm);
    for (AnmVm* vm : menu->ranks) anm_vm_free(vm);
    delete menu;
}
