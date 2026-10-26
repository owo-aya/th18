#include "replay/replay.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <utility>
#include <windows.h>

#include "cards/card_manager.h"
#include "core/app.h"
#include "core/game_session.h"
#include "core/input.h"
#include "core/rng.h"
#include "player/player.h"
#include "resources/dat.h"

namespace {
struct ReplayStage {
    ReplayStageHeader header{};
    std::vector<ReplayFrame> frames;
    std::vector<uint8_t> fps;
    size_t fps_offset = 0;
    bool present = false;
};

ReplayMode g_mode = ReplayMode::None;
ReplayInfo g_info{};
ReplayStage g_stages[8];
std::vector<uint8_t> g_decoded;
int32_t g_stage = 0;
uint32_t g_frame = 0;
uint32_t g_fps_cursor = 0;
size_t g_display_offset = 0;
bool g_ready = false;
bool g_finished = false;
bool g_started = false;
bool g_saved = false; // Original replay object +0x218 bit 0.

int32_t status_int(const uint8_t* status, size_t offset) {
    int32_t value;
    std::memcpy(&value, status + offset, sizeof(value));
    return value;
}

// FUN_00417a60 copies the status and rebuilds its two timer representations.
void copy_status(uint8_t* dest, const uint8_t* source) {
    std::memcpy(dest, source, 0xc4);
    for (size_t offset : {size_t{0xc4}, size_t{0xd8}}) {
        const int32_t current = status_int(source, offset + 4);
        uint32_t flags = static_cast<uint32_t>(status_int(dest, offset + 0x10));
        if ((flags & 1) == 0) {
            const int32_t zero = 0;
            std::memcpy(dest + offset + 0xc, &zero, 4);
            flags |= 1;
            std::memcpy(dest + offset + 0x10, &flags, 4);
        }
        const int32_t previous = current - 1;
        const float value = static_cast<float>(current);
        std::memcpy(dest + offset, &previous, 4);
        std::memcpy(dest + offset + 4, &current, 4);
        std::memcpy(dest + offset + 8, &value, 4);
    }
    std::memcpy(dest + 0xec, source + 0xec, 0x10);
}

void capture_status(uint8_t* dest) {
    copy_status(dest, reinterpret_cast<const uint8_t*>(&g_status));
}

void apply_status(const uint8_t* source) {
    copy_status(reinterpret_cast<uint8_t*>(&g_status), source);
    session().stage = status_int(source, 0);
    session().player = status_int(source, 0x18);
    session().difficulty = static_cast<Difficulty>(status_int(source, 0x24));
    session().score = static_cast<uint32_t>(status_int(source, 0x20));
    app_context()->current_stage = static_cast<uint32_t>(session().stage);
    app_context()->selected_player = session().player;
    app_context()->difficulty = static_cast<int32_t>(session().difficulty);
    g_player_character = session().player;
    g_player_shot_type = status_int(source, 0x1c);
}

int32_t card_count(const int32_t* ids) {
    int32_t count = 0;
    while (ids[count] >= 0) ++count;
    return count;
}

void capture_cards(int32_t* ids, int32_t* values, int32_t& selected) {
    const int32_t count = card_manager_card_count();
    for (int32_t i = 0; i < count; ++i) {
        const AbilityCard* card = card_manager_card_at(i);
        ids[i] = card->id;
        values[i] = card_manager_value(i);
    }
    ids[count] = -1;
    values[count] = -1;
    selected = card_manager_selected_id();
}

void apply_card_values(const int32_t* values, int32_t selected) {
    card_manager_select_id(selected);
    for (int32_t i = 0; i < card_manager_card_count(); ++i)
        card_manager_set_value(i, values[i]);
}

std::vector<uint8_t> decode(void* data) {
    const auto& header = *static_cast<const ReplayFileHeader*>(data);
    auto* compressed = static_cast<uint8_t*>(data) + 0x24;
    th_decrypt(compressed, header.compressed_size, 0x5c, 0xe1, 0x400,
               header.compressed_size);
    th_decrypt(compressed, header.compressed_size, 0x7d, 0x3a, 0x100,
               header.compressed_size);
    std::vector<uint8_t> decoded(header.decoded_size);
    th_unlzss(compressed, header.compressed_size, decoded.data(), decoded.size());
    return decoded;
}

const char* replay_path(char* path, size_t capacity, const char* name) {
    char relative[300];
    std::snprintf(relative, sizeof(relative), "replay/%s", name);
    return storage_user_path(path, capacity, relative);
}

void append(std::vector<uint8_t>& data, const void* bytes, size_t size) {
    const auto* first = static_cast<const uint8_t*>(bytes);
    if (size != 0) data.insert(data.end(), first, first + size);
}

void append_user(std::vector<uint8_t>& file, uint32_t kind, const char* text) {
    const uint32_t length = static_cast<uint32_t>((12 + std::strlen(text) + 1 + 3) & ~3u);
    const size_t start = file.size();
    file.resize(start + length);
    const uint32_t fields[3] = {0x52455355, length, kind};
    std::memcpy(file.data() + start, fields, sizeof(fields));
    std::memcpy(file.data() + start + 12, text, std::strlen(text) + 1);
}

void add_entry(std::vector<ReplayEntry>& entries, int32_t slot, const char* filename) {
    char path[600];
    size_t size;
    void* file = file_read(replay_path(path, sizeof(path), filename), &size, true);
    std::vector<uint8_t> data = decode(file);
    std::free(file);
    ReplayEntry entry;
    entry.slot = slot;
    entry.filename = filename;
    std::memcpy(&entry.info, data.data(), sizeof(entry.info));
    const uint8_t* cursor = data.data() + 0xc8;
    for (uint32_t i = 0; i < entry.info.stage_count; ++i) {
        ReplayStageHeader header;
        std::memcpy(&header, cursor, sizeof(header));
        entry.stage_present[header.stage] = true;
        entry.stage_scores[header.stage] = static_cast<uint32_t>(status_int(header.initial_status, 0x20));
        entry.stage_continues[header.stage] = status_int(header.initial_status, 0x28);
        cursor += 0x126c + header.data_size;
    }
    // 004683cc: the row shows the next stage's opening score, or the run total.
    for (int stage = 1; stage < 8; ++stage) {
        if (stage < 6 && entry.stage_present[stage + 1]) {
            entry.stage_scores[stage] = entry.stage_scores[stage + 1];
            entry.stage_continues[stage] = entry.stage_continues[stage + 1];
        } else {
            entry.stage_scores[stage] = entry.info.score;
            entry.stage_continues[stage] = entry.info.continues;
        }
    }
    entries.push_back(std::move(entry));
}
} // namespace

