#include "anm_file.h"

// 段扫描与解析，对应原版 sub_4865b0 加载 ANM 文件时的第一遍（数段、累加 sprites/scripts）
// 与第二遍（求各段内偏移）。
// 段头 sizeof == 0x40；sprite 偏移表在段 +0x40，script 偏移表紧随其后。

static const AnmHeader* segment_at(const uint8_t* file_data, uint32_t offset)
{
    return reinterpret_cast<const AnmHeader*>(file_data + offset);
}

uint32_t anm_count_segments(const void* file_data)
{
    const uint8_t* data = static_cast<const uint8_t*>(file_data);
    uint32_t count = 0;
    uint32_t offset = 0;
    for (;;) {
        const AnmHeader* header = segment_at(data, offset);
        ++count;
        if (header->next_offset == 0)
            break;
        offset += header->next_offset;
    }
    return count;
}

void anm_parse_segments(const void* file_data, AnmSegment* segments, uint32_t segment_count)
{
    const uint8_t* data = static_cast<const uint8_t*>(file_data);
    uint32_t offset = 0;
    for (uint32_t i = 0; i < segment_count; ++i) {
        const uint8_t* base = data + offset;
        const AnmHeader* header = reinterpret_cast<const AnmHeader*>(base);

        AnmSegment& segment = segments[i];
        segment.base = base;
        segment.header = header;
        segment.sprite_offsets = reinterpret_cast<const uint32_t*>(base + sizeof(AnmHeader));
        segment.script_offsets = reinterpret_cast<const AnmScriptOffset*>(
            base + sizeof(AnmHeader) + header->sprites * sizeof(uint32_t));
        segment.thtx = header->thtx_offset ? base + header->thtx_offset : nullptr;
        segment.name = reinterpret_cast<const char*>(base + header->name_offset);

        offset += header->next_offset;
    }
}
