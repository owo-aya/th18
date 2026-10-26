#pragma once
#include <cstddef>
#include <cstdint>

constexpr uint32_t kConfigSize = 0x88;
constexpr uint32_t kConfigVersion = 0x180002;
constexpr uint32_t kReplaySlotCount = 25;
constexpr uint32_t kReplayUserSlotCount = 50;

// th18.cfg: FUN_00444dd0 defaults / FUN_004543a0 load / FUN_004561e0 save.
// +04/+18/+2c: DInput / XInput / keyboard uint16_t[10] mappings.
// +40/+42: joystick X/Y threshold; +44: color format; +45: BGM mode;
// +46: SE enabled; +47: display mode (0..2 fullscreen, 3..7 window, 8..9 borderless);
// +48: frame skip; +49: presentation interval; +4a/+4b: BGM/SE volume (0..100);
// +4c/+4d: timing options; +4e: display scale; +50: flags; +54/+58: window X/Y.
struct ConfigBlob {
    uint8_t data[kConfigSize];
};

// All writes use this rebuilt-game directory. Empty selects the exe directory.
void storage_set_user_dir(const char* dir);
const char* storage_user_dir();
const char* storage_user_path(char* buf, size_t cap, const char* name);
void storage_set_game_dir(const char* dir);
const char* storage_game_dir();

// Load once before window/input/audio creation. Missing rebuilt data imports
// the original APPDATA copy read-only; saves use only user-dir.
void storage_config_init();
ConfigBlob& storage_config();
bool storage_config_load(ConfigBlob& out);
bool storage_config_save(const ConfigBlob& cfg);
bool storage_config_save();

// Official slots only; user replay filenames come from directory enumeration.
void storage_replay_name(char* buf, size_t cap, int32_t index);
void storage_log(const char* fmt, ...);