std::vector<ReplayEntry> replay_list() {
    std::vector<ReplayEntry> entries;
    char filename[32];
    char path[600];
    for (int32_t slot = 0; slot < 25; ++slot) {
        storage_replay_name(filename, sizeof(filename), slot);
        if (file_exists(replay_path(path, sizeof(path), filename)))
            add_entry(entries, slot, filename);
    }
    WIN32_FIND_DATAA found;
    HANDLE search = FindFirstFileA(replay_path(path, sizeof(path), "th18_ud????.rpy"), &found);
    if (search != INVALID_HANDLE_VALUE) {
        int32_t slot = 25;
        do {
            add_entry(entries, slot++, found.cFileName);
        } while (slot < 75 && FindNextFileA(search, &found));
        FindClose(search);
    }
    return entries;
}

void replay_close() {
    for (auto& stage : g_stages) stage = ReplayStage{};
    g_decoded.clear();
    g_info = ReplayInfo{};
    g_stage = 0;
    g_frame = g_fps_cursor = 0;
    g_display_offset = 0;
    g_ready = g_finished = g_started = false;
    g_saved = false;
    g_mode = ReplayMode::None;
}

bool replay_load(const char* name, bool from_dat) {
    char path[600];
    size_t size;
    void* file = from_dat ? dat_read(name, &size)
                         : file_read(replay_path(path, sizeof(path), name), &size, true);
    if (file == nullptr) return false;
    std::vector<uint8_t> data = decode(file);
    std::free(file);
    replay_close();
    g_decoded = std::move(data);
    std::memcpy(&g_info, g_decoded.data(), sizeof(g_info));
    const uint8_t* cursor = g_decoded.data() + 0xc8;
    for (uint32_t i = 0; i < g_info.stage_count; ++i) {
        ReplayStageHeader header;
        std::memcpy(&header, cursor, sizeof(header));
        auto& stage = g_stages[header.stage];
        stage.header = header;
        stage.present = true;
        stage.frames.resize(header.frame_count);
        std::memcpy(stage.frames.data(), cursor + 0x126c,
                    header.frame_count * sizeof(ReplayFrame));
        const uint8_t* fps = cursor + 0x126c + header.frame_count * sizeof(ReplayFrame);
        stage.fps_offset = static_cast<size_t>(fps - g_decoded.data());
        stage.fps.assign(fps, cursor + 0x126c + header.data_size);
        if (i == 0) g_stage = header.stage;
        cursor += 0x126c + header.data_size;
    }
    g_mode = ReplayMode::Playback;
    auto* app = app_context();
    app->selected_player = g_info.player;
    app->difficulty = g_info.difficulty;
    app->start_stage = static_cast<uint32_t>(g_stage);
    session().mode = GameMode::Replay;
    session().player = g_info.player;
    session().difficulty = static_cast<Difficulty>(g_info.difficulty);
    session().stage = g_stage;
    return true;
}

