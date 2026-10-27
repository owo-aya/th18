#include "presentation/ingame_menu.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "cards/card_menu.h"
#include "core/game_session.h"
#include "core/input.h"
#include "core/thread.h"
#include "gameplay/score_data.h"
#include "player/player.h"
#include "presentation/anm.h"
#include "presentation/pause_pages.h"
#include "presentation/sound.h"
#include "presentation/text.h"
#include "replay/replay.h"
#include "replay/replay_menu.h"

namespace {
enum class MenuStep { Closed, Opening, Main, Confirm, ConfirmClosing,
                      SaveOpening, Saving, Ranking, PageOpening, Page, Closing };
constexpr char kCharacters[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-=.,!?@:;[]()_/{}|~^#$%&*   ";
// 4b4190, indexed by the stored result byte (not just a numbered stage).
constexpr const char* kStageResults[] = {
    "test   ", "Stage 1", "Stage 2", "Stage 3", "Stage 4", "Stage 5", "Stage 6",
    "Extra  ", "Clear  ", "ExClear"
};
constexpr int kChoiceChildren[7][5] = {
    {0, 0, 0, 0, 0},
    {0x78, 0x7f, 0x86, 0x8a, 0x8c},
    {0x79, 0x80, 0x8d, 0, 0},
    {0x7a, 0x81, 0, 0, 0},
    {0x7b, 0x82, 0, 0, 0},
    {0x7c, 0x83, 0, 0, 0},
    {0x7d, 0, 0, 0, 0},
};
}

struct IngameMenu {
    AnmResource* front;
    AnmResource* text;
    AnmVm* root = nullptr;
    AnmVm* background = nullptr;
    ReplayMenu* replay = nullptr;
    PauseCards* cards = nullptr;
    PauseHelp* help = nullptr;
    PauseOptions* options = nullptr;
    Thread* page_logic = nullptr;
    Thread* page_render = nullptr;
    bool page_finished = false;
    IngameMenuReason reason = IngameMenuReason::Pause;
    MenuStep step = MenuStep::Closed;
    IngameMenuAction closing_action = IngameMenuAction::None;
    int timer = 0;
    int selection = 0;
    int credits = 0;
    int ranking_index = -1;
    bool enabled[7]{};
    bool confirming_save = false;
    int confirmation = 1;
    bool confirmed = false;
    char name[9]{};
    int name_length = 0;
    int character = 0;
};

namespace {
AnmVm* create_vm(AnmResource* resource, int script, AnmVm** owner_slot) {
    AnmVm* vm = anm_vm_alloc(resource);
    // 416a10: pause/result roots use the second ANM queue. Descendants keep
    // the queue selected by their own 500/501 instructions.
    vm->node_flags = 4;
    anm_vm_set_script(vm, resource, script);
    anm_queue_add(vm);
    anm_manage_vm(vm, owner_slot);
    return vm;
}

void set_step(IngameMenu* menu, MenuStep step) {
    menu->step = step;
    menu->timer = 0;
}

void set_tree_visible(AnmVm* vm, bool visible) {
    // 488e70 / 488eb0 hide drawing while the menu scripts keep advancing.
    if (visible) vm->flags2 |= 2u;
    else vm->flags2 &= ~2u;
    for (int queue = 0; queue < 2; ++queue)
        for (AnmVm* child = static_cast<AnmVm*>(queue == 0 ?
                 anm_manager()->queue_head : anm_manager()->queue_head2);
             child != nullptr; child = static_cast<AnmVm*>(child->node_next))
            if (child->parent == vm) set_tree_visible(child, visible);
}

ThreadStatus update_page(void* context) {
    auto* menu = static_cast<IngameMenu*>(context);
    switch (menu->selection) {
    case 3: menu->page_finished = pause_cards_update(menu->cards); break;
    case 4: menu->page_finished = pause_help_update(menu->help); break;
    case 5: menu->page_finished = pause_options_update(menu->options); break;
    }
    return THREAD_CONTINUE;
}

ThreadStatus render_page(void* context) {
    auto* menu = static_cast<IngameMenu*>(context);
    pause_options_render(menu->options);
    return THREAD_CONTINUE;
}

void destroy_page(IngameMenu* menu) {
    if (menu->page_logic != nullptr) {
        thread_remove(menu->page_logic);
        menu->page_logic = nullptr;
    }
    if (menu->page_render != nullptr) {
        thread_remove(menu->page_render);
        menu->page_render = nullptr;
    }
    if (menu->cards != nullptr) {
        pause_cards_destroy(menu->cards);
        menu->cards = nullptr;
    }
    if (menu->help != nullptr) {
        pause_help_destroy(menu->help);
        menu->help = nullptr;
    }
    if (menu->options != nullptr) {
        pause_options_destroy(menu->options);
        menu->options = nullptr;
    }
}

void open_page(IngameMenu* menu) {
    set_tree_visible(menu->root, false);
    switch (menu->selection) {
    case 3: menu->cards = pause_cards_create(); break;
    case 4: menu->help = pause_help_create(); break;
    case 5: menu->options = pause_options_create(); break;
    }
    menu->page_finished = false;
    menu->page_logic = thread_create(update_page);
    menu->page_logic->flags |= 2;
    thread_set_priority(menu->page_logic, 7);
    thread_set_ctx(menu->page_logic, menu);
    thread_enqueue_logic(menu->page_logic);
    if (menu->selection == 5) {
        menu->page_render = thread_create(render_page);
        menu->page_render->flags |= 2;
        thread_set_priority(menu->page_render, 0x50);
        thread_set_ctx(menu->page_render, menu);
        thread_enqueue_render(menu->page_render);
    }
    set_step(menu, MenuStep::Page);
}

void choice_label(IngameMenu* menu, int choice, int label) {
    for (int script : kChoiceChildren[choice]) {
        if (script == 0) break;
        AnmVm* child = anm_find_child(menu->root, script);
        if (child != nullptr) anm_set_label(child, label);
    }
}

void show_main(IngameMenu* menu) {
    if (menu->root == nullptr) {
        int script = 0x97;
        if (menu->reason == IngameMenuReason::Pause)
            script = replay_mode() == ReplayMode::Playback ? 0x95 : 0x94;
        else if (menu->reason == IngameMenuReason::ReplayEnd) script = 0x96;
        else if (menu->reason == IngameMenuReason::PracticeClear || session().mode != GameMode::Full)
            script = 0x98;
        menu->root = create_vm(menu->front, script, &menu->root);
        anm_interrupt(menu->root, 3);
    }
    set_tree_visible(menu->root, true);
    for (int choice = 0; choice < 7; ++choice)
        if (!menu->enabled[choice]) choice_label(menu, choice, 5);
    anm_set_label(menu->root, menu->selection + 7);
    set_step(menu, MenuStep::Main);
}

void begin_close(IngameMenu* menu, IngameMenuAction action) {
    if (menu->root != nullptr) anm_set_label(menu->root, 1);
    if (menu->background != nullptr) anm_set_label(menu->background, 1);
    menu->closing_action = action;
    set_step(menu, MenuStep::Closing);
}

void begin_confirmation(IngameMenu* menu, bool save) {
    menu->confirming_save = save;
    menu->confirmation = 1;
    set_step(menu, MenuStep::Confirm);
}

void begin_ranking(IngameMenu* menu) {
    std::memcpy(menu->name, score_data().state + 0xc, 9);
    menu->name_length = 8;
    while (menu->name_length > 0 && menu->name[menu->name_length - 1] == ' ')
        --menu->name_length;
    menu->character = menu->name_length == 0 ? 0 : 90;
    set_step(menu, MenuStep::Ranking);
}

void erase_name(IngameMenu* menu) {
    if (menu->name_length != 0) {
        --menu->name_length;
        menu->name[menu->name_length] = ' ';
        sound_play_se(9);
    }
}

void update_ranking_name(IngameMenu* menu, const FrameInput& input) {
    const uint32_t movement = input.pressed | input.repeat;
    const int old = menu->character;
    if (movement & ACTION_UP) menu->character = (menu->character + 91 - 13) % 91;
    if (movement & ACTION_DOWN) menu->character = (menu->character + 13) % 91;
    if (movement & ACTION_LEFT)
        menu->character += menu->character % 13 == 0 ? 12 : -1;
    if (movement & ACTION_RIGHT)
        menu->character += menu->character % 13 == 12 ? -12 : 1;
    if (old != menu->character) sound_play_se(10);
    if ((input.pressed & (ACTION_SHOT | ACTION_MENU_OK)) == 0) {
        if (input.pressed & (ACTION_BOMB | ACTION_MENU_CANCEL)) {
            if (menu->name_length == 0) sound_play_se(9);
            else erase_name(menu);
        }
        return;
    }
    if (menu->character == 89) {
        erase_name(menu);
        return;
    }
    if (menu->character == 90) {
        auto* record = score_data().character[session().player] + 0x10 +
            static_cast<int>(session().difficulty) * 0x140 + menu->ranking_index * 0x20;
        std::memcpy(record + 6, menu->name, 9);
        std::memcpy(score_data().state + 0xc, menu->name, 9);
        score_save();
        sound_play_se(7);
        show_main(menu);
        return;
    }
    const char letter = menu->character == 88 ? ' ' : kCharacters[menu->character];
    if (menu->name_length == 8) menu->name[7] = letter;
    else {
        menu->name[menu->name_length++] = letter;
        if (menu->name_length == 8) menu->character = 90;
    }
    sound_play_se(7);
}

void draw_text(float x, float y, uint32_t color, const char* text) {
    const TextPos pos{x * 2.0f, y * 2.0f, 0};
    text_set_color(color);
    text_queue_add(text_manager(), &pos, text);
}

void render_ranking(const IngameMenu* menu) {
    draw_text(48, 64, 0xffffffffu, "            Score Ranking!!");
    // 458090 calls 457bf0 before the rows; the new record's name is blank
    // until confirmation, so only this editable name occupies its column.
    draw_text(75, 96.0f + menu->ranking_index * 18.0f, 0xffffffffu, menu->name);
    draw_text(75 + 9.0f * (menu->name_length == 8 ? 7 : menu->name_length),
              96.0f + menu->ranking_index * 18.0f, 0xffffff00u, "_");
    for (int cell = 0; cell < 91; ++cell) {
        char letter[2] = {cell < 88 ? kCharacters[cell] :
                         static_cast<char>(cell == 88 ? 0x81 : cell == 89 ? 0x7f : 0x80), 0};
        draw_text(112.0f + (cell % 13) * 18.0f, 320.0f + (cell / 13) * 16.0f,
                  cell == menu->character ? 0xffffff00u : 0xff808080u, letter);
    }
    char line[128];
    for (int rank = 0; rank < 10; ++rank) {
        const uint8_t* record = score_data().character[session().player] + 0x10 +
            static_cast<int>(session().difficulty) * 0x140 + rank * 0x20;
        uint32_t score;
        int64_t timestamp;
        std::memcpy(&score, record, 4);
        std::memcpy(&timestamp, record + 0x10, 8);
        char date[16] = "--/--/--";
        if (timestamp != 0) {
            const std::time_t time = static_cast<std::time_t>(timestamp);
            const std::tm* utc = std::gmtime(&time);
            std::snprintf(date, sizeof(date), "%02d/%02d/%02d", utc->tm_year % 100,
                          utc->tm_mon + 1, utc->tm_mday);
        }
        std::snprintf(line, sizeof(line), "%2d %.8s %9u%d %s %s", rank + 1,
                      reinterpret_cast<const char*>(record + 6), score,
                      static_cast<int>(record[5]), date,
                      timestamp == 0 ? "Stage -" : kStageResults[record[4]]);
        draw_text(48, 96.0f + rank * 18.0f,
                  rank == menu->ranking_index ? 0xffffff00u : 0xff808080u, line);
    }
}
} // namespace

IngameMenu* ingame_menu_create() {
    auto* menu = new IngameMenu;
    // 0043a870 preloads front.anm into slot 5; 0043b800's fronttr name then
    // resolves through that occupied slot. This port loads the actual file.
    menu->front = anm_get(5, "front.anm");
    menu->text = anm_get(0, "text.anm");
    return menu;
}

void ingame_menu_destroy(IngameMenu* menu) {
    destroy_page(menu);
    if (menu->replay != nullptr) replay_menu_destroy(menu->replay);
    if (menu->root != nullptr) anm_vm_free(menu->root);
    if (menu->background != nullptr) anm_vm_free(menu->background);
    delete menu;
}

void ingame_menu_open(IngameMenu* menu, IngameMenuReason reason,
                      int credits, int ranking_index) {
    destroy_page(menu);
    if (menu->replay != nullptr) {
        replay_menu_destroy(menu->replay);
        menu->replay = nullptr;
    }
    if (menu->root != nullptr) {
        anm_vm_free(menu->root);
        menu->root = nullptr;
    }
    if (menu->background != nullptr) anm_vm_free(menu->background);
    menu->background = create_vm(menu->text, 0x3b, &menu->background);
    menu->reason = reason;
    menu->credits = credits;
    menu->ranking_index = ranking_index;
    std::memset(menu->enabled, 0, sizeof(menu->enabled));
    menu->enabled[1] = menu->enabled[6] = true;
    menu->enabled[0] = reason == IngameMenuReason::Pause ||
        (reason == IngameMenuReason::GameOver && session().mode == GameMode::Full && credits > 0);
    menu->enabled[2] = reason != IngameMenuReason::ReplayEnd &&
        replay_mode() == ReplayMode::Record && g_status.continues == 0;
    menu->enabled[3] = menu->enabled[5] = reason != IngameMenuReason::ReplayEnd;
    menu->enabled[4] = reason == IngameMenuReason::Pause ?
        replay_mode() != ReplayMode::Playback :
        reason == IngameMenuReason::GameOver && session().mode == GameMode::Full;
    menu->selection = reason == IngameMenuReason::PracticeClear ? 6 :
        reason == IngameMenuReason::ReplayEnd ? 1 : menu->enabled[0] ? 0 : 1;
    if (reason == IngameMenuReason::Pause || reason == IngameMenuReason::ReplayEnd) {
        menu->root = create_vm(menu->front,
            reason == IngameMenuReason::ReplayEnd ? 0x96 :
            replay_mode() == ReplayMode::Playback ? 0x95 : 0x94, &menu->root);
        anm_set_label(menu->root, 3);
    }
    set_step(menu, MenuStep::Opening);
    // 457a60 opens Pause from its idle branch and reaches only the counter
    // tail that frame. GameOver opens later from Player A0x17 instead.
    if (reason == IngameMenuReason::Pause)
        menu->timer = 1;
    if (reason == IngameMenuReason::Pause || reason == IngameMenuReason::GameOver)
        sound_play_se(14);
}

bool ingame_menu_active(const IngameMenu* menu) { return menu->step != MenuStep::Closed; }
AnmVm* ingame_menu_background(const IngameMenu* menu) { return menu->background; }

IngameMenuAction ingame_menu_update(IngameMenu* menu) {
    if (menu->step == MenuStep::Closed) return IngameMenuAction::None;
    const FrameInput input = input_frame();
    switch (menu->step) {
    case MenuStep::Opening:
        if (menu->timer >= 10) {
            // 00458e40 sequence 2: ranking is entered after the opening's ten
            // frames. Pause and practice results never insert this table.
            if (menu->reason == IngameMenuReason::GameOver &&
                session().mode == GameMode::Full && replay_mode() != ReplayMode::Playback)
                menu->ranking_index = score_insert_ranking(session().player,
                    static_cast<int>(session().difficulty), g_status.score,
                    session().stage, g_status.continues, std::time(nullptr),
                    static_cast<uint32_t>(replay_info().slow_rate),
                    "        "); // 457870 stores a blank name until entry is confirmed.
            if (menu->reason == IngameMenuReason::GameOver && session().mode == GameMode::Full &&
                menu->ranking_index >= 0)
                begin_ranking(menu);
            else show_main(menu);
        }
        break;
    case MenuStep::Main: {
        const uint32_t movement = input.pressed | input.repeat;
        int direction = 0;
        if (movement & ACTION_UP) direction = -1;
        if (movement & ACTION_DOWN) direction = 1;
        if (direction != 0) {
            do { menu->selection = (menu->selection + direction + 7) % 7; }
            while (!menu->enabled[menu->selection]);
            anm_set_label(menu->root, menu->selection + 7);
            sound_play_se(10);
        }
        if ((input.pressed & ACTION_MENU_CANCEL) && menu->reason == IngameMenuReason::Pause) {
            sound_play_se(7);
            begin_close(menu, IngameMenuAction::Resume);
        } else if (input.pressed & (ACTION_SHOT | ACTION_MENU_OK)) {
            sound_play_se(7);
            choice_label(menu, menu->selection, 6);
            switch (menu->selection) {
            case 0:
                begin_close(menu, menu->reason == IngameMenuReason::Pause ?
                            IngameMenuAction::Resume : IngameMenuAction::Continue);
                break;
            case 1:
                if (menu->reason == IngameMenuReason::Pause && replay_mode() == ReplayMode::Record)
                    begin_confirmation(menu, false);
                else begin_close(menu, IngameMenuAction::ReturnTitle);
                break;
            case 2:
                if (menu->reason == IngameMenuReason::Pause) begin_confirmation(menu, true);
                else set_step(menu, MenuStep::SaveOpening);
                break;
            case 3:
            case 4:
            case 5:
                set_step(menu, MenuStep::PageOpening);
                break;
            case 6:
                if (menu->reason == IngameMenuReason::Pause && replay_mode() == ReplayMode::Record)
                    begin_confirmation(menu, false);
                else begin_close(menu, IngameMenuAction::RestartRun);
                break;
            }
        }
        break;
    }
    case MenuStep::Confirm:
        if (menu->timer == 20) anm_set_label(menu->root, 14);
        if (menu->timer == 30) anm_set_label(menu->root, menu->confirmation + 15);
        if (menu->timer >= 30) {
            if ((input.pressed | input.repeat) & (ACTION_UP | ACTION_DOWN)) {
                menu->confirmation ^= 1;
                anm_set_label(menu->root, menu->confirmation + 15);
                sound_play_se(10);
            }
            if (input.pressed & (ACTION_SHOT | ACTION_MENU_OK)) {
                menu->confirmed = menu->confirmation == 0;
                AnmVm* choice = anm_find_child(menu->root, 0x92 + menu->confirmation);
                if (choice != nullptr) anm_set_label(choice, 6);
                sound_play_se(menu->confirmed ? 7 : 9);
                set_step(menu, menu->confirmed && menu->confirming_save ?
                         MenuStep::SaveOpening : MenuStep::ConfirmClosing);
            } else if (input.pressed & ACTION_MENU_CANCEL) {
                sound_play_se(7);
                begin_close(menu, IngameMenuAction::Resume);
            } else if (input.pressed & ACTION_BOMB) {
                sound_play_se(9);
                if (menu->confirmation == 0) {
                    menu->confirmation = 1;
                    anm_set_label(menu->root, 16);
                } else {
                    menu->confirmed = false;
                    set_step(menu, MenuStep::ConfirmClosing);
                }
            }
        }
        break;
    case MenuStep::ConfirmClosing:
        if (menu->timer >= 20) {
            if (menu->confirmed)
                begin_close(menu, menu->selection == 6 ? IngameMenuAction::RestartRun :
                            IngameMenuAction::ReturnTitle);
            else show_main(menu);
        }
        break;
    case MenuStep::SaveOpening:
        if (menu->timer >= 20) {
            set_tree_visible(menu->root, false);
            menu->replay = replay_menu_create(ReplayMenuMode::InGameSave);
            set_step(menu, MenuStep::Saving);
        }
        break;
    case MenuStep::Saving:
        if (replay_menu_update(menu->replay).action == ReplayMenuAction::Back) {
            replay_menu_destroy(menu->replay);
            menu->replay = nullptr;
            if (menu->reason == IngameMenuReason::Pause)
                begin_close(menu, IngameMenuAction::ReturnTitle);
            else show_main(menu);
        }
        break;
    case MenuStep::Ranking:
        if (menu->timer >= 10) update_ranking_name(menu, input);
        break;
    case MenuStep::PageOpening:
        if (menu->timer == 20) open_page(menu);
        break;
    case MenuStep::Page:
        if (menu->page_finished) {
            destroy_page(menu);
            set_tree_visible(menu->root, true);
            set_step(menu, MenuStep::Main);
        }
        break;
    case MenuStep::Closing:
        if (menu->timer >= 12) {
            const IngameMenuAction action = menu->closing_action;
            menu->step = MenuStep::Closed;
            if (menu->root != nullptr) { anm_vm_free(menu->root); menu->root = nullptr; }
            if (menu->background != nullptr) { anm_vm_free(menu->background); menu->background = nullptr; }
            return action;
        }
        break;
    case MenuStep::Closed:
        break;
    }
    ++menu->timer;
    return IngameMenuAction::None;
}

void ingame_menu_render(IngameMenu* menu) {
    if (menu->step == MenuStep::Closed) return;
    text_set_group(0); // B0x51 enqueues; native group 0 draws at B0x58.
    text_set_life(0);
    text_set_alignment(1, 1);
    text_set_style(0);
    text_set_size(1.0f, 1.0f);
    text_set_shadow(true); // 458090 enables this for its entire draw callback.
    if (menu->step == MenuStep::Saving) replay_menu_render(menu->replay);
    else if (menu->step == MenuStep::Ranking) render_ranking(menu);
    if (menu->reason == IngameMenuReason::GameOver && session().mode != GameMode::SpellPractice &&
             ((menu->step != MenuStep::PageOpening && menu->step != MenuStep::Page) || menu->selection == 3)) {
        char credit[32];
        std::snprintf(credit, sizeof(credit), "Credit %d", menu->credits);
        draw_text(184, 448, 0xffffffffu, credit);
    }
    text_set_color(0xffffffffu);
    text_set_shadow(false);
}
