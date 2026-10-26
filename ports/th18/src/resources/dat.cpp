#include "dat.h"

#include <cstdlib>
#include <cstring>
#include <windows.h>

// th14 加密参数表（原版 0x4c5360，12 字节/项）：
// 序号 = 条目名所有字节之和 & 7（sub_46ed10 计算方式与 thtk 一致）。
struct CryptParam {
    uint8_t key;
    uint8_t step;
    uint16_t tag; // 原版表里的两字符标记，仅用于对照
    uint32_t block;
    uint32_t limit;
};

static const CryptParam kCryptParams[8] = {
    {0x1b, 0x73, 0xaa, 0x100, 0x3800},
    {0x12, 0x43, 0xff, 0x200, 0x3e00},
    {0x35, 0x79, 0x11, 0x400, 0x3c00},
    {0x03, 0x91, 0xdd, 0x80, 0x6400},
    {0xab, 0xdc, 0xee, 0x80, 0x7000},
    {0x51, 0x9e, 0xbb, 0x100, 0x4000},
    {0xc1, 0x15, 0xcc, 0x400, 0x2c00},
    {0x99, 0x7d, 0x77, 0x80, 0x4400},
};

static DatFile g_dat;

static HANDLE file_open_read(const char* path)
{
    return CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                       FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
}

static bool file_seek(void* handle, int64_t offset, DWORD origin)
{
    LARGE_INTEGER li;
    li.QuadPart = offset;
    return SetFilePointerEx(handle, li, nullptr, origin) != 0;
}

static bool file_read_all(void* handle, void* buffer, size_t size)
{
    DWORD got = 0;
    return ReadFile(handle, buffer, (DWORD)size, &got, nullptr) != 0 && got == size;
}

static uint32_t file_size(void* handle)
{
    return GetFileSize(handle, nullptr);
}

// sub_401e40: th_decrypt。
void th_decrypt(void* data, uint32_t size, uint8_t key, uint8_t step, uint32_t block, uint32_t limit)
{
    auto* bytes = static_cast<uint8_t*>(data);
    uint8_t* temp = static_cast<uint8_t*>(malloc(block));
    uint32_t increment = (block >> 1) + (block & 1);

    if (size < (block >> 2))
        size = 0;
    else
        size -= ((size % block) < (block >> 2)) * (size % block) + (size % 2);

    if (limit % block != 0)
        limit += block - (limit % block);

    uint8_t* end = bytes + (size < limit ? size : limit);
    while (bytes < end) {
        if ((uintptr_t)(end - bytes) < block) {
            block = (uint32_t)(end - bytes);
            increment = (block >> 1) + (block & 1);
        }
        uint8_t* in = bytes;
        uint8_t* out = temp + block - 1;
        while (out > temp) {
            *out-- = *in ^ key;
            *out-- = *(in + increment) ^ (key + step * increment);
            ++in;
            key += step;
        }
        if (block & 1) {
            *out = *in ^ key;
            key += step;
        }
        key += step * increment;
        memcpy(bytes, temp, block);
        bytes += block;
    }
    free(temp);
}

// sub_401f50: th_encrypt（写侧，压缩回 DAT 时用）。
void th_encrypt(void* data, uint32_t size, uint8_t key, uint8_t step, uint32_t block, uint32_t limit)
{
    auto* bytes = static_cast<uint8_t*>(data);
    uint8_t* temp = static_cast<uint8_t*>(malloc(block));
    uint32_t increment = (block >> 1) + (block & 1);

    if (size < (block >> 2))
        size = 0;
    else
        size -= ((size % block) < (block >> 2)) * (size % block) + (size % 2);

    if (limit % block != 0)
        limit += block - (limit % block);

    uint8_t* end = bytes + (size < limit ? size : limit);
    while (bytes < end) {
        if ((uintptr_t)(end - bytes) < block) {
            block = (uint32_t)(end - bytes);
            increment = (block >> 1) + (block & 1);
        }
        uint8_t* in = bytes + block - 1;
        uint8_t* out = temp;
        while (in > bytes) {
            *out = *in-- ^ key;
            *(out + increment) = *in-- ^ (key + step * increment);
            ++out;
            key += step;
        }
        if (block & 1) {
            *out = *in ^ key;
            key += step;
        }
        key += step * increment;
        memcpy(bytes, temp, block);
        bytes += block;
    }
    free(temp);
}

