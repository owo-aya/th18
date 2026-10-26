#include "replay/replay_menu.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include "core/input.h"
#include "gameplay/score_data.h"
#include "presentation/anm.h"
#include "presentation/sound.h"
#include "presentation/text.h"
#include "replay/replay.h"

enum class ReplayPage { Enter, Slots, Stages, Name, Starting, Leaving };

struct ReplayMenu {
    ReplayMenuMode mode;
    ReplayPage page;
    int timer;
    int row;
    int bank;
    int selected_slot;
    int selected_stage;
    int character;
    int name_length;
    char name[9];
    ReplayInfo save_info;
    std::vector<ReplayEntry> entries;
    AnmVm* heading;
};

namespace {
int g_return_slot;
constexpr char kCharacters[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-=.,!?@:;[]()_/{}|~^#$%&*   ";
constexpr const char* kPlayers[] = {"Reimu ", "Marisa", "Sakuya", "Sanae "};
constexpr const char* kRanks[] = {"Easy   ", "Normal ", "Hard   ", "Lunatic", "Extra  "};
constexpr const char* kShortRanks[] = {"E ", "N ", "H ", "L ", "EX"};
constexpr const char* kStageResults[] = {"tst", "St1", "St2", "St3", "St4", "St5", "St6", "Ex ", "All"};
constexpr const char* kStageNames[] = {"Stage 1", "Stage 2", "Stage 3", "Stage 4", "Stage 5", "Stage 6", "Extra  "};

AnmVm* create_vm(AnmResource* resource, int script) {
    AnmVm* vm = anm_vm_alloc(resource);
    anm_vm_set_script(vm, resource, script);
    anm_queue_add(vm);
    return vm;
}

void change_page(ReplayMenu* menu, ReplayPage page) {
    menu->page = page;
    menu->timer = 0;
}

const ReplayEntry* slot_entry(const ReplayMenu* menu, int slot) {
    for (const ReplayEntry& entry : menu->entries)
        if (entry.slot == slot) return &entry;
    return nullptr;
}

void move_stage(ReplayMenu* menu, int direction) {
    const ReplayEntry& entry = *slot_entry(menu, menu->selected_slot);
    do {
        menu->selected_stage = (menu->selected_stage + direction + 7) % 7;
    } while (!entry.stage_present[menu->selected_stage + 1]);
}

void enter_name(ReplayMenu* menu) {
    menu->selected_slot = menu->row;
    std::memcpy(menu->name, score_data().state + 0xc, 9);
    menu->name_length = 8;
    while (menu->name_length > 0 && menu->name[menu->name_length - 1] == ' ')
        --menu->name_length;
    menu->character = std::memcmp(menu->name, "        ", 8) == 0 ? 0 : 90;
    replay_finish_run(menu->mode == ReplayMenuMode::ClearSave, replay_info().slow_rate);
    menu->save_info = replay_info();
    change_page(menu, ReplayPage::Name);
    sound_play_se(7);
}

void begin_playback(ReplayMenu* menu) {
    change_page(menu, ReplayPage::Starting);
    sound_play_se(50);
    // 0x467eda：原参数0.05经454720截断为0，下一A1停止标题曲。
    bgm_fade(0.05f);
}

void leave_menu(ReplayMenu* menu) {
    if (menu->heading) anm_set_label(menu->heading, 1);
}

void update_name(ReplayMenu* menu, const FrameInput& input) {
    const uint32_t movement = input.pressed | input.repeat;
    const int previous = menu->character;
    if (movement & ACTION_UP) menu->character = (menu->character + 78) % 91;
    if (movement & ACTION_DOWN) menu->character = (menu->character + 13) % 91;
    if (movement & ACTION_LEFT)
        menu->character += menu->character % 13 == 0 ? 12 : -1;
    if (movement & ACTION_RIGHT)
        menu->character += menu->character % 13 == 12 ? -12 : 1;
    if (menu->character != previous) sound_play_se(10);

    if (input.pressed & (ACTION_SHOT | ACTION_MENU_OK)) {
        if (menu->character < 89) {
            const char value = menu->character == 88 ? ' ' : kCharacters[menu->character];
            if (menu->name_length < 8) {
                menu->name[menu->name_length++] = value;
                if (menu->name_length == 8) menu->character = 90;
            } else {
                menu->name[7] = value;
            }
            sound_play_se(7);
        } else if (menu->character == 89) {
            if (menu->name_length > 0) {
                menu->name[--menu->name_length] = ' ';
                sound_play_se(menu->mode == ReplayMenuMode::InGameSave ? 9 : 7);
            } else return;
        } else {
            sound_play_se(17);
            if (replay_save(menu->row, menu->name)) {
                std::memcpy(score_data().state + 0xc, menu->name, 9);
                menu->entries = replay_list();
                change_page(menu, ReplayPage::Slots);
            }
            sound_play_se(7);
        }
        if (menu->mode == ReplayMenuMode::InGameSave) return;
    }
    if (input.pressed & (ACTION_BOMB | ACTION_MENU_CANCEL)) {
        if (menu->mode == ReplayMenuMode::InGameSave || menu->name_length > 0)
            sound_play_se(9);
        if (menu->name_length == 0)
            change_page(menu, ReplayPage::Slots);
        else
            menu->name[--menu->name_length] = ' ';
    }
}

void draw_row(const ReplayMenu* menu, int slot, const ReplayInfo* info,
              const char* filename, float y, bool selected) {
    text_set_color(menu->page == ReplayPage::Slots ?
        (selected ? 0xffffff00 : 0xff808080) : 0xffffffff);
    const bool in_game = menu->mode == ReplayMenuMode::InGameSave;
    const TextPos pos{in_game ? 96.0f : 116.0f, y * 2.0f, 0.0f};
    if (info == nullptr) {
        text_draw_fmt(text_manager(), &pos, in_game ?
            "No.%.2d -------- --/--/-- ------ -- St-" :
            (slot < 25 ? "No.%.2d -------- --/--/-- --:-- ------ ------- --- ---%%" :
                         "User  -------- --/--/-- --:-- ------  ------- --- ---%%"), slot + 1);
        return;
    }
    const __time32_t date = static_cast<__time32_t>(info->date);
    tm time;
    _gmtime32_s(&time, &date);
    const char* name = menu->page == ReplayPage::Name ? "        " : info->name;
    const bool spell = (info->mode_flags & 2) != 0;
    if (in_game) {
        if (spell)
            text_draw_fmt(text_manager(), &pos, "No.%.2d %s %.2d/%.2d/%.2d %s Sp %3d",
                slot + 1, name, time.tm_year % 100, time.tm_mon + 1, time.tm_mday,
                kPlayers[info->player + info->shot], info->spell_id + 1);
        else
            text_draw_fmt(text_manager(), &pos, "No.%.2d %s %.2d/%.2d/%.2d %s %s %s",
                slot + 1, name, time.tm_year % 100, time.tm_mon + 1, time.tm_mday,
                kPlayers[info->player + info->shot], kShortRanks[info->difficulty],
                kStageResults[info->last_stage]);
    } else if (slot >= 25) {
        char suffix[5];
        std::memcpy(suffix, filename + 7, 4);
        suffix[4] = 0;
        text_draw_fmt(text_manager(), &pos, "%s  %s %.2d/%.2d/%.2d %.2d:%.2d %s %s %s %2.1f%%",
            suffix, name, time.tm_year % 100, time.tm_mon + 1, time.tm_mday,
            time.tm_hour, time.tm_min, kPlayers[info->player + info->shot],
            kRanks[info->difficulty], kStageResults[info->last_stage], info->slow_rate);
    } else if (spell && menu->mode == ReplayMenuMode::Browse) {
        text_draw_fmt(text_manager(), &pos, "No.%.2d %s %.2d/%.2d/%.2d %.2d:%.2d %s SpellPr %3d %2.1f%%",
            slot + 1, name, time.tm_year % 100, time.tm_mon + 1, time.tm_mday,
            time.tm_hour, time.tm_min, kPlayers[info->player + info->shot],
            info->spell_id + 1, info->slow_rate);
    } else {
        text_draw_fmt(text_manager(), &pos, "No.%.2d %s %.2d/%.2d/%.2d %.2d:%.2d %s %s %s %2.1f%%",
            slot + 1, name, time.tm_year % 100, time.tm_mon + 1, time.tm_mday,
            time.tm_hour, time.tm_min, kPlayers[info->player + info->shot],
            kRanks[info->difficulty], kStageResults[info->last_stage], info->slow_rate);
    }
}
} // namespace

ReplayMenu* replay_menu_create(ReplayMenuMode mode) {
    ReplayMenu* menu = new ReplayMenu{};
    menu->mode = mode;
    menu->entries = replay_list();
    menu->page = mode == ReplayMenuMode::InGameSave ? ReplayPage::Slots : ReplayPage::Enter;
    if (mode == ReplayMenuMode::Browse) {
        menu->row = g_return_slot % 25;
        menu->bank = g_return_slot / 25;
        g_return_slot = 0;
    }
    if (mode != ReplayMenuMode::InGameSave) {
        menu->heading = create_vm(anm_get(0x10, "title.anm"),
            mode == ReplayMenuMode::Browse ? 40 : 44);
    }
    return menu;
}

void replay_menu_destroy(ReplayMenu* menu) {
    anm_vm_free(menu->heading);
    delete menu;
}

ReplayMenuResult replay_menu_update(ReplayMenu* menu) {
    if (menu->heading) anm_vm_update(menu->heading);
    const FrameInput input = input_frame();
    const uint32_t movement = input.pressed | input.repeat;
    ReplayMenuResult result{ReplayMenuAction::None, {}, 0};
    switch (menu->page) {
    case ReplayPage::Enter:
        if (menu->timer > 6) change_page(menu, ReplayPage::Slots);
        break;
    case ReplayPage::Slots: {
        if (menu->mode == ReplayMenuMode::InGameSave && menu->timer < 10) break;
        const int previous_row = menu->row;
        const int previous_bank = menu->bank;
        if (movement & ACTION_UP) menu->row = (menu->row + 24) % 25;
        if (movement & ACTION_DOWN) menu->row = (menu->row + 1) % 25;
        if (menu->mode == ReplayMenuMode::Browse) {
            if (movement & ACTION_LEFT) menu->bank = (menu->bank + 2) % 3;
            if (movement & ACTION_RIGHT) menu->bank = (menu->bank + 1) % 3;
        }
        if (previous_row != menu->row) sound_play_se(10);
        if (previous_bank != menu->bank) sound_play_se(10);
        if (input.pressed & (ACTION_BOMB | ACTION_MENU_CANCEL)) {
            sound_play_se(9);
            if (menu->mode == ReplayMenuMode::InGameSave) result.action = ReplayMenuAction::Back;
            else change_page(menu, ReplayPage::Leaving);
        } else if (input.pressed & (ACTION_SHOT | ACTION_MENU_OK)) {
            if (menu->mode == ReplayMenuMode::Browse) {
                menu->selected_slot = menu->bank * 25 + menu->row;
                const ReplayEntry* entry = slot_entry(menu, menu->selected_slot);
                if (entry) {
                    menu->selected_stage = 6;
                    move_stage(menu, 1);
                    sound_play_se(7);
                    if (entry->info.mode_flags & 2) begin_playback(menu);
                    else change_page(menu, ReplayPage::Stages);
                }
            } else {
                enter_name(menu);
            }
        }
        break;
    }
    case ReplayPage::Stages:
        if (menu->timer > 14) {
            const int previous = menu->selected_stage;
            if (movement & ACTION_UP) move_stage(menu, -1);
            if (movement & ACTION_DOWN) move_stage(menu, 1);
            if (previous != menu->selected_stage) sound_play_se(10);
            if (input.pressed & (ACTION_BOMB | ACTION_MENU_CANCEL)) {
                sound_play_se(9);
                change_page(menu, ReplayPage::Slots);
            } else if (input.pressed & (ACTION_SHOT | ACTION_MENU_OK)) begin_playback(menu);
        }
        break;
    case ReplayPage::Name:
        if (menu->mode != ReplayMenuMode::InGameSave || menu->timer >= 10)
            update_name(menu, input);
        break;
    case ReplayPage::Starting:
        if (menu->timer == 2) text_loading_start(480.0f, 392.0f);
        if (menu->timer > 31) {
            g_return_slot = menu->selected_slot;
            result = {ReplayMenuAction::Start, slot_entry(menu, menu->selected_slot)->filename,
                      menu->selected_stage + 1};
        }
        break;
    case ReplayPage::Leaving:
        if (menu->timer > 5) {
            leave_menu(menu);
            if (menu->mode == ReplayMenuMode::ClearSave) bgm_play(0);
            result.action = ReplayMenuAction::Back;
        }
        break;
    }
    ++menu->timer;
    return result;
}

void replay_menu_render(ReplayMenu* menu) {
    // ANM 根及子VM由管理器的B队列绘制；这里只提交原group 0的ASCII文字。
    text_set_group(0);
    text_set_life(0);
    text_set_style(0);
    text_set_alignment(1, 1);
    text_set_size(1.0f, 1.0f);
    const bool in_game = menu->mode == ReplayMenuMode::InGameSave;
    if (menu->page == ReplayPage::Slots) {
        for (int row = 0; row < 25; ++row) {
            const int slot = menu->bank * 25 + row;
            const ReplayEntry* entry = slot_entry(menu, slot);
            draw_row(menu, slot, entry ? &entry->info : nullptr,
                entry ? entry->filename.c_str() : nullptr,
                (in_game ? 64.0f : 80.0f) + 15.0f * row, menu->row == row);
        }
    } else if (menu->page == ReplayPage::Stages) {
        const ReplayEntry& entry = *slot_entry(menu, menu->selected_slot);
        const float y = menu->timer < 10 ? 80.0f + (10 - menu->timer) * menu->row * 1.5f : 80.0f;
        draw_row(menu, menu->selected_slot, &entry.info, entry.filename.c_str(), y, true);
        if (menu->timer > 9) {
            for (int stage = 0; stage < 7; ++stage) {
                const TextPos pos{440.0f, 256.0f + stage * 36.0f, 0};
                text_set_color(stage == menu->selected_stage ? 0xffffff00 : 0xff808080);
                if (entry.stage_present[stage + 1])
                    text_draw_fmt(text_manager(), &pos, "%s  %.8d%d", kStageNames[stage],
                        entry.stage_scores[stage + 1], entry.stage_continues[stage + 1]);
                else
                    text_draw_fmt(text_manager(), &pos, "%s  ---------", kStageNames[stage]);
            }
        }
    } else if (menu->page == ReplayPage::Name) {
        const float origin = (in_game ? 64.0f : 80.0f) + menu->row * 15.0f;
        const float target = in_game ? 224.0f : 240.0f;
        const float y = menu->timer < 10 ? origin + (target - origin) * menu->timer / 10.0f : target;
        draw_row(menu, menu->row, &menu->save_info, nullptr, y, true);
        if (in_game || menu->timer > 9) {
            const float name_x = in_game ? 102.0f : 112.0f;
            TextPos pos{name_x * 2.0f, y * 2.0f, 0};
            text_set_color(0xffffffff);
            text_queue_add(text_manager(), &pos, menu->name);
            pos.x += (menu->name_length - (menu->name_length == 8 ? 1 : 0)) * 18.0f;
            text_set_color(0xffffff00);
            text_queue_add(text_manager(), &pos, "_");
            for (int c = 0; c < 91; ++c) {
                pos = {((in_game ? 112.0f : 212.0f) + (c % 13) * 18.0f) * 2.0f,
                       ((in_game ? 320.0f : 360.0f) + (c / 13) * 16.0f) * 2.0f, 0};
                const char glyph[2] = {c < 88 ? kCharacters[c] :
                    static_cast<char>(c == 88 ? 0x81 : c == 89 ? 0x7f : 0x80), 0};
                text_set_color(c == menu->character ? 0xffffff00 : 0xff808080);
                text_queue_add(text_manager(), &pos, glyph);
            }
        }
    }
    text_set_color(0xffffffff);
}
