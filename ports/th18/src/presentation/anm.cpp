#include "anm.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <vector>
#include <list>

#include "../platform/gfx.h"
#include "../core/rng.h"
#include "../core/thread.h"
#include "../projectiles/laser.h"
#include "../resources/dat.h"
#include "../storage/storage.h"
#include "../stage/stage.h"
#include "../stage/background.h"
#include "../stage/session.h"
#include "../stage/extra_effects.h"
#include "text.h"

// windows.h 的 min/max 宏会破坏 std::min 的显式实参。
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

static void update_vm_self(AnmVm* vm);

// ANM v8 运行时（原版 AnmManager / AnmVm）的 P1 实现。
// 已确认的原版地址见 anm.h；未确认/未实现的部分只在函数体里按 opcode 注释说明。
// 绘制走 platform/gfx.h 的 gfx_device()，本层不保存第二份设备指针。

namespace {

constexpr int kResourceSlots = 0x200;
constexpr int kAllocatedVmSlots = 1024;
AnmVm* g_allocated_vms[kAllocatedVmSlots];
std::list<AnmVm*> g_independent_vms;

bool managed_vm(AnmVm* vm)
{
    while (vm->parent != nullptr) vm = static_cast<AnmVm*>(vm->parent);
    return vm->owner_slot != nullptr ||
           std::find(g_independent_vms.begin(), g_independent_vms.end(), vm) != g_independent_vms.end();
}

void collect_managed_delete(AnmVm* vm, std::list<AnmVm*>& pending)
{
    // 488110先标记整个子树；所有节点都要等本pass遍历完才归还。
    for (AnmVm* child : g_allocated_vms)
        if (child != nullptr && child->parent == vm) collect_managed_delete(child, pending);
    if ((vm->flags3 & 0x180u) != 0x100u) pending.push_front(vm);
    vm->flags3 = (vm->flags3 & ~0x80u) | 0x100u;
    vm->parent = nullptr;
}

ThreadStatus update_managed(void* context)
{
    const bool second = context != nullptr;
    // 488220的Session门；488250第二队列不检查这一条件。
    if (!second && stage_session() != nullptr) {
        const uint32_t flags = stage_session_flags();
        if ((flags & 5u) != 0 && (flags & 2u) != 0) return THREAD_CONTINUE;
    }
    AnmManager* manager = anm_manager();
    std::list<AnmVm*> pending;
    AnmVm* vm = static_cast<AnmVm*>(second ? manager->queue_head2 : manager->queue_head);
    while (vm != nullptr) {
        AnmVm* next = static_cast<AnmVm*>(vm->node_next);
        if (managed_vm(vm)) {
            const uint32_t state = vm->flags3 & 0x180u;
            if (state == 0x80u) collect_managed_delete(vm, pending);
            else if (state == 0) {
                update_vm_self(vm);
                if (vm->script_cursor < 0 && (vm->flags2 & 1u) == 0)
                    collect_managed_delete(vm, pending);
            }
        }
        vm = next;
    }
    for (AnmVm* ended : pending) anm_vm_free(ended);
    return THREAD_CONTINUE;
}

AnmManager* g_manager = nullptr;
int g_last_blend = -1;
float g_screen_w = 1280.0f;
float g_screen_h = 960.0f;

ThreadStatus draw_managed(void* context)
{
    const int pass = static_cast<int>(reinterpret_cast<intptr_t>(context));
    const int layer = pass & 255;
    if ((layer == 0 || layer == 2) && stage::g_stage != nullptr) {
        stage_background_render_anm(layer);
        return THREAD_CONTINUE;
    }
    if (pass & 0x100) {
        if (layer == 37) gfx_select_camera(2);
        if (layer == 41) gfx_select_camera(2, true);
    } else {
        if (layer == 3) gfx_select_camera(3);
        if (layer == 20 || layer == 23) gfx_select_camera(1);
        if (layer == 24) gfx_select_camera(2);
        if (layer == 28 || layer == 29) gfx_select_camera(2, true);
    }
    anm_draw_layer(layer);
    if (layer == 28 || layer == 29 || (layer == 41 && (pass & 0x100)))
        gfx_select_camera(2);
    return THREAD_CONTINUE;
}

// ANM 的随机变量使用 DAT_004cf280，和游戏弹幕的 DAT_004cf288 分开。
uint32_t anm_random_u32()
{
    Rng& random = card_rng();
    const uint32_t a = ((random.state ^ 0x9630u) + 0x9aadu) & 0xffffu;
    const uint32_t b = ((((a >> 14) + a * 4) ^ 0x9630u) + 0x9aadu) & 0xffffu;
    random.state = static_cast<uint16_t>(((b >> 14) + b * 4) & 0xffffu);
    random.counter += 2;
    return (a << 16) | b;
}

float anm_random01()
{
    return static_cast<float>(anm_random_u32()) * 2.3283064e-10f;
}

float total_rotation_z(const AnmVm* vm)
{
    float angle = vm->rot_z;
    if (vm->parent != nullptr && (vm->flags3 & 0x40000u) == 0) {
        const auto* root = static_cast<const AnmVm*>(vm->parent);
        while (root->parent != nullptr) root = static_cast<const AnmVm*>(root->parent);
        angle += root->rot_z;
    }
    return angle;
}

float* float_variable(AnmVm* vm, float* argument)
{
    const int id = static_cast<int>(*argument);
    uint8_t* raw = reinterpret_cast<uint8_t*>(vm);
    switch (id)
    {
    case 10004: return reinterpret_cast<float*>(raw + 0x4b4);
    case 10005: return reinterpret_cast<float*>(raw + 0x4b8);
    case 10006: return reinterpret_cast<float*>(raw + 0x4bc);
    case 10007: return reinterpret_cast<float*>(raw + 0x4c0);
    case 10013: return &vm->pos_x; // 0x477fdd
    case 10014: return &vm->pos_y; // 0x477fe6
    case 10015: return &vm->pos_z;
    case 10023: return &vm->rot_a;
    case 10024: return &vm->rot_b;
    case 10025: return &vm->rot_z;
    case 10027: return reinterpret_cast<float*>(raw + 0x4d8);
    case 10028: return reinterpret_cast<float*>(raw + 0x4dc);
    case 10033: return reinterpret_cast<float*>(raw + 0x4c4);
    case 10034: return reinterpret_cast<float*>(raw + 0x4c8);
    case 10035: return reinterpret_cast<float*>(raw + 0x4cc);
    default: return argument;
    }
}

float float_argument(AnmVm* vm, const uint8_t* payload, uint16_t mask, int index)
{
    float value = *reinterpret_cast<const float*>(payload + index * 4);
    if ((mask & (1u << index)) == 0)
        return value;
    const int id = static_cast<int>(value);
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(vm);
    if (id >= 10000 && id <= 10003)
        return static_cast<float>(*reinterpret_cast<const int32_t*>(raw + 0x4a4 + (id - 10000) * 4));
    if (id == 10008 || id == 10009)
        return static_cast<float>(*reinterpret_cast<const int32_t*>(raw + 0x4d0 + (id - 10008) * 4));
    if (id == 10010)
        return (anm_random01() * 2.0f - 1.0f) * *reinterpret_cast<const float*>(raw + 0x4dc);
    if (id == 10011)
        return anm_random01() * *reinterpret_cast<const float*>(raw + 0x4d8);
    if (id == 10012)
        return (anm_random01() * 2.0f - 1.0f) * *reinterpret_cast<const float*>(raw + 0x4d8);
    if (id == 10022) return static_cast<float>(anm_random_u32());
    if (id >= 10016 && id <= 10021) return stage_background_anm_value(id);
    if (id == 10026) return total_rotation_z(vm);
    if (id == 10029) return static_cast<float>(*reinterpret_cast<const int32_t*>(raw + 0x4e0));
    return *float_variable(vm, &value);
}

// TH 像素格式（anm_file.h PixelFormat）→ D3D9 格式 + 每像素字节数。
// 原版用表 DAT_004b4944（D3DFORMAT）与 DAT_004b4920（bpp）。
struct TextureFormat {
    D3DFORMAT d3d;
    uint32_t bytes;
};

const TextureFormat kFormats[8] = {
    {D3DFMT_UNKNOWN, 0},      // 0
    {D3DFMT_A8R8G8B8, 4},     // 1 BGRA8888
    {D3DFMT_UNKNOWN, 0},      // 2
    {D3DFMT_R5G6B5, 2},       // 3 RGB565
    {D3DFMT_UNKNOWN, 0},      // 4
    {D3DFMT_A4R4G4B4, 2},     // 5 ARGB4444
    {D3DFMT_UNKNOWN, 0},      // 6
    {D3DFMT_L8, 1},           // 7 GRAY8
};

struct SpriteVertex {
    float x, y, z;
    uint32_t color;
    float u, v;
};

constexpr uint32_t kFvfSprite = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;

void set_matrix(IDirect3DDevice9* dev, D3DTRANSFORMSTATETYPE state, const D3DMATRIX& m)
{
    dev->SetTransform(state, &m);
}

// 正交投影：像素坐标 (0,0) 左上，(g_screen_w, g_screen_h) 右下。
void setup_projection(IDirect3DDevice9* dev)
{
    D3DMATRIX proj;
    std::memset(&proj, 0, sizeof(proj));
    proj._11 = 2.0f / g_screen_w;
    proj._22 = -2.0f / g_screen_h;
    proj._33 = 1.0f;
    proj._41 = -1.0f;
    proj._42 = 1.0f;
    proj._44 = 1.0f;
    set_matrix(dev, D3DTS_PROJECTION, proj);

    D3DMATRIX view;
    std::memset(&view, 0, sizeof(view));
    view._11 = view._22 = view._33 = view._44 = 1.0f;
    set_matrix(dev, D3DTS_VIEW, view);
}

// sub_486bc0：单段贴图上传。原版走 D3DXCreateTexture + D3DXLoadSurfaceFromMemory，
// 字节流布局相同（thtx 头 +0x10 起像素数据，行跨度 = w * bpp）。
void upload_segment(AnmResource* res, uint32_t index)
{
    const AnmSegment& segment = res->segments[index];
    const AnmHeader* header = segment.header;
    const TextureFormat& format = kFormats[header->format];

    IDirect3DTexture9* texture = nullptr;
    gfx_device()->CreateTexture(header->width, header->height, 1, 0, format.d3d,
                                D3DPOOL_MANAGED, &texture, nullptr);
    if (texture == nullptr)
        return;

    const AnmThxHeader* thx = reinterpret_cast<const AnmThxHeader*>(segment.thtx);
    D3DLOCKED_RECT locked;
    texture->LockRect(0, &locked, nullptr, 0);
    // THTX 的 width/height 可以小于段声明尺寸（ascii.anm 的 512x288 数据放进 512x512 贴图），
    // 只拷 THTX 覆盖的行，剩余行清零。
    const uint32_t row_bytes = thx != nullptr ? thx->width * format.bytes : 0;
    const uint8_t* src = thx != nullptr ? thx->data : nullptr;
    for (uint32_t y = 0; y < header->height; ++y) {
        uint8_t* dst = static_cast<uint8_t*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch;
        if (thx != nullptr && y < thx->height) {
            std::memcpy(dst, src + static_cast<size_t>(y) * row_bytes, row_bytes);
        } else {
            std::memset(dst, 0, static_cast<size_t>(header->width) * format.bytes);
        }
    }
    texture->UnlockRect(0);
    res->segment_textures[index] = texture;
}

// 指令流表和 sprite 表是两张独立的表。op300 可以引用任意段里的 sprite，
// 并不要求它与当前脚本同号或同段（标题脚本 36 使用无脚本段里的 sprite 4）。
void build_scripts(AnmResource* res)
{
    uint32_t entry = 0;
    for (uint32_t seg_index = 0; seg_index < res->segment_count; ++seg_index)
    {
        const AnmSegment& segment = res->segments[seg_index];
        const AnmHeader* header = segment.header;
        for (uint32_t s = 0; s < header->scripts; ++s)
        {
            AnmScriptEntry& script = res->scripts[entry++];
            const AnmScriptOffset& offset = segment.script_offsets[s];
            script.id = offset.id;
            script.stream = segment.base + offset.offset;
            script.segment_index = seg_index;
        }
    }
}

AnmResource* load_resource(int index, const char* name)
{
    storage_log("anm: load %s idx=%d", name, index);
    AnmManager* manager = anm_manager();
    storage_log("anm: manager ok");
    AnmResource* res = static_cast<AnmResource*>(calloc(1, sizeof(AnmResource)));
    res->index = static_cast<uint32_t>(index);
    std::strcpy(reinterpret_cast<char*>(res->name), name);
    storage_log("anm: res ok name_off=%d", static_cast<int>(offsetof(AnmResource, name)));
    manager->resources[index] = res;

    anm_load_file(res, name);
    storage_log("anm: load_file ok segs=%u sprites=%u scripts=%u", res->segment_count,
                res->sprite_count, res->script_count);
    // 原版贴图上传由独立线程推进（sub_486920），这里加载即同步上传。
    anm_upload_step(manager, res);
    storage_log("anm: upload ok");
    return res;
}

} // namespace