void replay_begin_record(const ConfigBlob& config) {
    replay_close();
    g_mode = ReplayMode::Record;
    std::memset(g_info.name, ' ', 8);
    g_info.config = config;
    g_info.player = session().player;
    g_info.shot = g_player_shot_type;
    g_info.difficulty = static_cast<int32_t>(session().difficulty);
    g_info.mode_flags = session().mode == GameMode::SpellPractice ? 2 :
                        session().mode == GameMode::Practice ? 1 : 0;
    g_info.spell_id = app_context()->selected_spell;
}

ReplayMode replay_mode() { return g_mode; }
const ReplayInfo& replay_info() { return g_info; }
bool replay_has_stage(int32_t stage) { return g_stages[stage].present; }
const ReplayStageHeader& replay_stage_header(int32_t stage) { return g_stages[stage].header; }

void replay_begin_stage(int32_t stage_index) {
    g_stage = stage_index;
    g_ready = false;
    g_frame = g_fps_cursor = 0;
    g_finished = false;
    auto& stage = g_stages[g_stage];
    g_display_offset = stage.fps_offset;
    if (g_mode == ReplayMode::Record) {
        stage = ReplayStage{};
        stage.present = true;
        stage.header.stage = static_cast<uint16_t>(g_stage);
        stage.header.seed = rng().state;
        stage.header.flags = g_started ? 0 : 1;
        for (uint32_t i = 0; i < 20; ++i)
            stage.header.spell_times[i] = i * 0xdeaddeadU;
        stage.header.initial_selected = stage.header.shop_selected = -1;
    } else if (g_mode == ReplayMode::Playback) {
        rng().state = stage.header.seed;
        // 004615e0 restores status on first load; the 004424e0 continuation
        // branch preserves the previous shop result until late 00462ea0.
        if (!g_started) apply_status(stage.header.initial_status);
    }
    rng().counter = 0;
    if (g_started) card_rng().state = stage.header.seed;
    g_started = true;
}

const int32_t* replay_stage_cards() { return g_stages[g_stage].header.initial_cards; }
int32_t replay_stage_card_count() { return card_count(replay_stage_cards()); }

void replay_prepare_stage_cards() {
    input_battle_reset();
    g_ready = false;
    auto& header = g_stages[g_stage].header;
    if (g_mode == ReplayMode::Record) {
        capture_cards(header.initial_cards, header.initial_values, header.initial_selected);
    } else if (g_mode == ReplayMode::Playback) {
        apply_card_values(header.initial_values, header.initial_selected);
    }
}

