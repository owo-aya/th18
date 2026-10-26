// 声音与 BGM 播放实现（原版 sub_476410 / sub_452cb0 / sub_4546a0 / sub_48aba0 / sub_4895a0）。
// TH18 的 thbgm.fmt 里 preload_size == track_size，即整曲预载；
// 因此播放器把整条曲目读进一个静态 DirectSound 次缓冲，播到末尾时把播放位置
// 拉回 intro_size（循环点），与原来的流式缓冲效果一致。
#include <windows.h>
#include <mmreg.h>
#define DIRECTSOUND_VERSION 0x0800
#include <dsound.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "platform/gfx.h"
#include "gameplay/score_data.h"
#include "presentation/sound.h"
#include "resources/bgm_file.h"
#include "resources/dat.h"

namespace {

constexpr int kMaxTracks = 32;

struct BgmPlayer {
    IDirectSoundBuffer* buffer;   // 当前曲目缓冲（整曲）
    HANDLE file;                  // thbgm.dat
    HANDLE thread;                // 轮询播放位置的线程
    BgmFmtEntry tracks[kMaxTracks];
    int track_count;
    int track;                    // 当前曲目下标；-1 = 停止
    int loop_start;               // 循环点（字节，相对曲目起点）
    int track_end;                // 曲目结束（字节，相对曲目起点）
    int volume;                   // 0..100
    int fade_remaining;           // 原版 CStreamingSound+0x18
    int fade_total;               // +0x1c
    bool fading;                  // +0x20 == 1
    volatile LONG paused;          // 原kind6保存播放态，kind7只恢复此前播放中的缓冲。
    volatile LONG quit;
};

IDirectSound8* g_ds = nullptr;
IDirectSoundBuffer* g_primary = nullptr;
BgmPlayer g_bgm = {};
bool g_bgm_enabled = true;

// 原版 0x4b47a0 资源表和 0x4c9b80 编号表；编号表按逻辑 ID 重排。
constexpr const char* kSeResources[72] = {
    "se_plst00.wav",
    "se_enep00.wav",
    "se_pldead00.wav",
    "se_power0.wav",
    "se_power1.wav",
    "se_tan00.wav",
    "se_tan01.wav",
    "se_tan02.wav",
    "se_ok00.wav",
    "se_cancel00.wav",
    "se_select00.wav",
    "se_gun00.wav",
    "se_cat00.wav",
    "se_lazer00.wav",
    "se_lazer01.wav",
    "se_enep01.wav",
    "se_damage00.wav",
    "se_item00.wav",
    "se_kira00.wav",
    "se_kira01.wav",
    "se_kira02.wav",
    "se_timeout.wav",
    "se_graze.wav",
    "se_powerup.wav",
    "se_pause.wav",
    "se_cardget.wav",
    "se_damage01.wav",
    "se_timeout2.wav",
    "se_invalid.wav",
    "se_slash.wav",
    "se_ch00.wav",
    "se_ch01.wav",
    "se_extend.wav",
    "se_cardget.wav",
    "se_nep00.wav",
    "se_bonus.wav",
    "se_bonus2.wav",
    "se_enep02.wav",
    "se_lazer02.wav",
    "se_nodamage.wav",
    "se_boon00.wav",
    "se_don00.wav",
    "se_boon01.wav",
    "se_ch02.wav",
    "se_ch03.wav",
    "se_extend2.wav",
    "se_pin00.wav",
    "se_pin01.wav",
    "se_lgods1.wav",
    "se_lgods2.wav",
    "se_lgods3.wav",
    "se_lgods4.wav",
    "se_lgodsget.wav",
    "se_msl.wav",
    "se_msl2.wav",
    "se_pldead01.wav",
    "se_heal.wav",
    "se_msl3.wav",
    "se_fault.wav",
    "se_noise.wav",
    "se_etbreak.wav",
    "se_tan03.wav",
    "se_wolf.wav",
    "se_bonus4.wav",
    "se_big.wav",
    "se_item01.wav",
    "se_release.wav",
    "se_changeitem.wav",
    "se_trophy.wav",
    "se_warpl.wav",
    "se_warpr.wav",
    "se_notice.wav",
};
struct SeDefinition {
    int resource;
    LONG volume;
    DWORD play_flags;
};
constexpr SeDefinition kSeDefinitions[84] = {
    {0, -1900, 0}, // 0
    {0, -200, 0}, // 1
    {2, -1100, 0}, // 2
    {1, -1200, 0}, // 3
    {1, -1500, 0}, // 4
    {15, -900, 0}, // 5
    {37, -500, 0}, // 6
    {8, -500, 0}, // 7
    {8, -300, 0}, // 8
    {9, -400, 0}, // 9
    {10, -800, 0}, // 10
    {21, -500, 0}, // 11
    {27, -300, 0}, // 12
    {23, -100, 0}, // 13
    {24, -800, 0}, // 14
    {25, -800, 0}, // 15
    {28, 0, 0}, // 16
    {32, -100, 0}, // 17
    {13, -1300, 0}, // 18
    {14, -1400, 0}, // 19
    {38, -3000, 1}, // 20
    {5, -1900, 0}, // 21
    {6, -2200, 0}, // 22
    {7, -2400, 0}, // 23
    {5, -300, 0}, // 24
    {6, -1800, 0}, // 25
    {7, -1800, 0}, // 26
    {5, -1100, 0}, // 27
    {3, -700, 0}, // 28
    {4, -700, 0}, // 29
    {30, -300, 0}, // 30
    {31, -300, 0}, // 31
    {11, -1500, 0}, // 32
    {12, -300, 0}, // 33
    {16, -880, 0}, // 34
    {26, -500, 0}, // 35
    {39, -880, 0}, // 36
    {17, -1500, 0}, // 37
    {18, -1100, 0}, // 38
    {19, -1300, 0}, // 39
    {20, -1500, 0}, // 40
    {18, -500, 0}, // 41
    {22, -600, 0}, // 42
    {22, -700, 0}, // 43
    {29, 0, 0}, // 44
    {29, -600, 0}, // 45
    {33, 0, 0}, // 46
    {35, 0, 0}, // 47
    {36, 0, 0}, // 48
    {34, -200, 0}, // 49
    {40, -500, 0}, // 50
    {41, 0, 0}, // 51
    {42, 0, 0}, // 52
    {42, -300, 0}, // 53
    {43, -300, 0}, // 54
    {44, -1500, 1}, // 55
    {45, 0, 0}, // 56
    {46, -200, 0}, // 57
    {47, -200, 0}, // 58
    {48, -500, 0}, // 59
    {49, -500, 0}, // 60
    {50, -500, 0}, // 61
    {51, 0, 0}, // 62
    {52, 0, 0}, // 63
    {53, -900, 0}, // 64
    {54, -900, 0}, // 65
    {55, 0, 0}, // 66
    {56, 0, 0}, // 67
    {57, -2500, 0}, // 68
    {58, -500, 0}, // 69
    {59, 0, 0}, // 70
    {60, -400, 0}, // 71
    {61, -500, 0}, // 72
    {62, -500, 0}, // 73
    {63, 0, 0}, // 74
    {64, 0, 0}, // 75
    {65, 0, 0}, // 76
    {66, 0, 0}, // 77
    {67, 0, 0}, // 78
    {68, 0, 0}, // 79
    {70, 0, 0}, // 80
    {69, 0, 0}, // 81
    {68, -500, 0}, // 82
    {71, 0, 0}, // 83
};
struct SePlayer {
    IDirectSoundBuffer* buffer;
    bool resume;
};
struct SeRequest {
    int id;
    int count; // -1 = 停止；正值 = 合并的声像数。
    int pan_sum;
};
SePlayer g_se[84];
SeRequest g_se_requests[12];
int g_se_request_count;
int g_se_volume = 100;
bool g_se_enabled = true;

// sub_476830：按 RIFF chunk 长度走到所需块。
const uint8_t* wave_chunk(const uint8_t* wave, const char* name) {
    const uint8_t* chunk = wave + 12;
    while (std::memcmp(chunk, name, 4) != 0)
        chunk += 8 + *reinterpret_cast<const uint32_t*>(chunk + 4);
    return chunk + 8;
}

// sub_4776f0：同资源不同编号用 DuplicateSoundBuffer，播放状态各自独立。
void load_se() {
    for (int id = 0; id < 84; ++id) {
        int previous = 0;
        while (previous < id && kSeDefinitions[previous].resource != kSeDefinitions[id].resource)
            ++previous;
        if (previous != id) {
            g_ds->DuplicateSoundBuffer(g_se[previous].buffer, &g_se[id].buffer);
            continue;
        }
        size_t size;
        auto* wave = static_cast<uint8_t*>(dat_read(kSeResources[kSeDefinitions[id].resource], &size));
        WAVEFORMATEX format = {};
        std::memcpy(&format, wave_chunk(wave, "fmt "), 16);
        const uint8_t* pcm = wave_chunk(wave, "data");
        const DWORD pcm_size = *reinterpret_cast<const uint32_t*>(pcm - 4);
        DSBUFFERDESC desc = {};
        desc.dwSize = sizeof(desc);
        desc.dwFlags = DSBCAPS_LOCSOFTWARE | DSBCAPS_CTRLPAN | DSBCAPS_CTRLVOLUME | DSBCAPS_GLOBALFOCUS;
        desc.dwBufferBytes = pcm_size;
        desc.lpwfxFormat = &format;
        g_ds->CreateSoundBuffer(&desc, &g_se[id].buffer, nullptr);
        void* destination;
        DWORD bytes;
        g_se[id].buffer->Lock(0, pcm_size, &destination, &bytes, nullptr, nullptr, 0);
        std::memcpy(destination, pcm, pcm_size);
        g_se[id].buffer->Unlock(destination, bytes, nullptr, 0);
        std::free(wave);
    }
}

// sub_4775e3：SE 使用原版三次曲线及每个编号的基础音量。
LONG se_volume_db(int id) {
    if (g_se_volume == 0) return DSBVOLUME_MIN;
    const float amount = 1.0f - g_se_volume / 100.0f;
    return static_cast<LONG>((1.0f - amount * amount * amount) *
                             (kSeDefinitions[id].volume + 5000)) - 5000;
}

void queue_se(int id, int pan) {
    int index = 0;
    while (index < g_se_request_count && g_se_requests[index].id != id) ++index;
    if (index == g_se_request_count) {
        // 原版十二种编号的请求上限：按到达顺序占槽，没有优先级抢占。
        if (g_se_request_count == 12) return;
        g_se_requests[g_se_request_count++] = {id, 1, pan};
    } else {
        SeRequest& request = g_se_requests[index];
        if (request.count < 0 || request.count == 60) return;
        ++request.count;
        request.pan_sum += pan;
    }
}


// sub_452cb0: 读 thbgm.fmt。原版从 "../../bgm/thbgm.fmt" 读；
// 本发布的 thbgm.fmt 在 th18.dat 里，所以走 DAT。
void load_tracks() {
    size_t size = 0;
    void* data = dat_read("thbgm.fmt", &size);
    if (data == nullptr) {
        return;
    }
    int count = static_cast<int>(size / sizeof(BgmFmtEntry));
    if (count > kMaxTracks) {
        count = kMaxTracks;
    }
    std::memcpy(g_bgm.tracks, data, static_cast<size_t>(count) * sizeof(BgmFmtEntry));
    g_bgm.track_count = count;
    std::free(data);
}

// sub_48aba0: 打开 thbgm.dat（工作目录已由 platform 切到游戏目录）。
void open_dat() {
    g_bgm.file = CreateFileA("thbgm.dat", GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

// 把 DirectSound 音量单位（百分之一分贝）从 0..100 的百分比算出来。
LONG volume_db(int volume) {
    if (volume <= 0) {
        return DSBVOLUME_MIN;
    }
    if (volume >= 100) {
        return DSBVOLUME_MAX;
    }
    return static_cast<LONG>(2000.0 * std::log10(volume / 100.0));
}

// sub_4546a0 / sub_4895a0: 建整曲缓冲并开始播放。
void start_track(int track, double seconds) {
    if (g_ds == nullptr || track < 0 || track >= g_bgm.track_count) {
        return;
    }
    const BgmFmtEntry& entry = g_bgm.tracks[track];
    g_bgm.loop_start = entry.intro_size;
    g_bgm.track_end = entry.track_size;
    g_bgm.track = track;

    auto* pcm = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(entry.track_size)));
    if (pcm == nullptr) {
        return;
    }
    SetFilePointer(g_bgm.file, static_cast<LONG>(entry.track_offset), nullptr, FILE_BEGIN);
    DWORD read = 0;
    ReadFile(g_bgm.file, pcm, static_cast<DWORD>(entry.track_size), &read, nullptr);

    DSBUFFERDESC desc = {};
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DSBCAPS_CTRLVOLUME | DSBCAPS_STATIC;
    desc.dwBufferBytes = static_cast<DWORD>(entry.track_size);
    desc.lpwfxFormat = reinterpret_cast<WAVEFORMATEX*>(const_cast<BgmWaveFormat*>(&entry.wave_format));
    if (FAILED(g_ds->CreateSoundBuffer(&desc, &g_bgm.buffer, nullptr))) {
        std::free(pcm);
        return;
    }
    void* dst = nullptr;
    DWORD dst_bytes = 0;
    g_bgm.buffer->Lock(0, static_cast<DWORD>(entry.track_size), &dst, &dst_bytes, nullptr, nullptr, 0);
    std::memcpy(dst, pcm, static_cast<size_t>(entry.track_size));
    g_bgm.buffer->Unlock(dst, dst_bytes, nullptr, 0);
    std::free(pcm);

    g_bgm.buffer->SetVolume(volume_db(g_bgm.volume));
    // 48af10：按采样帧截断到字节位置，再减一个block；新曲seconds=0归零。
    int position = static_cast<int>(entry.wave_format.samples_per_sec * seconds) *
                   entry.wave_format.block_align - entry.wave_format.block_align;
    if (position < 0) position = 0;
    g_bgm.buffer->SetCurrentPosition(static_cast<DWORD>(position));
    g_bgm.buffer->Play(0, 0, 0);
}

// 播放位置到曲末时回到循环点（原版流式缓冲在区段边界做同样的事）。
void loop_update() {
    if (InterlockedCompareExchange(&g_bgm.paused, 0, 0) != 0) return;
    if (g_bgm.buffer == nullptr) {
        return;
    }
    DWORD play = 0;
    DWORD write = 0;
    g_bgm.buffer->GetCurrentPosition(&play, &write);
    if (play + 0x400 >= static_cast<DWORD>(g_bgm.track_end)) {
        g_bgm.buffer->SetCurrentPosition(static_cast<DWORD>(g_bgm.loop_start));
    }
}

DWORD WINAPI bgm_thread(LPVOID) {
    while (InterlockedCompareExchange(&g_bgm.quit, 0, 0) == 0) {
        loop_update();
        Sleep(5);
    }
    return 0;
}

}  // namespace

