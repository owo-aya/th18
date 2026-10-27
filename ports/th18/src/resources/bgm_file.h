#pragma once
#include <cstdint>

// thbgm.fmt / thbgm.dat 曲目表（发行资源的 fmt 在 th18.dat 内，音频另存 thbgm.dat）。
// 结构来自 thtk/extlib/thtypes/bgm_types.h，原版读取点 sub_452cb0；
// thbgm.dat 打开 sub_48aba0，流式缓冲创建 sub_4895a0 / sub_4898f0。
//
// thbgm.fmt = 若干 52 字节记录（TH18 共 18 条，末尾多 17 字节对齐填充）。
// thbgm.dat 里每条曲目的音频从 track_offset 开始，长度 track_size；
// intro_size 是循环点（从曲目起点算的字节偏移），
// preload_size 是预加载字节数（TH18 全部等于 track_size，即整曲预载）。
// 相邻曲目的 track_offset 差值正好等于前一条的 track_size。

// The file embeds the 18-byte WAVEFORMATEX layout. Default C++ alignment
// rounds this block to 20 bytes and the whole record to 56 instead of 52.
#pragma pack(push, 1)
struct BgmWaveFormat {
    uint16_t format_tag;      // 1 = WAVE_FORMAT_PCM
    uint16_t channels;        // 2
    uint32_t samples_per_sec; // 44100
    uint32_t avg_bytes_per_sec;
    uint16_t block_align;     // 4
    uint16_t bits_per_sample; // 16
    uint16_t cb_size;         // 0
};

struct BgmFmtEntry {
    char filename[16];       // +0x00 曲目文件名（如 "th18_01.wav"）
    uint32_t track_offset;   // +0x10 thbgm.dat 内起点
    uint32_t preload_size;   // +0x14 预载字节数
    int32_t intro_size;      // +0x18 循环点（字节偏移，从曲目起点算）
    int32_t track_size;      // +0x1c 曲目总字节数
    BgmWaveFormat wave_format; // +0x20 18 字节
    uint16_t padding;        // +0x32
};
#pragma pack(pop)

// 原版 BGM 流程（全部是 CSound 的方法/全局函数）：
//   sub_452cb0  bgm_init：anm_get(0, "text.anm") 存 DAT_004cd61c；
//               读 "../../bgm/thbgm.fmt" 到 DAT_0056c700；file_exists("thbgm.dat")
//               不满足则弹错误；歌曲切换/循环/停止的入口都在这个函数族里。
//   sub_4531b0  分配 BGM 管理对象（0x64c 字节）。
//   sub_4546a0  bgm_play(int mode, int track_id)：mode 0 = 关卡曲目，1 = Boss 曲目；
//               内部按 DAT_004cd014 位 0x10 先发 kind=4 请求，再入队 kind=2 请求。
//   sub_4546f0  bgm_play_alt：入队 kind = ((DAT_004cd014 >> 4) & 1) + 3。
//   sub_454820/sub_455ec0  混音/音量状态切换；
//   sub_454760/sub_4547a0 渲染状态前后配对（BGM 界面用）。
//   sub_48aba0  打开 thbgm.dat（Pbg::File::Open 封装）。
//   sub_4895a0 / sub_4898f0  创建流式 DirectSound 缓冲：
//     DSBUFFERDESC{ size=0x24, flags=0x18188, bufferBytes=? }，
//     并用 8 组 (offset,size) 表示循环区段（intro_size 之前/之后各一段），
//     缓冲大小按单块 param9 与是否 2 倍速（DAT_004ceacc）分支。
//   sub_476d20  CSound 线程步进（sub_453a70 反复调用），从队列取请求装填缓冲。
// SE：sub_4776f0 加载 0x54 个 se_*.wav；sub_477a50 入队播放请求（30 槽 × 0x10c）。