AnmManager* anm_manager()
{
    if (g_manager != nullptr)
        return g_manager;

    g_manager = static_cast<AnmManager*>(calloc(1, sizeof(AnmManager)));
    g_manager->resources = static_cast<AnmResource**>(calloc(kResourceSlots, sizeof(AnmResource*)));
    g_manager->queue_id = 1;
    g_manager->color_r = 0x80;
    g_manager->color_g = 0x80;
    g_manager->color_b = 0x80;
    g_manager->color_a = 0x80;
    // 483820：两个原逻辑pass，各按自己的队列顺序推进VM自身。
    for (int second = 0; second < 2; ++second) {
        Thread* task = thread_create(update_managed);
        task->flags |= 2;
        thread_set_ctx(task, reinterpret_cast<void*>(static_cast<intptr_t>(second)));
        thread_set_priority(task, second ? 0x0b : 0x22);
        thread_enqueue_logic(task);
    }
    // sub_483820：两队列沿同一组实际B任务分段绘制。
    constexpr uint8_t priorities[33] = {
        0x05,0x07,0x09,0x0a,0x0b,0x0d,0x10,0x12,0x14,0x15,0x16,
        0x18,0x1b,0x1c,0x1f,0x20,0x23,0x25,0x28,0x2b,0x2e,0x2f,
        0x36,0x38,0x3c,0x3d,0x40,0x42,0x45,0x47,0x54,0x56,0x59
    };
    for (int layer = 0; layer < 33; ++layer) {
        Thread* task = thread_create(draw_managed);
        thread_set_ctx(task, reinterpret_cast<void*>(static_cast<intptr_t>(layer)));
        thread_set_priority(task, priorities[layer]);
        thread_enqueue_render(task);
    }
    constexpr uint8_t second_layers[] = {37,38,39,40,41,41,43,44,45};
    constexpr uint8_t second_priorities[] = {0x39,0x3e,0x41,0x43,0x44,0x46,0x55,0x57,0x5a};
    for (int i = 0; i < 9; ++i) {
        Thread* task = thread_create(draw_managed);
        thread_set_ctx(task, reinterpret_cast<void*>(static_cast<intptr_t>(second_layers[i] | 0x100)));
        thread_set_priority(task, second_priorities[i]);
        thread_enqueue_render(task);
    }
    return g_manager;
}

void anm_set_screen(float width, float height)
{
    g_screen_w = width;
    g_screen_h = height;
}

void anm_load_file(AnmResource* res, const char* name)
{
    size_t size = 0;
    res->file_data = static_cast<uint8_t*>(file_read(name, &size, false));
    if (res->file_data == nullptr)
        return;

    res->segment_count = anm_count_segments(res->file_data);
    res->segments = static_cast<AnmSegment*>(malloc(res->segment_count * sizeof(AnmSegment)));
    anm_parse_segments(res->file_data, res->segments, res->segment_count);

    uint32_t sprites = 0;
    uint32_t scripts = 0;
    for (uint32_t i = 0; i < res->segment_count; ++i)
    {
        sprites += res->segments[i].header->sprites;
        scripts += res->segments[i].header->scripts;
    }
    res->sprite_count = sprites;
    res->script_count = scripts;

    res->segment_textures = static_cast<void**>(calloc(res->segment_count, sizeof(void*)));
    res->scripts = static_cast<AnmScriptEntry*>(calloc(scripts, sizeof(AnmScriptEntry)));
    build_scripts(res);

    // sub_4865b0：每 sprite 一个 VM（0x60c 字节）。脚本也会复用这些 VM，
    // 所以池大小取 sprite 与 script 的较大者。
    const uint32_t vm_count = sprites > scripts ? sprites : scripts;
    res->vms = static_cast<uint8_t*>(calloc(vm_count, sizeof(AnmVm)));
    for (uint32_t i = 0; i < vm_count; ++i)
    {
        AnmVm* vm = anm_vm_at(res, i);
        vm->anm_index = res->index;
        vm->anm_table_index = static_cast<int32_t>(res->index);
        vm->sprite_index = static_cast<int32_t>(i);
        vm->sprite_id = static_cast<uint16_t>(i);
        vm->color = 0xffffffff;
        vm->color_alt = 0xffffffff;
        vm->flags2 = static_cast<uint32_t>(ANM_TYPE_08) << 26;
        vm->scale_x = 1.0f;
        vm->scale_y = 1.0f;
        vm->scale_z = 1.0f;
        vm->script_cursor = -1;
    }
}

AnmResource* anm_load_resource(int index, const char* name)
{
    return load_resource(index, name);
}

AnmResource* anm_get(int index, const char* name)
{
    AnmManager* manager = anm_manager();
    AnmResource* res = manager->resources[index];
    if (res != nullptr)
        return res;
    return load_resource(index, name);
}

void anm_upload_step(AnmManager* manager, AnmResource* res)
{
    for (uint32_t i = 0; i < res->segment_count; ++i)
    {
        // '@'是原格式声明的空白动态贴图；text59的暂停底图使用其中segment1。
        if (res->segment_textures[i] == nullptr &&
            (res->segments[i].thtx != nullptr || res->segments[i].name[0] == '@'))
            upload_segment(res, i);
    }
    res->upload_state = 0;
}

void anm_load_image(AnmResource* res, uint32_t segment_index, const void* data, size_t size)
{
    using LoadSurfaceImage = HRESULT(WINAPI*)(IDirect3DSurface9*, const PALETTEENTRY*, const RECT*,
                                      const void*, UINT, const RECT*, DWORD, D3DCOLOR, void*);
    static const auto load = reinterpret_cast<LoadSurfaceImage>(GetProcAddress(
        GetModuleHandleA("d3dx9_43.dll"), "D3DXLoadSurfaceFromFileInMemory"));
    auto* texture = static_cast<IDirect3DTexture9*>(res->segment_textures[segment_index]);
    IDirect3DSurface9* surface;
    texture->GetSurfaceLevel(0, &surface);
    load(surface, nullptr, nullptr, data, static_cast<UINT>(size), nullptr, 1, 0, nullptr);
    D3DSURFACE_DESC description;
    surface->GetDesc(&description);
    D3DLOCKED_RECT locked;
    surface->LockRect(&locked, nullptr, 0);
    // Help的动态segment是A8R8G8B8；4858e0只从四邻有alpha的像素取RGB，保留alpha0。
    for (UINT y = 0; y < description.Height; ++y) {
        auto* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(locked.pBits) + y * locked.Pitch);
        for (UINT x = 0; x < description.Width; ++x) {
            if (row[x] >> 24) continue;
            uint32_t red = 0, green = 0, blue = 0, count = 0;
            const auto accumulate = [&](uint32_t color) {
                if (color >> 24) {
                    red += (color >> 16) & 255;
                    green += (color >> 8) & 255;
                    blue += color & 255;
                    ++count;
                }
            };
            if (x != 0) accumulate(row[x - 1]);
            if (x + 1 < description.Width) accumulate(row[x + 1]);
            if (y != 0) accumulate(*reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(row + x) - locked.Pitch));
            if (y + 1 < description.Height) accumulate(*reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(row + x) + locked.Pitch));
            if (count > 1) { red /= count; green /= count; blue /= count; }
            row[x] = (red << 16) | (green << 8) | blue;
        }
    }
    surface->UnlockRect();
    surface->Release();
}

// sub_488350：把 VM 插入绘制队列（尾插），返回句柄。
// 节点是 VM 内 +0x574 的三指针 {self, next, prev}。
uint32_t anm_queue_add(AnmVm* vm)
{
    AnmManager* manager = anm_manager();
    const bool second = (vm->node_flags & 4u) != 0;
    AnmVm* head = static_cast<AnmVm*>(second ? manager->queue_head2 : manager->queue_head);
    AnmVm* tail = static_cast<AnmVm*>(second ? manager->queue_tail2 : manager->queue_tail);

    vm->node_self = vm;
    if (vm->node_flags & 2u) {
        // sub_488400 / sub_488540：同一绘制队列的前端插入。
        vm->node_next = head;
        vm->node_prev = nullptr;
        if (head != nullptr)
            head->node_prev = vm;
        else
            tail = vm;
        head = vm;
    } else {
        vm->node_next = nullptr;
        vm->node_prev = tail;
        if (tail != nullptr)
            tail->node_next = vm;
        else
            head = vm;
        tail = vm;
    }
    if (second) {
        manager->queue_head2 = head;
        manager->queue_tail2 = tail;
    } else {
        manager->queue_head = head;
        manager->queue_tail = tail;
    }

    manager->queue_id = (manager->queue_id + 1) & 0x1ffff;
    if (manager->queue_id == 0)
        manager->queue_id = 1;
    vm->handle = (manager->queue_id << 15) | (vm->handle_mask & 0x7fff);
    return vm->handle;
}

// sub_488cf0：按句柄从队列移除。
void anm_queue_remove(uint32_t handle)
{
    AnmManager* manager = anm_manager();
    for (int second = 0; second < 2; ++second) {
    for (AnmVm* vm = static_cast<AnmVm*>(second ? manager->queue_head2 : manager->queue_head); vm != nullptr;
         vm = static_cast<AnmVm*>(vm->node_next))
    {
        if (vm->handle != handle)
            continue;

        AnmVm* prev = static_cast<AnmVm*>(vm->node_prev);
        AnmVm* next = static_cast<AnmVm*>(vm->node_next);
        if (prev != nullptr)
            prev->node_next = next;
        else
            (second ? manager->queue_head2 : manager->queue_head) = next;
        if (next != nullptr)
            next->node_prev = prev;
        else
            (second ? manager->queue_tail2 : manager->queue_tail) = prev;
        vm->node_self = nullptr;
        vm->node_next = nullptr;
        vm->node_prev = nullptr;
        return;
    }
    }
}

// sub_488260：按层号绘制队列中匹配的 VM。
// 层号换算与原版一致：0x26..0x2d 归一到 0x19..0x20，第一队列其余按原值。
void anm_draw_layer(int layer)
{
    AnmManager* manager = anm_manager();
    for (int second = 0; second < 2; ++second)
      for (AnmVm* vm = static_cast<AnmVm*>(second ? manager->queue_head2 : manager->queue_head); vm != nullptr; vm = static_cast<AnmVm*>(vm->node_next))
      {
        if (((vm->node_flags & 4u) != 0) != (second != 0)) continue;
        int32_t effective = vm->draw_layer - 0xd;
        if (static_cast<uint32_t>(vm->draw_layer - 0x25) > 8)
            effective = vm->draw_layer;
        if (vm->node_flags & 4u)
        {
            effective = vm->draw_layer;
            if (static_cast<uint32_t>(effective - 0x18) < 9)
                effective += 0xd;
            else if (effective < 0x25 || effective > 0x2d)
                effective = 0x27;
        }
        if (effective != layer)
            continue;
        anm_draw_vm(vm);
      }
}

// sub_481210 / sub_480160：单 VM 绘制。这里统一按 type 8 的精灵绘制路径实现，
// 其它类型的差异（线段/三角/雾/第二套贴图槽）待后续按原版函数补。
void anm_apply_blend(AnmVm* vm)
{
    IDirect3DDevice9* dev = gfx_device();
    const int mode = (vm->flags2 >> 5) & 15;
    if (g_last_blend != mode) {
        g_last_blend = mode;
        // sub_47d910先恢复独立alpha；406d00随后可覆盖它用于loading遮罩。
        dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
        dev->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_ZERO);
        dev->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
    }
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
    dev->SetRenderState(D3DRS_ALPHAREF, 0);
    switch ((vm->flags2 >> 5) & 15)
    {
    case 1:
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        break;
    case 2:
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_REVSUBTRACT);
        break;
    case 0:
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        break;
    case 3:
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO);
        break;
    case 4:
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_INVDESTCOLOR);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCCOLOR);
        break;
    case 5:
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_DESTCOLOR);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO);
        break;
    case 6:
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_INVSRCCOLOR);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        break;
    case 7:
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_DESTALPHA);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVDESTALPHA);
        break;
    case 8:
    case 9:
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        dev->SetRenderState(D3DRS_BLENDOP, ((vm->flags2 >> 5) & 15) == 8 ? D3DBLENDOP_MIN : D3DBLENDOP_MAX);
        break;
    }

}

