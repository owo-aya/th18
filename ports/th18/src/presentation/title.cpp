#include "presentation/title.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <windows.h>

#include "core/app.h"
#include "core/input.h"
#include "core/thread.h"
#include "core/game_session.h"
#include "replay/replay.h"
#include "replay/replay_menu.h"
#include "gameplay/score_data.h"
#include "player/player.h"
#include "cards/card_profile.h"
#include "cards/card_select.h"
#include "presentation/anm.h"
#include "presentation/text.h"
#include "presentation/sound.h"
#include "presentation/trophy.h"
#include "presentation/music_room.h"
#include "presentation/practice_menu.h"
#include "presentation/achievement_menu.h"
#include "presentation/ending.h"

struct TitleScene {
    AnmVm* signature;
    Thread* logic_thread;
    Thread* render_thread;
    int frames;
    bool ready;
};

struct TitleMenu {
    TitleScene* scene;
    AnmResource* anm;
    AnmResource* anm_v;
    AnmVm* title;
    AnmVm* logo;
    AnmVm* version;
    AnmVm* ambient;
    AnmVm* rank;
    AnmVm* rank_heading;
    AnmVm* player;
    AnmVm* player_heading;
    CardSelect* cards;
    ReplayMenu* replay;
    MusicRoom* music_room;
    SpellPracticeMenu* spell_practice;
    AchievementMenu* achievements;
    AnmVm* data_heading;
    AnmVm* data_choices;
    int data_selection;
    AnmVm* practice_heading;
    int practice_selection;
    AnmVm* result_heading;
    AnmVm* result_player;
    AnmVm* result_rank;
    int ranking_index;
    int name_length;
    int character;
    char name[9];
    Thread* logic_thread;
    Thread* render_thread;
    int state;
    int substate;
    int timer;
    int selection;
    int rank_selection;
    int normal_rank_selection;
    bool extra_unlocked[4];
    int player_selection;
    int player_focus;
    int move_timer;
    int confirm_timer;
    int music_timer = -1;
    bool first_entry;
};

