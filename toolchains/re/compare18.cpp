#include <windows.h>
#include <conio.h>
#include <shlobj.h>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <string>
#include <userenv.h>
#include <vector>

#include "compare_fields.h"
#include "th18_layout.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "userenv.lib")

enum StepKind {
    StepInput = 'i',
    StepWait = 'w',
    StepPrint = 'p',
    StepStop = 's',
};

struct Step {
    StepKind kind;
    uint32_t bits;
    uint32_t end_tick;
};

namespace {

struct Recipe {
    uint32_t seed;
    bool seed_set;
    std::vector<Step> steps;
};

enum ThreadStop {
    StopNone,
    StopException,
    StopBoundary,
    StopQuit,
};

const HANDLE kNoThread = nullptr;

struct Process {
    STARTUPINFOW start = {sizeof(start)};
    PROCESS_INFORMATION info{};
    void* environment = nullptr;
    HANDLE ready;
    HANDLE mapping;
    Snapshot* snapshot;
    HWND window = nullptr;
    bool has_window;
};

struct DebugThread {
    DWORD id;
    HANDLE handle;
    ThreadStop stop = StopNone;
};

struct Hook {
    void* address;
    uint8_t original;
};

struct Target {
    Process process;
    DebugThread thread;
    Hook input_hook;
    Hook boundary_hook;
    uint32_t tick = 0;
    uint32_t forced_bits = 0;
    bool halted = false;
    const Recipe* recipe = nullptr;
    size_t step_index = 0;
    bool seeded;
};

uint32_t parse_bits(const wchar_t* text) {
    return static_cast<uint32_t>(wcstoul(text, nullptr, 0));
}

uint32_t parse_tick(const wchar_t* text) {
    return static_cast<uint32_t>(wcstoul(text, nullptr, 10));
}

void add_step(Recipe& recipe, StepKind kind, uint32_t bits, uint32_t end_tick) {
    if ((kind == StepWait || kind == StepPrint || kind == StepStop) && end_tick == 0)
        return;
    recipe.steps.push_back({kind, bits, end_tick});
}

void load_recipe(Recipe& recipe, const wchar_t* path) {
    FILE* file = _wfopen(path, L"r");
    if (file == nullptr) return;
    char line[256];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        char kind[8] = {};
        unsigned bits = 0;
        unsigned end_tick = 0;
        if (std::sscanf(line, "seed %x", &bits) == 1) {
            recipe.seed_set = true;
            recipe.seed = bits;
        } else if (std::sscanf(line, "%7s %x %u", kind, &bits, &end_tick) >= 2) {
            if (std::strcmp(kind, "i") == 0)
                add_step(recipe, StepInput, bits, 0);
            else if (std::strcmp(kind, "w") == 0)
                add_step(recipe, StepWait, bits, end_tick);
            else if (std::strcmp(kind, "p") == 0)
                add_step(recipe, StepPrint, 0, end_tick);
            else if (std::strcmp(kind, "s") == 0)
                add_step(recipe, StepStop, 0, end_tick);
        }
    }
    std::fclose(file);
}

void save_recipe(const Recipe& recipe, const wchar_t* path) {
    FILE* file = _wfopen(path, L"w");
    if (file == nullptr) return;
    if (recipe.seed_set) std::fprintf(file, "seed %04x\n", recipe.seed);
    for (const Step& step : recipe.steps) {
        if (step.kind == StepInput)
            std::fprintf(file, "i %08x\n", step.bits);
        else if (step.kind == StepWait)
            std::fprintf(file, "w %08x %u\n", step.bits, step.end_tick);
        else
            std::fprintf(file, "%c 0 %u\n", static_cast<char>(step.kind),
                         step.end_tick);
    }
    std::fclose(file);
}