// sub_476410: CSound::Init。
void sound_init() {
    if (FAILED(DirectSoundCreate8(nullptr, &g_ds, nullptr))) {
        return;
    }
    g_ds->SetCooperativeLevel(gfx_hwnd(), DSSCL_PRIORITY);

    DSBUFFERDESC desc = {};
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DSBCAPS_PRIMARYBUFFER;
    g_ds->CreateSoundBuffer(&desc, &g_primary, nullptr);
    WAVEFORMATEX format = {WAVE_FORMAT_PCM, 2, 44100, 176400, 4, 16, 0};
    g_primary->SetFormat(&format);

    g_se_request_count = 0;
    load_se();

    g_bgm.track = -1;
    g_bgm.volume = 100;
    load_tracks();
    open_dat();
    if (g_bgm.file != INVALID_HANDLE_VALUE) {
        g_bgm.thread = CreateThread(nullptr, 0, bgm_thread, nullptr, 0, nullptr);
    }
}

// sub_476320: CSound::Shutdown。
void sound_shutdown() {
    InterlockedExchange(&g_bgm.quit, 1);
    if (g_bgm.thread != nullptr) {
        WaitForSingleObject(g_bgm.thread, 1000);
        CloseHandle(g_bgm.thread);
        g_bgm.thread = nullptr;
    }
    bgm_stop();
    for (SePlayer& se : g_se) {
        se.buffer->Stop();
        se.buffer->Release();
        se = {};
    }
    g_se_request_count = 0;
    if (g_bgm.file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_bgm.file);
        g_bgm.file = INVALID_HANDLE_VALUE;
    }
    if (g_primary != nullptr) {
        g_primary->Release();
        g_primary = nullptr;
    }
    if (g_ds != nullptr) {
        g_ds->Release();
        g_ds = nullptr;
    }
}