// thtk 的 bitstream 是 MSB first。
struct BitReader {
    const uint8_t* pos;
    uint32_t byte;
    uint32_t bits;
};

static uint32_t bits_read(BitReader* b, uint32_t count)
{
    while (count > b->bits) {
        b->byte = (b->byte << 8) | *b->pos++;
        b->bits += 8;
    }
    b->bits -= count;
    return (b->byte >> b->bits) & ((1u << count) - 1);
}

// sub_46f840: th_unlzss。
size_t th_unlzss(const void* src, size_t src_size, void* dst, size_t dst_size)
{
    (void)src_size;
    uint8_t dict[0x2000];
    memset(dict, 0, sizeof(dict));
    uint32_t dict_head = 1;
    auto* out = static_cast<uint8_t*>(dst);
    size_t written = 0;

    BitReader br{static_cast<const uint8_t*>(src), 0, 0};
    while (written < dst_size) {
        if (bits_read(&br, 1)) {
            uint8_t c = (uint8_t)bits_read(&br, 8);
            out[written++] = c;
            dict[dict_head] = c;
            dict_head = (dict_head + 1) & 0x1fff;
        } else {
            uint32_t match_offset = bits_read(&br, 13);
            if (match_offset == 0)
                break;
            uint32_t match_len = bits_read(&br, 4) + 3;
            for (uint32_t i = 0; i < match_len && written < dst_size; ++i) {
                uint8_t c = dict[(match_offset + i) & 0x1fff];
                out[written++] = c;
                dict[dict_head] = c;
                dict_head = (dict_head + 1) & 0x1fff;
            }
        }
    }
    return written;
}

namespace {
struct LzssNode {
    int parent;
    int left;
    int right;
};

// FUN_0046fa30 / FUN_0046fc70: original 8192-byte ring and binary search tree.
struct LzssDictionary {
    uint8_t bytes[0x2000] = {};
    LzssNode nodes[0x2001] = {};

    void replace(int old_node, int new_node) {
        const int parent = nodes[old_node].parent;
        if (nodes[parent].left == old_node)
            nodes[parent].left = new_node;
        else
            nodes[parent].right = new_node;
        nodes[new_node] = nodes[old_node];
        nodes[nodes[new_node].left].parent = new_node;
        nodes[nodes[new_node].right].parent = new_node;
        nodes[old_node].parent = 0;
    }

    int insert(int offset, int& match_offset) {
        if (offset == 0)
            return 0;
        int best_length = 0;
        int node = nodes[0x2000].right;
        for (;;) {
            int length = 0;
            int difference = 0;
            while (length < 18) {
                difference = bytes[(offset + length) & 0x1fff] - bytes[(node + length) & 0x1fff];
                if (difference != 0)
                    break;
                ++length;
            }
            if (best_length <= length) {
                best_length = length;
                match_offset = node;
                if (length == 18) {
                    replace(node, offset);
                    return length;
                }
            }
            int& child = difference < 0 ? nodes[node].left : nodes[node].right;
            if (child == 0) {
                child = offset;
                nodes[offset] = {node, 0, 0};
                return best_length;
            }
            node = child;
        }
    }

    void remove(int offset) {
        if (nodes[offset].parent == 0)
            return;
        int replacement = nodes[offset].right;
        if (replacement == 0)
            replacement = nodes[offset].left;
        else if (nodes[offset].left != 0) {
            replacement = nodes[offset].left;
            while (nodes[replacement].right != 0)
                replacement = nodes[replacement].right;
            remove(replacement);
            replace(offset, replacement);
            return;
        }
        const int parent = nodes[offset].parent;
        nodes[replacement].parent = parent;
        if (nodes[parent].right == offset)
            nodes[parent].right = replacement;
        else
            nodes[parent].left = replacement;
        nodes[offset].parent = 0;
    }
};
} // namespace