void replay_start_stage() {
    auto& header = g_stages[g_stage].header;
    if (g_mode == ReplayMode::Record) {
        capture_status(header.initial_status);
        header.x_sub = g_player->x_sub;
        header.y_sub = g_player->y_sub;
        header.focus = g_player->focus_mode;
    } else if (g_mode == ReplayMode::Playback) {
        apply_status(header.initial_status);
        g_player->x_sub = header.x_sub;
        g_player->y_sub = header.y_sub;
        g_player->x = static_cast<float>(header.x_sub) / 128.0f;
        g_player->y = static_cast<float>(header.y_sub) / 128.0f;
        g_player->focus_mode = header.focus;
        for (auto& point : g_player->trail) {
            point[0] = header.x_sub;
            point[1] = header.y_sub;
        }
        player_refresh_power_options();
    }
    input_battle_reset();
    g_frame = g_fps_cursor = 0;
    g_ready = true;
}

bool replay_next_input(uint32_t& held) {
    if (!g_ready || g_finished) return false;
    auto& stage = g_stages[g_stage];
    if (g_frame >= stage.frames.size()) {
        ++g_frame;
        return false;
    }
    const ReplayFrame& input = stage.frames[g_frame];
    if (input.held == 0xffff && input.pressed == 0xffff && input.released == 0xffff) {
        g_finished = true;
        return false;
    }
    // 00462b80..00462bb5 reads before advancing the FPS pointer on frame 0,30,...
    // Keep its offset into the whole decoded file, including the next stage byte.
    g_display_offset = stage.fps_offset + g_fps_cursor;
    if (g_frame % 30 == 0) ++g_fps_cursor;
    ++g_frame;
    held = input.held;
    return true;
}

void replay_record_input(const FrameInput& input, float fps) {
    if (g_mode != ReplayMode::Record || !g_ready) return;
    auto& stage = g_stages[g_stage];
    if (g_frame % 30 == 0) {
        const float rounded = fps + 0.5f;
        stage.fps.push_back(rounded >= 256.0f ? 255 : static_cast<uint8_t>(rounded));
    }
    stage.frames.push_back({static_cast<uint16_t>(input.held),
                            static_cast<uint16_t>(input.pressed),
                            static_cast<uint16_t>(input.released)});
    ++g_frame;
}

bool replay_finished() { return g_finished; }
bool replay_display_fps(uint8_t& fps) {
    if (g_display_offset >= g_decoded.size()) return false;
    fps = g_decoded[g_display_offset];
    return true;
}

void replay_shop_result() {
    // 00417880 clears battle input through 00418ce0 before saving/restoring cards.
    input_battle_reset();
    auto& header = g_stages[g_stage].header;
    if (g_mode == ReplayMode::Record) {
        capture_status(header.shop_status);
        capture_cards(header.shop_cards, header.shop_values, header.shop_selected);
    } else if (g_mode == ReplayMode::Playback && status_int(header.shop_status, 0) == g_stage) {
        card_manager_clear_cards();
        for (int32_t i = 0; i < card_count(header.shop_cards); ++i)
            card_manager_acquire(static_cast<CardId>(header.shop_cards[i]),
                                 CardAdd_NoImmediate | CardAdd_CheckEquip);
        apply_card_values(header.shop_values, header.shop_selected);
        apply_status(header.shop_status);
    }
}

uint32_t replay_spell_time(int32_t index, uint32_t value) {
    auto& stored = g_stages[g_stage].header.spell_times[index];
    if (g_mode == ReplayMode::Record) stored = value;
    return g_mode == ReplayMode::Playback ? stored : value;
}

void replay_finish_run(bool cleared, float slow_rate) {
    if (g_mode != ReplayMode::Record) return;
    g_info.date = static_cast<int64_t>(std::time(nullptr));
    g_info.score = static_cast<uint32_t>(status_int(reinterpret_cast<const uint8_t*>(&g_status), 0x20));
    g_info.continues = status_int(reinterpret_cast<const uint8_t*>(&g_status), 0x28);
    g_info.last_stage = cleared ? 8 : g_stage;
    g_info.slow_rate = slow_rate;
}