void parse_recipe(Recipe& recipe, int argc, wchar_t** argv,
                  std::vector<std::wstring>& original_args,
                  std::vector<std::wstring>& candidate_args,
                  std::wstring& original_user,
                  std::wstring& candidate_user) {
    wchar_t appdata[520];
    GetEnvironmentVariableW(L"APPDATA", appdata, 520);
    original_user = appdata;
    candidate_user = appdata;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--original") == 0) {
            original_args.push_back(argv[++i]);
        } else if (wcscmp(argv[i], L"--candidate") == 0) {
            candidate_args.push_back(argv[++i]);
        } else if (wcscmp(argv[i], L"--original-user-dir") == 0 && i + 1 < argc) {
            original_user = argv[++i];
        } else if (wcscmp(argv[i], L"--candidate-user-dir") == 0 && i + 1 < argc) {
            candidate_user = argv[++i];
        } else if (wcscmp(argv[i], L"--compare-seed") == 0 && i + 1 < argc) {
            recipe.seed_set = true;
            recipe.seed = parse_bits(argv[++i]);
        } else if (wcscmp(argv[i], L"--compare-load") == 0 && i + 1 < argc) {
            load_recipe(recipe, argv[++i]);
        } else if (wcscmp(argv[i], L"--compare-save") == 0 && i + 1 < argc) {
            save_recipe(recipe, argv[++i]);
        } else if (wcscmp(argv[i], L"--compare-step") == 0 && i + 1 < argc) {
            const wchar_t* kind = argv[i + 1];
            if (wcscmp(kind, L"i") == 0 && i + 2 < argc) {
                add_step(recipe, StepInput, parse_bits(argv[i + 2]), 0);
                i += 2;
            } else if (wcscmp(kind, L"w") == 0 && i + 3 < argc) {
                add_step(recipe, StepWait, parse_bits(argv[i + 2]),
                         parse_tick(argv[i + 3]));
                i += 3;
            } else if (wcscmp(kind, L"p") == 0 && i + 2 < argc) {
                add_step(recipe, StepPrint, 0, parse_tick(argv[i + 2]));
                i += 2;
            } else if (wcscmp(kind, L"s") == 0 && i + 2 < argc) {
                add_step(recipe, StepStop, 0, parse_tick(argv[i + 2]));
                i += 2;
            }
        }
    }
    if (recipe.seed_set) {
        std::vector<Step> with_seed;
        with_seed.push_back({StepInput, recipe.seed, 0});
        with_seed.insert(with_seed.end(), recipe.steps.begin(), recipe.steps.end());
        recipe.steps.swap(with_seed);
    }
}

std::wstring absolute_path(const std::wstring& path) {
    wchar_t full[520];
    if (GetFullPathNameW(path.c_str(), 520, full, nullptr) == 0) return path;
    return full;
}

void make_user_tree(const std::wstring& root) {
    SHCreateDirectoryExW(nullptr, root.c_str(), nullptr);
    wchar_t replay[600];
    std::swprintf(replay, 600, L"%s\\ShanghaiAlice\\th18\\replay", root.c_str());
    SHCreateDirectoryExW(nullptr, replay, nullptr);
    wchar_t snapshot[600];
    std::swprintf(snapshot, 600, L"%s\\ShanghaiAlice\\th18\\snapshot", root.c_str());
    SHCreateDirectoryExW(nullptr, snapshot, nullptr);
}

bool launch(Process& process, const std::vector<std::wstring>& arguments,
            const std::wstring& user_root, bool debug) {
    std::wstring command;
    for (size_t i = 0; i < arguments.size(); ++i) {
        if (i != 0) command += L" ";
        command += L"\"";
        command += arguments[i];
        command += L"\"";
    }
    make_user_tree(user_root);
    CreateEnvironmentBlock(&process.environment, nullptr, FALSE);
    if (process.environment == nullptr) return false;
    wchar_t* cursor = static_cast<wchar_t*>(process.environment);
    std::vector<wchar_t> rebuilt;
    while (*cursor != L'\0') {
        const size_t length = wcslen(cursor) + 1;
        if (_wcsnicmp(cursor, L"APPDATA=", 8) == 0) {
            std::wstring item = L"APPDATA=" + user_root;
            rebuilt.insert(rebuilt.end(), item.begin(), item.end());
            rebuilt.push_back(L'\0');
        } else {
            rebuilt.insert(rebuilt.end(), cursor, cursor + length);
        }
        cursor += length;
    }
    rebuilt.push_back(L'\0');
    const DWORD flags = CREATE_UNICODE_ENVIRONMENT |
                        (debug ? DEBUG_PROCESS : CREATE_NO_WINDOW);
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, flags,
                        rebuilt.data(), nullptr, &process.start, &process.info)) {
        std::fwprintf(stderr, L"CreateProcess failed: %lu\n", GetLastError());
        DestroyEnvironmentBlock(process.environment);
        process.environment = nullptr;
        return false;
    }
    DestroyEnvironmentBlock(process.environment);
    process.environment = nullptr;
    return true;
}