size_t th_lzss(const void* src, size_t src_size, void* dst)
{
    const auto* input = static_cast<const uint8_t*>(src);
    auto* output = static_cast<uint8_t*>(dst);
    size_t written = 0;
    uint8_t byte = 0;
    uint8_t mask = 0x80;
    auto write_bits = [&](uint32_t value, int count) {
        for (int bit = count - 1; bit >= 0; --bit) {
            if ((value & (1u << bit)) != 0)
                byte |= mask;
            mask >>= 1;
            if (mask == 0) {
                output[written++] = byte;
                byte = 0;
                mask = 0x80;
            }
        }
    };
    LzssDictionary dict;
    size_t read = 0;
    int waiting = 0;
    while (waiting < 18 && read < src_size)
        dict.bytes[1 + waiting++] = input[read++];
    dict.nodes[0x2000].right = 1;
    dict.nodes[1].parent = 0x2000;
    int head = 1;
    int match_length = 0;
    int match_offset = 0;
    while (waiting != 0) {
        if (waiting < match_length)
            match_length = waiting;
        int consumed = match_length;
        if (match_length < 3) {
            write_bits(1, 1);
            write_bits(dict.bytes[head], 8);
            consumed = 1;
        } else {
            write_bits(0, 1);
            write_bits(match_offset, 13);
            write_bits(match_length - 3, 4);
        }
        for (int i = 0; i < consumed; ++i) {
            const int next = (head + 18) & 0x1fff;
            dict.remove(next);
            if (read < src_size)
                dict.bytes[next] = input[read++];
            else
                --waiting;
            head = (head + 1) & 0x1fff;
            if (waiting != 0)
                match_length = dict.insert(head, match_offset);
        }
    }
    // Original emits a zero flag and zero 13-bit offset and returns full bytes.
    write_bits(0, 14);
    return written;
}

static int crypt_param_index(const char* name)
{
    uint8_t index = 0;
    while (*name)
        index = (uint8_t)(index + (uint8_t)*name++);
    return index & 7;
}

// sub_46eb80 + sub_46ee90: 打开 DAT，解析条目表。
bool dat_open(const char* path)
{
    HANDLE handle = file_open_read(path);
    uint8_t header[16];
    if (!file_read_all(handle, header, sizeof(header)))
        return false;

    th_decrypt(header, 0x10, 0x1b, 0x37, 0x10, 0x10);

    uint32_t table_size;
    uint32_t table_zsize;
    uint32_t count;
    memcpy(&table_size, header + 4, 4);
    memcpy(&table_zsize, header + 8, 4);
    memcpy(&count, header + 12, 4);
    table_size -= 123456789u;
    table_zsize -= 987654321u;
    count -= 135792468u;

    const uint32_t total_size = file_size(handle);
    file_seek(handle, -(int64_t)table_zsize, FILE_END);

    auto* zdata = static_cast<uint8_t*>(malloc(table_zsize));
    if (!file_read_all(handle, zdata, table_zsize))
        return false;
    th_decrypt(zdata, table_zsize, 0x3e, 0x9b, 0x80, table_zsize);

    auto* table = static_cast<uint8_t*>(malloc(table_size));
    th_unlzss(zdata, table_zsize, table, table_size);
    free(zdata);

    g_dat.handle = handle;
    g_dat.count = count;
    g_dat.entries = static_cast<DatEntry*>(malloc(sizeof(DatEntry) * (count + 1)));

    const uint8_t* ptr = table;
    DatEntry* prev = nullptr;
    for (uint32_t i = 0; i < count; ++i) {
        DatEntry* entry = &g_dat.entries[i];
        entry->name = _strdup(reinterpret_cast<const char*>(ptr));
        const size_t name_len = strlen(entry->name);
        ptr += name_len + (4 - name_len % 4);
        memcpy(&entry->offset, ptr, 4);
        memcpy(&entry->size, ptr + 4, 4);
        ptr += 12;
        entry->zsize = 0;
        if (prev)
            prev->zsize = entry->offset - prev->offset;
        prev = entry;
    }
    if (prev)
        prev->zsize = total_size - table_zsize - prev->offset;
    // 原版条目数组末尾还有一条 offset = 数据区结束的哨兵，读取时不会用到。
    g_dat.entries[count].name = nullptr;
    g_dat.entries[count].offset = total_size - table_zsize;
    g_dat.entries[count].size = 0;
    g_dat.entries[count].zsize = 0;

    free(table);
    return true;
}

