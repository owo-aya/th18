#include "dev/compare.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <shellapi.h>

#include "../../../toolchains/re/compare_fields.h"

#include "core/app.h"
#include "core/game_session.h"
#include "core/input.h"
#include "core/rng.h"
#include "player/player.h"

namespace {

enum class StepKind { Input, Wait, Print, Stop };

struct Step {
    StepKind kind;
    uint32_t bits;
    uint32_t end_tick;
};

const wchar_t* const kReadyName = L"Local\\TH18CompareCandidateReady";
const wchar_t* const kContinueName = L"Local\\TH18CompareCandidateContinue";
const wchar_t* const kShareName = L"Local\\TH18CompareCandidate";

bool g_enabled;
bool g_ipc;
bool g_seed_set;
bool g_halt;
uint32_t g_seed;
uint32_t g_tick;
uint32_t g_forced_bits;
std::vector<Step> g_steps;
size_t g_step_index;
char g_recipe_path[600];
char g_save_path[600];
HANDLE g_ready;
HANDLE g_continue;
HANDLE g_mapping;
Snapshot* g_snapshot;

uint32_t parse_bits(const wchar_t* text) {
    return static_cast<uint32_t>(wcstoul(text, nullptr, 0));
}

uint32_t parse_tick(const wchar_t* text) {
    return static_cast<uint32_t>(wcstoul(text, nullptr, 10));
}

void push_step(StepKind kind, uint32_t bits, uint32_t end_tick) {
    if ((kind == StepKind::Wait || kind == StepKind::Print ||
         kind == StepKind::Stop) && end_tick == 0)
        return;
    g_steps.push_back({kind, bits, end_tick});
}

void load_recipe(const char* path) {
    FILE* file = std::fopen(path, "r");
    if (file == nullptr) return;
    char line[256];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        char kind[8] = {};
        unsigned bits = 0;
        unsigned end_tick = 0;
        if (std::sscanf(line, "seed %x", &bits) == 1) {
            g_seed_set = true;
            g_seed = bits;
        } else if (std::sscanf(line, "%7s %x %u", kind, &bits, &end_tick) >= 2) {
            if (std::strcmp(kind, "i") == 0)
                push_step(StepKind::Input, bits, 0);
            else if (std::strcmp(kind, "w") == 0)
                push_step(StepKind::Wait, bits, end_tick);
            else if (std::strcmp(kind, "p") == 0)
                push_step(StepKind::Print, 0, end_tick);
            else if (std::strcmp(kind, "s") == 0)
                push_step(StepKind::Stop, 0, end_tick);
        }
    }
    std::fclose(file);
}

void wide_to_ansi(const wchar_t* source, char* target, size_t capacity) {
    WideCharToMultiByte(CP_ACP, 0, source, -1, target,
                        static_cast<int>(capacity), nullptr, nullptr);
}

void save_recipe(const char* path) {
    FILE* file = std::fopen(path, "w");
    if (file == nullptr) return;
    if (g_seed_set) std::fprintf(file, "seed %04x\n", g_seed);
    for (const Step& step : g_steps) {
        if (step.kind == StepKind::Input)
            std::fprintf(file, "i %08x\n", step.bits);
        else if (step.kind == StepKind::Wait)
            std::fprintf(file, "w %08x %u\n", step.bits, step.end_tick);
        else
            std::fprintf(file, "%c 0 %u\n",
                         step.kind == StepKind::Print ? 'p' : 's', step.end_tick);
    }
    std::fclose(file);
}

void load_command_line() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) return;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--compare") == 0) {
            g_enabled = true;
        } else if (wcscmp(argv[i], L"--compare-ipc") == 0) {
            g_ipc = true;
            g_enabled = true;
        } else if (wcscmp(argv[i], L"--compare-seed") == 0 && i + 1 < argc) {
            g_seed_set = true;
            g_seed = parse_bits(argv[++i]);
        } else if (wcscmp(argv[i], L"--compare-load") == 0 && i + 1 < argc) {
            wide_to_ansi(argv[++i], g_recipe_path, sizeof(g_recipe_path));
        } else if (wcscmp(argv[i], L"--compare-save") == 0 && i + 1 < argc) {
            wide_to_ansi(argv[++i], g_save_path, sizeof(g_save_path));
        }
    }
    if (g_recipe_path[0] != '\0') load_recipe(g_recipe_path);
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--compare-step") != 0) continue;
        if (i + 1 >= argc) break;
        const wchar_t* kind = argv[i + 1];
        if (wcscmp(kind, L"i") == 0 && i + 2 < argc) {
            push_step(StepKind::Input, parse_bits(argv[i + 2]), 0);
            i += 2;
        } else if (wcscmp(kind, L"w") == 0 && i + 3 < argc) {
            push_step(StepKind::Wait, parse_bits(argv[i + 2]), parse_tick(argv[i + 3]));
            i += 3;
        } else if ((wcscmp(kind, L"p") == 0 || wcscmp(kind, L"s") == 0) &&
                   i + 2 < argc) {
            push_step(wcscmp(kind, L"p") == 0 ? StepKind::Print : StepKind::Stop,
                      0, parse_tick(argv[i + 2]));
            i += 2;
        }
    }
    LocalFree(argv);
    if (g_save_path[0] != '\0') save_recipe(g_save_path);
}

