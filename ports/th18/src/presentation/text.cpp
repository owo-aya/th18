// 文字系统实现（原版 sub_41b310 / sub_419b90 / sub_4198b0 / sub_419cb0 / sub_419dd0）。
// TextManager 的完整字段布局写在 text.h 的注释里；这里按已确认的字节偏移操作队列。
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <algorithm>
#include <vector>
#include <windows.h>
#include <d3d9.h>

#include "presentation/anm.h"
#include "presentation/text.h"
#include "resources/anm_file.h"
#include "storage/storage.h"
#include "core/thread.h"
#include "platform/gfx.h"

namespace {

void init_sjis_fonts();
void destroy_sjis_fonts();

// TextManager 里本文件用到的字段偏移（见 text.h 注释与 sub_419xxx 反编译）。
constexpr uint32_t kQueueOffset = 0xc24;
constexpr uint32_t kQueueCapacity = 0x140;
constexpr uint32_t kCountOffset = 0x19224;
constexpr uint32_t kColorOffset = 0x19228;
constexpr uint32_t kColor2Offset = 0x1922c;
constexpr uint32_t kStyleOffset = 0x19244;
constexpr uint32_t kAuxOffset = 0x19240;    // +0x19240/48 进 item 的 +0x124/+0x128
constexpr uint32_t kLifeOffset = 0x1924c;
constexpr uint32_t kFrameOffset = 0x19250;  // +0x19250/54 float；帧计数在 +0x1925c
constexpr uint32_t kSizeXOffset = 0x19230;  // 字号横向缩放（item+0x110 的来源）
constexpr uint32_t kSizeYOffset = 0x19234;  // 字号纵向缩放/行高（item+0x114 的来源）
constexpr uint32_t kSizeZOffset = 0x19238;
constexpr uint32_t kFrameCounterOffset = 0x1925c;
constexpr uint32_t kAsciiAnmOffset = 0x19260;
constexpr uint32_t kLoadingVmOffset = 0x19268;
constexpr uint32_t kVmAsciiOffset = 0x00c;
constexpr uint32_t kVmAsciiAltOffset = 0x618;
constexpr int kTextAnmIndex = 2;            // anm_get(2, "ascii.anm")

TextManager* g_text_manager = nullptr;
AnmVm* g_loading_vm = nullptr;
bool g_loading_stopping = false;
Thread* g_text_tasks[5];

ThreadStatus update_text(void*) {
    text_update_task(g_text_manager);
    return THREAD_CONTINUE;
}

ThreadStatus draw_text(void* context) {
    const uint32_t group = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(context));
    gfx_select_camera(group == 1 ? 0 : 2, group == 2);
    text_render(g_text_manager, group);
    gfx_select_camera(2);
    return THREAD_CONTINUE;
}

template <typename T>
T load(const uint8_t* base, uint32_t offset) {
    T value;
    std::memcpy(&value, base + offset, sizeof(T));
    return value;
}

template <typename T>
void store(uint8_t* base, uint32_t offset, T value) {
    std::memcpy(base + offset, &value, sizeof(T));
}

TextItem* item_at(uint8_t* base, int32_t index) {
    return reinterpret_cast<TextItem*>(base + kQueueOffset) + index;
}

// sub_41a2b0 的字符→字模 sprite 映射（switch(param_1+0x120)）：
//   style 0：c-0x20；style 1：c+0x42；style 2/3：a-z→c+0x74、A-Z→c+0x94，符号用固定 id；
//   style 6/8：c+0x120；style 7/9：c+0x1e4。
int glyph_sprite(uint32_t style, uint8_t c) {
    switch (style) {
    case 4:
    case 5:
    case 10:
    case 11: {
        const int base = style == 10 ? 0x10d : (style == 11 ? 0xfe : 0xef);
        switch (c) {
        case '/': return base + 10;
        case '.': return base + 11;
        case 's': return base + 12;
        case '*': return base + 13;
        case ',': return base + 14;
        default: return base + c - '0';
        }
    }
    case 0:
        return c - 0x20;
    case 1:
        return c + 0x42;
    case 2:
    case 3:
        if (c >= 'a' && c <= 'z') {
            return c + 0x74;
        }
        if (c >= 'A' && c <= 'Z') {
            return c + 0x94;
        }
        switch (c) {
        case ':': return 0xcf;
        case '-': return 0xd0;
        case '*': return 0xd1;
        case '%': return 0xd2;
        case '$': return 0x11f;
        case '.': return 0xd3;
        case '+': return 0xd4;
        default: return c - 0x20;
        }
    case 6:
        return c + 0x120;
    case 7:
        return c + 0x1e4;
    case 8:
        return c + 0x182;
    case 9:
        return c + 0x246;
    default:
        return c - 0x20;
    }
}

}  // namespace

