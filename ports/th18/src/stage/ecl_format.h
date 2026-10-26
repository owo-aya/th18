#pragma once
#include <cstdint>
struct AnmResource;

// TH18 ECL 文件格式（原版 th18.exe 解析器地址见下）。
//   FUN_0048d9d0  EclResourceInf 解析单个 .ecl 文件（this, fileData）
//   FUN_0042d3e0  EclResourceInf vtable[0]：按文件名从 DAT 读入后调 FUN_0048d9d0
//   FUN_0042da90  EclResourceInf vtable[1]：处理文件头之后的 include 区（先 ANM 后 ECLI）
// 加载后：资源对象累计子程序数（+8），按名字排序合并 {name, 子程序偏移} 表（+0x8c）。
namespace ecl {

constexpr uint32_t kMagicScript = 0x54504353;   // "SCPT"
constexpr uint32_t kMagicAnimList = 0x4d494e41; // "ANIM"
constexpr uint32_t kMagicEclList = 0x494c4345;  // "ECLI"
constexpr uint32_t kMagicSub = 0x484c4345;      // "ECLH"
constexpr uint32_t kHeaderSize = 0x24;          // 36 字节
constexpr uint32_t kSubHeaderSize = 0x10;       // 16 字节
constexpr uint32_t kAnimNameSize = 0x104;       // ANIM 名单每项 260 字节路径

// 文件头。FUN_0048d9d0 校验 magic == "SCPT" 且 version == 1；
// include_size 是 +0x24 起 include 区的总长度，子程序偏移表紧跟其后。
struct FileHeader {
    char magic[4];           // "SCPT"
    uint16_t version;        // == 1
    uint16_t include_size;
    uint32_t include_offset; // == kHeaderSize
    uint32_t zero0;
    uint32_t sub_count;
    uint32_t zero1[4];
};

// include 区开头两项（后面按 4 字节对齐接 ECLI 名单）。
// 名字不是定长数组也不是偏移表：count 个 NUL 结尾字符串连续排布，之后对齐到 4。
// 实例（st01.ecl，include_size=0x4c）：
//   ANIM 02 00 00 00 "enemy.anm\0" "st01enm.anm\0" 00 → 对齐 0x44
//   ECLI 03 00 00 00 "default.ecl\0" "st01bs.ecl\0" "st01mbs.ecl\0" 00 → 对齐 0x70
struct AnimList {
    char magic[4];   // "ANIM"
    uint32_t count;
    // char names[count][]，NUL 结尾，之后 4 字节对齐
};

struct EclIncludeList {
    char magic[4];   // "ECLI"
    uint32_t count;
    // char names[count][]，NUL 结尾，之后 4 字节对齐
};

// EclResourceInf 对象（0x0042dc20 构造，分配 0x1098 字节）。0x0048d9d0 往里追加内容：
// 每加载一个 .ecl 文件：+4 = 文件序号；+8 = 子程序总数；+0xc = 已加载文件数据指针数组；
// +0x8c = 按名字升序的子程序表 {const char* name; const void* data;}。
// 结构定义放在 SubEntry 之后（见文件末尾）。

// 每个子程序以该头开始，其后紧跟指令流（指令见 InsHeader）。
struct SubHeader {
    char magic[4];        // "ECLH"
    uint32_t data_offset; // == kSubHeaderSize
    uint32_t zero[2];
};

// 子程序偏移表项（加载后 EclResourceInf +0x8c 处的数组，8 字节一项）。
// FUN_0048dbe0 用 sub 索引取第 2 个 dword 再加 kSubHeaderSize + 指令偏移得到当前指令指针。
struct SubEntry {
    const char* name;
    const void* data;  // 指向子程序 "ECLH" 头（指令流在 +0x10 之后）
};

// 单条指令头（thtk th10_instr_t）。参数紧跟其后，按 ecl_opcodes.h 的 format 解码。
struct InsHeader {
    uint32_t time;       // 相对子程序起点的逻辑帧
    uint16_t id;
    uint16_t size;       // 含头与参数的总字节数
    uint16_t param_mask; // 参数存在位（第 i 位 = 第 i 个参数存在）
    uint8_t rank_mask;   // 1=easy 2=normal 4=hard 8=lunatic，其余位恒 1
    uint8_t param_count;
    uint32_t stack_refs; // th13+ 的栈引用计数；th18 恒 0
};

// 参数类型字符（见 ecl_opcodes.h）。
//   S  int32        f  float
//   m  const char*  x  const char*
//   o  const char*  t  const char*
//   D  栈变量（后跟 bitmask + 变量号）
//   H  子程序参数
//   *  重复到指令尾

constexpr uint32_t kResourceSize = 0x1098;
constexpr uint32_t kSubTableOffset = 0x8c;
struct EclResource {
    void** vtable;       // +0x000 EclResourceInf vftable 0x4b6540
    int32_t file_index;  // +0x004 已加载文件数
    int32_t sub_count;   // +0x008 子程序总数
    void** files;        // +0x00c 已加载文件数据
    AnmResource* animations[16];
    int32_t animation_count;
    uint8_t pad054[kSubTableOffset - 0x054];
    SubEntry* subs;  // +0x08c 名字升序
    uint8_t pad090[kResourceSize - 0x090];
};
static_assert(sizeof(EclResource) == kResourceSize);

}  // namespace ecl