namespace {

AnmVm* g_finishing_ambient = nullptr;
int g_practice_selection = 0;

constexpr const char* kMenuItems[] = {
    "Game Start", "Extra Start", "Practice", "Spell Practice", "Replay",
    "Player Data", "Music Room", "Option", "Manual", "Quit",
};
constexpr char kNameCharacters[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-=.,!?@:;[]()_/{}|~^#$%&*   ";

// First float of each 8-byte entry at 004b6208. 0041b1c0 multiplies it by
// 0.5 in 640x480 coordinates; conversion to this scene's 1280x960 cancels it.
constexpr float kMenuMoveOffsets[] = {2.0f, -1.0f, 0.0f, 3.0f, -3.0f, 2.0f, -2.0f, 0.0f, 2.0f};

AnmVm* create_vm(AnmResource* resource, int script)
{
    AnmVm* vm = anm_vm_alloc(resource);
    anm_vm_set_script(vm, resource, script);
    anm_queue_add(vm);
    return vm;
}

void update_vm(AnmVm* vm)
{
    if (vm != nullptr)
        anm_vm_update(vm);
}

void set_substate(TitleMenu* menu, int substate)
{
    menu->substate = substate;
    menu->timer = 0;
}

AnmVm* child_vm(AnmVm* parent, int script)
{
    for (int queue = 0; queue < 2; ++queue)
        for (AnmVm* vm = static_cast<AnmVm*>(queue == 0 ?
                 anm_manager()->queue_head : anm_manager()->queue_head2);
             vm != nullptr; vm = static_cast<AnmVm*>(vm->node_next))
            if (vm->parent == parent && vm->script->id == script)
                return vm;
    return nullptr;
}

void set_child_label(AnmVm* parent, int script, int label)
{
    AnmVm* child = child_vm(parent, script);
    if (child != nullptr)
        anm_set_label(child, label);
}

void show_player_clear_stamps(TitleMenu* menu, bool visible)
{
    for (int player = 0; player < 4; ++player) {
        AnmVm* stamp = child_vm(menu->player, player + 0x17);
        if (visible && card_profile_cleared(player, menu->rank_selection))
            stamp->flags2 |= 2u;
        else
            stamp->flags2 &= ~2u;
    }
}

// 00453380 requests the title menu as soon as its resources are ready. The
// menu's own task waits for the signature's 180 displayed frames (00464b80).
ThreadStatus title_logic_step(void* ctx)
{
    TitleScene* scene = static_cast<TitleScene*>(ctx);
    if (scene->ready) {
        scene->ready = false;
        app_context()->next_state = 4;
    }
    update_vm(scene->signature);
    return THREAD_CONTINUE;
}

ThreadStatus title_render_step(void* ctx)
{
    TitleScene* scene = static_cast<TitleScene*>(ctx);
    ++scene->frames;
    return THREAD_CONTINUE;
}

void enter_main_menu(TitleMenu* menu)
{
    // 00465aa0 / 004647c0: each character unlocks Extra with a no-continue
    // clear on any of the four ordinary difficulties.
    for (int player = 0; player < 4; ++player) {
        menu->extra_unlocked[player] = false;
        for (int rank = 0; rank < 4; ++rank)
            menu->extra_unlocked[player] |= card_profile_cleared(player, rank);
    }
    // 00465aa0: only these two roots are created. Their op500 instructions
    // create the background, character, logo and copyright children.
    if (menu->first_entry) {
        menu->title = create_vm(menu->anm, 7);
        menu->logo = create_vm(menu->anm, 0x22);
        menu->first_entry = false;
        set_substate(menu, 1);
    } else {
        anm_interrupt(menu->title, 2);
        anm_vm_free(menu->logo);
        menu->logo = create_vm(menu->anm, 0x22);
        anm_interrupt(menu->logo, 2);
        anm_vm_free(menu->version);
        menu->version = create_vm(menu->anm_v, 0);
        set_substate(menu, 1);
        menu->timer = 0x78;
    }
    menu->state = 1;
}

void enter_rank_menu(TitleMenu* menu)
{
    // 00466280 creates ASCII 19 once at TitleInf+0x650. Its op502 instances
    // continue through character/card selection and a return to the title.
    if (menu->ambient == nullptr)
        menu->ambient = create_vm(anm_get(2, "ascii.anm"), 19);
    // 00466280: script 59 contains the four ordinary ranks; script 60
    // contains Extra alone, whose local selection index is zero.
    anm_interrupt(menu->title, 3);
    anm_vm_free(menu->rank);
    anm_vm_free(menu->rank_heading);
    const bool extra = menu->rank_selection == 4;
    menu->rank = create_vm(menu->anm, extra ? 0x3c : 0x3b);
    anm_interrupt(menu->rank, 3);
    anm_set_label(menu->rank, extra ? 0xd : menu->rank_selection + 0xd);
    for (int rank = extra ? 4 : 0; rank < (extra ? 5 : 4); ++rank) {
        // 00464810 shows the rank stamp after all four characters clear it.
        bool cleared = true;
        for (int player = 0; player < 4; ++player)
            cleared &= card_profile_cleared(player, rank);
        if (!cleared)
            child_vm(menu->rank, rank + 0x1b)->flags2 &= ~2u;
    }
    menu->rank_heading = create_vm(menu->anm, 0x25);
    menu->state = 5;
    set_substate(menu, 1);
}

void enter_player_menu(TitleMenu* menu)
{
    // 00466740: normal-game bank 0; Extra bank 1. CardSelect mode 2 starts
    // collapsed, at (640,70), with the character row focused.
    if (menu->rank_selection == 4 && !menu->extra_unlocked[menu->player_selection]) {
        menu->player_selection = 0;
        while (!menu->extra_unlocked[menu->player_selection])
            ++menu->player_selection;
    }
    anm_vm_free(menu->player);
    anm_vm_free(menu->player_heading);
    menu->player = create_vm(menu->anm, menu->rank_selection == 4 ? 0x16 : 0x15);
    menu->player_heading = create_vm(menu->anm, 0x26);
    anm_interrupt(menu->player, 3);
    anm_set_label(menu->player, menu->player_selection + 7);
    show_player_clear_stamps(menu, true);
    menu->cards = card_select_create(menu->selection == 2 ? 2 : menu->rank_selection == 4 ? 1 : 0);
    menu->player_focus = 0;
    menu->state = 6;
    set_substate(menu, 1);
}

void enter_stage_practice(TitleMenu* menu)
{
    anm_vm_free(menu->practice_heading);
    menu->practice_heading = create_vm(menu->anm, 45);
    menu->practice_selection = g_practice_selection;
    card_select_stage_select(menu->cards);
    menu->state = 8;
    set_substate(menu, 1);
}

void enter_spell_practice(TitleMenu* menu, bool returning_from_game)
{
    anm_interrupt(menu->title, 3);
    if (menu->ambient == nullptr)
        menu->ambient = create_vm(anm_get(2, "ascii.anm"), 19);
    menu->spell_practice = spell_practice_create(menu->player_selection, returning_from_game);
    menu->state = 18;
}

// 4686e0: Player Data is the parent selector of the achievement page.
void enter_player_data(TitleMenu* menu)
{
    if(menu->ambient==nullptr)menu->ambient=create_vm(anm_get(2,"ascii.anm"),19);
    anm_vm_free(menu->data_heading);anm_vm_free(menu->data_choices);
    menu->data_heading=create_vm(menu->anm,0x2f);
    menu->data_choices=create_vm(menu->anm,0x6d);
    anm_interrupt(menu->data_choices,3);
    anm_set_label(menu->data_choices,menu->data_selection+7);
    menu->state=10;set_substate(menu,1);
}

void update_player_data(TitleMenu* menu)
{
    if(menu->substate==1) {
        if(menu->timer>6)set_substate(menu,2);
    } else if(menu->substate==2) {
        const FrameInput input=input_frame();const uint32_t movement=input.pressed|input.repeat;
        const int previous=menu->data_selection;
        if(movement&(ACTION_UP|ACTION_LEFT))menu->data_selection=(menu->data_selection+3)%4;
        if(movement&(ACTION_DOWN|ACTION_RIGHT))menu->data_selection=(menu->data_selection+1)%4;
        if(previous!=menu->data_selection) {
            sound_play_se(10);anm_interrupt(menu->data_choices,3);
            anm_set_label(menu->data_choices,menu->data_selection+7);
        }
        if(input.pressed&(ACTION_BOMB|ACTION_MENU_CANCEL)) {
            sound_play_se(9);anm_set_label(menu->data_choices,1);set_substate(menu,4);
        } else if(menu->data_selection==3 && (input.pressed&(ACTION_SHOT|ACTION_MENU_OK))) {
            sound_play_se(7);anm_interrupt(menu->data_choices,6);
            set_child_label(menu->data_choices,app_context()->difficulty<4?0x34:0x35,2);
            set_substate(menu,3);
        }
    } else if(menu->substate==3 && menu->timer>13) {
        anm_set_label(menu->data_heading,1);
        menu->achievements=achievement_menu_create(false);menu->state=23;
    } else if(menu->substate==4 && menu->timer>5) {
        anm_set_label(menu->data_heading,1);
        anm_set_label(menu->ambient,1);g_finishing_ambient=menu->ambient;menu->ambient=nullptr;
        enter_main_menu(menu);
    }
}

// 469c80: title state 15, reached after the ending or Extra clear.
void enter_clear_result(TitleMenu* menu)
{
    bgm_play(17);
    menu->title = create_vm(menu->anm, 7);
    anm_interrupt(menu->title, 3);
    menu->first_entry = false;
    menu->ambient = create_vm(anm_get(2, "ascii.anm"), 19);
    menu->result_heading = create_vm(menu->anm, 0x2b);
    menu->result_player = create_vm(menu->anm, 0x3d + session().player);
    menu->result_rank = create_vm(menu->anm, 0x41 + static_cast<int>(session().difficulty));
    std::memcpy(menu->name, score_data().state + 0xc, 9);
    menu->name_length = 8;
    while (menu->name_length > 0 && menu->name[menu->name_length - 1] == ' ')
        --menu->name_length;
    menu->character = menu->name_length == 0 ? 0 : 90;
    menu->ranking_index = score_insert_ranking(session().player,
        static_cast<int>(session().difficulty), g_status.score, 8, g_status.continues,
        std::time(nullptr), static_cast<uint32_t>(replay_info().slow_rate), "        ");
    menu->state = 15;
    set_substate(menu, 1);
}

void finish_clear_result(TitleMenu* menu)
{
    score_save();
    replay_close();
    app_context()->run_cleared = false;
    anm_set_label(menu->ambient, 1);
    g_finishing_ambient = menu->ambient;
    menu->ambient = nullptr;
    bgm_play(0);
    enter_main_menu(menu);
}

void update_clear_result(TitleMenu* menu)
{
    if (menu->substate == 1) {
        if (menu->timer > 6) set_substate(menu, 2);
        return;
    }
    if (menu->substate == 3) {
        if (menu->timer > 5) {
            anm_set_label(menu->result_heading, 1);
            anm_set_label(menu->result_player, 1);
            anm_set_label(menu->result_rank, 1);
            score_save();
            if (g_status.continues == 0) {
                menu->replay = replay_menu_create(ReplayMenuMode::ClearSave);
                menu->state = 16;
            } else finish_clear_result(menu);
        }
        return;
    }
    const FrameInput input = input_frame();
    if (menu->ranking_index < 0) {
        if (input.pressed & (ACTION_SHOT | ACTION_MENU_OK | ACTION_BOMB | ACTION_MENU_CANCEL)) {
            sound_play_se(7);
            set_substate(menu, 3);
        }
        return;
    }
    const uint32_t movement = input.pressed | input.repeat;
    const int previous = menu->character;
    if (movement & ACTION_UP) menu->character = (menu->character + 78) % 91;
    if (movement & ACTION_DOWN) menu->character = (menu->character + 13) % 91;
    if (movement & ACTION_LEFT) menu->character += menu->character % 13 == 0 ? 12 : -1;
    if (movement & ACTION_RIGHT) menu->character += menu->character % 13 == 12 ? -12 : 1;
    if (menu->character != previous) sound_play_se(10);
    if (input.pressed & (ACTION_SHOT | ACTION_MENU_OK)) {
        if (menu->character == 90) {
            uint8_t* record = score_data().character[session().player] + 0x10 +
                static_cast<int>(session().difficulty) * 0x140 + menu->ranking_index * 0x20;
            std::memcpy(record + 6, menu->name, 9);
            std::memcpy(score_data().state + 0xc, menu->name, 9);
            set_substate(menu, 3);
        } else if (menu->character == 89) {
            if (menu->name_length == 0) return;
            menu->name[--menu->name_length] = ' ';
        } else {
            const char letter = menu->character == 88 ? ' ' : kNameCharacters[menu->character];
            if (menu->name_length == 8) menu->name[7] = letter;
            else {
                menu->name[menu->name_length++] = letter;
                if (menu->name_length == 8) menu->character = 90;
            }
        }
        sound_play_se(7);
    }
    if ((input.pressed & (ACTION_BOMB | ACTION_MENU_CANCEL)) && menu->name_length != 0) {
        menu->name[--menu->name_length] = ' ';
        sound_play_se(9);
    }
}

void update_replay_page(TitleMenu* menu)
{
    const ReplayMenuResult result = replay_menu_update(menu->replay);
    if (result.action == ReplayMenuAction::Back) {
        replay_menu_destroy(menu->replay);
        menu->replay = nullptr;
        if (menu->state == 16) finish_clear_result(menu);
        else enter_main_menu(menu);
    } else if (result.action == ReplayMenuAction::Start) {
        replay_load(result.filename.c_str(), false);
        auto* app = app_context();
        app->start_stage = app->current_stage = result.stage;
        app->next_state = 0xd;
    }
}

// 46a260: the title uses the original ranking rows and 13-by-7 name grid.
void render_clear_result(const TitleMenu* menu)
{
    if (menu->substate != 2) return;
    text_set_group(0);
    text_set_life(0);
    text_set_style(0);
    text_set_alignment(1, 1);
    text_set_size(1, 1);
    for (int rank = 0; rank < 10; ++rank) {
        const uint8_t* record = score_data().character[session().player] + 0x10 +
            static_cast<int>(session().difficulty) * 0x140 + rank * 0x20;
        const uint32_t score = *reinterpret_cast<const uint32_t*>(record);
        const int64_t timestamp = *reinterpret_cast<const int64_t*>(record + 0x10);
        TextPos pos{112.0f, 320.0f + 36.0f * rank, 0};
        text_set_color(menu->ranking_index < 0 ? (0xffffffffu - rank * 16u) :
            rank == menu->ranking_index ? 0xffffffffu : 0xff808040u);
        if (timestamp == 0) {
            text_draw_fmt(text_manager(), &pos,
                "%2d  %.8s  %9u%d  ----/--/-- --:--  Stage -  ---%%", rank + 1,
                reinterpret_cast<const char*>(record + 6), score, record[5]);
        } else {
            const std::time_t time = static_cast<std::time_t>(timestamp);
            const std::tm* utc = std::gmtime(&time);
            char stage[16];
            if (record[4] == 8) std::strcpy(stage, "All    ");
            else if (record[4] == 7) std::strcpy(stage, "Extra  ");
            else std::sprintf(stage, "Stage %d", record[4]);
            text_draw_fmt(text_manager(), &pos,
                "%2d  %.8s  %9u%d  %.4d/%.2d/%.2d %.2d:%.2d  %s  %2.1f%%", rank + 1,
                reinterpret_cast<const char*>(record + 6), score, record[5],
                utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday, utc->tm_hour, utc->tm_min,
                stage, static_cast<float>(*reinterpret_cast<const uint32_t*>(record + 0x18)));
        }
    }
    if (menu->ranking_index >= 0) {
        TextPos pos{184.0f, 320.0f + menu->ranking_index * 36.0f, 0};
        text_set_color(0xffffffffu);
        text_queue_add(text_manager(), &pos, menu->name);
        pos.x += (menu->name_length - (menu->name_length == 8 ? 1 : 0)) * 18.0f;
        text_set_color(0xffffff00u);
        text_queue_add(text_manager(), &pos, "_");
        for (int c = 0; c < 91; ++c) {
            pos = {424.0f + (c % 13) * 36.0f, 720.0f + (c / 13) * 32.0f, 0};
            const char glyph[2] = {c < 88 ? kNameCharacters[c] :
                static_cast<char>(c == 88 ? 0x81 : c == 89 ? 0x7f : 0x80), 0};
            text_set_color(c == menu->character ? 0xffffff00u : 0xff808080u);
            text_queue_add(text_manager(), &pos, glyph);
        }
    }
    text_set_color(0xffffffffu);
}

void update_main_menu(TitleMenu* menu)
{
    switch (menu->substate) {
    case 1:
        if (menu->timer == 0x78)
            menu->version = create_vm(menu->anm_v, 0);
        if (menu->timer > 0x82)
            set_substate(menu, 2);
        break;
    case 2: {
        trophy_allow_deferred();
        const FrameInput input = input_frame();
        const uint32_t movement = input.pressed | input.repeat;
        const int previous = menu->selection;
        const bool extra_unlocked = menu->extra_unlocked[0] || menu->extra_unlocked[1] ||
            menu->extra_unlocked[2] || menu->extra_unlocked[3];
        if ((movement & ACTION_UP) != 0)
            do { menu->selection = (menu->selection + 9) % 10; }
            while (menu->selection == 1 && !extra_unlocked);
        if ((movement & ACTION_DOWN) != 0)
            do { menu->selection = (menu->selection + 1) % 10; }
            while (menu->selection == 1 && !extra_unlocked);
        if (menu->selection != previous) {
            menu->move_timer = 8;
            sound_play_se(10);
        }
        bool confirm = (input.pressed & (ACTION_MENU_OK | ACTION_SHOT)) != 0;
        if ((input.pressed & (ACTION_MENU_CANCEL | ACTION_BOMB)) != 0) {
            if (menu->selection == 9)
                confirm = true;
            else {
                menu->selection = 9;
                sound_play_se(9);
            }
        }
        if (confirm && (menu->selection <= 5 ||
                        menu->selection == 6 || menu->selection == 9)) {
            sound_play_se(menu->selection == 9 ? 9 : 7);
            menu->confirm_timer = 30;
            if (menu->selection != 9) {
                anm_set_label(menu->logo, 1);
                anm_set_label(menu->version, 1);
            }
            set_substate(menu, 4);
        }
        break;
    }
    case 4:
        if (menu->timer > 0x13) {
            if (menu->selection <= 2) {
                session().mode = menu->selection == 2 ? GameMode::Practice : GameMode::Full;
                app_context()->selected_spell = -1;
                app_context()->practice_lives = 0;
                menu->rank_selection = menu->selection == 1 ? 4 : menu->normal_rank_selection;
                enter_rank_menu(menu);
            } else if (menu->selection == 3) {
                session().mode = GameMode::SpellPractice;
                enter_spell_practice(menu, false);
            } else if (menu->selection == 4) {
                anm_interrupt(menu->title, 3);
                menu->replay = replay_menu_create(ReplayMenuMode::Browse);
                menu->state = 12;
            }
            else if(menu->selection==5) {
                anm_interrupt(menu->title,3);menu->data_selection=0;
                enter_player_data(menu);
            }
            else if (menu->selection == 6) {
                anm_interrupt(menu->title, 3);
                menu->music_room = music_room_create(menu->ambient);
                menu->ambient = nullptr;
                menu->state = 14;
            }
            else
                app_context()->next_state = 0x11;
        }
        break;
    }
}

void update_rank_menu(TitleMenu* menu)
{
    switch (menu->substate) {
    case 0:
        enter_rank_menu(menu);
        break;
    case 1:
        if (menu->timer > 6)
            set_substate(menu, 2);
        break;
    case 2: {
        const FrameInput input = input_frame();
        const uint32_t movement = input.pressed | input.repeat;
        const int previous = menu->rank_selection;
        if (menu->rank_selection < 4 && (movement & (ACTION_UP | ACTION_LEFT)) != 0)
            menu->rank_selection = (menu->rank_selection + 3) % 4;
        if (menu->rank_selection < 4 && (movement & (ACTION_DOWN | ACTION_RIGHT)) != 0)
            menu->rank_selection = (menu->rank_selection + 1) % 4;
        if (menu->rank_selection != previous) {
            sound_play_se(10);
            anm_interrupt(menu->rank, 3);
            anm_set_label(menu->rank, menu->rank_selection + 7);
        }
        if ((input.pressed & (ACTION_MENU_CANCEL | ACTION_BOMB)) != 0) {
            sound_play_se(9);
            anm_set_label(menu->rank, 1);
            set_substate(menu, 4);
        } else if ((input.pressed & (ACTION_MENU_OK | ACTION_SHOT)) != 0) {
            sound_play_se(7);
            anm_set_label(menu->rank, 6);
            set_child_label(menu->rank, menu->rank_selection + 0x31, 2);
            app_context()->difficulty = menu->rank_selection;
            if (menu->rank_selection < 4)
                menu->normal_rank_selection = menu->rank_selection;
            set_substate(menu, 3);
        }
        break;
    }
    case 3:
        if (menu->timer > 0xd) {
            anm_set_label(menu->rank_heading, 1);
            menu->state = 6;
            set_substate(menu, 0);
        }
        break;
    case 4:
        if (menu->timer > 5) {
            anm_set_label(menu->rank_heading, 1);
            if (menu->rank_selection == 4 || menu->selection == 2) {
                anm_set_label(menu->ambient, 1);
                g_finishing_ambient = menu->ambient;
                menu->ambient = nullptr;
            } else {
                menu->normal_rank_selection = menu->rank_selection;
            }
            enter_main_menu(menu);
        }
        break;
    }
}

void update_player_menu(TitleMenu* menu)
{
    switch (menu->substate) {
    case 0:
        enter_player_menu(menu);
        break;
    case 1:
        if (menu->timer > 6)
            set_substate(menu, 2);
        break;
    case 2: {
        const FrameInput input = input_frame();
        const uint32_t movement = input.pressed | input.repeat;
        const int previous_focus = menu->player_focus;
        if (menu->player_focus == 0) {
            if ((movement & ACTION_LEFT) != 0) {
                sound_play_se(10);
                anm_interrupt(menu->player, menu->player_selection + 0x19);
                do { menu->player_selection = (menu->player_selection + 3) % 4; }
                while (menu->rank_selection == 4 && !menu->extra_unlocked[menu->player_selection]);
                anm_set_label(menu->player, menu->player_selection + 0xd);
            }
            if ((movement & ACTION_RIGHT) != 0) {
                sound_play_se(10);
                anm_interrupt(menu->player, menu->player_selection + 0x13);
                do { menu->player_selection = (menu->player_selection + 1) % 4; }
                while (menu->rank_selection == 4 && !menu->extra_unlocked[menu->player_selection]);
                anm_set_label(menu->player, menu->player_selection + 7);
            }
            if ((input.pressed & (ACTION_MENU_OK | ACTION_SHOT)) != 0) {
                sound_play_se(7);
                set_child_label(menu->player, menu->player_selection + 9, 6);
                set_child_label(menu->player, menu->player_selection + 0xd, 6);
                set_child_label(menu->player, 5, 6);
                set_child_label(menu->player, 6, 6);
                set_substate(menu, 3);
                return;
            }
            if ((input.pressed & (ACTION_MENU_CANCEL | ACTION_BOMB)) != 0) {
                sound_play_se(9);
                set_substate(menu, 4);
                return;
            }
        } else {
            if ((input.pressed & (ACTION_MENU_OK | ACTION_SHOT)) != 0) {
                sound_play_se(7);
                card_select_open(menu->cards);
                anm_set_label(menu->player_heading, 1);
                set_substate(menu, 5);
                return;
            }
            if ((input.pressed & (ACTION_MENU_CANCEL | ACTION_BOMB)) != 0)
                menu->player_focus = 0;
        }
        if ((movement & ACTION_UP) != 0)
            menu->player_focus = 0;
        if ((movement & ACTION_DOWN) != 0)
            menu->player_focus = 1;
        if (menu->player_focus != previous_focus) {
            sound_play_se(10);
            const bool card_focus = menu->player_focus != 0;
            anm_interrupt(menu->player, card_focus ? 5 : 4);
            card_select_focus(menu->cards, card_focus);
            show_player_clear_stamps(menu, !card_focus);
        }
        break;
    }
    case 3:
        if (menu->timer == 10) {
            if (menu->selection == 2) {
                app_context()->selected_player = menu->player_selection;
                app_context()->difficulty = menu->rank_selection;
                anm_set_label(menu->player_heading, 1);
                enter_stage_practice(menu);
                break;
            }
            app_begin_stage_loading();
        }
        if (menu->timer > 0x27) {
            AppContext* app = app_context();
            app->selected_player = menu->player_selection;
            app->difficulty = menu->rank_selection;
            app->start_stage = app->current_stage = menu->rank_selection == 4 ? 7 : 1;
            const int bank = menu->rank_selection == 4 ? 1 : 0;
            app->initial_card_count = card_profile_deck_count(bank);
            for (int slot = 0; slot < app->initial_card_count; ++slot)
                app->initial_cards[slot] = card_profile_deck_id(bank, slot);
            app->next_state = 7;
            app->selected_spell = -1;
            replay_close();
            session().mode = GameMode::Full;
        }
        break;
    case 4:
        if (menu->timer > 5) {
            anm_set_label(menu->player, 1);
            anm_set_label(menu->player_heading, 1);
            card_select_destroy(menu->cards);
            menu->cards = nullptr;
            menu->state = 5;
            set_substate(menu, 0);
        }
        break;
    case 5:
        if (card_select_closed(menu->cards)) {
            anm_vm_free(menu->player_heading);
            menu->player_heading = create_vm(menu->anm, 0x26);
            set_substate(menu, 2);
        }
        break;
    }
}

void update_stage_practice(TitleMenu* menu)
{
    switch (menu->substate) {
    case 1:
        if (menu->timer > 10) set_substate(menu, 2);
        break;
    case 2: {
        const FrameInput input = input_frame();
        const uint32_t movement = input.pressed | input.repeat;
        const int previous = menu->practice_selection;
        if (movement & ACTION_UP) menu->practice_selection = (menu->practice_selection + 5) % 6;
        if (movement & ACTION_DOWN) menu->practice_selection = (menu->practice_selection + 1) % 6;
        if (menu->practice_selection != previous) sound_play_se(10);
        if (input.pressed & (ACTION_BOMB | ACTION_MENU_CANCEL)) {
            g_practice_selection = menu->practice_selection;
            sound_play_se(9);
            set_substate(menu, 4);
        } else if (input.pressed & (ACTION_SHOT | ACTION_MENU_OK)) {
            const uint8_t* record = score_data().character[menu->player_selection] + 0x12ef0 +
                (menu->rank_selection * 9 + menu->practice_selection + 1) * 8;
            // 467320 tests the reached-stage byte, distinct from the clear byte.
            if (record[5] == 0) {
                sound_play_se(16);
                break;
            }
            g_practice_selection = menu->practice_selection;
            sound_play_se(7);
            sound_play_se(50);
            app_context()->practice_lives = 0;
            uint8_t keys[256];
            GetKeyboardState(keys);
            for (int digit = 1; digit <= 9; ++digit) {
                if (keys['0' + digit] & 0x80) {
                    app_context()->practice_lives = digit;
                    break;
                }
            }
            bgm_fade(0.05f);
            set_substate(menu, 3);
        }
        break;
    }
    case 3:
        if (menu->timer == 10) app_begin_stage_loading();
        if (menu->timer > 39) {
            AppContext* app = app_context();
            app->selected_player = menu->player_selection;
            app->difficulty = menu->rank_selection;
            app->selected_spell = -1;
            app->start_stage = app->current_stage = menu->practice_selection + 1;
            app->initial_card_count = card_profile_deck_count(2);
            for (int slot = 0; slot < app->initial_card_count; ++slot)
                app->initial_cards[slot] = card_profile_deck_id(2, slot);
            replay_close();
            session().mode = GameMode::Practice;
            app->next_state = 7;
        }
        break;
    case 4:
        if (menu->timer > 5) {
            anm_set_label(menu->practice_heading, 1);
            card_select_destroy(menu->cards);
            menu->cards = nullptr;
            enter_player_menu(menu);
        }
        break;
    }
}

void render_stage_practice(const TitleMenu* menu)
{
    if ((menu->substate != 2 || menu->timer <= 9) && menu->substate != 3) return;
    text_set_group(0);
    text_set_life(0);
    text_set_alignment(1, 1);
    text_set_size(1, 1);
    text_set_style(7);
    for (int stage = 1; stage <= 6; ++stage) {
        const uint8_t* record = score_data().character[menu->player_selection] + 0x12ef0 +
            (menu->rank_selection * 9 + stage) * 8;
        uint32_t color = 0xff808080;
        if (menu->practice_selection == stage - 1)
            color = record[5] == 0 ? 0xffdfdfdf :
                menu->substate == 3 && menu->timer % 4 > 1 ? 0xff000000 : 0xffffff00;
        text_set_color(color);
        TextPos pos{80, 340.0f + (stage - 1) * 68.0f, 0};
        text_draw_fmt(text_manager(), &pos, "Stage %d", stage);
        pos.x += 288;
        if (record[5] == 0) text_draw_fmt(text_manager(), &pos, "---------");
        else text_draw_fmt(text_manager(), &pos, "%.7u0", *reinterpret_cast<const uint32_t*>(record));
    }
    text_set_style(0);
    text_set_color(0xffffffff);
}

ThreadStatus title_menu_logic_step(void* ctx)
{
    TitleMenu* menu = static_cast<TitleMenu*>(ctx);
    // 00464ed0, +5c48/+5c4c: a newly entered ordinary title requests track 0
    // after 30 menu ticks. Returning from Rank/Player keeps the same request.
    if (menu->music_timer >= 0 && ++menu->music_timer >= 30) {
        bgm_play(0);
        menu->music_timer = -1;
    }
    update_vm(menu->title);
    update_vm(menu->logo);
    update_vm(menu->version);
    update_vm(menu->ambient);
    update_vm(menu->rank);
    update_vm(menu->rank_heading);
    update_vm(menu->player);
    update_vm(menu->player_heading);
    update_vm(menu->result_heading);
    update_vm(menu->result_player);
    update_vm(menu->result_rank);
    update_vm(menu->practice_heading);
    update_vm(menu->data_heading);
    update_vm(menu->data_choices);
    // 464ed0 freezes menu input until the overlaid ending sets flags+1c bit8.
    if(ending_active()) {
        if(ending_finished()) {
            ending_destroy();bgm_play(0);score_data().state[0x26]=1;
        }
        return THREAD_CONTINUE;
    }
    if (menu->state == 0) {
        if (menu->scene->frames < 0xb4)
            return THREAD_CONTINUE;
        text_loading_stop();
        anm_vm_free(menu->scene->signature);
        menu->scene->signature = nullptr;
        if(app_context()->return_to_achievements) {
            app_context()->return_to_achievements=false;
            menu->selection=5;menu->data_selection=2;
            menu->title=create_vm(menu->anm,7);menu->first_entry=false;
            anm_interrupt(menu->title,3);
            menu->ambient=create_vm(anm_get(2,"ascii.anm"),19);
            menu->achievements=achievement_menu_create(true);menu->state=23;
            bgm_play(0);score_data().state[0x26]=1;menu->music_timer=-1;
        } else if (app_context()->run_cleared)
            enter_clear_result(menu);
        else if (app_context()->prev_state == 7 && session().mode == GameMode::Practice) {
            menu->selection = 2;
            menu->rank_selection = menu->normal_rank_selection = app_context()->difficulty;
            menu->player_selection = app_context()->selected_player;
            // 464ed0 return reason 4 skips the already confirmed rank/player pages.
            menu->title = create_vm(menu->anm, 7);
            menu->first_entry = false;
            enter_rank_menu(menu);
            anm_interrupt(menu->rank, menu->rank_selection + 7);
            anm_interrupt(menu->rank, 6);
            anm_set_label(menu->rank_heading, 1);
            enter_player_menu(menu);
            anm_interrupt(menu->player, menu->player_selection + 7);
            set_child_label(menu->player, menu->player_selection + 9, 6);
            set_child_label(menu->player, menu->player_selection + 13, 6);
            set_child_label(menu->player, 5, 6);
            set_child_label(menu->player, 6, 6);
            anm_set_label(menu->player_heading, 1);
            card_select_update(menu->cards);
            g_practice_selection = static_cast<int>(app_context()->start_stage) - 1;
            enter_stage_practice(menu);
            menu->music_timer = 0;
        } else if (app_context()->prev_state == 7 && session().mode == GameMode::SpellPractice) {
            menu->selection = 3;
            menu->player_selection = app_context()->selected_player;
            // Return reason 5 creates the background root without the main-menu logo.
            menu->title = create_vm(menu->anm, 7);
            menu->first_entry = false;
            enter_spell_practice(menu, true);
            menu->music_timer = 0;
        } else {
            enter_main_menu(menu);
            menu->music_timer = 0;
        }
    } else if (menu->state == 1) {
        update_main_menu(menu);
    } else if (menu->state == 5) {
        update_rank_menu(menu);
    } else if (menu->state == 6) {
        update_player_menu(menu);
    } else if (menu->state == 8) {
        update_stage_practice(menu);
    } else if(menu->state==10) {
        update_player_data(menu);
    } else if(menu->state==23) {
        if(achievement_menu_update(menu->achievements)) {
            achievement_menu_destroy(menu->achievements);menu->achievements=nullptr;
            enter_player_data(menu);
        }
    } else if (menu->state == 18) {
        if (spell_practice_update(menu->spell_practice)) {
            menu->player_selection = spell_practice_player(menu->spell_practice);
            spell_practice_destroy(menu->spell_practice);
            menu->spell_practice = nullptr;
            anm_set_label(menu->ambient, 1);
            g_finishing_ambient = menu->ambient;
            menu->ambient = nullptr;
            enter_main_menu(menu);
        }
    } else if (menu->state == 15) {
        update_clear_result(menu);
    } else if (menu->state == 12 || menu->state == 16) {
        update_replay_page(menu);
    } else if (menu->state == 14 && music_room_update(menu->music_room)) {
        music_room_destroy(menu->music_room);
        menu->music_room = nullptr;
        enter_main_menu(menu);
    }
    if (menu->cards != nullptr)
        card_select_update(menu->cards);
    ++menu->timer;
    // 00464ed0 decrements after the state callback and before B-queue drawing:
    // an arrow change resets to 8, so its first displayed table index is 7.
    if (menu->move_timer > 0)
        --menu->move_timer;
    if (menu->confirm_timer > 0)
        --menu->confirm_timer;
    return THREAD_CONTINUE;
}

ThreadStatus title_menu_render_step(void* ctx)
{
    TitleMenu* menu = static_cast<TitleMenu*>(ctx);
    if(ending_active())return THREAD_CONTINUE;
    if(menu->achievements)achievement_menu_render(menu->achievements);
    if (menu->state == 8) render_stage_practice(menu);
    if (menu->spell_practice != nullptr) spell_practice_render(menu->spell_practice);
    if (menu->cards != nullptr)
        card_select_render(menu->cards);
    if (menu->state == 15)
        render_clear_result(menu);
    if (menu->replay != nullptr)
        replay_menu_render(menu->replay);
    if (menu->state != 1 || menu->substate < 2 || menu->substate > 4)
        return THREAD_CONTINUE;

    TextManager* text = text_manager();
    uint8_t* text_fields = reinterpret_cast<uint8_t*>(text);
    // 00465870 centres each string at (510, 200 + 23*i). The ANM projection is
    // 1280x960. The menu positions use the 640x480 base; the large glyphs in
    // ascii.anm already have twice the displayed dimensions (47px high).
    *reinterpret_cast<int32_t*>(text_fields + 0x19250) = 0;
    *reinterpret_cast<int32_t*>(text_fields + 0x19254) = 1;
    text_set_group(0);
    text_set_life(0);
    text_set_size(1.0f, 1.0f);
    for (int i = 0; i < 10; ++i) {
        TextPos pos{1020.0f, 400.0f + 46.0f * i, 0.0f};
        uint32_t main_color = 0xff606030;
        uint32_t outline_color = 0xffffffff;
        if (i == 1 && !(menu->extra_unlocked[0] || menu->extra_unlocked[1] ||
                       menu->extra_unlocked[2] || menu->extra_unlocked[3])) {
            // 0041b1c0: disabled entries retain the original dim outline.
            main_color = 0xff808080;
            outline_color = 0x40ffffff;
        } else if (i == menu->selection) {
            if (menu->move_timer > 0) {
                const float offset = kMenuMoveOffsets[menu->move_timer];
                pos.x += offset;
                pos.y += offset;
            }
            main_color = 0xff000000;
            outline_color = 0xff20d0ff;
            if (menu->confirm_timer != 0) {
                if (menu->confirm_timer % 4 < 2)
                    outline_color = 0xffffff00;
                else {
                    main_color = 0xffffff00;
                    outline_color = 0xffffffff;
                }
            }
        }
        text_set_style(9);
        text_set_color(outline_color);
        text_queue_add(text, &pos, kMenuItems[i]);
        text_set_style(7);
        text_set_color(main_color);
        text_queue_add(text, &pos, kMenuItems[i]);
    }
    *reinterpret_cast<int32_t*>(text_fields + 0x19250) = 1;
    *reinterpret_cast<int32_t*>(text_fields + 0x19254) = 1;
    text_set_size(1.0f, 1.0f);
    text_set_style(0);
    text_set_color(0xffffffff);
    return THREAD_CONTINUE;
}

} // namespace

TitleScene* title_scene_create()
{
    anm_set_screen(1280.0f, 960.0f);
    TitleScene* scene = new TitleScene{};
    // 00452cb0/004533f0: the loading scene owns sig.anm script 0 only.
    scene->signature = create_vm(anm_get(1, "sig.anm"), 0);
    text_loading_start(480.0f, 392.0f);
    scene->ready = true;
    scene->logic_thread = thread_create(title_logic_step);
    thread_set_ctx(scene->logic_thread, scene);
    thread_set_priority(scene->logic_thread, 4);
    thread_enqueue_logic(scene->logic_thread);
    scene->render_thread = thread_create(title_render_step);
    thread_set_ctx(scene->render_thread, scene);
    thread_set_priority(scene->render_thread, 0x4a);
    thread_enqueue_render(scene->render_thread);
    return scene;
}

void title_scene_destroy(TitleScene* scene)
{
    thread_remove(scene->logic_thread);
    thread_remove(scene->render_thread);
    anm_vm_free(scene->signature);
    delete scene;
}

TitleMenu* title_menu_create()
{
    app_context()->extra_ending_view=-1; // 464b80 resets the Extra viewer global.
    TitleMenu* menu = new TitleMenu{};
    card_profile_init();
    menu->scene = static_cast<TitleScene*>(app_context()->title_obj);
    menu->anm = anm_get(0x10, "title.anm");
    menu->anm_v = anm_get(0x11, "title_v.anm");
    menu->rank_selection = 1;
    menu->normal_rank_selection = 1;
    menu->first_entry = true;
    menu->logic_thread = thread_create(title_menu_logic_step);
    thread_set_ctx(menu->logic_thread, menu);
    thread_set_priority(menu->logic_thread, 8);
    thread_enqueue_logic(menu->logic_thread);
    menu->render_thread = thread_create(title_menu_render_step);
    thread_set_ctx(menu->render_thread, menu);
    thread_set_priority(menu->render_thread, 0x4b);
    thread_enqueue_render(menu->render_thread);
    return menu;
}

void title_menu_destroy(TitleMenu* menu)
{
    if(ending_active())ending_destroy();
    thread_remove(menu->logic_thread);
    thread_remove(menu->render_thread);
    anm_vm_free(menu->title);
    anm_vm_free(menu->logo);
    anm_vm_free(menu->version);
    anm_vm_free(menu->rank);
    anm_vm_free(menu->rank_heading);
    anm_vm_free(menu->player);
    anm_vm_free(menu->player_heading);
    anm_vm_free(menu->result_heading);
    anm_vm_free(menu->result_player);
    anm_vm_free(menu->result_rank);
    anm_vm_free(menu->practice_heading);
    anm_vm_free(menu->data_heading);anm_vm_free(menu->data_choices);
    if(menu->achievements)achievement_menu_destroy(menu->achievements);
    if (menu->spell_practice != nullptr) spell_practice_destroy(menu->spell_practice);
    if (menu->replay != nullptr) replay_menu_destroy(menu->replay);
    if (menu->music_room != nullptr) music_room_destroy(menu->music_room);
    if (menu->ambient != nullptr) {
        // 00464d20 sends label 1; ASCII 19 and its 18 children fade for ten
        // frames under the global ANM task after TitleInf has been destroyed.
        anm_set_label(menu->ambient, 1);
        g_finishing_ambient = menu->ambient;
    }
    if (menu->cards != nullptr)
        card_select_destroy(menu->cards);
    delete menu;
}

void title_finish_ambient_step()
{
    if (g_finishing_ambient != nullptr) {
        anm_vm_update(g_finishing_ambient);
        if (g_finishing_ambient->script_cursor < 0) {
            anm_vm_free(g_finishing_ambient);
            g_finishing_ambient = nullptr;
        }
    }
}