void sound_play_se(int id) {
    queue_se(id, 0);
}

void sound_play_se_at(int id, float x) {
    queue_se(id, static_cast<int>((x * 1000.0f) / 192.0f));
}

void sound_stop_se(int id) {
    int index = 0;
    while (index < g_se_request_count && g_se_requests[index].id != id) ++index;
    if (index == g_se_request_count) {
        if (g_se_request_count == 12) return;
        g_se_requests[g_se_request_count++] = {id, -1, 0};
    } else {
        g_se_requests[index].count = -1;
    }
}

void sound_update() {
    // sub_453460：先消费 kind 5，再在同一逻辑帧递减，SE 开关不影响 BGM。
    if (g_bgm.fading && InterlockedCompareExchange(&g_bgm.paused, 0, 0) == 0) {
        --g_bgm.fade_remaining;
        if (g_bgm.fade_remaining < 1) {
            g_bgm.fading = false;
            g_bgm.buffer->Stop();
        } else {
            const int step = g_bgm.fade_remaining * 5000 / g_bgm.fade_total - 5000;
            // sub_48a5a0：淡出阶段使用原版平方音量曲线。
            const float amount = 1.0f - g_bgm.volume / 100.0f;
            const LONG volume = g_bgm.volume == 0 ? DSBVOLUME_MIN :
                static_cast<LONG>((1.0f - amount * amount) * (step + 5000)) - 5000;
            g_bgm.buffer->SetVolume(volume);
        }
    }
    if (!g_se_enabled) return;
    for (int index = 0; index < g_se_request_count; ++index) {
        const SeRequest& request = g_se_requests[index];
        SePlayer& se = g_se[request.id];
        if (request.count < 0) {
            DWORD status;
            se.buffer->GetStatus(&status);
            se.resume = (status & DSBSTATUS_PLAYING) != 0;
            se.buffer->Stop();
        } else {
            se.buffer->Stop();
            se.buffer->SetCurrentPosition(0);
            se.buffer->SetPan(request.pan_sum / request.count);
            se.buffer->SetVolume(se_volume_db(request.id));
            se.buffer->Play(0, 0, kSeDefinitions[request.id].play_flags);
        }
    }
    g_se_request_count = 0;
}