void advance_to_input() {
    g_forced_bits = 0;
    while (g_step_index < g_steps.size()) {
        const Step& step = g_steps[g_step_index];
        if (step.kind == StepKind::Input) {
            g_forced_bits = step.bits;
            ++g_step_index;
            return;
        }
        if (step.kind == StepKind::Wait) {
            if (g_tick < step.end_tick) {
                g_forced_bits = step.bits;
                return;
            }
            ++g_step_index;
            continue;
        }
        break;
    }
}

bool consume_boundary() {
    bool print = false;
    while (g_step_index < g_steps.size()) {
        const Step& step = g_steps[g_step_index];
        if ((step.kind == StepKind::Print || step.kind == StepKind::Stop) &&
            g_tick < step.end_tick)
            break;
        if (step.kind == StepKind::Print) print = true;
        if (step.kind == StepKind::Stop) {
            g_halt = true;
            ++g_step_index;
            return true;
        }
        ++g_step_index;
    }
    return print;
}

uint64_t as_u32(uint32_t value) { return value; }

uint64_t as_float(float value) {
    uint64_t result = 0;
    std::memcpy(&result, &value, sizeof(value));
    return result;
}

void fill_snapshot(Snapshot& snapshot) {
    const InputState& input = input_state();
    const InputState& battle = input_battle_state();
    const Rng& random = rng();
    const Rng& cards = card_rng();
    const GameSession& game = session();
    const AppContext& app = *app_context();
    snapshot.forced_bits = g_forced_bits;
    snapshot.field_count = kCompareFieldCount;
    snapshot.fields[FieldTick] = as_u32(g_tick);
    snapshot.fields[FieldState] = as_u32(app.state);
    snapshot.fields[FieldNextState] = as_u32(app.next_state);
    snapshot.fields[FieldStage] = as_u32(game.stage);
    snapshot.fields[FieldCurrentStage] = as_u32(app.current_stage);
    snapshot.fields[FieldStartStage] = as_u32(app.start_stage);
    snapshot.fields[FieldPlayer] = as_u32(game.player);
    snapshot.fields[FieldShot] = as_u32(g_status.shot);
    snapshot.fields[FieldDifficulty] = as_u32(static_cast<uint32_t>(game.difficulty));
    snapshot.fields[FieldInputHeld] = as_u32(input.held);
    snapshot.fields[FieldInputPressed] = as_u32(input.pressed);
    snapshot.fields[FieldBattleHeld] = as_u32(battle.held);
    snapshot.fields[FieldBattlePressed] = as_u32(battle.pressed);
    snapshot.fields[FieldRngState] = as_u32(random.state);
    snapshot.fields[FieldRngCounter] = as_u32(random.counter);
    snapshot.fields[FieldCardRngState] = as_u32(cards.state);
    snapshot.fields[FieldCardRngCounter] = as_u32(cards.counter);
    snapshot.fields[FieldScore] = as_u32(g_status.score);
    snapshot.fields[FieldMoney] = as_u32(g_status.money);
    snapshot.fields[FieldPower] = as_u32(g_status.power);
    snapshot.fields[FieldLives] = as_u32(g_status.lives);
    if (g_player != nullptr) {
        snapshot.fields[FieldPlayerX] = as_float(g_player->x);
        snapshot.fields[FieldPlayerY] = as_float(g_player->y);
        snapshot.fields[FieldPlayerState] = as_u32(g_player->state);
    }
    std::snprintf(snapshot.text, sizeof(snapshot.text),
        "CANDIDATE tick=%u state=%u next=%u stage=%u in=%08x/%08x "
        "rng=%04x:%u card=%04x:%u score=%u money=%d power=%d lives=%d",
        g_tick, app.state, app.next_state, game.stage, input.held, input.pressed,
        random.state, random.counter, cards.state, cards.counter,
        g_status.score, g_status.money, g_status.power, g_status.lives);
}

void send_snapshot() {
    if (!g_ipc || g_snapshot == nullptr) return;
    fill_snapshot(*g_snapshot);
    SetEvent(g_ready);
    WaitForSingleObject(g_continue, INFINITE);
}

void initialize_ipc() {
    g_ready = CreateEventW(nullptr, FALSE, FALSE, kReadyName);
    g_continue = CreateEventW(nullptr, FALSE, FALSE, kContinueName);
    g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                   0, sizeof(Snapshot), kShareName);
    if (g_ready == nullptr || g_continue == nullptr || g_mapping == nullptr)
        return;
    g_snapshot = static_cast<Snapshot*>(MapViewOfFile(
        g_mapping, FILE_MAP_WRITE, 0, 0, sizeof(Snapshot)));
}

}  // namespace

void compare_parse_command_line() {
    load_command_line();
    if (!g_enabled) return;
    advance_to_input();
}

void compare_ipc_start() {
    if (!g_enabled || !g_ipc) return;
    initialize_ipc();
}

void compare_apply_seed() {
    if (g_enabled && g_seed_set) rng_reset(static_cast<uint16_t>(g_seed));
}

bool compare_enabled() { return g_enabled; }

bool compare_wait_frame() {
    if (!g_enabled) return true;
    if (g_halt) return false;
    if (!g_ipc || g_snapshot == nullptr) return true;
    advance_to_input();
    return WaitForSingleObject(g_continue, INFINITE) == WAIT_OBJECT_0;
}

void compare_end_frame() {
    if (!g_enabled) return;
    ++g_tick;
    if (consume_boundary()) send_snapshot();
}
