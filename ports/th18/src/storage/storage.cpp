#include "storage/storage.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <windows.h>
#include <shlobj.h>

#include "cards/card_info.h"
#include "core/rng.h"
#include "gameplay/score_data.h"
#include "presentation/trophy.h"
#include "resources/dat.h"

namespace {
char g_game_dir[520];
char g_user_dir[520];
ConfigBlob g_config;
bool g_config_loaded;
ScoreData g_score;
ScoreData g_previous_score;
uint32_t g_score_header[6];
uint32_t g_previous_header[6];
bool g_score_loaded;

void original_user_path(char* buf, size_t cap, const char* name) {
    char appdata[520];
    GetEnvironmentVariableA("APPDATA", appdata, sizeof(appdata));
    std::snprintf(buf, cap, "%s\\ShanghaiAlice\\th18\\%s", appdata, name);
}

void config_defaults(ConfigBlob& cfg) {
    std::memset(cfg.data, 0, sizeof(cfg.data));
    const uint16_t mappings[30] = {
        0, 1, 2, 5, 0xffff, 0xffff, 0xffff, 0xffff, 3, 4,
        0, 1, 5, 10, 0xffff, 0xffff, 0xffff, 0xffff, 2, 3,
        'Z', 'X', VK_SHIFT, VK_ESCAPE, 0xffff, 0xffff, 0xffff, 0xffff, 'C', 'D'
    };
    *reinterpret_cast<uint32_t*>(cfg.data) = kConfigVersion;
    std::memcpy(cfg.data + 4, mappings, sizeof(mappings));
    *reinterpret_cast<uint32_t*>(cfg.data + 0x40) = 0x02580258;
    cfg.data[0x45] = 1;
    cfg.data[0x46] = 1;
    cfg.data[0x47] = 8;
    cfg.data[0x49] = 2;
    cfg.data[0x4a] = 100;
    cfg.data[0x4b] = 80;
    cfg.data[0x4d] = 2;
    *reinterpret_cast<uint32_t*>(cfg.data + 0x50) = 0x100;
    *reinterpret_cast<int32_t*>(cfg.data + 0x54) = INT32_MIN;
    *reinterpret_cast<int32_t*>(cfg.data + 0x58) = INT32_MIN;
}

// FUN_00463350 / FUN_00463670: original first-run rankings, cards and decks.
void score_defaults(ScoreData& score, uint32_t* header) {
    std::memset(&score, 0, sizeof(score));
    const uint32_t initial_header[6] = {0x31384854, 0, 6, 0x100, 0, 0};
    std::memcpy(header, initial_header, sizeof(initial_header));
    for (int player = 0; player < 5; ++player) {
        uint8_t* cr = score.character[player];
        *reinterpret_cast<uint32_t*>(cr) = 0x35243;
        *reinterpret_cast<uint32_t*>(cr + 8) = kScoreCharacterSize;
        *reinterpret_cast<int32_t*>(cr + 0xc) = player;
        for (int difficulty = 0; difficulty < 6; ++difficulty) {
            for (int rank = 0; rank < 10; ++rank) {
                uint8_t* record = cr + 0x10 + difficulty * 0x140 + rank * 0x20;
                *reinterpret_cast<uint32_t*>(record) = (10 - rank) * 10000;
                record[4] = 1;
                std::memcpy(record + 6, "--------", 9);
            }
        }
        for (int spell = 0; spell < 97; ++spell) {
            *reinterpret_cast<int32_t*>(cr + 0x9a0 + spell * 0xdc) = spell;
            *reinterpret_cast<int32_t*>(cr + 0x9a4 + spell * 0xdc) = spell < 84 ? spell % 4 : 4;
        }
    }
    uint8_t* st = score.state;
    *reinterpret_cast<uint32_t*>(st) = 0x65453;
    *reinterpret_cast<uint32_t*>(st + 8) = kScoreStateSize;
    std::memcpy(st + 0xc, "        ", 9);
    for (int id = 0; id <= Null; ++id)
        st[0xd0 + id] = static_cast<uint8_t>(card_info_by_id(id)->unk_24);
    std::memset(st + 0x150, Null, 3 * 16);
    st[0x150] = Kozuchi;
    st[0x160] = Kozuchi;
    st[0x161] = YoumuOp;
    st[0x162] = DeadBombExtend;
    st[0x170] = Kozuchi;
    auto* counts = reinterpret_cast<int32_t*>(st + 0x1c0);
    counts[0] = 1;
    counts[1] = 3;
    counts[2] = 5;
    // Original ST trailing 0x200 bytes consume one 16-bit RNG step per word.
    Rng& random = rng();
    auto* random_words = reinterpret_cast<uint16_t*>(st + 0x1d0);
    for (int i = 0; i < 256; ++i) {
        const uint16_t value = static_cast<uint16_t>((random.state ^ 0x9630u) + 0x9aadu);
        random.state = static_cast<uint16_t>((value >> 14) + (value << 2));
        ++random.counter;
        random_words[i] = random.state;
    }
}

// Read existing data directly in its native format. Source selection happens
// before this call: a present rebuilt file is never replaced by an original file.
void read_score(const char* path, ScoreData& score, uint32_t* header) {
    size_t size;
    auto* file = static_cast<uint8_t*>(file_read(path, &size, true));
    if (file == nullptr)
        return;
    std::memcpy(header, file, 0x18);
    th_decrypt(file + 0x18, header[4], 0xac, 0x35, 0x10, header[4]);
    std::vector<uint8_t> data(header[5]);
    th_unlzss(file + 0x18, header[4], data.data(), data.size());
    for (uint8_t* chunk = data.data(); chunk < data.data() + data.size();
         chunk += *reinterpret_cast<uint32_t*>(chunk + 8)) {
        const uint16_t tag = *reinterpret_cast<uint16_t*>(chunk);
        if (tag == 0x5243) {
            const int player = *reinterpret_cast<int32_t*>(chunk + 0xc);
            std::memcpy(score.character[player], chunk, kScoreCharacterSize);
        } else if (tag == 0x5453) {
            std::memcpy(score.state, chunk, kScoreStateSize);
        }
    }
    std::free(file);
}

// CR/ST +4 is a native serialized field, the byte sum of the chunk from +8.
// Reading does not add any format/version/integrity checks.
void write_chunk_sum(uint8_t* chunk, size_t size) {
    uint32_t sum = 0;
    for (size_t i = 8; i < size; ++i)
        sum += chunk[i];
    *reinterpret_cast<uint32_t*>(chunk + 4) = sum;
}

bool write_score(const char* name, ScoreData& score, uint32_t* header) {
    constexpr size_t payload_size = 5 * kScoreCharacterSize + kScoreStateSize;
    std::vector<uint8_t> payload(payload_size);
    uint8_t* next = payload.data();
    for (int player = 0; player < 5; ++player) {
        uint8_t* cr = score.character[player];
        *reinterpret_cast<int32_t*>(cr + 0xc) = player;
        write_chunk_sum(cr, kScoreCharacterSize);
        std::memcpy(next, cr, kScoreCharacterSize);
        next += kScoreCharacterSize;
    }
    write_chunk_sum(score.state, kScoreStateSize);
    std::memcpy(next, score.state, kScoreStateSize);
    std::vector<uint8_t> file(0x18 + payload_size * 2 + 3);
    header[4] = static_cast<uint32_t>(th_lzss(payload.data(), payload.size(), file.data() + 0x18));
    header[5] = static_cast<uint32_t>(payload.size());
    header[1] = header[4] + 0x18;
    th_encrypt(file.data() + 0x18, header[4], 0xac, 0x35, 0x10, header[4]);
    std::memcpy(file.data(), header, 0x18);
    char path[600];
    return file_write(storage_user_path(path, sizeof(path), name), file.data(), header[1]);
}
} // namespace