void anm_draw_vm(AnmVm* vm)
{
    if ((vm->flags2 & 3) != 3)
        return;
    if ((vm->flags3 & 0x180) != 0)
        return;
    if(vm->callback_index==7){extra_maple_draw(vm);return;}
    AnmManager* manager = anm_manager();
    const uint32_t render_type = (vm->flags2 >> 26) & 31;
    const bool background_3d = render_type == 8 && vm->draw_layer == 2 && stage::g_stage != nullptr;
    const bool primitive = render_type == 0x10 || render_type == 0x11 || render_type == 0x12 || render_type == 0x14 ||
                           render_type == 0x15 || render_type == 0x16 || render_type == 0x1c;
    IDirect3DTexture9* texture = static_cast<IDirect3DTexture9*>(const_cast<void*>(vm->texture));
    if (texture == nullptr && !primitive)
        return;

    // 颜色：VM 颜色 × 管理器颜色乘数（原版右移 7 后 clamp）。
    // 小端 ARGB：B@0x524 G@0x525 R@0x526 A@0x527。
    uint32_t color = vm->color;
    const uint32_t color_mode = (vm->flags2 >> 17) & 7;
    if (color_mode == 1)
        color = vm->color_alt;
    else if (color_mode == 4)
    {
        uint32_t multiplied = 0;
        for (int shift = 0; shift < 32; shift += 8)
            multiplied |= (((vm->color >> shift) & 255) * ((vm->color_alt >> shift) & 255) / 255) << shift;
        color = multiplied;
    }
    uint32_t r = ((color >> 16) & 0xff) * manager->color_r >> 7;
    uint32_t g = ((color >> 8) & 0xff) * manager->color_g >> 7;
    uint32_t b = (color & 0xff) * manager->color_b >> 7;
    uint32_t a = ((color >> 24) & 0xff) * manager->color_a >> 7;
    if (r > 0xff) r = 0xff;
    if (g > 0xff) g = 0xff;
    if (b > 0xff) b = 0xff;
    if (a > 0xff) a = 0xff;
    color = (a << 24) | (r << 16) | (g << 8) | b;

    IDirect3DDevice9* dev = gfx_device();
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, background_3d ? TRUE : FALSE);
    if (background_3d)
        dev->SetRenderState(D3DRS_ZWRITEENABLE, (vm->flags2 & 0x2000u) == 0);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_TEXTUREFACTOR, color);

    // sub_47d910：混合由 op303 的 flags2 bit5..8 决定；op313 是坐标缩放。
    anm_apply_blend(vm);

    dev->SetTexture(0, primitive ? nullptr : texture);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, primitive ? D3DTOP_SELECTARG2 : D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, primitive ? D3DTOP_SELECTARG2 : D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

    // 文字画笔会逐字改位置后立即绘制，因此变换取当前字段，不依赖上次 update 的矩阵。
    D3DMATRIX world{};
    const float rotation_z = total_rotation_z(vm);
    const float cz = std::cos(rotation_z), sz = std::sin(rotation_z);
    const float cx = std::cos(vm->rot_a), sx = std::sin(vm->rot_a);
    const float cy = std::cos(vm->rot_b), sy = std::sin(vm->rot_b);
    world._11 = cy * cz * vm->scale_x;
    world._12 = cy * sz * vm->scale_x;
    world._13 = -sy * vm->scale_x;
    world._21 = (sx * sy * cz - cx * sz) * vm->scale_y;
    world._22 = (sx * sy * sz + cx * cz) * vm->scale_y;
    world._23 = sx * cy * vm->scale_y;
    if (render_type == 1 || render_type == 3 || primitive) {
        // sub_481210 -> sub_47ed50：这两类是平面旋转精灵，仅使用Z角。
        // abcard12 同时写X/Z角；把X角送入D3D深度会裁掉大部分卡面。
        world._11 = cz * vm->scale_x;
        world._12 = sz * vm->scale_x;
        world._13 = 0.0f;
        world._21 = -sz * vm->scale_y;
        world._22 = cz * vm->scale_y;
        world._23 = 0.0f;
    }
    if(vm->sprite_callback==1) {
        const float scale=stage::bullet_scale_for_vm(vm);
        world._11*=scale;world._12*=scale;world._13*=scale;
        world._21*=scale;world._22*=scale;world._23*=scale;
    }
    world._33 = 1.0f;
    const float* offset = reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(vm) + 0x4e4);
    world._41 = vm->pos_x + vm->extra_x + offset[0];
    world._42 = vm->pos_y + vm->extra_y + offset[1];
    world._43 = render_type == 8 ? vm->pos_z + vm->extra_z + offset[2] : 0.0f;
    world._44 = 1.0f;
    // sub_4063d0 / sub_47e8f0：op313=2 的素材用 1280 坐标，其余标题
    // primitive 用 640 坐标。当前投影统一为 1280，所以后者在这里乘二。
    const uint32_t coordinate_mode = (vm->flags3 >> 22) & 7;
    // Background layer 2 uses native world units, not the 1280 UI canvas.
    const float position_scale = (background_3d || coordinate_mode == 2 || coordinate_mode == 4) ? 1.0f : 2.0f;
    const float size_scale = (background_3d || coordinate_mode == 2) ? 1.0f : 2.0f;
    world._11 *= size_scale;
    world._12 *= size_scale;
    world._13 *= size_scale;
    world._21 *= size_scale;
    world._22 *= size_scale;
    world._23 *= size_scale;
    world._41 *= position_scale;
    world._42 *= position_scale;
    world._43 *= position_scale;
    const AnmVm* position_root = vm;
    while (position_root->parent != nullptr && (position_root->flags3 & 0x40000u) == 0)
    {
        const AnmVm* parent = static_cast<const AnmVm*>(position_root->parent);
        const uint32_t mode = (parent->flags3 >> 22) & 7;
        const float factor = (mode == 2 || mode == 4) ? 1.0f : 2.0f;
        const float* parent_offset = reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(parent) + 0x4e4);
        world._41 += (parent->pos_x + parent->extra_x + parent_offset[0]) * factor;
        world._42 += (parent->pos_y + parent->extra_y + parent_offset[1]) * factor;
        if (render_type == 8) world._43 += (parent->pos_z + parent->extra_z + parent_offset[2]) * factor;
        position_root = parent;
    }
    if (!background_3d && (position_root->flags3 & 0x300000u) == 0x200000u) {
        // 4063d0: UI layers 20..23 add the origin once, at the root.
        // Fold the original camera's gameplay viewport into our 1280 scene.
        // Gameplay VMs (0x100000) already carry their scene origin in extra_x/y.
        world._41 += 448.0f;
        world._42 += 32.0f;
    }
    // sub_47e8f0：父VM的比例同样参与子精灵大小（abcard0 的 .75 外壳）。
    if (vm->parent != nullptr && (vm->flags3 & 0x40000u) == 0) {
        const AnmVm* parent = static_cast<const AnmVm*>(vm->parent);
        world._11 *= parent->scale_x;
        world._12 *= parent->scale_x;
        world._13 *= parent->scale_x;
        world._21 *= parent->scale_y;
        world._22 *= parent->scale_y;
        world._23 *= parent->scale_y;
    }
    set_matrix(dev, D3DTS_WORLD, world);

    const float half_w = vm->sprite_w * 0.5f;
    const float half_h = vm->sprite_h * 0.5f;
    const int anchor_x = (vm->flags2 >> 22) & 3;
    const int anchor_y = (vm->flags2 >> 24) & 3;
    const float left = (anchor_x == 1 ? 0.0f : (anchor_x == 2 ? -vm->sprite_w : -half_w)) - vm->origin_x;
    const float top = (anchor_y == 1 ? 0.0f : (anchor_y == 2 ? -vm->sprite_h : -half_h)) - vm->origin_y;
    const float right = left + vm->sprite_w;
    const float bottom = top + vm->sprite_h;
    uint32_t right_color = color;
    if (render_type == 0x14 || render_type == 0x16)
        right_color = vm->color_alt;
    const float u1 = vm->uv[0] + (vm->uv[2] - vm->uv[0]) * vm->uv_scale_u;
    const float v1 = vm->uv[1] + (vm->uv[3] - vm->uv[1]) * vm->uv_scale_v;
    SpriteVertex vertices[4] = {
        {left, top, 0.0f, color, vm->uv[0] + vm->uv_offset_u, vm->uv[1] + vm->uv_offset_v},
        {right, top, 0.0f, right_color, u1 + vm->uv_offset_u, vm->uv[1] + vm->uv_offset_v},
        {left, bottom, 0.0f, color, vm->uv[0] + vm->uv_offset_u, v1 + vm->uv_offset_v},
        {right, bottom, 0.0f, right_color, u1 + vm->uv_offset_u, v1 + vm->uv_offset_v},
    };

    const DWORD filter = (vm->flags3 & 0x2000) ? D3DTEXF_POINT : D3DTEXF_LINEAR;
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, filter);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, filter);
    // 47d910: ANM 312 values 0/1/2 map to D3D wrap/clamp/mirror.
    const D3DTEXTUREADDRESS address[3] = {D3DTADDRESS_WRAP, D3DTADDRESS_CLAMP, D3DTADDRESS_MIRROR};
    const uint32_t address_u = (vm->flags3 >> 2) & 3;
    const uint32_t address_v = vm->flags3 & 3;
    if (address_u < 3) dev->SetSamplerState(0, D3DSAMP_ADDRESSU, address[address_u]);
    if (address_v < 3) dev->SetSamplerState(0, D3DSAMP_ADDRESSV, address[address_v]);

    dev->SetFVF(kFvfSprite);
    if (render_type == 0x12) {
        // 481210 -> 482e70：无纹理的闭合线圆；605的半径不是精灵宽高。
        const int segments = static_cast<int>(vm->frame_counter);
        float radius = vm->rot_x * vm->scale_x * size_scale;
        if (vm->parent != nullptr && (vm->flags3 & 0x40000u) == 0)
            radius *= static_cast<const AnmVm*>(vm->parent)->scale_x;
        std::vector<SpriteVertex> line(segments + 1);
        for (int i = 0; i <= segments; ++i) {
            const float angle = rotation_z + i * 6.283185482f / segments;
            line[i] = {world._41 + std::cos(angle) * radius,
                       world._42 + std::sin(angle) * radius, 0, color, 0, 0};
        }
        D3DMATRIX identity{};
        identity._11 = identity._22 = identity._33 = identity._44 = 1;
        set_matrix(dev, D3DTS_WORLD, identity);
        dev->DrawPrimitiveUP(D3DPT_LINESTRIP, segments, line.data(), sizeof(SpriteVertex));
        ++manager->draw_count;
        return;
    }
    if (render_type == 0x11 || render_type == 0x1c)
    {
        const uint8_t factors[4] = {manager->color_b, manager->color_g, manager->color_r, manager->color_a};
        const auto modulate = [&](uint32_t raw_color) {
            uint32_t result = 0;
            for (int i = 0; i < 4; ++i)
                result |= std::min(255u, ((raw_color >> (i * 8)) & 255) * factors[i] >> 7) << (i * 8);
            return result;
        };
        const uint32_t primary = modulate(vm->color), secondary = modulate(vm->color_alt);
        if (render_type == 0x1c) {
            // sub_482810：两点在左边、尖端在右边中心，原调用绘制一个三角形。
            SpriteVertex triangle[3] = {
                {left, top, 0, primary, 0, 0},
                {right, (top + bottom) * .5f, 0, secondary, 0, 0},
                {left, bottom, 0, primary, 0, 0},
            };
            dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 1, triangle, sizeof(SpriteVertex));
        } else {
            // sub_482c10：圆心为主色，整圈周缘为副色。
            const int segments = static_cast<int>(vm->frame_counter);
            float radius = vm->rot_x * vm->scale_x * size_scale;
            if (vm->parent && !(vm->flags3 & 0x40000))
                radius *= static_cast<const AnmVm*>(vm->parent)->scale_x;
            std::vector<SpriteVertex> fan(segments + 2);
            fan[0] = {world._41, world._42, 0, primary, 0, 0};
            for (int i = 0; i <= segments; ++i) {
                const float angle = rotation_z + i * 6.283185482f / segments;
                fan[i + 1] = {world._41 + std::cos(angle) * radius,
                              world._42 + std::sin(angle) * radius, 0, secondary, 0, 0};
            }
            D3DMATRIX identity{};
            identity._11 = identity._22 = identity._33 = identity._44 = 1;
            set_matrix(dev, D3DTS_WORLD, identity);
            dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, segments, fan.data(), sizeof(SpriteVertex));
        }
        manager->draw_count++;
        return;
    }
    if (render_type == 9 || render_type == 0xd || render_type == 0xe)
    {
        // sub_47bef0 -> 480f70：两个半径组成贴图条带，scale_x为厚度、scale_y为半径。
        const int points = static_cast<int>(vm->frame_counter);
        const int repeat = *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(vm) + 0x4a8);
        float outer = (vm->scale_y + vm->scale_x * 0.5f) * size_scale;
        float inner = (vm->scale_y - vm->scale_x * 0.5f) * size_scale;
        if (vm->parent && !(vm->flags3 & 0x40000)) {
            const auto* parent = static_cast<const AnmVm*>(vm->parent);
            outer *= parent->scale_x;
            inner *= parent->scale_y;
        }
        uint32_t raw_color = render_type == 9 || (vm->flags2 & 0xe0000) == 0 ? vm->color : vm->color_alt;
        const uint8_t channels[4] = {manager->color_b, manager->color_g, manager->color_r, manager->color_a};
        color = 0;
        for (int i = 0; i < 4; ++i)
            color |= std::min(255u, ((raw_color >> (i * 8)) & 255) * channels[i] >> 7) << (i * 8);
        const float center_x = world._41;
        const float center_y = world._42;
        const float span = render_type == 9 ? 6.283185482f : vm->rot_a;
        const float start = vm->rot_z - (render_type == 0xd ? span * 0.5f : 0.0f);
        std::vector<SpriteVertex> strip(vm->draw_arg * 2);
        for (int i = 0; i < points; ++i) {
            const float ratio = static_cast<float>(i) / (points - 1);
            const float angle = start + span * ratio;
            const float c = std::cos(angle), s = std::sin(angle);
            const float v = ratio * repeat + vm->uv_offset_v;
            strip[i * 2] = {center_x + outer * c, center_y + outer * s, 0,
                            color, vm->uv[0] + vm->uv_offset_u, v};
            strip[i * 2 + 1] = {center_x + inner * c, center_y + inner * s, 0,
                                color, vm->uv[2] + vm->uv_offset_u, v};
        }
        if (render_type == 9) {
            // 闭环最后一对复制第一对位置，V继续累加到重复数。
            strip[(points - 1) * 2].x = strip[0].x;
            strip[(points - 1) * 2].y = strip[0].y;
            strip[(points - 1) * 2 + 1].x = strip[1].x;
            strip[(points - 1) * 2 + 1].y = strip[1].y;
        }
        D3DMATRIX identity{};
        identity._11 = identity._22 = identity._33 = identity._44 = 1;
        set_matrix(dev, D3DTS_WORLD, identity);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, points * 2 - 2, strip.data(), sizeof(SpriteVertex));
        manager->draw_count++;
        return;
    }
    if (render_type == 0x15 || render_type == 0x16)
    {
        // sub_482b40：先画高一像素、alpha 减半的边缘，再画原尺寸。
        SpriteVertex edge[4];
        std::memcpy(edge, vertices, sizeof(edge));
        const float top_offset = anchor_y == 1 ? 0.0f : (anchor_y == 2 ? -1.0f : -0.5f);
        const float bottom_offset = top_offset + 1.0f;
        for (int i = 0; i < 4; ++i)
        {
            edge[i].y += i < 2 ? top_offset : bottom_offset;
            edge[i].color = (edge[i].color & 0xffffffu) | ((edge[i].color >> 1) & 0x7f000000u);
        }
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, edge, sizeof(SpriteVertex));
        manager->draw_count++;
    }
    if (render_type == 8 && !background_3d) {
        // 480160保留三维顶点，54950用屏幕平面的透视相机；不能把旋转后的Z压在0..1。
        // 世界XY仍使用本层已映射到最终Scene的原点，camera1的离屏裁剪在此等价折入viewport。
        D3DVIEWPORT9 previous_viewport;
        D3DMATRIX previous_view, previous_projection;
        dev->GetViewport(&previous_viewport);
        dev->GetTransform(D3DTS_VIEW, &previous_view);
        dev->GetTransform(D3DTS_PROJECTION, &previous_projection);
        const int camera = gfx_current_camera();
        const D3DVIEWPORT9 viewport = camera == 2 ? D3DVIEWPORT9{0,0,640,480,0,1}
            : camera == 3 ? D3DVIEWPORT9{116,4,408,472,0,1}
                          : D3DVIEWPORT9{32,16,384,448,0,1};
        const float cotangent = 1.0f / std::tan(0.5235987901687622f * 0.5f);
        D3DMATRIX view{};
        view._11 = view._44 = 1;
        view._22 = view._33 = -1;
        view._41 = -static_cast<float>(viewport.X * 2 + viewport.Width);
        view._42 = static_cast<float>(viewport.Y * 2 + viewport.Height);
        view._43 = viewport.Height * cotangent;
        D3DMATRIX projection{};
        projection._11 = cotangent * viewport.Height / viewport.Width;
        projection._22 = cotangent;
        projection._33 = 20000.0f / 19998.0f;
        projection._34 = 1;
        projection._43 = -2.0f * projection._33;
        float shake_x, shake_y;
        gfx_get_camera_shake(camera, shake_x, shake_y);
        projection._31 = 2.0f * shake_x / viewport.Width;
        projection._32 = -2.0f * shake_y / viewport.Height;
        dev->SetViewport(&viewport);
        set_matrix(dev, D3DTS_VIEW, view);
        set_matrix(dev, D3DTS_PROJECTION, projection);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(SpriteVertex));
        dev->SetViewport(&previous_viewport);
        set_matrix(dev, D3DTS_VIEW, previous_view);
        set_matrix(dev, D3DTS_PROJECTION, previous_projection);
    } else dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(SpriteVertex));
    manager->draw_count++;
}