void open_channel(Process& process) {
    process.ready = CreateEventW(nullptr, FALSE, FALSE,
                                 L"Local\\TH18CompareCandidateReady");
    process.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                         PAGE_READWRITE, 0, sizeof(Snapshot),
                                         L"Local\\TH18CompareCandidate");
    if (process.ready == nullptr || process.mapping == nullptr) return;
    process.snapshot = static_cast<Snapshot*>(MapViewOfFile(
        process.mapping, FILE_MAP_WRITE, 0, 0, sizeof(Snapshot)));
}

struct WindowFind {
    DWORD process_id;
    HWND window;
};

BOOL CALLBACK find_window(HWND window, LPARAM parameter) {
    WindowFind& find = *reinterpret_cast<WindowFind*>(parameter);
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id != find.process_id) return TRUE;
    wchar_t class_name[16];
    if (!GetClassNameW(window, class_name, 16) ||
        wcscmp(class_name, L"TH18") != 0)
        return TRUE;
    find.window = window;
    return FALSE;
}

HWND find_game_window(const Process& process) {
    WindowFind find{process.info.dwProcessId, nullptr};
    EnumWindows(find_window, reinterpret_cast<LPARAM>(&find));
    return find.window;
}

uint32_t read_u32(HANDLE process, uint32_t address) {
    uint32_t value = 0;
    SIZE_T read = 0;
    ReadProcessMemory(process, reinterpret_cast<void*>(address), &value,
                      sizeof(value), &read);
    return value;
}

uint64_t read_u64(HANDLE process, uint32_t value) { return value; }

uint64_t read_float(HANDLE process, uint32_t address) {
    float value = 0.0f;
    SIZE_T read = 0;
    ReadProcessMemory(process, reinterpret_cast<void*>(address), &value,
                      sizeof(value), &read);
    uint64_t result = 0;
    std::memcpy(&result, &value, sizeof(value));
    return result;
}

void fill_snapshot(Target& target, Snapshot& snapshot) {
    HANDLE process = target.process.info.hProcess;
    snapshot.forced_bits = target.forced_bits;
    snapshot.field_count = kCompareFieldCount;
    snapshot.fields[FieldTick] = read_u64(process, target.tick);
    snapshot.fields[FieldState] = read_u64(process, read_u32(process, kAppState));
    snapshot.fields[FieldNextState] = read_u64(process, read_u32(process, kAppNextState));
    snapshot.fields[FieldStage] = read_u64(process, read_u32(process, kStatusCurrentStage));
    snapshot.fields[FieldCurrentStage] = read_u64(process, read_u32(process, kStatusCurrentStage));
    snapshot.fields[FieldStartStage] = read_u64(process, read_u32(process, kStatusStartStage));
    snapshot.fields[FieldPlayer] = read_u64(process, read_u32(process, kStatusPlayer));
    snapshot.fields[FieldShot] = read_u64(process, read_u32(process, kStatusShot));
    snapshot.fields[FieldDifficulty] = read_u64(process, read_u32(process, kStatusDifficulty));
    snapshot.fields[FieldInputHeld] = read_u64(process, read_u32(process, kInput));
    snapshot.fields[FieldInputPressed] = read_u64(process, read_u32(process, kInput + 0x0c));
    snapshot.fields[FieldBattleHeld] = read_u64(process, read_u32(process, kInput) & 0xffff);
    snapshot.fields[FieldBattlePressed] = read_u64(process, read_u32(process, kInput + 0x0c) & 0xffff);
    snapshot.fields[FieldRngState] = read_u64(process, read_u32(process, kRng) & 0xffff);
    snapshot.fields[FieldRngCounter] = read_u64(process, read_u32(process, kRng + 4));
    snapshot.fields[FieldCardRngState] = read_u64(process, read_u32(process, kCardRng) & 0xffff);
    snapshot.fields[FieldCardRngCounter] = read_u64(process, read_u32(process, kCardRng + 4));
    snapshot.fields[FieldScore] = read_u64(process, read_u32(process, kStatusScore));
    snapshot.fields[FieldMoney] = read_u64(process, read_u32(process, kStatusMoney));
    snapshot.fields[FieldPower] = read_u64(process, read_u32(process, kStatusPower));
    snapshot.fields[FieldLives] = read_u64(process, read_u32(process, kStatusLives));
    const uint32_t player = read_u32(process, kPlayerPointer);
    if (player != 0) {
        snapshot.fields[FieldPlayerX] = read_float(process, player + kPlayerX);
        snapshot.fields[FieldPlayerY] = read_float(process, player + kPlayerY);
        snapshot.fields[FieldPlayerState] = read_u64(process, read_u32(process, player + kPlayerState));
    }
    std::snprintf(snapshot.text, sizeof(snapshot.text),
        "ORIGINAL tick=%u state=%u next=%u stage=%u in=%08x/%08x "
        "rng=%04x:%u card=%04x:%u score=%u money=%d power=%d lives=%d",
        target.tick, static_cast<unsigned>(snapshot.fields[FieldState]),
        static_cast<unsigned>(snapshot.fields[FieldNextState]),
        static_cast<unsigned>(snapshot.fields[FieldStage]),
        static_cast<unsigned>(snapshot.fields[FieldInputHeld]),
        static_cast<unsigned>(snapshot.fields[FieldInputPressed]),
        static_cast<unsigned>(snapshot.fields[FieldRngState]),
        static_cast<unsigned>(snapshot.fields[FieldRngCounter]),
        static_cast<unsigned>(snapshot.fields[FieldCardRngState]),
        static_cast<unsigned>(snapshot.fields[FieldCardRngCounter]),
        static_cast<unsigned>(snapshot.fields[FieldScore]),
        static_cast<int>(snapshot.fields[FieldMoney]),
        static_cast<int>(snapshot.fields[FieldPower]),
        static_cast<int>(snapshot.fields[FieldLives]));
}