// sub_419390: 创建文字系统。原版按屏幕宽选 ascii.anm / ascii_960.anm / ascii1280.anm；
// 端口后备缓冲固定 640x480，对应最小号 ascii.anm。
// 两个字模 VM 内嵌在管理器里（+0x00c 与 +0x618），原版用 sub_477b00(anmRes, vm, 0) 绑定字模。
TextManager* text_create() {
    init_sjis_fonts();
    storage_log("text: begin");
    TextManager* text = static_cast<TextManager*>(std::calloc(1, TEXT_MANAGER_SIZE));
    uint8_t* base = reinterpret_cast<uint8_t*>(text);

    storage_log("text: calloc ok");
    AnmResource* ascii = anm_get(kTextAnmIndex, "ascii.anm");
    storage_log("text: anm_get=%p", ascii);
    store<AnmResource*>(base, kAsciiAnmOffset, ascii);

    AnmVm* vm = reinterpret_cast<AnmVm*>(base + kVmAsciiOffset);
    anm_vm_reset(vm);
    vm->anm_index = kTextAnmIndex;
    vm->anm_table_index = kTextAnmIndex;
    anm_vm_reset(reinterpret_cast<AnmVm*>(base + kVmAsciiAltOffset));
    reinterpret_cast<AnmVm*>(base + kVmAsciiAltOffset)->anm_index = kTextAnmIndex;
    reinterpret_cast<AnmVm*>(base + kVmAsciiAltOffset)->anm_table_index = kTextAnmIndex;

    // HUD/关卡每帧入队前会按原版写 +0x19228 颜色；这里给白字默认值。
    store<uint32_t>(base, kColorOffset, 0xffffffffu);
    store<uint32_t>(base, kColor2Offset, 0xffffffffu);
    store<int32_t>(base, kLifeOffset, 1);
    // 字号缩放的默认值（原版文本对象创建时写这几个字段，全 1 表示按字体原始尺寸）。
    store<float>(base, kSizeXOffset, 1.0f);
    store<float>(base, kSizeYOffset, 1.0f);
    store<float>(base, kSizeZOffset, 1.0f);
    store<uint32_t>(base, kAuxOffset, 0); // 419390: drop shadow defaults to off.
    store<int32_t>(base, kFrameOffset, 1);
    store<int32_t>(base, kFrameOffset + 4, 1);

    g_text_manager = text;
    g_text_tasks[0] = thread_create(update_text);
    thread_set_priority(g_text_tasks[0], 5);
    thread_enqueue_logic(g_text_tasks[0]);
    constexpr uint8_t groups[] = {1, 3, 2, 0};
    constexpr uint8_t priorities[] = {0x37, 0x3f, 0x48, 0x58};
    for (int i = 0; i < 4; ++i) {
        Thread* task = g_text_tasks[i + 1] = thread_create(draw_text);
        thread_set_ctx(task, reinterpret_cast<void*>(static_cast<uintptr_t>(groups[i])));
        thread_set_priority(task, priorities[i]);
        thread_enqueue_render(task);
    }
    return text;
}

TextManager* text_manager() {
    return g_text_manager;
}

// sub_419690: 释放文字系统（原版还释放任务、VM、ANM 句柄；端口 ANM 资源按管理器统一管理）。
void text_destroy(TextManager* text) {
    for (Thread* task : g_text_tasks) thread_remove(task);
    destroy_sjis_fonts();
    anm_vm_free(g_loading_vm);
    g_loading_vm = nullptr;
    if (text == g_text_manager) {
        g_text_manager = nullptr;
    }
    std::free(text);
}

// sub_41b310: 原版是 sprintf 的薄包装（把格式化参数放在 this 槽位）。
// 移植版接受调用者提供的缓冲区，语义一致：不追加、不检查长度。
void text_format(char* out, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vsprintf(out, fmt, args);
    va_end(args);
}