void anm_vm_corners(const AnmVm* vm, float corners[4][2])
{
    const uint32_t coordinate_mode = (vm->flags3 >> 22) & 7;
    const float position_scale = (coordinate_mode == 2 || coordinate_mode == 4) ? 0.5f : 1.0f;
    const float size_scale = coordinate_mode == 2 ? 0.5f : 1.0f;
    const float* vm_offset = reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(vm) + 0x4e4);
    float center_x = (vm->pos_x + vm->extra_x + vm_offset[0]) * position_scale;
    float center_y = (vm->pos_y + vm->extra_y + vm_offset[1]) * position_scale;
    const int anchor_x = (vm->flags2 >> 22) & 3;
    const int anchor_y = (vm->flags2 >> 24) & 3;
    const float left = (anchor_x == 1 ? 0.0f : anchor_x == 2 ? -vm->sprite_w : -vm->sprite_w * 0.5f) - vm->origin_x;
    const float right = left + vm->sprite_w;
    const float top = (anchor_y == 1 ? 0.0f : anchor_y == 2 ? -vm->sprite_h : -vm->sprite_h * 0.5f) - vm->origin_y;
    const float bottom = top + vm->sprite_h;
    const float bases[4][2] = {{left, top}, {right, top}, {left, bottom}, {right, bottom}};

    const float angle = total_rotation_z(vm);
    const float cz = std::cos(angle), sz = std::sin(angle);
    const float cx = std::cos(vm->rot_a), sx = std::sin(vm->rot_a);
    const float cy = std::cos(vm->rot_b), sy = std::sin(vm->rot_b);
    float xx = cy * cz * vm->scale_x;
    float xy = cy * sz * vm->scale_x;
    float yx = (sx * sy * cz - cx * sz) * vm->scale_y;
    float yy = (sx * sy * sz + cx * cz) * vm->scale_y;
    const uint32_t render_type = (vm->flags2 >> 26) & 0x1f;
    if (render_type == 1 || render_type == 3) {
        xx = cz * vm->scale_x;
        xy = sz * vm->scale_x;
        yx = -sz * vm->scale_y;
        yy = cz * vm->scale_y;
    }

    const AnmVm* position_root = vm;
    while (position_root->parent != nullptr && (position_root->flags3 & 0x40000u) == 0) {
        const AnmVm* parent = static_cast<const AnmVm*>(position_root->parent);
        const uint32_t mode = (parent->flags3 >> 22) & 7;
        const float factor = (mode == 2 || mode == 4) ? 0.5f : 1.0f;
        const float* parent_offset = reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(parent) + 0x4e4);
        center_x += (parent->pos_x + parent->extra_x + parent_offset[0]) * factor;
        center_y += (parent->pos_y + parent->extra_y + parent_offset[1]) * factor;
        position_root = parent;
    }
    if ((position_root->flags3 & 0x300000u) == 0x200000u) {
        center_x += 224.0f;
        center_y += 16.0f;
    }

    if (vm->parent != nullptr && (vm->flags3 & 0x40000u) == 0) {
        const AnmVm* parent = static_cast<const AnmVm*>(vm->parent);
        xx *= parent->scale_x;
        xy *= parent->scale_x;
        yx *= parent->scale_y;
        yy *= parent->scale_y;
    }

    for (int i = 0; i < 4; ++i) {
        corners[i][0] = center_x + (bases[i][0] * xx + bases[i][1] * yx) * size_scale;
        corners[i][1] = center_y + (bases[i][0] * xy + bases[i][1] * yy) * size_scale;
    }
}

// sub_4781e0：读 VM 脚本变量（id 映射见 anm.h 注释）。
uint32_t anm_vm_get_var(AnmVm* vm, uint32_t id)
{
    switch (id)
    {
    case 10000: return vm->frame_counter;
    case 0x2711: return *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(vm) + 0x4a8);
    case 0x2712: return *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(vm) + 0x4ac);
    case 0x2713: return *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(vm) + 0x4b0);
    case 0x2714: return static_cast<uint32_t>(*reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(vm) + 0x4b4));
    case 0x2715: return static_cast<int32_t>(*reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(vm) + 0x4b8));
    case 0x2716: return static_cast<int32_t>(*reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(vm) + 0x4bc));
    case 0x2717: return static_cast<int32_t>(*reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(vm) + 0x4c0));
    case 0x2718: return *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(vm) + 0x4d0);
    case 0x2719: return *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(vm) + 0x4d4);
    case 0x272b: return static_cast<uint32_t>(*reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(vm) + 0x4d8));
    case 0x272c: return static_cast<int32_t>(*reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(vm) + 0x4dc));
    case 0x272d: return *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(vm) + 0x4e0);
    case 0x2731: return static_cast<uint32_t>(*reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(vm) + 0x4c4));
    case 0x2732: return static_cast<int32_t>(*reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(vm) + 0x4c8));
    case 0x2733: return static_cast<int32_t>(*reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(vm) + 0x4cc));
    case 10022: {
        const uint32_t range = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(vm) + 0x4e0);
        return range == 0 ? 0 : anm_random_u32() % range; // 4615a0
    }
    default: return id;
    }
}

// sub_478480：写 VM 脚本变量地址；mask bit0 表示把 id 当作变量编号解析。
uint32_t* anm_vm_var_ptr(AnmVm* vm, uint32_t* id, uint8_t mask)
{
    if ((mask & 1) == 0)
        return id;
    switch (*id)
    {
    case 10000: return &vm->frame_counter;
    case 0x2711: return reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(vm) + 0x4a8);
    case 0x2712: return reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(vm) + 0x4ac);
    case 0x2713: return reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(vm) + 0x4b0);
    case 0x2718: return reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(vm) + 0x4d0);
    case 0x2719: return reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(vm) + 0x4d4);
    case 0x272d: return reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(vm) + 0x4e0);
    default: return id;
    }
}

// sub_477b00：设置 sprite（op 300）。按 sprite id 在资源脚本表里找表项，
// 把 UV/尺寸写进 VM。注意它不切换当前指令流（流由 VM 自己的脚本决定）。
// 原版参数 <0 时改用文字系统的字模 ANM（ascii.anm）取 sprite 0x120。
void anm_set_sprite(AnmVm* vm, AnmResource* res, int sprite)
{
    if (res == nullptr)
        return;
    for (uint32_t segment_index = 0; segment_index < res->segment_count; ++segment_index)
    {
        const AnmSegment& segment = res->segments[segment_index];
        for (uint32_t i = 0; i < segment.header->sprites; ++i)
        {
            const AnmSpriteDef& def = *segment.sprite(i);
            if (def.id != static_cast<uint32_t>(sprite))
                continue;
            vm->uv[0] = def.x / segment.header->width;
            vm->uv[1] = def.y / segment.header->height;
            vm->uv[2] = (def.x + def.w) / segment.header->width;
            vm->uv[3] = (def.y + def.h) / segment.header->height;
            vm->sprite_w = def.w;
            vm->sprite_h = def.h;
            vm->texture = res->segment_textures[segment_index];
            vm->sprite_index = sprite;
            vm->sprite_id = static_cast<uint16_t>(sprite);
            vm->flags2 |= 1u;
            return;
        }
    }
}

void anm_copy_game_region(AnmVm* target)
{
    AnmResource* resource = anm_manager()->resources[target->anm_table_index];
    for (uint32_t n = 0; n < resource->segment_count; ++n) {
        const AnmSegment& segment = resource->segments[n];
        for (uint32_t i = 0; i < segment.header->sprites; ++i) {
            const AnmSpriteDef& sprite = *segment.sprite(i);
            if (sprite.id != target->sprite_id) continue;
            const RECT destination{static_cast<LONG>(sprite.x), static_cast<LONG>(sprite.y),
                static_cast<LONG>(sprite.x + sprite.w - 1),
                static_cast<LONG>(sprite.y + sprite.h - 1)};
            auto* texture = static_cast<IDirect3DTexture9*>(resource->segment_textures[n]);
            IDirect3DSurface9* surface;
            texture->GetSurfaceLevel(0, &surface);
            gfx_copy_game_surface(surface, destination);
            // 487580：复制颜色后将目标区域alpha全部置为255。
            D3DLOCKED_RECT locked;
            surface->LockRect(&locked, &destination, 0);
            for (LONG y = 0; y < destination.bottom - destination.top; ++y) {
                auto* row = static_cast<uint8_t*>(locked.pBits) + y * locked.Pitch;
                for (LONG x = 0; x < destination.right - destination.left; ++x)
                    row[x * 4 + 3] = 255;
            }
            surface->UnlockRect();
            surface->Release();
            return;
        }
    }
}

// sub_488be0：设置要等待的 label（脚本流里的 type 5 指令）。
void anm_set_label(AnmVm* vm, int label)
{
    vm->label = label;
    for (AnmVm* child : g_allocated_vms)
        if (child != nullptr && child->parent == vm)
            anm_set_label(child, label);
}

void anm_interrupt(AnmVm* vm, int label)
{
    if (managed_vm(vm)) {
        // 488c60：根先立即执行，再依次对子VM设置同一label并立即执行。
        vm->label = label;
        update_vm_self(vm);
        for (AnmVm* child : g_allocated_vms)
            if (child != nullptr && child->parent == vm) anm_interrupt(child, label);
        return;
    }
    anm_set_label(vm, label);
    anm_vm_update(vm);
}

AnmVm* anm_find_child(AnmVm* vm, int script, int index)
{
    for (AnmVm* child : g_allocated_vms) {
        if (child == nullptr || child->parent != vm)
            continue;
        if (script == -1 || child->script->id == script) {
            if (index == 0)
                return child;
            --index;
        }
        if (AnmVm* found = anm_find_child(child, script, index))
            return found;
    }
    return nullptr;
}

// sub_41b430：VM 重置（清脚本状态、颜色、变换）。
void anm_vm_reset(AnmVm* vm)
{
    vm->flags2 = 3u;
    vm->script_cursor = -1;
    vm->script = nullptr;
    vm->sprite_index = 0;
    vm->sprite_id = 0;
    vm->color = 0xffffffff;
    vm->color_alt = 0xffffffff;
    vm->pos_x = 0.0f;
    vm->pos_y = 0.0f;
    vm->pos_z = 0.0f;
    vm->rot_z = 0.0f;
    vm->rot_x = 0.0f;
    vm->rot_y = 0.0f;
    vm->origin_x = vm->origin_y = 0.0f;
    vm->uv_offset_u = vm->uv_offset_v = 0.0f;
    vm->uv_scale_u = vm->uv_scale_v = 1.0f;
    vm->uv_velocity_u = vm->uv_velocity_v = 0.0f;
    vm->scale_x = 1.0f;
    vm->scale_y = 1.0f;
    vm->scale_z = 1.0f;
    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(vm) + 0x4d8) = 1.0f;
    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(vm) + 0x4dc) = 3.1415927410125732f;
    *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(vm) + 0x4e0) = 0x10000;
    std::memset(vm->matrix_world, 0, sizeof(vm->matrix_world));
    vm->matrix_world[0] = 1.0f;
    vm->matrix_world[5] = 1.0f;
    vm->matrix_world[10] = 1.0f;
    vm->matrix_world[15] = 1.0f;
}

// sub_4885d0：释放 VM（摘出绘制队列并清零）。
void anm_free_vm(AnmVm* vm)
{
    if (vm->handle != 0)
        anm_queue_remove(vm->handle);
    anm_vm_reset(vm);
}