void sound_pause_se() {
    g_se_request_count = 0;
    for (SePlayer& se : g_se) {
        DWORD status;
        se.buffer->GetStatus(&status);
        se.resume = (status & DSBSTATUS_PLAYING) != 0;
        se.buffer->Stop();
    }
}

void sound_resume_se() {
    for (int id = 0; id < 84; ++id)
        if (g_se[id].resume)
            g_se[id].buffer->Play(0, 0, kSeDefinitions[id].play_flags);
}

void sound_set_se_volume(int volume) {
    g_se_volume = volume;
}

void sound_set_se_enabled(bool enabled) {
    g_se_enabled = enabled;
}

// sub_4546a0: 切换曲目。
void bgm_play(int track) {
    bgm_stop();
    if (g_bgm_enabled) start_track(track, 0);
    // 4546a0/43e360: music-room order differs from the last four FMT entries.
    // GameOver's th128_08 also writes the title-song flag in the original.
    constexpr int music_index[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 15, 16, 13, 14, 0};
    score_data().state[0x26 + music_index[track]] = 1;
}

void bgm_preview(const char* name) {
    const char* basename = std::strrchr(name, '/');
    basename = basename == nullptr ? name : basename + 1;
    char filename[64];
    std::sprintf(filename, "%s.wav", basename);
    bgm_stop();
    for (int track = 0; track < g_bgm.track_count; ++track) {
        if (std::strcmp(filename, g_bgm.tracks[track].filename) == 0) {
            if (g_bgm_enabled) start_track(track, 0);
            break;
        }
    }
    // Music-room playback calls the sound queue directly, then marks only #0.
    score_data().state[0x26] = 1;
}