void storage_set_game_dir(const char* dir) {
    std::snprintf(g_game_dir, sizeof(g_game_dir), "%s", dir);
}

const char* storage_game_dir() { return g_game_dir; }

void storage_set_user_dir(const char* dir) {
    char base[520];
    GetModuleFileNameA(nullptr, base, sizeof(base));
    *std::strrchr(base, '\\') = '\0';
    if (dir == nullptr || dir[0] == '\0')
        std::snprintf(g_user_dir, sizeof(g_user_dir), "%s", base);
    else if (dir[1] == ':' || dir[0] == '\\' || dir[0] == '/')
        std::snprintf(g_user_dir, sizeof(g_user_dir), "%s", dir);
    else
        std::snprintf(g_user_dir, sizeof(g_user_dir), "%s\\%s", base, dir);
    SHCreateDirectoryExA(nullptr, g_user_dir, nullptr);
    char replay_dir[600];
    storage_user_path(replay_dir, sizeof(replay_dir), "replay");
    SHCreateDirectoryExA(nullptr, replay_dir, nullptr);
}

const char* storage_user_dir() { return g_user_dir; }

const char* storage_user_path(char* buf, size_t cap, const char* name) {
    std::snprintf(buf, cap, "%s\\%s", g_user_dir, name);
    return buf;
}