// —— 动态 VM（原版 sub_489320 从管理器池取 / sub_4885d0 归还） ——
// 原版池是 manager+0xd0c 起、槽步长 0x624 的数组；端口用堆分配 + 登记表，
// 对外语义相同：取一个已重置的 VM、绑定资源下标，用完归还。
namespace {
int find_allocated(AnmVm* vm)
{
    for (int i = 0; i < kAllocatedVmSlots; ++i)
    {
        if (g_allocated_vms[i] == vm)
            return i;
    }
    return -1;
}
} // namespace

AnmVm* anm_vm_alloc(AnmResource* res)
{
    AnmVm* vm = static_cast<AnmVm*>(calloc(1, sizeof(AnmVm)));
    anm_vm_reset(vm);
    vm->anm_index = static_cast<int32_t>(res->index);
    vm->anm_table_index = static_cast<int32_t>(res->index);
    for (int i = 0; i < kAllocatedVmSlots; ++i)
    {
        if (g_allocated_vms[i] == nullptr)
        {
            g_allocated_vms[i] = vm;
            break;
        }
    }
    return vm;
}

void anm_manage_vm(AnmVm* vm, AnmVm** owner_slot)
{
    if (vm->owner_slot != nullptr) *vm->owner_slot = nullptr;
    vm->owner_slot = owner_slot;
    if (owner_slot != nullptr) *owner_slot = vm;
    g_independent_vms.remove(vm);
    if (owner_slot == nullptr && vm->parent == nullptr) g_independent_vms.push_back(vm);
}

void anm_vm_free(AnmVm* vm)
{
    g_independent_vms.remove(vm);
    if (vm == nullptr)
        return;
    if(vm->callback_index==7)extra_maple_free(vm);
    if (vm->owner_slot != nullptr) *vm->owner_slot = nullptr;
    for (AnmVm* child : g_allocated_vms)
        if (child != nullptr && child->parent == vm)
            anm_vm_free(child);
    anm_queue_remove(vm->handle);
    int slot = find_allocated(vm);
    if (slot >= 0)
        g_allocated_vms[slot] = nullptr;
    free(vm);
}

// sub_407420：设脚本号并允许绘制。
void anm_vm_bind_script(AnmVm* vm, AnmResource* res, int script_id)
{
    vm->anm_index = static_cast<int32_t>(res->index);
    vm->anm_table_index = static_cast<int32_t>(res->index);
    for (uint32_t i = 0; i < res->script_count; ++i)
        if (res->scripts[i].id == script_id)
            vm->script = &res->scripts[i];
    vm->script_cursor = 0;
    vm->time = static_cast<uint32_t>(-1);
    vm->flags2 |= 1u;
    // 原资源模板86920只执行-1初始化；不提前进入Bullet的0帧或label2。
    anm_execute_script(vm);
    vm->time = 0;
    vm->expr_stack_a2 = 0;
}

void anm_vm_set_script(AnmVm* vm, AnmResource* res, int script_id)
{
    vm->anm_index = static_cast<int32_t>(res->index);
    vm->anm_table_index = static_cast<int32_t>(res->index);
    for (uint32_t i = 0; i < res->script_count; ++i)
        if (res->scripts[i].id == script_id)
            vm->script = &res->scripts[i];
    vm->script_cursor = 0;
    vm->time = 0;
    vm->flags2 |= 1u;
    anm_vm_update(vm);
}

// —— 插值器（原版块结构，sub_405aa0 / sub_406750 / sub_47c960） ——
// 1 分量块（alpha，base = vm+0x140）：+0x00 起始值，+0x04 结束值（type 7/0x11 当增量），
//   +0x08/+0x0c hermite A/B，+0x10 当前值，+0x14 上次帧号，+0x18 帧计数，
//   +0x1c 累计时间，+0x20 复位，+0x24 flags(bit0)，+0x28 持续时间，+0x2c 类型。
// 3 分量块（旋转/位置）：+0x00/+0x04/+0x08 起始值，+0x0c..+0x14 结束值（type 7/0x11 当增量），
//   +0x18..+0x20 hermite A，+0x24..+0x2c hermite B，+0x30..+0x38 当前值，
//   +0x3c 上次帧号，+0x40 帧计数，+0x44 累计时间，+0x48 复位，+0x4c flags(bit0 初始完成)，
//   +0x50 持续时间，+0x54 类型（7 每帧累加 / 0x11 双向累加 / 8 hermite / 其它线性缓动）。
// 2 分量块（缩放）：同样的字段整体前移 0x14 字节（+0x28 last / +0x2c frame / +0x30 time /
//   +0x34 reset / +0x38 flags / +0x3c duration / +0x40 type）。
float easing_ratio(uint32_t type, float current, float duration)
{
    // 402fb0 / jump table 4035d8: 1..3 accelerate (powers 2..4),
    // 4..6 decelerate (powers 2..4). These IDs also drive STD/ECL movement.
    if (duration == 0.0f)
        return 0.0f;
    const float t = current / duration;
    const float u = 1.0f - t;
    switch (type)
    {
    case 1: return t * t;
    case 2: return t * t * t;
    case 3: return t * t * t * t;
    case 4: return 1.0f - u * u;
    case 5: return 1.0f - u * u * u;
    case 6: return 1.0f - u * u * u * u;
    case 9: return t < 0.5f ? 2.0f * t * t : 1.0f - 2.0f * u * u;
    default: return t;
    }
}

static void interpolation_clock(int32_t& previous, int32_t& frame, float& time)
{
    previous = frame;
    // sub_402a80 / sub_406750 的全局速度分支。
    if (stage::g_speed_scale > .99f && stage::g_speed_scale < 1.01f) {
        ++frame;
        time += 1;
    } else {
        time += stage::g_speed_scale;
        frame = static_cast<int32_t>(time);
    }
}

void interp_eval3(uint8_t* base, float out[3])
{
    float* f = reinterpret_cast<float*>(base);
    int32_t* i = reinterpret_cast<int32_t*>(base);
    int32_t duration = i[20];
    uint32_t type = static_cast<uint32_t>(i[21]);

    if (duration >= 1)
    {
        if (i[18] != 0)
            i[18] = 0;
        interpolation_clock(i[15], i[16], f[17]);
        if (i[16] >= duration)
        {
            if ((i[19] & 1) == 0)
            {
                i[18] = 0;
                i[19] |= 1;
            }
            i[16] = duration;
            i[15] = duration - 1;
            f[17] = static_cast<float>(duration);
            i[20] = 0;
            if (type == 7 || type == 0x11)
                std::memcpy(out, f, sizeof(float) * 3);
            else
                std::memcpy(out, f + 3, sizeof(float) * 3);
            return;
        }
    }
    else if (duration == 0)
    {
        if (type == 7 || type == 0x11)
            std::memcpy(out, f, sizeof(float) * 3);
        else
            std::memcpy(out, f + 3, sizeof(float) * 3);
        return;
    }

    if (type == 7)
    {
        f[0] += f[3];
        f[1] += f[4];
        f[2] += f[5];
        std::memcpy(out, f, sizeof(float) * 3);
    }
    else if (type == 0x11)
    {
        f[0] += f[9];
        f[1] += f[10];
        f[2] += f[11];
        f[9] += f[3];
        f[10] += f[4];
        f[11] += f[5];
        std::memcpy(out, f, sizeof(float) * 3);
    }
    else if (type == 8)
    {
        float t = f[17] / static_cast<float>(duration);
        float t1 = t - 1.0f;
        float h1 = t1 * t1 * (t + t + 1.0f);
        float h2 = t1 * t * t;
        float h3 = t * t * (3.0f - (t + t));
        float h4 = (1.0f - t) * (1.0f - t) * t;
        out[0] = f[3] * h3 + f[0] * h1 + f[6] * h4 + f[9] * h2;
        out[1] = f[4] * h3 + f[1] * h1 + f[7] * h4 + f[10] * h2;
        out[2] = f[5] * h3 + f[2] * h1 + f[8] * h4 + f[11] * h2;
    }
    else
    {
        float t = easing_ratio(type, f[17], static_cast<float>(duration));
        out[0] = (f[3] - f[0]) * t + f[0];
        out[1] = (f[4] - f[1]) * t + f[1];
        out[2] = (f[5] - f[2]) * t + f[2];
    }
    f[12] = out[0];
    f[13] = out[1];
    f[14] = out[2];
}

// sub_47cf40：1 分量（alpha）插值器评估。
float interp_eval1(uint8_t* base)
{
    float* f = reinterpret_cast<float*>(base);
    int32_t* i = reinterpret_cast<int32_t*>(base);
    int32_t duration = i[10];
    uint32_t type = static_cast<uint32_t>(i[11]);

    if (duration >= 1)
    {
        if (i[8] != 0)
            i[8] = 0;
        interpolation_clock(i[5], i[6], f[7]);
        if (i[6] >= duration)
        {
            if ((i[9] & 1) == 0)
            {
                i[8] = 0;
                i[9] |= 1;
            }
            i[6] = duration;
            i[5] = duration - 1;
            f[7] = static_cast<float>(duration);
            i[10] = 0;
            return (type == 7 || type == 0x11) ? f[0] : f[1];
        }
    }
    else if (duration == 0)
    {
        return (type == 7 || type == 0x11) ? f[0] : f[1];
    }

    if (type == 7)
    {
        f[0] += f[1];
        f[4] = f[0];
        return f[0];
    }
    if (type == 0x11)
    {
        f[0] += f[3];
        f[3] += f[1];
        f[4] = f[0];
        return f[0];
    }
    if (type == 8)
    {
        float t = f[7] / static_cast<float>(duration);
        float t1 = t - 1.0f;
        float h1 = t1 * t1 * (t + t + 1.0f);
        float h2 = t1 * t * t;
        float h3 = t * t * (3.0f - (t + t));
        float h4 = (1.0f - t) * (1.0f - t) * t;
        f[4] = f[1] * h3 + f[0] * h1 + f[2] * h4 + f[3] * h2;
        return f[4];
    }
    float t = easing_ratio(type, f[7], static_cast<float>(duration));
    f[4] = (f[1] - f[0]) * t + f[0];
    return f[4];
}

// sub_47d120：颜色块评估（3 分量块，起始/结束是 0..255 的浮点分量），
// 算完把 B/G/R 三个字节写回小端 ARGB。
void color_interp_apply(uint8_t* base, uint32_t* color)
{
    float rgb[3];
    interp_eval3(base, rgb);
    uint32_t channel[3];
    for (int c = 0; c < 3; ++c)
    {
        float v = rgb[c];
        if (v < 0.0f)
            v = 0.0f;
        if (v > 255.0f)
            v = 255.0f;
        channel[c] = static_cast<uint32_t>(v);
    }
    *color = (*color & 0xff000000u) | (channel[2] << 16) | (channel[1] << 8) | channel[0];
}

void interp_eval2(uint8_t* base, float out[2])
{
    float* f = reinterpret_cast<float*>(base);
    int32_t* i = reinterpret_cast<int32_t*>(base);
    int32_t duration = i[15];
    uint32_t type = static_cast<uint32_t>(i[16]);

    if (duration >= 1)
    {
        if (i[13] != 0)
            i[13] = 0;
        interpolation_clock(i[10], i[11], f[12]);
        if (i[11] >= duration)
        {
            if ((i[14] & 1) == 0)
            {
                i[13] = 0;
                i[14] |= 1;
            }
            i[11] = duration;
            i[10] = duration - 1;
            f[12] = static_cast<float>(duration);
            i[15] = 0;
            if (type == 7 || type == 0x11)
                std::memcpy(out, f, sizeof(float) * 2);
            else
                std::memcpy(out, f + 2, sizeof(float) * 2);
            return;
        }
    }
    else if (duration == 0)
    {
        if (type == 7 || type == 0x11)
            std::memcpy(out, f, sizeof(float) * 2);
        else
            std::memcpy(out, f + 2, sizeof(float) * 2);
        return;
    }

    if (type == 7)
    {
        f[0] += f[2];
        f[1] += f[3];
        std::memcpy(out, f, sizeof(float) * 2);
    }
    else if (type == 0x11)
    {
        f[0] += f[6];
        f[1] += f[7];
        f[6] += f[2];
        f[7] += f[3];
        std::memcpy(out, f, sizeof(float) * 2);
    }
    else if (type == 8)
    {
        float t = f[12] / static_cast<float>(duration);
        float t1 = t - 1.0f;
        float h1 = t1 * t1 * (t + t + 1.0f);
        float h2 = t1 * t * t;
        float h3 = t * t * (3.0f - (t + t));
        float h4 = (1.0f - t) * (1.0f - t) * t;
        out[0] = f[2] * h3 + f[0] * h1 + f[4] * h4 + f[6] * h2;
        out[1] = f[3] * h3 + f[1] * h1 + f[5] * h4 + f[7] * h2;
    }
    else
    {
        float t = easing_ratio(type, f[12], static_cast<float>(duration));
        out[0] = (f[2] - f[0]) * t + f[0];
        out[1] = (f[3] - f[1]) * t + f[1];
    }
    f[8] = out[0];
    f[9] = out[1];
}