void print_text(const char* label, const Snapshot& snapshot) {
    std::printf("%s", snapshot.text);
    float x;
    float y;
    std::memcpy(&x, &snapshot.fields[FieldPlayerX], sizeof(x));
    std::memcpy(&y, &snapshot.fields[FieldPlayerY], sizeof(y));
    std::printf(" px=%.9g py=%.9g pstate=%u\n", x, y,
                static_cast<unsigned>(snapshot.fields[FieldPlayerState]));
    std::fflush(stdout);
}

void advance_to_input(Target& target, const Recipe& recipe) {
    target.forced_bits = 0;
    while (target.step_index < recipe.steps.size()) {
        const Step& step = recipe.steps[target.step_index];
        if (step.kind != StepInput && step.kind != StepWait) break;
        if (step.kind == StepInput) {
            target.forced_bits = step.bits;
            ++target.step_index;
            return;
        }
        if (target.tick < step.end_tick) {
            target.forced_bits = step.bits;
            return;
        }
        ++target.step_index;
    }
}

bool boundary_actions(Target& target, const Recipe& recipe) {
    bool print = false;
    while (target.step_index < recipe.steps.size()) {
        const Step& step = recipe.steps[target.step_index];
        if ((step.kind == StepPrint || step.kind == StepStop) &&
            target.tick < step.end_tick)
            break;
        if (step.kind == StepPrint) print = true;
        if (step.kind == StepStop) {
            target.thread.stop = StopBoundary;
            ++target.step_index;
            return true;
        }
        ++target.step_index;
    }
    return print;
}

void set_software_hook(Target& target, void* address, uint8_t original) {
    SIZE_T written = 0;
    const uint8_t breakpoint = 0xcc;
    WriteProcessMemory(target.process.info.hProcess, address, &breakpoint, 1, &written);
    FlushInstructionCache(target.process.info.hProcess, address, 1);
}

void clear_software_hook(Target& target, void* address, uint8_t original) {
    SIZE_T written = 0;
    WriteProcessMemory(target.process.info.hProcess, address, &original, 1, &written);
    FlushInstructionCache(target.process.info.hProcess, address, 1);
}

bool resume_single_step(Target& target, uint32_t address) {
    CONTEXT context = {};
    context.ContextFlags = CONTEXT_CONTROL;
    if (!GetThreadContext(target.process.info.hThread, &context)) return false;
    context.Eip = address;
    context.EFlags |= 0x100;
    return SetThreadContext(target.process.info.hThread, &context) != FALSE;
}