bool storage_config_load(ConfigBlob& out) {
    char path[600];
    storage_user_path(path, sizeof(path), "th18.cfg");
    if (!file_exists(path))
        original_user_path(path, sizeof(path), "th18.cfg");
    size_t size;
    void* data = file_read(path, &size, true);
    if (data == nullptr)
        return false;
    std::memcpy(out.data, data, kConfigSize);
    std::free(data);
    return true;
}

void storage_config_init() {
    if (g_config_loaded)
        return;
    config_defaults(g_config);
    storage_config_load(g_config);
    storage_config_save(g_config);
    g_config_loaded = true;
}

ConfigBlob& storage_config() { return g_config; }

bool storage_config_save(const ConfigBlob& cfg) {
    char path[600];
    return file_write(storage_user_path(path, sizeof(path), "th18.cfg"), cfg.data, kConfigSize);
}

bool storage_config_save() { return storage_config_save(g_config); }

void score_init() {
    if (g_score_loaded)
        return;
    char main_path[600];
    char backup_path[600];
    storage_user_path(main_path, sizeof(main_path), "scoreth18.dat");
    storage_user_path(backup_path, sizeof(backup_path), "scoreth18bak.dat");
    if (!file_exists(main_path) && !file_exists(backup_path)) {
        original_user_path(main_path, sizeof(main_path), "scoreth18.dat");
        original_user_path(backup_path, sizeof(backup_path), "scoreth18bak.dat");
    }
    score_defaults(g_previous_score, g_previous_header);
    read_score(backup_path, g_previous_score, g_previous_header);
    score_defaults(g_score, g_score_header);
    g_score = g_previous_score;
    read_score(main_path, g_score, g_score_header);
    g_previous_score = g_score;
    g_score_loaded = true;
}

ScoreData& score_data() { return g_score; }

bool score_save() {
    const bool backup_written = write_score("scoreth18bak.dat", g_previous_score, g_previous_header);
    const bool main_written = write_score("scoreth18.dat", g_score, g_score_header);
    g_previous_score = g_score;
    return backup_written && main_written;
}

void score_mark_achievement(int id) {
    uint8_t& flag = g_score.state[0x50 + id];
    if (flag != 0)
        return;
    flag = 1;
    trophy_achievement(id);
}