// sub_478580：ANM 脚本解释器。
// 指令流：vm->script->stream（= 段基址 + script offset），游标 vm->script_cursor；
// 执行条件 instr->time <= (int)vm->time，执行后 cursor += instr->length。
void anm_execute_script(AnmVm* vm)
{
    if (vm->script == nullptr)
        return;

    // sub_478580 开头（LAB_00478806/LAB_0047885e）：label 非 0 时在当前脚本流里找
    // type 5 且 payload(float) == label 的指令，把游标设到该指令并清 label 与
    // flags2 的 0x4000 位；没有精确入口时使用脚本中的 label -1。
    if (vm->label != 0)
    {
        uint32_t offset = 0;
        int32_t destination = -1;
        int32_t fallback = -1;
        for (;;)
        {
            const AnmInstr* instr = reinterpret_cast<const AnmInstr*>(
                vm->script->stream + offset);
            if (instr->type == 0xffff)
                break;
            const int32_t instr_label = *reinterpret_cast<const int32_t*>(
                reinterpret_cast<const uint8_t*>(instr) + 8);
            if (instr->type == 5) {
                if (instr_label == vm->label) {
                    destination = static_cast<int32_t>(offset);
                    break;
                }
                if (instr_label == -1) fallback = static_cast<int32_t>(offset);
            }
            offset += instr->length;
        }
        if (destination < 0) destination = fallback;
        vm->label = 0;
        vm->flags2 &= ~0x4000u;
        if (destination >= 0) {
            const auto* entry = reinterpret_cast<const AnmInstr*>(vm->script->stream + destination);
            vm->saved_cursor = vm->script_cursor;
            vm->saved_time = static_cast<int32_t>(vm->time);
            vm->script_cursor = destination + entry->length;
            vm->time = entry->time;
            vm->flags2 |= 1u;
        }
    }

    while (vm->script_cursor >= 0)
    {
        if (vm->script == nullptr)
            return;
        const AnmInstr* instr = reinterpret_cast<const AnmInstr*>(
            vm->script->stream + vm->script_cursor);
        if (instr->time > static_cast<int16_t>(vm->time))
            break;

        const uint16_t length = instr->length;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(instr);
        const uint8_t* payload = bytes + sizeof(AnmInstr);
        // AnmInstr = {u16 type; u16 length; i16 time; u16 param_mask}，参数从 +8 起；
        // 参数表达式由 param_mask（+6）决定。
        const uint8_t mask = bytes[6];

        switch (instr->type)
        {
        case 1:  // delete
            vm->flags2 &= ~1u;
            vm->script_cursor = -1;
            return;
        case 2:  // stop：停止脚本，保留当前图像
        case 0xffff:
            vm->script_cursor = -1;
            return;

        case 3:  // 等待中断；游标和脚本时间停在这条指令，插值仍继续
        case 4:
            if (instr->type == 4)
                vm->flags2 &= ~1u;
            vm->flags2 |= 0x4000u;
            return;

        case 0:
        case 5:  // 中断入口；顺序执行只跳过标记
            vm->script_cursor += length;
            break;

        case 6:  // 0x4787c7 NEG：从脚本时钟减去等待帧数（插值器继续走）
        {
            uint32_t delta = *reinterpret_cast<const uint32_t*>(payload);
            if (mask & 1)
                delta = anm_vm_get_var(vm, delta);
            vm->time -= delta;
            vm->script_cursor += length;
            break;
        }

        case 7:  // 从中断返回到原先等待位置和时间
            vm->script_cursor = vm->saved_cursor;
            vm->time = vm->saved_time;
            break;

        case 100: // 0x64 赋值：payload[0..3] 变量 id，payload[4..7] 值
        {
            uint32_t value = *reinterpret_cast<const uint32_t*>(payload + 4);
            uint32_t id = *reinterpret_cast<const uint32_t*>(payload);
            if ((mask & 2) != 0)
                value = anm_vm_get_var(vm, value);
            uint32_t* target = anm_vm_var_ptr(vm, &id, mask);
            *target = value;
            vm->script_cursor += length;
            break;
        }

        case 101: case 103: case 105: case 107: case 109:
        {
            float* target = const_cast<float*>(reinterpret_cast<const float*>(payload));
            if (mask & 1)
                target = float_variable(vm, target);
            const float operand = float_argument(vm, payload, mask, 1);
            if (instr->type == 101) *target = operand;
            if (instr->type == 103) *target += operand;
            if (instr->type == 105) *target -= operand;
            if (instr->type == 107) *target *= operand;
            if (instr->type == 109) *target /= operand;
            vm->script_cursor += length;
            break;
        }
        case 102: case 104: case 106: case 108:
        {
            uint32_t id = *reinterpret_cast<const uint32_t*>(payload);
            uint32_t operand = *reinterpret_cast<const uint32_t*>(payload + 4);
            if (mask & 2)
                operand = anm_vm_get_var(vm, operand);
            int32_t* target = reinterpret_cast<int32_t*>(anm_vm_var_ptr(vm, &id, mask));
            if (instr->type == 102) *target += static_cast<int32_t>(operand);
            if (instr->type == 104) *target -= static_cast<int32_t>(operand);
            if (instr->type == 106) *target *= static_cast<int32_t>(operand);
            if (instr->type == 108) *target /= static_cast<int32_t>(operand);
            vm->script_cursor += length;
            break;
        }
        case 112: // 三参数整数加法，loading 用浮点变量转整数后设置亮度
        {
            uint32_t id = *reinterpret_cast<const uint32_t*>(payload);
            uint32_t left = *reinterpret_cast<const uint32_t*>(payload + 4);
            uint32_t right = *reinterpret_cast<const uint32_t*>(payload + 8);
            if (mask & 2) left = anm_vm_get_var(vm, left);
            if (mask & 4) right = anm_vm_get_var(vm, right);
            *anm_vm_var_ptr(vm, &id, mask) = left + right;
            vm->script_cursor += length;
            break;
        }
        case 113: // 三参数浮点加法（abcard12 记下并移动卡槽位置）
        case 115: // 三参数浮点减法
        case 117: // 三参数浮点乘法
        case 119: // 三参数浮点除法
        {
            float* target = const_cast<float*>(reinterpret_cast<const float*>(payload));
            if (mask & 1)
                target = float_variable(vm, target);
            const float left = float_argument(vm, payload, mask, 1);
            const float right = float_argument(vm, payload, mask, 2);
            if (instr->type == 113) *target = left + right;
            if (instr->type == 115) *target = left - right;
            if (instr->type == 117) *target = left * right;
            if (instr->type == 119) *target = left / right;
            vm->script_cursor += length;
            break;
        }

        case 118: // 三参数有符号整数除法（effect20的分段数计算）。
        {
            uint32_t id = *reinterpret_cast<const uint32_t*>(payload);
            int32_t left = *reinterpret_cast<const int32_t*>(payload + 4);
            int32_t right = *reinterpret_cast<const int32_t*>(payload + 8);
            if (mask & 2) left = static_cast<int32_t>(anm_vm_get_var(vm, left));
            if (mask & 4) right = static_cast<int32_t>(anm_vm_get_var(vm, right));
            *anm_vm_var_ptr(vm, &id, mask) = left / right;
            vm->script_cursor += length;
            break;
        }

        case 120: // 整数余数；effect47/142使用10022的有界随机整数。
        {
            uint32_t id = *reinterpret_cast<const uint32_t*>(payload);
            int32_t left = *reinterpret_cast<const int32_t*>(payload + 4);
            int32_t right = *reinterpret_cast<const int32_t*>(payload + 8);
            if (mask & 2) left = static_cast<int32_t>(anm_vm_get_var(vm, left));
            if (mask & 4) right = static_cast<int32_t>(anm_vm_get_var(vm, right));
            *anm_vm_var_ptr(vm, &id, mask) = left % right;
            vm->script_cursor += length;
            break;
        }
        case 124:
        case 125:
        {
            const float angle = float_argument(vm, payload, mask, 1);
            float* target = const_cast<float*>(reinterpret_cast<const float*>(payload));
            if (mask & 1) target = float_variable(vm, target);
            *target = instr->type == 124 ? std::sin(angle) : std::cos(angle);
            vm->script_cursor += length;
            break;
        }
        case 130: // 47d470：极坐标写两个独立变量。
        case 131: // 半径区间和随机角；原调用先取随机半径，再取随机角。
        {
            const float first = float_argument(vm, payload, mask, 2);
            const float second = float_argument(vm, payload, mask, 3);
            float angle = first, radius = second;
            if (instr->type == 131) {
                radius = (anm_random01() * 2.0f - 1.0f) * (second - first) + first;
                angle = (anm_random01() * 2.0f - 1.0f) * 3.1415927410125732f;
            }
            float* x = const_cast<float*>(reinterpret_cast<const float*>(payload));
            float* y = const_cast<float*>(reinterpret_cast<const float*>(payload + 4));
            if (mask & 1) x = float_variable(vm, x);
            if (mask & 2) y = float_variable(vm, y);
            *x = std::cos(angle) * radius;
            *y = std::sin(angle) * radius;
            vm->script_cursor += length;
            break;
        }

        case 201: // sub_478480：计数变量先减1，大于0时跳回offset/time。
        {
            uint32_t id = *reinterpret_cast<const uint32_t*>(payload);
            --*anm_vm_var_ptr(vm, &id, mask);
            const int32_t count = mask & 1 ? anm_vm_get_var(vm, id) : id;
            if (count > 0) {
                vm->script_cursor = *reinterpret_cast<const int32_t*>(payload + 4);
                vm->time = *reinterpret_cast<const int32_t*>(payload + 8);
            } else vm->script_cursor += length;
            break;
        }

        case 200: // 跳转：两个 int32，目标字节偏移与目标脚本时间
            vm->script_cursor = *reinterpret_cast<const int32_t*>(payload);
            vm->time = *reinterpret_cast<const int32_t*>(payload + 4);
            break;

        case 204: // 整数不等时跳转；effect142随机选择纵向镜像。
        {
            uint32_t left = *reinterpret_cast<const uint32_t*>(payload);
            uint32_t right = *reinterpret_cast<const uint32_t*>(payload + 4);
            if (mask & 1) left = anm_vm_get_var(vm, left);
            if (mask & 2) right = anm_vm_get_var(vm, right);
            if (left != right) {
                vm->script_cursor = *reinterpret_cast<const int32_t*>(payload + 8);
                vm->time = *reinterpret_cast<const int32_t*>(payload + 12);
            } else vm->script_cursor += length;
            break;
        }

        case 500: // sub_4888f0：创建依附于当前 VM 的独立脚本实例
        case 501: // 同上，param4=4，加入第二绘制队列
        case 502: // param4=2，第一绘制队列前端
        case 503: // param4=6，第二绘制队列前端
        case 505: // 第一队列子VM，执行初帧后另写+4e4/+4e8。
        {
            int32_t script_id = *reinterpret_cast<const int32_t*>(payload);
            if (mask & 1)
                script_id = static_cast<int32_t>(anm_vm_get_var(vm, script_id));
            AnmResource* res = anm_manager()->resources[vm->anm_index];
            AnmVm* child = anm_vm_alloc(res);
            child->draw_layer = vm->draw_layer;
            child->parent = vm;
            child->flags3 = 0x1000 | (vm->flags3 & 0x8000000u);
            child->node_flags = (instr->type == 501 || instr->type == 503 ? 4u : 0u) |
                                (instr->type == 502 || instr->type == 503 ? 2u : 0u);
            anm_vm_set_script(child, res, script_id);
            if (instr->type == 505) {
                float* position = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(child) + 0x4e4);
                position[0] = float_argument(vm, payload, mask, 1);
                position[1] = float_argument(vm, payload, mask, 2);
            }
            anm_queue_add(child);
            vm->script_cursor += length;
            break;
        }

        case 509: // 原class66：复制直接父VM的脚本变量4a4..4e0。
            if (vm->parent != nullptr)
                std::memcpy(reinterpret_cast<uint8_t*>(vm) + 0x4a4,
                            static_cast<const uint8_t*>(vm->parent) + 0x4a4, 0x40);
            vm->script_cursor += length;
            break;

        case 504: // sub_488a40；pl00普通灵符script7每帧产生script12拖尾。
        case 506: // 同一独立发射，再用参数覆盖XY。
        {
            uint32_t script_id = *reinterpret_cast<const uint32_t*>(payload);
            if (mask & 1) script_id = anm_vm_get_var(vm, script_id);
            AnmResource* res = anm_manager()->resources[vm->anm_index];
            AnmVm* emitted = anm_vm_alloc(res);
            emitted->draw_layer = vm->draw_layer;
            emitted->flags3 |= 0x1000 | (vm->flags3 & 0x8000000u);
            emitted->extra_x = vm->extra_x;
            emitted->extra_y = vm->extra_y;
            emitted->extra_z = vm->extra_z;
            emitted->rot_a = vm->rot_a;
            emitted->rot_b = vm->rot_b;
            emitted->rot_z = vm->rot_z;
            float* position = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(emitted) + 0x4e4);
            std::memcpy(position, &vm->pos_x, 12);
            anm_vm_set_script(emitted, res, script_id);
            if (instr->type == 506) {
                position[0] = float_argument(vm, payload, mask, 1);
                position[1] = float_argument(vm, payload, mask, 2);
            }
            anm_queue_add(emitted);
            anm_manage_vm(emitted, nullptr);
            vm->script_cursor += length;
            break;
        }

        case 300: // 设置 sprite（原版 sub_477b00，参数 mask bit0 表示走表达式）
        {
            uint32_t sprite = *reinterpret_cast<const uint32_t*>(payload);
            if ((mask & 1) != 0)
                sprite = anm_vm_get_var(vm, sprite);
            vm->script_sprite_id = static_cast<uint16_t>(sprite);
            if (vm->sprite_callback == 1)
                sprite = stage::bullet_sprite_for_vm(vm, static_cast<int>(sprite));
            else if (vm->sprite_callback == 2 || vm->sprite_callback == 3)
                sprite = laser::sprite_for_vm(vm, static_cast<int>(sprite));
            vm->flags2 |= 1u;
            if (static_cast<int32_t>(sprite) < 0)
            {
                // 原版：参数为负时改用文字系统的字模 ANM，取 sprite 0x120。
                TextManager* text = text_manager();
                AnmResource* text_anm = text != nullptr
                    ? *reinterpret_cast<AnmResource**>(reinterpret_cast<uint8_t*>(text) + 0x19260)
                    : nullptr;
                anm_set_sprite(vm, text_anm, 0x120);
            }
            else
            {
                anm_set_sprite(vm, anm_manager()->resources[vm->anm_index],
                               static_cast<int>(sprite));
            }
            vm->script_cursor += length;
            break;
        }

        // 400 = 设置位置：3 个 float（原版 case 0x11）。flags2 bit0x400 时写到 +0x4e4
        // 的附加位移而不是 +0x30 的位置。
        case 400:
        {
            const float values[3] = {float_argument(vm, payload, mask, 0),
                                     float_argument(vm, payload, mask, 1),
                                     float_argument(vm, payload, mask, 2)};
            if ((vm->flags2 & 0x400) != 0)
            {
                float* extra = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(vm) + 0x4e4);
                extra[0] = values[0];
                extra[1] = values[1];
                extra[2] = values[2];
            }
            else
            {
                vm->pos_x = values[0];
                vm->pos_y = values[1];
                vm->pos_z = values[2];
            }
            vm->script_cursor += length;
            break;
        }

        // 401 = 设置旋转：3 个 float 写 +0x3c/+0x40/+0x44（原版 case 0x12），flags2 |= 4。
        case 401:
        {
            const float values[3] = {float_argument(vm, payload, mask, 0),
                                     float_argument(vm, payload, mask, 1),
                                     float_argument(vm, payload, mask, 2)};
            vm->rot_a = values[0];
            vm->rot_b = values[1];
            vm->rot_z = values[2];
            vm->flags2 |= 4u;
            vm->script_cursor += length;
            break;
        }

        // 402 = 设置缩放：2 个 float 写 +0x54/+0x58（原版 case 0x13），flags2 |= 8。
        case 402:
        {
            const float values[2] = {float_argument(vm, payload, mask, 0),
                                     float_argument(vm, payload, mask, 1)};
            vm->scale_x = values[0];
            vm->scale_y = values[1];
            vm->flags2 |= 8u;
            vm->script_cursor += length;
            break;
        }

        case 407: // sub_405a00..405aa0：位置插值，duration/type/x/y/z
        case 433: // 同一个位置插值器，目标由angle/radius换算。
        {
            uint8_t* base = reinterpret_cast<uint8_t*>(vm) + 0x90;
            float* f = reinterpret_cast<float*>(base);
            int32_t* i = reinterpret_cast<int32_t*>(base);
            std::memset(base, 0, 0x58);
            f[0] = vm->pos_x;
            f[1] = vm->pos_y;
            f[2] = vm->pos_z;
            if (instr->type == 433) {
                const float radius = float_argument(vm, payload, mask, 3);
                const float angle = float_argument(vm, payload, mask, 2);
                f[3] = std::cos(angle) * radius;
                f[4] = std::sin(angle) * radius;
                f[5] = 0;
            } else {
                f[3] = float_argument(vm, payload, mask, 2);
                f[4] = float_argument(vm, payload, mask, 3);
                f[5] = float_argument(vm, payload, mask, 4);
            }
            i[15] = -1;
            i[19] = 1;
            i[20] = *reinterpret_cast<const int32_t*>(payload);
            if (mask & 1) i[20] = anm_vm_get_var(vm, i[20]);
            i[21] = *reinterpret_cast<const int32_t*>(payload + 4);
            vm->script_cursor += length;
            break;
        }

        case 429: // UV缩放，原+0x6c/+0x70。
            vm->uv_scale_u = float_argument(vm, payload, mask, 0);
            vm->uv_scale_v = float_argument(vm, payload, mask, 1);
            vm->flags2 |= 0x10;
            vm->script_cursor += length;
            break;

        case 430: // sub_47d500，UV缩放的二分量插值块+0x280。
        {
            uint8_t* base = reinterpret_cast<uint8_t*>(vm) + 0x280;
            auto* f = reinterpret_cast<float*>(base);
            auto* i = reinterpret_cast<int32_t*>(base);
            std::memset(base, 0, 0x44);
            f[0] = vm->uv_scale_u;
            f[1] = vm->uv_scale_v;
            f[2] = float_argument(vm, payload, mask, 2);
            f[3] = float_argument(vm, payload, mask, 3);
            i[10] = -1;
            i[14] = 1;
            const int duration = *reinterpret_cast<const int32_t*>(payload);
            i[15] = mask & 1 ? anm_vm_get_var(vm, duration) : duration;
            i[16] = payload[4];
            vm->flags2 |= 0x10;
            vm->script_cursor += length;
            break;
        }

        case 415: // sub_47d8d0：每帧角速度
        {
            float* angular = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(vm) + 0x48);
            angular[0] = float_argument(vm, payload, mask, 0);
            angular[1] = float_argument(vm, payload, mask, 1);
            angular[2] = float_argument(vm, payload, mask, 2);
            vm->flags3 |= 0x4000000u;
            vm->script_cursor += length;
            break;
        }

        case 302: // flags2 的渲染类型位 0x7c000000 = value << 26
        {
            uint32_t value = *reinterpret_cast<const uint32_t*>(payload);
            vm->flags2 = (vm->flags2 & ~0x7c000000u) | ((value << 26) & 0x7c000000u);
            vm->script_cursor += length;
            break;
        }

        case 303: // flags2 bit5..8 = value（原版 case 2）
        {
            uint32_t value = *reinterpret_cast<const uint32_t*>(payload);
            vm->flags2 = (vm->flags2 & ~0x1e0u) | ((value << 5) & 0x1e0u);
            vm->script_cursor += length;
            break;
        }

        case 304: { // 4066b0: layer, coordinate origin, then default coordinate scale.
            const int32_t layer = *reinterpret_cast<const int32_t*>(payload);
            vm->draw_layer = layer;
            vm->flags3 &= ~0x300000u;
            if (layer >= 3 && layer <= 19) vm->flags3 |= 0x100000u;
            else if (layer >= 20 && layer <= 23) vm->flags3 |= 0x200000u;
            if ((layer >= 20 && layer <= 32) || (layer >= 37 && layer <= 45))
                vm->flags3 = (vm->flags3 & ~0x1c00000u) | 0x400000u;
            vm->script_cursor += length;
            break;
        }

        case 305: // 原版深度写入开关（不控制雾）。
            vm->flags2 = (vm->flags2 & ~0x2000u) | ((payload[0] & 1u) << 13);
            vm->script_cursor += length;
            break;

        case 306: // 478580 case5: compensate direct STD camera moves.
            vm->flags2 = (vm->flags2 & ~0x8000u) | ((payload[0] & 1u) << 15);
            vm->script_cursor += length;
            break;

        case 436: // 原class53：局部原点偏移。
            vm->origin_x = float_argument(vm, payload, mask, 0);
            vm->origin_y = float_argument(vm, payload, mask, 1);
            vm->script_cursor += length;
            break;

        case 307: // flags3 bit0x1000
            vm->flags3 = (vm->flags3 & ~0x1000u) |
                         ((static_cast<uint32_t>(payload[0]) << 12) & 0x1000u);
            vm->script_cursor += length;
            break;

        case 308: // 原case7：水平镜像；st01enm脚本1/3/6。
            vm->scale_x = -vm->scale_x;
            vm->flags2 = (vm->flags2 ^ 0x800u) | 8u;
            vm->script_cursor += length;
            break;

        case 309: // 原case8：垂直镜像。
            vm->scale_y = -vm->scale_y;
            vm->flags2 = (vm->flags2 ^ 0x1000u) | 8u;
            vm->script_cursor += length;
            break;

        case 310: // HUD命/Bomb格：独立绘制开关，原版 case 9
            vm->flags2 = (vm->flags2 & ~1u) | (payload[0] & 1u);
            vm->script_cursor += length;
            break;

        case 311: // flags3 bit0x2000
            vm->flags3 = (vm->flags3 & ~0x2000u) |
                         ((*reinterpret_cast<const uint32_t*>(payload) << 13) & 0x2000u);
            vm->script_cursor += length;
            break;

        case 312: // sub_47d910：U/V地址，0 wrap、1 clamp、2 mirror
        {
            uint32_t u = *reinterpret_cast<const uint32_t*>(payload);
            uint32_t v = *reinterpret_cast<const uint32_t*>(payload + 4);
            if (mask & 1) u = anm_vm_get_var(vm, u);
            if (mask & 2) v = anm_vm_get_var(vm, v);
            vm->flags3 = (vm->flags3 & ~15u) | ((u & 3u) << 2) | (v & 3u);
            vm->script_cursor += length;
            break;
        }

        case 313: // flags3 混合位 0x1c00000 = value << 22
        {
            uint32_t value = *reinterpret_cast<const uint32_t*>(payload);
            if ((mask & 1) != 0)
                value = anm_vm_get_var(vm, value);
            vm->flags3 = (vm->flags3 & ~0x1c00000u) | ((value << 22) & 0x1c00000u);
            vm->script_cursor += length;
            break;
        }

        case 403: // vm+0x527（颜色 A 字节）字节写入（原版 case 0x14）
            *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(vm) + 0x527) = payload[0];
            vm->script_cursor += length;
            break;

        case 404: // 颜色三字节（原版 case 0x15：+0x526/+0x525/+0x524）
        {
            uint8_t* raw_color = reinterpret_cast<uint8_t*>(vm) + 0x524;
            for (int i = 0; i < 3; ++i)
            {
                uint32_t value = *reinterpret_cast<const uint32_t*>(payload + i * 4);
                if (mask & (1u << i)) value = anm_vm_get_var(vm, value);
                raw_color[2 - i] = static_cast<uint8_t>(value);
            }
            vm->script_cursor += length;
            break;
        }

        case 405: // vm+0x52b（第二套颜色的 A 字节，原版 case 0x16）
            *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(vm) + 0x52b) = payload[0];
            vm->script_cursor += length;
            break;

        case 406: // 第二套颜色三字节（原版 case 0x17：+0x52a/+0x529/+0x528）
        {
            uint8_t* raw_color = reinterpret_cast<uint8_t*>(vm) + 0x528;
            raw_color[2] = payload[0];
            raw_color[1] = payload[4];
            raw_color[0] = payload[8];
            vm->script_cursor += length;
            break;
        }

        case 421: // flags2 0xc00000/0x3000000 两个字节（原版 case 0x26）
        {
            uint16_t v0 = *reinterpret_cast<const uint16_t*>(payload);
            uint16_t v1 = *reinterpret_cast<const uint16_t*>(payload + 2);
            vm->flags2 = (vm->flags2 & ~0xc00000u) | ((static_cast<uint32_t>(v0) << 22) & 0xc00000u);
            vm->flags2 = (vm->flags2 & ~0x3000000u) | ((static_cast<uint32_t>(v1) << 24) & 0x3000000u);
            vm->script_cursor += length;
            break;
        }

        case 423: // flags2 bit0xe0000
            vm->flags2 = (vm->flags2 & ~0xe0000u) |
                         ((static_cast<uint32_t>(payload[0]) << 17) & 0xe0000u);
            vm->script_cursor += length;
            break;

        case 425:
        case 426:
            (instr->type == 425 ? vm->uv_velocity_u : vm->uv_velocity_v) =
                float_argument(vm, payload, mask, 0);
            vm->flags3 |= 0x4000000u;
            vm->script_cursor += length;
            break;

        case 431: // flags3 bit0x400
            vm->flags3 = (vm->flags3 & ~0x400u) |
                         ((static_cast<uint32_t>(payload[0]) << 10) & 0x400u);
            vm->script_cursor += length;
            break;

        case 439: // 原版近距alpha开关及距离参数，不经过表达式取值
            vm->flags3 = (vm->flags3 & ~0x40000000u) |
                         ((*reinterpret_cast<const uint32_t*>(payload) << 30) & 0x40000000u);
            vm->near_alpha_start = *reinterpret_cast<const float*>(payload + 4);
            vm->near_alpha_end = *reinterpret_cast<const float*>(payload + 8);
            vm->script_cursor += length;
            break;

        case 437: // flags3 bit0x70
            vm->flags3 = (vm->flags3 & ~0x70u) |
                         ((*reinterpret_cast<const uint32_t*>(payload) << 4) & 0x70u);
            vm->script_cursor += length;
            break;

        case 424: // 原case0x29：运动学调用方按切线设置Z角。
            vm->flags3 = (vm->flags3 & ~0x200u) | ((static_cast<uint32_t>(payload[0]) & 1u) << 9);
            vm->script_cursor += length;
            break;

        case 507: // flags3 bit0x40000
            vm->flags3 = (vm->flags3 & ~0x40000u) |
                         ((static_cast<uint32_t>(payload[0]) << 18) & 0x40000u);
            vm->script_cursor += length;
            break;

        case 606: // 渲染类型 0x14（原版 case 0x49）
            vm->flags2 = (vm->flags2 & 0xd3ffffffu) | 0x50000000u;
            vm->sprite_w = *reinterpret_cast<const float*>(payload);
            vm->sprite_h = *reinterpret_cast<const float*>(payload + 4);
            vm->script_cursor += length;
            break;

        case 604: // sub_481210 -> 482c10：径向渐变实心圆，半径/分段数。
        case 605: // sub_481210 -> 482e70：无贴图线圆，半径/分段数。
        {
            vm->flags2 = instr->type == 604 ? (vm->flags2 & 0xc7ffffffu) | 0x44000000u
                                           : (vm->flags2 & 0xcbffffffu) | 0x48000000u;
            vm->rot_x = float_argument(vm, payload, mask, 0);
            uint32_t segments = *reinterpret_cast<const uint32_t*>(payload + 4);
            if (mask & 2) segments = anm_vm_get_var(vm, segments);
            vm->frame_counter = segments;
            vm->script_cursor += length;
            break;
        }

        case 614: // sub_482810：梯度三角形的宽/高。
            vm->flags2 = (vm->flags2 & 0xf3ffffffu) | 0x70000000u;
            vm->rot_x = vm->sprite_w = float_argument(vm, payload, mask, 0);
            vm->rot_y = vm->sprite_h = float_argument(vm, payload, mask, 1);
            vm->script_cursor += length;
            break;

        case 608: // 渲染类型 0x16（原版 case 0x4b）
            vm->flags2 = (vm->flags2 & 0xdbffffffu) | 0x58000000u;
            vm->sprite_w = *reinterpret_cast<const float*>(payload);
            vm->sprite_h = *reinterpret_cast<const float*>(payload + 4);
            vm->script_cursor += length;
            break;

        // 410 = 旋转插值器（原版 case 0x1b）：payload[0..3] 持续时间（mask bit0 走变量）、
        // payload[4..7] 类型、payload[8..19] 三个结束值（mask bit4/8/0x10 表示该值走表达式）。
        case 410:
        {
            uint8_t* base = reinterpret_cast<uint8_t*>(vm) + 0x170;
            float* f = reinterpret_cast<float*>(base);
            int32_t* i = reinterpret_cast<int32_t*>(base);
            uint32_t duration = *reinterpret_cast<const uint32_t*>(payload);
            if ((mask & 1) != 0)
                duration = anm_vm_get_var(vm, duration);
            const uint32_t type = *reinterpret_cast<const uint32_t*>(payload + 4);
            f[0] = vm->rot_a;
            f[1] = vm->rot_b;
            f[2] = vm->rot_z;
            f[3] = *reinterpret_cast<const float*>(payload + 8);
            f[4] = *reinterpret_cast<const float*>(payload + 12);
            f[5] = *reinterpret_cast<const float*>(payload + 16);
            f[6] = f[7] = f[8] = 0.0f;
            f[9] = f[10] = f[11] = 0.0f;
            f[12] = f[13] = f[14] = 0.0f;
            i[15] = -1;
            i[16] = 0;
            f[17] = 0.0f;
            i[18] = 0;
            i[19] |= 1;
            i[20] = static_cast<int32_t>(duration);
            i[21] = static_cast<int32_t>(type);
            vm->flags2 |= 4u;
            vm->script_cursor += length;
            break;
        }

        // 412 = 缩放插值器（原版 case 0x1d → sub_4614b0）：payload[0..3] 持续时间、
        // payload[4] 类型、payload[8..15] 两个结束值（mask bit2/bit3 表示走表达式）。
        case 412:
        {
            uint8_t* base = reinterpret_cast<uint8_t*>(vm) + 0x1f8;
            float* f = reinterpret_cast<float*>(base);
            int32_t* i = reinterpret_cast<int32_t*>(base);
            uint32_t duration = *reinterpret_cast<const uint32_t*>(payload);
            if ((mask & 1) != 0)
                duration = anm_vm_get_var(vm, duration);
            const uint32_t type = payload[4];
            f[0] = vm->scale_x;
            f[1] = vm->scale_y;
            f[2] = float_argument(vm, payload, mask, 2);
            f[3] = float_argument(vm, payload, mask, 3);
            f[4] = f[5] = f[6] = f[7] = 0.0f;
            f[8] = f[9] = 0.0f;
            i[10] = -1;
            i[11] = 0;
            f[12] = 0.0f;
            i[13] = 0;
            i[14] |= 1;
            i[15] = static_cast<int32_t>(duration);
            i[16] = static_cast<int32_t>(type);
            vm->flags2 |= 8u;
            vm->script_cursor += length;
            break;
        }

        // 409 = 设置 alpha 插值器（原版 case 0x1a → sub_406630）：
        // payload[0..3] 持续时间、payload[4] 类型、payload[8] 结束 alpha 字节；
        // 起始 alpha 取 VM 当前颜色的 A 字节。
        case 409:
        case 414:
        {
            const bool alt = instr->type == 414;
            uint8_t* base = reinterpret_cast<uint8_t*>(vm) + (alt ? 0x31c : 0x140);
            float* f = reinterpret_cast<float*>(base);
            int32_t* i = reinterpret_cast<int32_t*>(base);
            uint32_t duration = *reinterpret_cast<const uint32_t*>(payload);
            if ((mask & 1) != 0)
                duration = anm_vm_get_var(vm, duration);
            uint32_t type = payload[4];
            uint32_t end = payload[8];
            if ((mask & 4) != 0)
                end = anm_vm_get_var(vm, end);
            const float start = static_cast<float>(((alt ? vm->color_alt : vm->color) >> 24) & 0xff);
            i[0] = 0;
            i[1] = 0;
            i[2] = 0;
            i[3] = 0;
            i[4] = 0;
            i[5] = -1;
            i[6] = 0;
            i[7] = 0;
            i[8] = 0;
            i[9] |= 1;
            i[10] = static_cast<int32_t>(duration);
            i[11] = static_cast<int32_t>(type);
            f[0] = start;
            f[1] = static_cast<float>(end);
            if (alt && (vm->flags2 & 0xe0000u) == 0)
                vm->flags2 = (vm->flags2 & ~0xc0000u) | 0x20000u;
            vm->script_cursor += length;
            break;
        }

        // 408 = 第一套颜色插值器（原版 case 0x19 → sub_47d790，块在 vm+0xe8）。
        // payload：+0 持续时间、+4 类型、+8 结束 R、+0xc 结束 G、+0x10 结束 B；
        // mask bit0/bit2/bit3/bit4 分别表示该参数走变量。
        case 408:
        case 413:
        {
            const bool alt = (instr->type == 413);
            uint8_t* base = reinterpret_cast<uint8_t*>(vm) + (alt ? 0x2c4 : 0xe8);
            float* f = reinterpret_cast<float*>(base);
            int32_t* i = reinterpret_cast<int32_t*>(base);
            uint32_t duration = *reinterpret_cast<const uint32_t*>(payload);
            if ((mask & 1) != 0)
                duration = anm_vm_get_var(vm, duration);
            const uint32_t type = payload[4];
            uint32_t r = *reinterpret_cast<const uint32_t*>(payload + 8);
            if ((mask & 4) != 0)
                r = anm_vm_get_var(vm, r);
            uint32_t g = *reinterpret_cast<const uint32_t*>(payload + 12);
            if ((mask & 8) != 0)
                g = anm_vm_get_var(vm, g);
            uint32_t b = *reinterpret_cast<const uint32_t*>(payload + 16);
            if ((mask & 0x10) != 0)
                b = anm_vm_get_var(vm, b);
            const uint32_t cur = alt ? vm->color_alt : vm->color;
            f[0] = static_cast<float>(cur & 0xff);
            f[1] = static_cast<float>((cur >> 8) & 0xff);
            f[2] = static_cast<float>((cur >> 16) & 0xff);
            f[3] = static_cast<float>(b);
            f[4] = static_cast<float>(g);
            f[5] = static_cast<float>(r);
            f[6] = f[7] = f[8] = 0.0f;
            f[9] = f[10] = f[11] = 0.0f;
            i[15] = -1;
            i[16] = 0;
            f[17] = 0.0f;
            i[18] = 0;
            i[19] |= 1;
            i[20] = static_cast<int32_t>(duration);
            i[21] = static_cast<int32_t>(type);
            vm->script_cursor += length;
            break;
        }

        case 600:
        case 601:
        case 602:
        {
            const uint32_t shape = instr->type == 600 ? 9 : instr->type == 601 ? 0xd : 0xe;
            vm->flags2 = (vm->flags2 & 0x83ffffffu) | (shape << 26);
            const uint32_t count = *reinterpret_cast<const uint32_t*>(payload);
            vm->draw_arg = mask & 1 ? anm_vm_get_var(vm, count) : count;
            vm->script_cursor += length;
            break;
        }

        case 603: // 原case0x46 / sub_47b380：无贴图矩形，尺寸+0x74/+0x78。
            vm->flags2 = (vm->flags2 & 0xc3ffffffu) | 0x40000000u;
            vm->rot_x = float_argument(vm, payload, mask, 0);
            vm->rot_y = float_argument(vm, payload, mask, 1);
            vm->sprite_w = vm->rot_x;
            vm->sprite_h = vm->rot_y;
            vm->script_cursor += length;
            break;

        default:
            vm->script_cursor += length;
            break;
        }
    }
}