bool replay_save(int32_t slot, const char* name) {
    if (!g_saved && g_info.last_stage < 8)
        g_stages[g_stage].frames.push_back({0xffff, 0xffff, 0xffff});
    g_info.score = g_status.score;
    std::memset(g_info.name, ' ', 8);
    const size_t name_size = std::strlen(name);
    std::memcpy(g_info.name, name, name_size < 8 ? name_size : 8);
    g_info.name[8] = '\0';
    g_info.stage_count = 0;
    for (const auto& stage : g_stages)
        if (stage.present) ++g_info.stage_count;
    std::vector<uint8_t> decoded;
    append(decoded, &g_info, sizeof(g_info));
    int32_t first_stage = 0;
    int32_t last_stage = 0;
    for (int32_t i = 1; i < 8; ++i) {
        auto& stage = g_stages[i];
        if (!stage.present) continue;
        if (first_stage == 0) first_stage = i;
        last_stage = i;
        if (!g_saved) {
            stage.header.frame_count = static_cast<uint32_t>(stage.frames.size());
            stage.header.data_size = static_cast<uint32_t>(stage.frames.size() * 6 + stage.fps.size());
        }
        append(decoded, &stage.header, sizeof(stage.header));
        append(decoded, stage.frames.data(), stage.frames.size() * sizeof(ReplayFrame));
        append(decoded, stage.fps.data(), stage.fps.size());
    }
    std::vector<uint8_t> compressed(decoded.size() * 2 + 3);
    const uint32_t size = static_cast<uint32_t>(th_lzss(decoded.data(), decoded.size(), compressed.data()));
    compressed.resize(size);
    th_encrypt(compressed.data(), size, 0x7d, 0x3a, 0x100, size);
    th_encrypt(compressed.data(), size, 0x5c, 0xe1, 0x400, size);
    ReplayFileHeader header{};
    header.magic = 0x72383174;
    header.version = 6;
    header.user_offset = size + 0x24;
    header.flags = 0x100;
    header.compressed_size = size;
    header.decoded_size = static_cast<uint32_t>(decoded.size());
    std::vector<uint8_t> file;
    append(file, &header, sizeof(header));
    append(file, compressed.data(), compressed.size());

    const char* players[] = {"Reimu ", "Marisa", "Sakuya", "Sanae "};
    const char* difficulties[] = {"Easy   ", "Normal ", "Hard   ", "Lunatic", "Extra  "};
    const std::time_t date = static_cast<std::time_t>(g_info.date);
    const std::tm* utc = std::gmtime(&date);
    char stage_text[64];
    if (g_info.last_stage >= 8)
        std::snprintf(stage_text, sizeof(stage_text), "%s", first_stage == 7 ? "Extra Stage Clear" : "Stage All Clear");
    else if (first_stage == 7)
        std::snprintf(stage_text, sizeof(stage_text), "Extra Stage");
    else if (first_stage == last_stage)
        std::snprintf(stage_text, sizeof(stage_text), "Stage %d", first_stage);
    else
        std::snprintf(stage_text, sizeof(stage_text), "Stage %d \x81\x60 %d", first_stage, last_stage);
    char text[512];
    std::snprintf(text, sizeof(text),
        "\x93\x8c\x95\xfb\x93\xf8\x97\xb4\x93\xb4 "
        "\x83\x8a\x83\x76\x83\x8c\x83\x43\x83\x74\x83\x40\x83\x43\x83\x8b\x8f\xee\x95\xf1\r\n"
        "Version 1.00a\r\nName %s\r\nDate %02d/%02d/%02d %02d:%02d\r\n"
        "Chara %s\r\nRank %s\r\n%s\r\nScore %u\r\nSlow Rate %2.2f\r\n",
        g_info.name, utc->tm_year % 100, utc->tm_mon + 1, utc->tm_mday, utc->tm_hour, utc->tm_min,
        players[g_info.player + g_info.shot], difficulties[g_info.difficulty], stage_text,
        g_info.score, g_info.slow_rate);
    append_user(file, 0, text);
    append_user(file, 1, "\x83\x52\x83\x81\x83\x93\x83\x67\x82\xf0\x8f\x91\x82\xaf\x82\xdc\x82\xb7");
    char filename[32];
    char path[600];
    storage_replay_name(filename, sizeof(filename), slot);
    const bool written = file_write(replay_path(path, sizeof(path), filename), file.data(), file.size());
    g_saved = true;
    return written;
}