int score_insert_ranking(int player, int difficulty, uint32_t score_div10,
                         int stage, int continues, int64_t timestamp,
                         uint32_t slowdown, const char* name) {
    uint8_t* records = g_score.character[player] + 0x10 + difficulty * 0x140;
    for (int rank = 0; rank < 10; ++rank) {
        uint8_t* record = records + rank * 0x20;
        if (*reinterpret_cast<uint32_t*>(record) <= score_div10) {
            std::memmove(record + 0x20, record, (9 - rank) * 0x20);
            *reinterpret_cast<uint32_t*>(record) = score_div10;
            record[4] = static_cast<uint8_t>(stage);
            record[5] = static_cast<uint8_t>(continues);
            std::memcpy(record + 6, name, std::strlen(name) + 1);
            *reinterpret_cast<int64_t*>(record + 0x10) = timestamp;
            *reinterpret_cast<uint32_t*>(record + 0x18) = slowdown;
            return rank;
        }
    }
    return -1;
}

void score_record_clear(int player, int difficulty, bool continued) {
    auto* cleared = reinterpret_cast<int32_t*>(g_score.character[player] + 0x64d0);
    if (cleared[difficulty] < 99999)
        ++cleared[difficulty];
    auto* no_continue = reinterpret_cast<int32_t*>(g_score.character[player] + 0x64ec);
    if (!continued && no_continue[difficulty] < 99999)
        ++no_continue[difficulty];
}

void score_record_practice(int player, int difficulty, int stage,
                           uint32_t score_div10, bool cleared) {
    uint8_t* record = g_score.character[player] + 0x12ef0 + (difficulty * 9 + stage) * 8;
    auto* best = reinterpret_cast<uint32_t*>(record);
    if (*best < score_div10)
        *best = score_div10;
    if (cleared)
        record[4] = 1;
}

void score_add_play_time(int player, uint64_t centiseconds) {
    *reinterpret_cast<uint64_t*>(g_score.character[player] + 0x64c8) += centiseconds;
    *reinterpret_cast<uint64_t*>(g_score.state + 0x48) += centiseconds;
}

void score_spell_start(int player, int spell, bool spell_practice, const char* name) {
    const int characters[2] = {player, 4};
    for (int character : characters) {
        uint8_t* record = g_score.character[character] + 0x8d0 + spell * 0xdc;
        std::memcpy(record, name, std::strlen(name) + 1);
        auto* attempts = reinterpret_cast<int32_t*>(record + 0xc8 + (spell_practice ? 4 : 0));
        if (*attempts < 99999)
            ++*attempts;
    }
}

void score_spell_capture(int player, int spell, bool spell_practice) {
    const int characters[2] = {player, 4};
    for (int character : characters) {
        uint8_t* record = g_score.character[character] + 0x8d0 + spell * 0xdc;
        auto* captures = reinterpret_cast<int32_t*>(record + 0xc0 + (spell_practice ? 4 : 0));
        if (*captures < 99999)
            ++*captures;
    }
    for (int id = 0; id < 97; ++id) {
        const auto* captures = reinterpret_cast<const int32_t*>(g_score.character[4] + 0x990 + id * 0xdc);
        if (captures[0] == 0 && captures[1] == 0)
            return;
    }
    score_mark_achievement(0x1c);
}

void score_record_spell_practice(int player, int spell, uint32_t score_div10) {
    auto* best = reinterpret_cast<uint32_t*>(g_score.character[player] + 0x9a8 + spell * 0xdc);
    const uint32_t rounded = score_div10 / 10 * 10;
    if (*best < rounded)
        *best = rounded;
}

void storage_replay_name(char* buf, size_t cap, int32_t index) {
    std::snprintf(buf, cap, "th18_%02d.rpy", index + 1);
}

void storage_log(const char* fmt, ...) {
    char path[600];
    char line[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    FILE* fp = std::fopen(storage_user_path(path, sizeof(path), "log.txt"), "a");
    if (fp == nullptr)
        return;
    std::fprintf(fp, "%s\n", line);
    std::fclose(fp);
}