void anm_release_resource(int index)
{
    anm_delete_independent(index);
    AnmResource* resource = anm_manager()->resources[index];
    for (uint32_t i = 0; i < resource->segment_count; ++i)
        if (resource->segment_textures[i])
            static_cast<IDirect3DTexture9*>(resource->segment_textures[i])->Release();
    std::free(resource->segment_textures);
    std::free(resource->scripts);
    std::free(resource->segments);
    std::free(resource->vms);
    std::free(resource->file_data);
    std::free(resource);
    anm_manager()->resources[index] = nullptr;
}

void anm_delete_independent(int resource_index)
{
    for (auto it = g_independent_vms.begin(); it != g_independent_vms.end();) {
        AnmVm* vm = *it++;
        if (vm->anm_index == resource_index || vm->anm_table_index == resource_index) anm_vm_free(vm);
    }
}

// sub_478580 的入口 + 每帧矩阵重建。
// 原版矩阵来自脚本指令（VM +0x3d4），本层按 pos/scale/rot_z 重建，语义等价于
// “平移到 pos、绕 Z 旋转 rot_z、缩放到 sprite 尺寸”。
void anm_vm_update(AnmVm* vm)
{
    // owner推进的嵌入对象仍持整树；原托管VM的子节点各由自己的A22/A0b队列推进。
    if (!managed_vm(vm)) {
        for (AnmVm* child : g_allocated_vms)
            if (child != nullptr && child->parent == vm) {
                anm_vm_update(child);
                if (child->script_cursor < 0 && (child->flags2 & 1u) == 0)
                    anm_vm_free(child);
            }
    }
    update_vm_self(vm);
}

