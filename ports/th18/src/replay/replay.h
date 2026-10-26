#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/frame_input.h"
#include "storage/storage.h"

// FUN_004615e0 / 00462680 / 00461e90. All offsets are file offsets.
#pragma pack(push, 1)
struct ReplayFileHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved_06;
    uint32_t reserved_08;
    uint32_t user_offset;
    uint32_t flags;
    uint32_t reserved_14;
    uint32_t reserved_18;
    uint32_t compressed_size;
    uint32_t decoded_size;
};

struct ReplayInfo {
    char name[9];
    uint8_t reserved_09;
    uint8_t mode_flags;             // bit 0 practice, bit 1 spell practice
    uint8_t reserved_0b[5];
    int64_t date;
    uint32_t score;
    ConfigBlob config;
    float slow_rate;
    uint32_t stage_count;
    int32_t player;
    int32_t shot;
    int32_t difficulty;
    int32_t last_stage;             // 1..7 unfinished, 8 clear
    int32_t continues;
    int32_t spell_id;
    uint32_t reserved_c4;
};

struct ReplayStageHeader {
    uint16_t stage;
    uint16_t seed;
    uint32_t frame_count;
    uint32_t data_size;
    int32_t x_sub;
    int32_t y_sub;
    int32_t focus;
    uint32_t spell_times[20];
    uint8_t initial_status[0xfc];
    int32_t initial_cards[256];
    int32_t initial_values[256];
    int32_t initial_selected;
    uint8_t shop_status[0xfc];
    int32_t shop_cards[256];
    int32_t shop_values[256];
    int32_t shop_selected;
    uint32_t flags;
};

struct ReplayFrame {
    uint16_t held;
    uint16_t pressed;
    uint16_t released;
};
#pragma pack(pop)

struct ReplayEntry {
    int32_t slot;                   // 0..24 official; >=25 enumerated user files
    std::string filename;
    ReplayInfo info;
    bool stage_present[8]{};
    uint32_t stage_scores[8]{};
    int32_t stage_continues[8]{};
};

enum class ReplayMode { None, Record, Playback };

std::vector<ReplayEntry> replay_list();
// from_dat selects demo1.rpy..demo4.rpy; other names are below user-dir/replay.
bool replay_load(const char* name, bool from_dat);
void replay_begin_record(const ConfigBlob& config);
void replay_close();
ReplayMode replay_mode();
const ReplayInfo& replay_info();
bool replay_has_stage(int32_t stage);
const ReplayStageHeader& replay_stage_header(int32_t stage);

// After player construction, before cards/stage scripts consume random numbers.
// Playback restores status early only for its first stage (004615e0).
void replay_begin_stage(int32_t stage);
const int32_t* replay_stage_cards();
int32_t replay_stage_card_count();
// 00462d20: after initial cards are installed with flags=1, before STD creation
// and card_manager_stage_start. Capture/restore card values and selected card.
// Clears battle input and leaves replay input disabled until start_stage.
void replay_prepare_stage_cards();
// 00462ea0: after STD, card slot13/slot0 and Enemy/ECL resource creation;
// before creating the "main" enemy and before the first gameplay frame.
// Capture/restore the resulting status and player position, then enable frame 0.
// Resets battle input only; physical input belongs to the platform/menu path.
void replay_start_stage();

// Playback A0x11 only, outside shop/pause. True supplies one actual input frame;
// false means not ready, terminator, or exhausted: do not advance battle input.
bool replay_next_input(uint32_t& held);
// Record A0x11 updates battle input from physical held's low 16 bits first,
// then records it outside shop/pause. Physical input never passes through Replay.
void replay_record_input(const FrameInput& input, float fps);
bool replay_finished();
// Original FPS pointer moves to the next sample after frame 0,30,... . The last
// stage can address a byte outside the decoded file; that byte has no value here.
bool replay_display_fps(uint8_t& fps);

// FUN_00417880: recording captures the purchase result; playback restores it.
// Playback shop reaches this at timer 30 (FUN_00417cc0).
// The common stage exit path calls card_manager_shop_exit once after this.
void replay_shop_result();
// FUN_00473390 stores the spell timing value in this existing format field.
uint32_t replay_spell_time(int32_t index, uint32_t value);
// 00461e40 / save-menu selection: update metadata without stopping recording.
// 00461e90 appends the partial-run terminator only on the first actual save.
void replay_finish_run(bool cleared, float slow_rate);
bool replay_save(int32_t slot, const char* name);