void continue_thread(Target& target, const DEBUG_EVENT& event, DWORD status) {
    ContinueDebugEvent(event.dwProcessId, event.dwThreadId, status);
}

void store_u32(HANDLE process, uint32_t address, uint32_t value) {
    SIZE_T written = 0;
    WriteProcessMemory(process, reinterpret_cast<void*>(address), &value,
                       sizeof(value), &written);
}

void handle_breakpoint(Target& target, const DEBUG_EVENT& event) {
    const Recipe& recipe = *target.recipe;
    const uint32_t address = reinterpret_cast<uint32_t>(
        event.u.Exception.ExceptionRecord.ExceptionAddress);
    if (address == reinterpret_cast<uint32_t>(target.input_hook.address)) {
        CONTEXT context = {};
        context.ContextFlags = CONTEXT_INTEGER | CONTEXT_CONTROL;
        if (GetThreadContext(target.process.info.hThread, &context) &&
            context.Ecx == kInput) {
            store_u32(target.process.info.hProcess, kInput, target.forced_bits);
        }
        clear_software_hook(target, target.input_hook.address,
                            target.input_hook.original);
        if (resume_single_step(target, address))
            target.thread.stop = StopException;
        return;
    }
    if (address == reinterpret_cast<uint32_t>(target.boundary_hook.address)) {
        ++target.tick;
        const bool print = boundary_actions(target, recipe);
        if (print && target.process.snapshot != nullptr) {
            fill_snapshot(target, *target.process.snapshot);
            print_text("ORIGINAL", *target.process.snapshot);
        }
        if (target.thread.stop == StopBoundary) target.halted = true;
        advance_to_input(target, recipe);
        clear_software_hook(target, target.boundary_hook.address,
                            target.boundary_hook.original);
        if (resume_single_step(target, address))
            target.thread.stop = StopException;
        if (target.thread.stop == StopException) target.thread.stop = StopBoundary;
    }
}

void handle_single_step(Target& target, const DEBUG_EVENT& event) {
    if (target.thread.stop != StopException) return;
    set_software_hook(target, target.input_hook.address,
                      target.input_hook.original);
    if (!target.halted) {
        set_software_hook(target, target.boundary_hook.address,
                          target.boundary_hook.original);
    }
    target.thread.stop = StopNone;
}

void configure_hardware(Target& target) {
    CONTEXT context = {};
    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(target.process.info.hThread, &context)) return;
    context.Dr0 = kInputUpdate;
    context.Dr1 = kSchedulerReturn;
    context.Dr7 = (context.Dr7 & ~0x00003303u) | 0x00000101u;
    SetThreadContext(target.process.info.hThread, &context);
}

bool wait_channel(Target& target) {
    if (target.process.snapshot == nullptr) return true;
    return WaitForSingleObject(target.process.ready, INFINITE) == WAIT_OBJECT_0;
}

void continue_channel(Target& target) {
    if (target.process.snapshot == nullptr) return;
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE,
                              L"Local\\TH18CompareCandidateContinue");
    if (event != nullptr) {
        SetEvent(event);
        CloseHandle(event);
    }
}

void compare(const Snapshot& original, const Snapshot& candidate) {
    for (size_t i = 0; i < kCompareFieldCount; ++i) {
        if (original.fields[i] == candidate.fields[i]) continue;
        static const char* names[kCompareFieldCount] = {
            "tick", "state", "next_state", "stage", "current_stage", "start_stage",
            "player", "shot", "difficulty", "input_held", "input_pressed",
            "battle_held", "battle_pressed", "rng_state", "rng_counter",
            "card_rng_state", "card_rng_counter", "score", "money", "power",
            "lives", "player_x", "player_y", "player_state",
        };
        std::printf("FIRST_DIFF tick=%u field=%s original=%llu candidate=%llu\n",
                    static_cast<unsigned>(original.fields[FieldTick]), names[i],
                    static_cast<unsigned long long>(original.fields[i]),
                    static_cast<unsigned long long>(candidate.fields[i]));
        std::fflush(stdout);
        return;
    }
}