static void update_vm_self(AnmVm* vm)
{
    if(vm->callback_index==7 && extra_maple_update(vm)) {
        vm->script_cursor=-1;vm->flags2&=~1u;return;
    }
    anm_execute_script(vm);

    if (vm->flags2 & 0x8000u)
        stage_background_apply_camera_delta(vm);

    // sub_47c750：每帧从插值块把当前值算回 VM 字段（位置/旋转/缩放）。
    uint8_t* raw = reinterpret_cast<uint8_t*>(vm);
    if (*reinterpret_cast<int32_t*>(raw + 0x0e0) != 0)
    {
        float pos[3];
        interp_eval3(raw + 0x90, pos);
        vm->pos_x = pos[0];
        vm->pos_y = pos[1];
        vm->pos_z = pos[2];
    }
    if (*reinterpret_cast<int32_t*>(raw + 0x1c0) != 0)
    {
        float rot[3];
        interp_eval3(raw + 0x170, rot);
        vm->rot_a = rot[0];
        vm->rot_b = rot[1];
        vm->rot_z = rot[2];
        vm->flags2 |= 4u;
    }
    if (*reinterpret_cast<int32_t*>(raw + 0x234) != 0)
    {
        float scale[2];
        interp_eval2(raw + 0x1f8, scale);
        vm->scale_x = scale[0];
        vm->scale_y = scale[1];
        vm->flags2 |= 8u;
    }
    if (*reinterpret_cast<int32_t*>(raw + 0x2bc) != 0)
    {
        float scale[2];
        interp_eval2(raw + 0x280, scale);
        vm->uv_scale_u = scale[0];
        vm->uv_scale_v = scale[1];
        vm->flags2 |= 0x10u;
    }
    // alpha 插值块（vm+0x140，sub_47cf40）：算完写回颜色的 A 字节。
    if (*reinterpret_cast<int32_t*>(raw + 0x168) != 0)
    {
        int32_t alpha = static_cast<int32_t>(interp_eval1(raw + 0x140));
        if (alpha < 0)
            alpha = 0;
        if (alpha > 0xff)
            alpha = 0xff;
        vm->color = (vm->color & 0x00ffffffu) | (static_cast<uint32_t>(alpha) << 24);
    }
    if (*reinterpret_cast<int32_t*>(raw + 0x344) != 0)
    {
        int32_t alpha = static_cast<int32_t>(interp_eval1(raw + 0x31c));
        if (alpha < 0) alpha = 0;
        if (alpha > 255) alpha = 255;
        vm->color_alt = (vm->color_alt & 0x00ffffffu) | (static_cast<uint32_t>(alpha) << 24);
    }
    // 第一/第二套颜色插值块（vm+0xe8 / vm+0x2c4，sub_47d120）：算完写回 RGB 字节。
    if (*reinterpret_cast<int32_t*>(raw + 0x138) != 0)
        color_interp_apply(raw + 0xe8, &vm->color);
    if (*reinterpret_cast<int32_t*>(raw + 0x314) != 0)
        color_interp_apply(raw + 0x2c4, &vm->color_alt);
    if (vm->flags3 & 0x4000000u)
    {
        const float* angular = reinterpret_cast<const float*>(raw + 0x48);
        vm->rot_a += angular[0];
        vm->rot_b += angular[1];
        vm->rot_z += angular[2];
    }
    vm->uv_offset_u += vm->uv_velocity_u;
    vm->uv_offset_v += vm->uv_velocity_v;
    vm->uv_offset_u -= std::floor(vm->uv_offset_u);
    vm->uv_offset_v -= std::floor(vm->uv_offset_v);

    const float cos_z = std::cos(vm->rot_z);
    const float sin_z = std::sin(vm->rot_z);
    const float sx = vm->scale_x;
    const float sy = vm->scale_y;
    float* m = vm->matrix_world;
    std::memset(m, 0, sizeof(vm->matrix_world));
    m[0] = cos_z * sx;
    m[1] = sin_z * sx;
    m[4] = -sin_z * sy;
    m[5] = cos_z * sy;
    m[10] = vm->scale_z;
    m[12] = vm->pos_x;
    m[13] = vm->pos_y;
    m[14] = vm->pos_z;
    m[15] = 1.0f;
    if ((vm->flags2 & 0x4000u) == 0)
        vm->time++;
}