// sub_46ec50: 释放。
void dat_close()
{
    if (!g_dat.entries)
        return;
    for (uint32_t i = 0; i < g_dat.count; ++i)
        free(const_cast<char*>(g_dat.entries[i].name));
    free(g_dat.entries);
    g_dat.entries = nullptr;
    g_dat.count = 0;
    CloseHandle(g_dat.handle);
    g_dat.handle = nullptr;
}

// sub_46ed10: 按名读条目。
void* dat_read(const char* name, size_t* out_size)
{
    const DatEntry* found = nullptr;
    for (uint32_t i = 0; i < g_dat.count; ++i) {
        if (_stricmp(g_dat.entries[i].name, name) == 0) {
            found = &g_dat.entries[i];
            break;
        }
    }
    if (!found)
        return nullptr;

    auto* zdata = static_cast<uint8_t*>(malloc(found->zsize));
    file_seek(g_dat.handle, found->offset, FILE_BEGIN);
    if (!file_read_all(g_dat.handle, zdata, found->zsize)) {
        free(zdata);
        return nullptr;
    }

    const CryptParam* param = &kCryptParams[crypt_param_index(found->name)];
    th_decrypt(zdata, found->zsize, param->key, param->step, param->block, param->limit);

    if (found->zsize == found->size) {
        if (out_size)
            *out_size = found->size;
        return zdata;
    }

    auto* data = static_cast<uint8_t*>(malloc(found->size));
    th_unlzss(zdata, found->zsize, data, found->size);
    free(zdata);
    if (out_size)
        *out_size = found->size;
    return data;
}

bool dat_exists(const char* name)
{
    for (uint32_t i = 0; i < g_dat.count; ++i) {
        if (_stricmp(g_dat.entries[i].name, name) == 0)
            return true;
    }
    return false;
}

int dat_count()
{
    return (int)g_dat.count;
}

const DatEntry* dat_entry(int index)
{
    if (index < 0 || (uint32_t)index >= g_dat.count)
        return nullptr;
    return &g_dat.entries[index];
}

// sub_402060: 全游戏统一读入口。
void* file_read(const char* path, size_t* out_size, bool external)
{
    if (!external) {
        const char* base = path;
        const char* backslash = strrchr(path, '\\');
        if (backslash)
            base = backslash + 1;
        const char* slash = strrchr(base, '/');
        if (slash)
            base = slash + 1;
        void* data = dat_read(base, out_size);
        if (data)
            return data;
    }

    HANDLE handle = file_open_read(path);
    if (handle == INVALID_HANDLE_VALUE)
        return nullptr;
    const uint32_t size = file_size(handle);
    auto* data = static_cast<uint8_t*>(malloc(size));
    if (!file_read_all(handle, data, size)) {
        free(data);
        CloseHandle(handle);
        return nullptr;
    }
    CloseHandle(handle);
    if (out_size)
        *out_size = size;
    return data;
}

bool file_exists(const char* path)
{
    HANDLE handle = file_open_read(path);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    CloseHandle(handle);
    return true;
}

bool file_write(const char* path, const void* data, size_t size)
{
    HANDLE handle = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = WriteFile(handle, data, (DWORD)size, &written, nullptr) != 0 && written == size;
    CloseHandle(handle);
    return ok;
}

bool file_delete(const char* path)
{
    return DeleteFileA(path) != 0;
}
