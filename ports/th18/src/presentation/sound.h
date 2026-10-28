#pragma once
#include <cstdint>

// 声音（DirectSound 8）与 BGM（thbgm.fmt/thbgm.dat 播放）。
// 原版实现地址：
//   sub_476410  CSound::Init     DirectSoundCreate8 + SetCooperativeLevel(2) + 主缓冲
//   sub_476320  CSound::Shutdown 等待并关闭声音线程
//   sub_452cb0  bgm_load_fmt     读 thbgm.fmt（原版路径 "../../bgm/thbgm.fmt"）
//   sub_4531b0  bgm_alloc        分配 0x64c 的 BGM 管理对象
//   sub_4546a0  bgm_play         关卡曲目 / Boss 曲目切换（CSound 队列 kind 2）
//   sub_48aba0  bgm_open_dat     打开 thbgm.dat
//   sub_4895a0 / sub_4898f0      创建流式 DirectSound 缓冲（循环区段表）
//   sub_476d20  声音线程步进
//
// CSound 全局实例：DAT_0056ad80。字段：
//   +0x004  IDirectSoundBuffer* 次缓冲（44100/16/2，0x8000 字节，循环播放）
//   +0x00c  临时指针（初始化时 new(4) 后置 0）
//   +0x01c..+0x048  各组音量常量
//   +0x1a90  SE 槽起始（0x54 = 84 项，每项 6 dword）
//   +0x2484  播放请求队列，30 槽 × 0x10c 字节
//   +0x5730  BGM 音量（初始 100）
//   +0x5734  SE 音量（初始 100）
// 两个声音线程句柄：DAT_00570498 / DAT_0057049c。
//
// BGM 编号是 thbgm.fmt 记录下标（th18_01..th18_17 + th128_08，共 18 条，
// 见 resources/bgm_file.h）。SE 编号为原版 0..83，映射表在 sound.cpp。

// CSound::Init（sub_476410）。窗口句柄取 platform/gfx.h 的 gfx_hwnd()。
// 原版同时建 250ms 定时器推进声音线程；端口用独立线程轮询播放位置。
void sound_init();
// CSound::Shutdown（sub_476320）。
void sound_shutdown();

// sub_476be0 / sub_476c70：同一轮合并同编号请求；x 是游戏坐标，中心为 0。
void sound_play_se(int id);
void sound_play_se_at(int id, float x);
void sound_set_se_pan(int id, float x); // SetPan without restarting a sustained sound.
// 原版停止请求覆盖同轮同编号播放请求。
void sound_stop_se(int id);
// sub_476d20：在 app 主逻辑开头消费上一轮的请求。
void sound_update();
// sub_444d80 / sub_45a4a0：保存播放状态并原位停止，随后从原位置恢复。
void sound_pause_se();
void sound_resume_se();
// 原版配置：0..100 音量只影响后续播放；SE 禁用时不消费请求队列。
void sound_set_se_volume(int volume);
void sound_set_se_enabled(bool enabled);

// sub_4546a0: 播放 thbgm.fmt 的第 track 条曲目（0 起）。曲末回到该条目的 intro_size。
void bgm_play(int track);
// 0046aff0: musiccmt.txt names are auditioned without unlocking that song.
void bgm_preview(const char* name);
void bgm_stop();
// 原477a50 kind6/7：保留当前播放位置与曲目，不释放/重开缓冲。
void bgm_pause();
void bgm_resume();
// 原GameOver 458a30保存曲名/48ae50秒位置，Continue 48af10在原曲位置恢复。
int bgm_current_track();
double bgm_position();
void bgm_play_at(int track, double seconds);
// sub_454720 / kind 5：seconds 已由 MSG 按速度换算；原版先截断秒数再乘 60。
// 事件触发时调一次，由 sound_update 每逻辑帧推进；结束仅停止，保留缓冲。
void bgm_fade(float seconds);
// BGM 音量：0..100（原版 CSound+0x5730，初始 100）。内部转成 DirectSound 的百分之一分贝。
void bgm_set_volume(int volume);
// sub_476d20：预载路径只在 cfg[0x45] != 0 时播放；流式路径不查此模式。
// 配置调用方传 !(cfg[0x50] & 0x10) || cfg[0x45] != 0；影响下一次 bgm_play。
void bgm_set_enabled(bool enabled);
