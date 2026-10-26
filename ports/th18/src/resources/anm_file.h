#pragma once
#include <cstdint>

// ANM 资源文件（.anm，TH18 为 version 8）。
// 二进制布局由 thtk/thanm 的 format 定义 + 原版加载器 sub_4865b0 双方确认。
// 文件 = 若干段（segment）链：每段有自己的头、sprite 表、script 偏移表、script 数据、
// 可选内嵌 thtx 贴图；段头 +0x24 的 next_offset 指向下一段（0 = 最后一段）。
//
// 原版加载（sub_4865b0）行为：
//   - 从 DAT 读整个文件（sub_402060 external=0）；
//   - 逐段累加 sprites/scripts 数量，统计段数；
//   - hasdata==0 且名字首字符不是 '@' 时按 external=1 从磁盘读同名贴图文件；
//   - 为所有 sprite 实例分配 0x60c 字节的 VM（presentation/anm.h）。
//
// 内嵌贴图 thtx 布局（原版 sub_486bc0 上传时按此读）：
//   +0x00 char magic[4] ('THTX')
//   +0x04 uint16 zero
//   +0x06 uint16 format（PixelFormat）
//   +0x08 uint16 width
//   +0x0a uint16 height
//   +0x0c uint32 size（像素数据字节数）
//   +0x10 像素数据

enum class PixelFormat : uint16_t {
    BGRA8888 = 1, // D3DFMT_A8R8G8B8 (0x15)
    RGB565 = 3,   // D3DFMT_R5G6B5   (0x17)
    ARGB4444 = 5, // D3DFMT_A4R4G4B4 (0x1a)
    GRAY8 = 7,    // D3DFMT_L8       (0x32)
};

// anm_header11_t：TH11 以后所有版本共用的段头。TH18 用前 0x2c 字节，sizeof = 0x40
// （尾部 w_max/h_max 与 zero2[5] 在 TH18 为 0，但占用布局，sprite 偏移表从段 +0x40 开始）。
struct AnmHeader {
    uint32_t version;         // +0x00  TH18 = 8
    uint16_t sprites;         // +0x04  本段 sprite 数
    uint16_t scripts;         // +0x06  本段 script 数
    uint16_t zero1;           // +0x08
    uint16_t width;           // +0x0a  贴图宽
    uint16_t height;          // +0x0c  贴图高
    uint16_t format;          // +0x0e  PixelFormat
    uint32_t name_offset;     // +0x10  段内相对偏移，指向名字（决定外部贴图文件名 / '@' 标记）
    uint16_t x;               // +0x14
    uint16_t y;               // +0x16
    uint32_t memory_priority; // +0x18  0=杂项,1=data/ascii/loading,10=普通精灵,11=背景/ascii
    uint32_t thtx_offset;     // +0x1c  段内相对偏移，0 = 无内嵌贴图
    uint16_t has_data;        // +0x20  0 = 贴图在外部文件（除非名字以 '@' 开头）
    uint8_t low_res_scale;    // +0x22
    uint8_t jpeg_quality;     // +0x23  TH19 起使用
    uint32_t next_offset;     // +0x24  下一段相对偏移，0 = 最后一段
    uint16_t w_max;           // +0x28  TH19
    uint16_t h_max;           // +0x2a  TH19
    uint32_t zero2[5];        // +0x2c  占位，TH18 为 0；使 sizeof == 0x40
};

// sprite_t：段内 sprite 定义，每项 20 字节。
// 段头 +0x40 起是 uint32 sprite_offsets[header->sprites]（相对段基址），指向这些结构。
struct AnmSpriteDef {
    uint32_t id; // sprite 编号（脚本里通过 sprite id 引用）
    float x;     // 贴图内左上角 X（像素）
    float y;
    float w;
    float h;
};

// anm_instr_t：TH11+ 的脚本指令（v8）。脚本数据 = 指令序列，
// type=0 且 length=0 表示脚本结束（原版执行器走到该处停）。
struct AnmInstr {
    uint16_t type;       // +0x00 指令号
    uint16_t length;     // +0x02 整条指令字节数，包含 8 字节头
    int16_t time;        // +0x04 相对时间（插值用）
    uint16_t param_mask; // +0x06 参数类型掩码（i/f/s 组合）
    // 后接 length - 8 字节参数
};

// 内嵌贴图头（thtx）：+0x10 起是像素数据，行跨度 = width * bpp。
struct AnmThxHeader {
    char magic[4];        // +0x00 'THTX'
    uint16_t zero;        // +0x04
    uint16_t format;      // +0x06 PixelFormat
    uint16_t width;       // +0x08
    uint16_t height;      // +0x0a
    uint32_t size;        // +0x0c 像素字节数
    uint8_t data[];       // +0x10
};

// anm_offset_t：script 偏移表项（相对段基址）。
struct AnmScriptOffset {
    int32_t id;       // +0x00 script 编号
    uint32_t offset;  // +0x04 相对段基址的指令流偏移
};

// 单个 ANM 段在文件中的位置信息。原版加载器（sub_4865b0）解析出这些后把 sprite 展开成 VM。
struct AnmSegment {
    const uint8_t* base;            // 段基址（段内所有偏移相对这里）
    const AnmHeader* header;        // base + 0
    const uint32_t* sprite_offsets; // base + 0x40，header->sprites 个 u32
    const AnmScriptOffset* script_offsets; // sprite 偏移表之后，header->scripts 项
    const uint8_t* thtx;            // base + header->thtx_offset，0 则 nullptr
    const char* name;               // base + header->name_offset 的 NUL 串

    const AnmSpriteDef* sprite(uint32_t index) const {
        return reinterpret_cast<const AnmSpriteDef*>(base + sprite_offsets[index]);
    }
};

// 遍历段链。返回段数；segments 由调用者提供缓冲（原版先扫一遍数段数再解析）。
// 原版地址 sub_4865b0 的段扫描循环：head += next_offset 直到 0。
uint32_t anm_count_segments(const void* file_data);
void anm_parse_segments(const void* file_data, AnmSegment* segments, uint32_t segment_count);

// 原版 ANM 管理器对象 0x13c 字节，其字段（dword 索引 → 字节偏移）：
//   +0x000  资源编号（加载时传入的 idx）
//   +0x00c  名字缓冲区（最多 0x100）
//   +0x108  ANM 文件原始数据
//   +0x10c  每 sprite 一个 VM 的数组，0x60c 字节/个
//   +0x110  段数
//   +0x114  sprite 总数
//   +0x118  script 总数
//   +0x11c  script 表：0x44 字节/script；[+8] = 使用的贴图槽编号
//   +0x120  每 sprite 的 IDirect3DTexture9* 表
//   +0x124  段信息表：0x18 字节/段；[+4] = 贴图数据指针，[+8] = 大小，
//           [+0x10] = thtx 指针，[+0x14] = 标志
//   +0x128  贴图上传进度（0 = 空闲，加载线程写，等待线程读）
struct AnmResource; // 见 presentation/anm.h