int bgm_current_track() { return g_bgm.track; }

double bgm_position() {
    if (g_bgm.buffer == nullptr) return 0;
    DWORD play = 0, write = 0;
    g_bgm.buffer->GetCurrentPosition(&play, &write);
    return static_cast<double>(play) / g_bgm.tracks[g_bgm.track].wave_format.avg_bytes_per_sec;
}

void bgm_play_at(int track, double seconds) {
    bgm_stop();
    if (g_bgm_enabled) start_track(track, seconds);
}

void bgm_stop() {
    g_bgm.fading = false;
    if (g_bgm.buffer != nullptr) {
        g_bgm.buffer->Stop();
        g_bgm.buffer->Release();
        g_bgm.buffer = nullptr;
    }
    g_bgm.track = -1;
    InterlockedExchange(&g_bgm.paused, 0);
}

// 476d20 kind6/7：同一缓冲原位停/续，保留track、循环点和fade剩余时间。
void bgm_pause() {
    if (g_bgm_enabled && g_bgm.buffer != nullptr) {
        DWORD status = 0;
        g_bgm.buffer->GetStatus(&status);
        const bool playing = (status & DSBSTATUS_PLAYING) != 0;
        InterlockedExchange(&g_bgm.paused, playing ? 1 : 0);
        if (playing) g_bgm.buffer->Stop();
    }
}

void bgm_resume() {
    if (g_bgm_enabled && g_bgm.buffer != nullptr &&
        InterlockedExchange(&g_bgm.paused, 0) != 0)
        g_bgm.buffer->Play(0, 0, 0);
}

// sub_454720 / sub_476d20 kind 5：MSG 已在调用前按游戏速度换算秒数。
void bgm_fade(float seconds) {
    if (g_bgm.buffer != nullptr) {
        g_bgm.fade_total = static_cast<int>(static_cast<float>(static_cast<int>(seconds)) * 60.0f);
        g_bgm.fade_remaining = g_bgm.fade_total;
        g_bgm.fading = true;
    }
}

// CSound+0x5730 音量。
void bgm_set_volume(int volume) {
    g_bgm.volume = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
    if (g_bgm.buffer != nullptr) {
        g_bgm.buffer->SetVolume(volume_db(g_bgm.volume));
    }
}

void bgm_set_enabled(bool enabled) {
    g_bgm_enabled = enabled;
}