// sub_419b90: 把 pos 与字符串压进队列尾部。
// 原版先按屏幕缩放 DAT_0056aca0 放大 pos；端口还没有屏幕缩放全局，
// 等 platform 提供缩放值后在这里乘上即可。
void text_queue_add(TextManager* text, const TextPos* pos, const char* str) {
    uint8_t* base = reinterpret_cast<uint8_t*>(text);
    int32_t count = load<int32_t>(base, kCountOffset);
    if (count >= static_cast<int32_t>(kQueueCapacity)) {
        return;
    }
    TextItem* item = item_at(base, count);
    std::strcpy(item->text, str);
    item->x = pos->x;
    item->y = pos->y;
    item->z = pos->z;
    item->color = load<uint32_t>(base, kColorOffset);
    item->style = load<uint32_t>(base, kStyleOffset);
    item->life = load<int32_t>(base, kLifeOffset);
    item->dirty_110 = load<float>(base, kSizeXOffset);
    item->dirty_114 = load<float>(base, kSizeYOffset);
    item->dirty_118 = load<float>(base, kSizeZOffset);
    item->dirty_124 = load<uint32_t>(base, kAuxOffset);
    item->dirty_128 = load<uint32_t>(base, kAuxOffset + 8);
    item->dirty_130 = load<uint32_t>(base, kFrameOffset);
    item->dirty_134 = load<uint32_t>(base, kFrameOffset + 4);
    store<int32_t>(base, kCountOffset, count + 1);
}

// sub_4198b0: 每帧任务。所有项 life 减一，life<0 的项丢弃并把后面的项前移。
void text_update_task(TextManager* text) {
    if (g_loading_vm != nullptr) {
        anm_vm_update(g_loading_vm);
        if (g_loading_stopping && g_loading_vm->script_cursor < 0) {
            bool children_running = false;
            for (int queue = 0; queue < 2; ++queue) {
            for (AnmVm* vm = static_cast<AnmVm*>(queue == 0 ? anm_manager()->queue_head :
                                               anm_manager()->queue_head2); vm != nullptr;
                 vm = static_cast<AnmVm*>(vm->node_next)) {
                if (vm->parent == g_loading_vm && vm->script_cursor >= 0)
                    children_running = true;
            }
            }
            if (!children_running) {
                anm_vm_free(g_loading_vm);
                g_loading_vm = nullptr;
            }
        }
    }
    uint8_t* base = reinterpret_cast<uint8_t*>(text);
    int32_t count = load<int32_t>(base, kCountOffset);
    TextItem* queue = reinterpret_cast<TextItem*>(base + kQueueOffset);
    for (int32_t i = 0; i < count; ++i) {
        --queue[i].life;
        if (queue[i].life < 0) {
            if (i + 1 < count) {
                std::memmove(&queue[i], &queue[i + 1],
                             static_cast<size_t>(count - i - 1) * sizeof(TextItem));
            }
            --count;
            --i;
        }
    }
    store<int32_t>(base, kCountOffset, count);
    store<int32_t>(base, kFrameCounterOffset, load<int32_t>(base, kFrameCounterOffset) + 1);
}

void text_loading_start(float x, float y) {
    uint8_t* base = reinterpret_cast<uint8_t*>(g_text_manager);
    AnmResource* ascii = load<AnmResource*>(base, kAsciiAnmOffset);
    g_loading_vm = anm_vm_alloc(ascii);
    g_loading_vm->extra_x = x * 2.0f;
    g_loading_vm->extra_y = y * 2.0f;
    anm_vm_set_script(g_loading_vm, ascii, 17);
    anm_queue_add(g_loading_vm);
    g_loading_stopping = false;
    store<AnmVm*>(base, kLoadingVmOffset, g_loading_vm);
}

void text_loading_stop() {
    anm_set_label(g_loading_vm, 1);
    g_loading_stopping = true;
    store<AnmVm*>(reinterpret_cast<uint8_t*>(g_text_manager), kLoadingVmOffset, nullptr);
}

// sub_419cb0: printf 风格入队。原版对样式 6/7/10 会再用副色排一行（样式转 8/9/11），
// 副色先入队，随后恢复主色与样式。
void text_draw_fmt(TextManager* text, const TextPos* pos, const char* fmt, ...) {
    char buffer[0x400];
    va_list args;
    va_start(args, fmt);
    std::vsprintf(buffer, fmt, args);
    va_end(args);
    uint8_t* base = reinterpret_cast<uint8_t*>(text);
    const uint32_t style = load<uint32_t>(base, kStyleOffset);
    if (style == 6 || style == 7 || style == 10) {
        const uint32_t color = load<uint32_t>(base, kColorOffset);
        store<uint32_t>(base, kColorOffset, load<uint32_t>(base, kColor2Offset));
        store<uint32_t>(base, kStyleOffset, style == 10 ? 11 : style + 2);
        text_queue_add(text, pos, buffer);
        store<uint32_t>(base, kColorOffset, color);
        store<uint32_t>(base, kStyleOffset, style);
    }
    text_queue_add(text, pos, buffer);
}