void debug_loop(Target& target, const Recipe& recipe) {
    DEBUG_EVENT event;
    for (;;) {
        if (target.thread.stop == StopBoundary) {
            const int command = _getch();
            if (command == 'q' || command == 'Q') {
                if (target.process.window == nullptr)
                    target.process.window = find_game_window(target.process);
                if (target.process.window != nullptr)
                    PostMessageW(target.process.window, WM_CLOSE, 0, 0);
                target.thread.stop = StopNone;
            } else if (command == 'r' || command == 'R') {
                Snapshot snapshot;
                fill_snapshot(target, snapshot);
                print_text("ORIGINAL", snapshot);
            }
            continue;
        }
        if (!WaitForDebugEvent(&event, 1000)) continue;
        DWORD status = DBG_CONTINUE;
        if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            const DWORD code = event.u.Exception.ExceptionRecord.ExceptionCode;
            if (code == STATUS_SINGLE_STEP) {
                handle_single_step(target, event);
            } else if (code == STATUS_BREAKPOINT && event.u.Exception.dwFirstChance) {
                handle_breakpoint(target, event);
            } else {
                status = DBG_EXCEPTION_NOT_HANDLED;
            }
        } else if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
            continue_thread(target, event, DBG_CONTINUE);
            DWORD exit_code = 0;
            GetExitCodeProcess(target.process.info.hProcess, &exit_code);
            std::printf("original exit=%lu\n", exit_code);
            std::fflush(stdout);
            return;
        } else if (event.dwDebugEventCode == CREATE_THREAD_DEBUG_EVENT) {
            target.thread.id = event.dwThreadId;
            target.thread.handle = event.u.CreateThread.hThread;
            configure_hardware(target);
        }
        continue_thread(target, event, status);
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::fwprintf(stderr,
        L"usage: compare18.exe --original EXE [args] --candidate EXE [args] "
        L"--original-user-dir DIR --candidate-user-dir DIR "
        L"[--compare-seed N] [--compare-load FILE] [--compare-save FILE] "
        L"[--compare-step i BITS|w BITS TICK|p TICK|s TICK]\n");

    Recipe recipe{};
    std::vector<std::wstring> original_args;
    std::vector<std::wstring> candidate_args;
    std::wstring original_user;
    std::wstring candidate_user;
    parse_recipe(recipe, argc, argv, original_args, candidate_args,
                 original_user, candidate_user);
    if (original_args.empty() || candidate_args.empty()) return 2;
    original_user = absolute_path(original_user);
    candidate_user = absolute_path(candidate_user);

    Target original;
    Target candidate;
    original.recipe = &recipe;
    candidate.recipe = &recipe;
    if (!launch(original.process, original_args, original_user, true) ||
        !launch(candidate.process, candidate_args, candidate_user, false))
        return 1;
    open_channel(candidate.process);
    original.input_hook.address = reinterpret_cast<void*>(kInputUpdate);
    original.boundary_hook.address = reinterpret_cast<void*>(kSchedulerReturn);
    ReadProcessMemory(original.process.info.hProcess, original.input_hook.address,
                      &original.input_hook.original, 1, nullptr);
    ReadProcessMemory(original.process.info.hProcess, original.boundary_hook.address,
                      &original.boundary_hook.original, 1, nullptr);
    set_software_hook(original, original.input_hook.address,
                      original.input_hook.original);
    set_software_hook(original, original.boundary_hook.address,
                      original.boundary_hook.original);
    advance_to_input(original, recipe);

    DEBUG_EVENT event;
    for (;;) {
        if (!WaitForDebugEvent(&event, 1000)) continue;
        DWORD status = DBG_CONTINUE;
        if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            const DWORD code = event.u.Exception.ExceptionRecord.ExceptionCode;
            if (code == STATUS_BREAKPOINT && event.u.Exception.dwFirstChance) {
                handle_breakpoint(original, event);
        } else if (code == STATUS_SINGLE_STEP) {
                handle_single_step(original, event);
            } else {
                status = DBG_EXCEPTION_NOT_HANDLED;
            }
        } else if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
            ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE);
            break;
        }
        ContinueDebugEvent(event.dwProcessId, event.dwThreadId, status);
    }
    CloseHandle(original.process.info.hThread);
    CloseHandle(original.process.info.hProcess);
    CloseHandle(candidate.process.info.hThread);
    CloseHandle(candidate.process.info.hProcess);
    return 0;
}
