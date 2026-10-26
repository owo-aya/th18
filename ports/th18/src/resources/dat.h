#pragma once
#include <cstddef>
#include <cstdint>

// th18.dat 归档读取。
// 原版实现（th18.exe）走 thtk 的 archive_th95 + th14 加密参数表：
//   sub_46eb80  DAT_Init          —— 打开 th18.dat、解析条目表
//   sub_46ec50  DAT_Free          —— 释放条目名、条目数组、关闭文件
//   sub_46ee90  DAT_OpenTable     —— 读 16B 头 + 解密 + LZSS 解出条目表
//   sub_46ed10  DAT_ReadEntry     —— 按条目读数据、解密、LZSS 解压
//   sub_402060  read_file         —— 全游戏统一读入口：external=0 走 DAT，1 走磁盘
//   sub_402220  file_exists       —— CreateFileA 探测
//   sub_4022a0  file_write
//   sub_4023e0  file_delete
//   sub_401e40  th_decrypt        —— TH 加解密（thcrypt.c 同款）
//   sub_401f50  th_encrypt
//   sub_46f840  th_unlzss         —— LZSS 解压
//   sub_46f5b0  th_lzss           —— LZSS 压缩
//   sub_46f230  Pbg::File::Open   —— CreateFileA 包装，模式字符 r/w/a
//   sub_46f410  Pbg::File::Read / sub_46f450 Write / sub_46f490 Tell
//   sub_46f4b0  Pbg::File::Size   / sub_46f4d0 Seek / sub_46f500 ReadWhole
//
// 条目表布局（thtk/thtk/thdat95.c 一致）：
//   名字（NUL 结尾、补齐到 4 字节）+ uint32 offset + uint32 size + uint32 extra(0)
//   zsize 不是存字段，而是"下一条 offset - 本条 offset"；最后一条用
//   (文件大小 - 表压缩长度) - offset。

struct DatEntry {
    const char* name; // 原版 strdup 拷贝（sub_46ee90）；条目数组末尾有 count+1 哨兵
    uint32_t offset;  // 数据在 th18.dat 中的绝对偏移
    uint32_t size;    // 解压后大小
    uint32_t zsize;   // 存储大小；等于 size 时表示未压缩
};

struct DatFile {
    void* handle;      // Pbg::File：CreateFileA 句柄（sub_46eb80 创建）
    DatEntry* entries; // 原版全局 DAT_005217c0，前面带一个计数 dword
    uint32_t count;    // 原版全局 DAT_005217c4
};

// sub_46eb80: 打开归档，成功返回 true（原版失败直接走错误处理，不返回）。
bool dat_open(const char* path);
// sub_46ec50: 释放归档。
void dat_close();

// sub_46ed10: 按名读条目，返回 malloc 的缓冲（调用者 free）。
// 原版加锁：sub_404e80/sub_404f60（临界区 0x521690）。
void* dat_read(const char* name, size_t* out_size);
// 名字匹配规则：stricmp，THDAT_BASENAME 标志下存取都取 basename。
bool dat_exists(const char* name);
int dat_count();
const DatEntry* dat_entry(int index);

// sub_402060: 全游戏统一读入口；external=1 时绕开 DAT 直接读磁盘。
// 返回 malloc 的缓冲（调用者 free）。
void* file_read(const char* path, size_t* out_size, bool external);
bool file_exists(const char* path);
bool file_write(const char* path, const void* data, size_t size);
bool file_delete(const char* path);

// 底层算法（原版 sub_401e40 / sub_401f50 / sub_46f840），供其它模块复用。
void th_decrypt(void* data, uint32_t size, uint8_t key, uint8_t step, uint32_t block, uint32_t limit);
void th_encrypt(void* data, uint32_t size, uint8_t key, uint8_t step, uint32_t block, uint32_t limit);
// 返回已解压字节数；输入不足时按 0 偏移结束。
size_t th_unlzss(const void* src, size_t src_size, void* dst, size_t dst_size);
// sub_46f5b0: caller supplies src_size * 2 + 3 output bytes; returns packed size.
size_t th_lzss(const void* src, size_t src_size, void* dst);