// sub_419dd0: 画数字。原版按每 3 位插一个逗号（分数/金钱显示）。
void text_draw_number(TextManager* text, const TextPos* pos, uint32_t value) {
    char digits[16];
    int length = std::sprintf(digits, "%u", value);
    char buffer[0x100];
    int out = 0;
    for (int i = 0; i < length; ++i) {
        if (i > 0 && ((length - i) % 3) == 0) {
            buffer[out++] = ',';
        }
        buffer[out++] = digits[i];
    }
    buffer[out] = '\0';
    text_queue_add(text, pos, buffer);
}

// sub_419f30: 另一种数字格式（分组方式不同，第 3 个参数参与低位计算）。
// 格式细节还没从反编译确认，等 HUD/卡牌面板接上时按实际显示还原。

// sub_41a2b0: 绘制队列里的全部文字。
// 原版用管理器内嵌的字模 VM（+0x00c，第二张在 +0x618）当画笔：逐字符从 ascii.anm
// 的脚本表取 UV、设置位置/缩放/颜色后立即绘制；换屏幕分辨率只需换 ascii.anm。
// 每项的位置是屏幕坐标（1280x960 虚拟空间），行高按管理器 +0x19248 的竖向缩放。
void text_render(TextManager* text, uint32_t group) {
    if (text == nullptr) {
        return;
    }
    uint8_t* base = reinterpret_cast<uint8_t*>(text);
    AnmResource* res = load<AnmResource*>(base, kAsciiAnmOffset);
    if (res == nullptr || res->scripts == nullptr) {
        return;
    }
    AnmVm* vm = reinterpret_cast<AnmVm*>(base + kVmAsciiOffset);
    int32_t count = load<int32_t>(base, kCountOffset);
    TextItem* queue = reinterpret_cast<TextItem*>(base + kQueueOffset);
    for (int32_t i = 0; i < count; ++i) {
        TextItem* item = &queue[i];
        if (item->dirty_128 != group) continue;
        const bool numeric = item->style == 4 || item->style == 5 ||
                             item->style == 10 || item->style == 11;
        float scale_x = item->dirty_110;
        float scale_y = item->dirty_114;
        if (scale_x <= 0.0f) {
            scale_x = 1.0f;
        }
        if (scale_y <= 0.0f) {
            scale_y = 1.0f;
        }
        // 419390/41a2b0: style 0 uses native 640-space scale, a 14px
        // glyph and a separate 9px advance. Positions are already doubled.
        const bool ascii = item->style == 0;
        if (ascii) {
            scale_x *= 2.0f;
            scale_y *= 2.0f;
        }
        float x = item->x;
        float y = item->y;
        float width = 0.0f;
        float height = 0.0f;
        for (const char* p = item->text; *p != '\0'; ++p) {
            anm_set_sprite(vm, res, glyph_sprite(item->style, static_cast<uint8_t>(*p)));
            // 41a2b0 subtracts 12/4 for alignment, then advances by +12/+4.
            // The comma's -4 alignment delta is not a negative glyph width.
            width += numeric ? (*p == ',' ? 4.0f : 12.0f) * scale_x
                             : (ascii ? 9.0f : vm->sprite_w) * scale_x;
            height = (numeric ? 16.0f : vm->sprite_h) * scale_y;
        }
        if (item->dirty_130 == 0) x -= width * 0.5f;
        if (item->dirty_130 == 2) x -= width;
        if (item->dirty_134 == 0) y -= height * 0.5f;
        if (item->dirty_134 == 2) y -= height;
        float numeric_advance = 12.0f * scale_x;
        for (const char* p = item->text; *p != '\0'; ++p) {
            uint8_t c = static_cast<uint8_t>(*p);
            if ((numeric || ascii) && c == ' ') {
                x += numeric ? numeric_advance : 9.0f * scale_x;
                continue;
            }
            if (c == '\n') {
                x = item->x;
                y += ascii ? 7.0f * scale_y * scale_y : 32.0f * scale_y;
                continue;
            }
            anm_set_sprite(vm, res, glyph_sprite(item->style, c));
            vm->pos_x = x + vm->sprite_w * scale_x * 0.5f;
            // Native numeric glyphs reset Y to the item's baseline; commas
            // add 3 screen pixels, independent of glyph scale (6 in our canvas).
            vm->pos_y = (numeric ? item->y : y) + vm->sprite_h * scale_y * 0.5f +
                (numeric && c == ',' ? 6.0f : 0.0f);
            vm->pos_z = item->z;
            vm->scale_x = scale_x;
            vm->scale_y = scale_y;
            vm->color = item->color;
            vm->color_alt = item->color;
            vm->flags2 = (vm->flags2 & ~(0x7c000000u | 0x3u)) | (0x08u << 26) | 0x3u;
            // 队列采用 1280 虚拟坐标；字模原始尺寸由投影缩为半尺寸。
            vm->flags3 = (vm->flags3 & ~(0x1c00000u | 0x180u)) | 0x800000u;
            // Reset filtering per style; a preceding shadow must not leak its
            // point filter into another font using this same embedded VM.
            const bool point = item->style == 3 || item->style == 5 ||
                (ascii && scale_x != 2.0f) ||
                (item->style >= 6 && item->style <= 9 && scale_x != 1.0f);
            vm->flags3 = (vm->flags3 & ~0x2000u) | (point ? 0x2000u : 0);
            if (item->dirty_124 != 0) {
                // 41a2b0: black, half-alpha shadow at +2,+2 screen pixels.
                vm->pos_x += 4.0f;
                vm->pos_y += 4.0f;
                vm->color = vm->color_alt = (item->color >> 25) << 24;
                vm->flags3 = (vm->flags3 & ~0x2000u) | (scale_x == 2.0f ? 0x2000u : 0);
                anm_draw_vm(vm);
                vm->pos_x -= 4.0f;
                vm->pos_y -= 4.0f;
                vm->color = vm->color_alt = item->color;
            }
            anm_draw_vm(vm);
            numeric_advance = (c == ',' ? 4.0f : 12.0f) * scale_x;
            x += numeric ? numeric_advance : (ascii ? 9.0f : vm->sprite_w) * scale_x;
        }
    }
}
// 入队前的属性设置：直接写 TextManager 的全局字段（原版 HUD 每帧这么写）。
void text_set_color(uint32_t argb) {
    store<uint32_t>(reinterpret_cast<uint8_t*>(g_text_manager), kColorOffset, argb);
}

void text_set_group(uint32_t group) {
    store<uint32_t>(reinterpret_cast<uint8_t*>(g_text_manager), kAuxOffset + 8, group);
}

void text_set_life(int frames) {
    store<int32_t>(reinterpret_cast<uint8_t*>(g_text_manager), kLifeOffset, frames);
}

void text_set_size(float scale_x, float scale_y) {
    uint8_t* base = reinterpret_cast<uint8_t*>(g_text_manager);
    store<float>(base, kSizeXOffset, scale_x);
    store<float>(base, kSizeYOffset, scale_y);
}

void text_set_style(uint32_t style) {
    store<uint32_t>(reinterpret_cast<uint8_t*>(g_text_manager), kStyleOffset, style);
}

void text_set_secondary_color(uint32_t argb) {
    store<uint32_t>(reinterpret_cast<uint8_t*>(g_text_manager), kColor2Offset, argb);
}

void text_set_shadow(bool enabled) {
    store<uint32_t>(reinterpret_cast<uint8_t*>(g_text_manager), kAuxOffset, enabled ? 1u : 0u);
}

void text_set_alignment(int horizontal, int vertical) {
    uint8_t* base = reinterpret_cast<uint8_t*>(g_text_manager);
    store<int32_t>(base, kFrameOffset, horizontal);
    store<int32_t>(base, kFrameOffset + 4, vertical);
}

namespace {
HFONT g_sjis_fonts[12];
bool g_meiryo;
constexpr float kSjisFontSize[12] = {16.5f,16.5f,16.5f,16.5f,20.5f,20.5f,20.5f,20.5f,14,12,32,32};

int CALLBACK found_sjis_font(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM) {
    g_meiryo = true;
    return 0;
}

void init_sjis_fonts() {
    // 0x4704c0，字体存在性决定原版的字体与源图裁剪位置。
    constexpr const wchar_t* meiryo = L"\u30e1\u30a4\u30ea\u30aa";
    constexpr const wchar_t* gothic = L"\uff2d\uff33 \u30b4\u30b7\u30c3\u30af";
    constexpr const wchar_t* mincho = L"\uff2d\uff33 \u660e\u671d";
    LOGFONTW query{};
    std::wcscpy(query.lfFaceName, meiryo);
    query.lfCharSet = DEFAULT_CHARSET;
    HDC dc = GetDC(nullptr);
    g_meiryo = false;
    EnumFontFamiliesExW(dc, &query, found_sjis_font, 0, 0);
    ReleaseDC(nullptr, dc);
    const int heights[12] = {32,32,15,15,40,40,15,15,28,24,64,64};
    const int meiryo_heights[12] = {48,32,16,15,60,40,16,15,42,36,96,64};
    for (int i = 0; i < 12; ++i) {
        const bool serif = i == 1 || i == 3 || i == 5 || i == 7 || i == 11;
        const bool small_font = i == 2 || i == 3 || i == 6 || i == 7;
        const bool use_meiryo = g_meiryo && !serif;
        g_sjis_fonts[i] = CreateFontW(use_meiryo ? meiryo_heights[i] : heights[i], 0, 0, 0,
                                     serif ? (small_font ? 700 : 600) : (small_font && !g_meiryo ? 700 : 400),
                                     0, 0, 0, SHIFTJIS_CHARSET, 0, 0, 2,
                                     use_meiryo ? 1 : 0x11, use_meiryo ? meiryo : serif ? mincho : gothic);
    }
}

void destroy_sjis_fonts() {
    for (HFONT font : g_sjis_fonts) DeleteObject(font);
}

void draw_sjis_texture(AnmVm* vm, uint32_t color, uint32_t outline_color,
                       int font, int x, int spacing, const char* sjis) {
    // 0x470a40/4703a0/470010：工作图保持原A4R4G4B4的alpha与边缘颜色。
    constexpr int width = 1024;
    constexpr int height = 129;
    const int work_height = std::max(17, static_cast<int>(kSjisFontSize[font])) * 2 + 12;
    BITMAPV4HEADER bitmap{};
    bitmap.bV4Size = sizeof(bitmap);
    bitmap.bV4Width = width;
    bitmap.bV4Height = -height;
    bitmap.bV4Planes = 1;
    bitmap.bV4BitCount = 16;
    bitmap.bV4V4Compression = BI_BITFIELDS;
    bitmap.bV4RedMask = 0x0f00;
    bitmap.bV4GreenMask = 0x00f0;
    bitmap.bV4BlueMask = 0x000f;
    bitmap.bV4AlphaMask = 0xf000;
    HDC dc = CreateCompatibleDC(nullptr);
    uint16_t* pixels;
    HBITMAP dib = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bitmap), DIB_RGB_COLORS,
                                  reinterpret_cast<void**>(&pixels), nullptr, 0);
    HGDIOBJ old_bitmap = SelectObject(dc, dib);
    HGDIOBJ old_font = SelectObject(dc, g_sjis_fonts[font]);
    SetBkMode(dc, TRANSPARENT);
    std::memset(pixels, 0, width * height * 2);
    for (int i = 0; i < width * work_height; ++i) pixels[i] = 0xf000;
    const int bytes = static_cast<int>(std::strlen(sjis));
    const auto draw = [&](int xx, int yy, const char* str, int count) {
        std::vector<wchar_t> wide(count);
        const int length = MultiByteToWideChar(932, 0, str, count, wide.data(), count);
        TextOutW(dc, xx, yy, wide.data(), length);
    };
    const bool outline = (vm->flags3 & 0x4000) == 0;
    if (spacing == 0) {
        x += 2;
        if (outline) {
            SetTextColor(dc, outline_color);
            const int offsets[8][2] = {{1,1},{-1,1},{1,3},{-1,3},{2,2},{-2,2},{0,0},{0,4}};
            for (const auto& offset : offsets) draw(x + offset[0], offset[1], sjis, bytes);
        }
        SetTextColor(dc, color);
        draw(x, 2, sjis, bytes);
    } else {
        for (int i = 0; i < bytes; i += 2, x += spacing) {
            if (outline) {
                SetTextColor(dc, outline_color);
                draw(x + 2, 4, sjis + i, 2); draw(x - 2, 4, sjis + i, 2);
                draw(x + 2, 0, sjis + i, 2); draw(x - 2, 0, sjis + i, 2);
            }
            SetTextColor(dc, color);
            draw(x, 2, sjis + i, 2);
        }
    }
    GdiFlush();
    for (int i = 0; i < width * work_height; ++i) {
        pixels[i] ^= 0xf000;
        if ((pixels[i] & 0xf000) != 0 && (pixels[i] & 0xf000) != 0xf000) pixels[i] |= 0xf000;
    }
    std::vector<uint16_t> source(pixels, pixels + width * (work_height + 1));
    for (int i = width; i < width * (work_height - 1); ++i) {
        if (source[i] < 0x1000 && i % width != 0) {
            const int alpha = ((source[i - width - 1] >> 12) + (source[i - width + 1] >> 12) +
                (source[i + width - 1] >> 12) + (source[i + width + 1] >> 12) +
                2 * ((source[i - width] >> 12) + (source[i + width] >> 12) +
                     (source[i - 1] >> 12) + (source[i + 1] >> 12))) / 14;
            pixels[i] = static_cast<uint16_t>((alpha << 12) | (pixels[i] & 0xfff));
        }
    }
    for (int i = 0; i < width * work_height; ++i) {
        if (pixels[i] >= 0x1000) continue;
        int r = 0, g = 0, b = 0, count = 0;
        const auto include = [&](int index) {
            const uint16_t pixel = pixels[index];
            if (pixel >= 0x1000) {
                r += (pixel >> 8) & 15; g += (pixel >> 4) & 15; b += pixel & 15;
                ++count;
            }
        };
        if (i % width != 0) include(i - 1);
        if (i % width != width - 1) include(i + 1);
        if (i >= width) include(i - width);
        if (i < width * (height - 2)) include(i + width);
        if (count > 1) { r /= count; g /= count; b /= count; }
        pixels[i] = static_cast<uint16_t>((r >> 1) << 8 | (g >> 1) << 4 | (b >> 1));
    }
    auto* texture = static_cast<IDirect3DTexture9*>(const_cast<void*>(vm->texture));
    D3DSURFACE_DESC desc;
    texture->GetLevelDesc(0, &desc);
    const RECT rect{static_cast<LONG>(vm->uv[0] * desc.Width), static_cast<LONG>(vm->uv[1] * desc.Height),
                    static_cast<LONG>(vm->uv[2] * desc.Width), static_cast<LONG>(vm->uv[3] * desc.Height)};
    const int copy_y = g_meiryo && font != 2 && font != 6 ? 6 : 0;
    D3DLOCKED_RECT locked;
    texture->LockRect(0, &locked, &rect, 0);
    for (int y = 0; y < rect.bottom - rect.top; ++y) {
        const uint16_t* row = pixels + (y + copy_y) * width;
        auto* target = static_cast<uint8_t*>(locked.pBits) + y * locked.Pitch;
        if (desc.Format == D3DFMT_A4R4G4B4) {
            std::memcpy(target, row, (rect.right - rect.left) * 2);
        } else {
            auto* dst = reinterpret_cast<uint32_t*>(target);
            for (int xx = 0; xx < rect.right - rect.left; ++xx) {
                const uint16_t p = row[xx];
                dst[xx] = (((p >> 12) & 15) << 24 | ((p >> 8) & 15) << 16 |
                           ((p >> 4) & 15) << 8 | (p & 15)) * 17u;
            }
        }
    }
    texture->UnlockRect(0);
    SelectObject(dc, old_font);
    SelectObject(dc, old_bitmap);
    DeleteObject(dib);
    DeleteDC(dc);
    vm->flags2 |= 1;
}
} // namespace

void text_write_texture(AnmVm* vm, uint32_t color, uint32_t outline_color,
                        int font, int x, int spacing, const char* sjis) {
    draw_sjis_texture(vm, color, outline_color, font, x * 2, spacing * 2, sjis);
}

void text_write_texture_centered(AnmVm* vm, uint32_t color, uint32_t outline_color,
                                 int font, int spacing, const char* sjis) {
    const int x = static_cast<int>(vm->sprite_w * 0.5f -
                                   (kSjisFontSize[font] * 2 - 1) * std::strlen(sjis) * 0.25f);
    draw_sjis_texture(vm, color, outline_color, font, x, spacing * 2, sjis);
}
