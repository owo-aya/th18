#include "stage/background.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/game_session.h"
#include "core/thread.h"
#include "platform/gfx.h"
#include "presentation/anm.h"
#include "resources/dat.h"
#include "resources/std_file.h"
#include "stage/stage.h"

namespace {

struct Vec3 { float x, y, z; };
struct BackgroundQuad {
    const StdQuad* definition;
    AnmVm* vm;
};
struct BackgroundObject {
    int layer;
    std::vector<BackgroundQuad> quads;
};
struct BackgroundOverlay {
    AnmVm* vm = nullptr;
    int layer = 0;
};
struct Background {
    bool visible = true;
    int resource_index;
    bool retiring = false;
    bool incoming = false;
    float transition_time = 0;
    uint8_t* data = nullptr;
    const uint8_t* script = nullptr;
    const StdFace* instances = nullptr;
    AnmResource* anm = nullptr;
    IDirect3DSurface9* depth = nullptr;
    std::vector<BackgroundObject> objects;
    BackgroundOverlay overlays[8];
    int cursor = 0;
    int time = 0;
    Vec3 eye{0.0f, 0.0f, -600.0f};
    Vec3 look{0.0f, 300.0f, 600.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
    Vec3 eye_offset{};
    Vec3 look_offset{};
    Vec3 direction{};
    Vec3 eye_delta{};
    float fov = 0.785398185f;
    float eye_interp[22]{};
    float look_interp[22]{};
    float up_interp[22]{};
    float fov_interp[12]{};
    // 0041b7a0 / 0041efa0: start, end, B, G, R, A.
    float fog[6]{};
    float fog_source[6]{};
    float fog_target[6]{};
    int fog_duration = 0;
    int fog_time = 0;
    int fog_mode = 0;
    int motion = 0;
    int motion_time = 0;
    D3DMATRIX view{};
    D3DMATRIX projection{};
    Vec3 right{};
    Vec3 view_up{};
};

Background* g_background = nullptr;
Background* g_previous;
float g_camera_values[6]{};
Vec3 g_camera_delta{};
Thread* g_retained_render;
Thread* g_retained_render_upper;
Thread* g_fade_logic;
Thread* g_fade_render;
float g_fade_time;
int g_fade_alpha;
constexpr float kTau = 6.2831854820251465f;

Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 multiply(Vec3 a, float b) { return {a.x * b, a.y * b, a.z * b}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec3 normalized(Vec3 v) { return multiply(v, 1.0f / std::sqrt(dot(v, v))); }

void start_vector(float* block, const Vec3& source, const float* target, int duration, int mode) {
    std::memset(block, 0, 0x58);
    std::memcpy(block, &source, 12);
    std::memcpy(block + 3, target, 12);
    int32_t* i = reinterpret_cast<int32_t*>(block);
    i[15] = -1;
    i[19] = 1;
    i[20] = duration;
    i[21] = mode;
}

void evaluate_vector(float* block, Vec3& output) {
    if (reinterpret_cast<int32_t*>(block)[20] != 0)
        interp_eval3(reinterpret_cast<uint8_t*>(block), &output.x);
}

void start_fog(Background& bg, const uint32_t color, float start, float end) {
    bg.fog[0] = start;
    bg.fog[1] = end;
    for (int i = 0; i < 4; ++i)
        bg.fog[i + 2] = static_cast<float>((color >> (i * 8)) & 255);
}

void run_script(Background& bg) {
    bool advance = true;
    for (;;) {
        const auto* instruction = reinterpret_cast<const StdInstr*>(bg.script + bg.cursor);
        if (static_cast<int32_t>(instruction->time) > bg.time)
            break;
        const int32_t* i = reinterpret_cast<const int32_t*>(instruction + 1);
        const float* f = reinterpret_cast<const float*>(i);
        switch (instruction->type) {
        case 0:
            advance = false;
            goto finished;
        case 1:
            bg.cursor = i[0];
            bg.time = i[1];
            continue;
        case 2:
            // 41d260: only direct camera moves publish a displacement; the
            // interpolated movement remains relative to the existing rain.
            bg.eye_delta = {f[0] - bg.eye.x, f[1] - bg.eye.y, f[2] - bg.eye.z};
            std::memcpy(&bg.eye, f, 12);
            break;
        case 3: start_vector(bg.eye_interp, bg.eye, f + 2, i[0], i[1]); break;
        case 4: std::memcpy(&bg.look, f, 12); break;
        case 5: start_vector(bg.look_interp, bg.look, f + 2, i[0], i[1]); break;
        case 6: std::memcpy(&bg.up, f, 12); break;
        case 7: bg.fov = f[0]; break;
        case 8: start_fog(bg, static_cast<uint32_t>(i[0]), f[1], f[2]); break;
        case 9:
            std::memcpy(bg.fog_source, bg.fog, sizeof(bg.fog));
            bg.fog_target[0] = f[3];
            bg.fog_target[1] = f[4];
            for (int channel = 0; channel < 4; ++channel)
                bg.fog_target[channel + 2] = static_cast<float>((static_cast<uint32_t>(i[2]) >> (channel * 8)) & 255);
            bg.fog_duration = i[0];
            bg.fog_mode = i[1];
            bg.fog_time = 0;
            break;
        case 12:
            bg.motion = i[0];
            bg.motion_time = i[0] == 2 ? 512 : 0;
            if (i[0] == 0) bg.eye_offset = {};
            break;
        // 41d930 writes the app's scene clear color, not the STD fog color.
        case 13: gfx_set_clear_color(static_cast<uint32_t>(i[0])); break;
        case 14: {
            BackgroundOverlay& overlay = bg.overlays[i[0]];
            if (i[1] >= 0) {
                if (overlay.vm != nullptr) anm_vm_free(overlay.vm);
                overlay.vm = anm_vm_alloc(bg.anm);
                anm_vm_set_script(overlay.vm, bg.anm, i[1]);
            } else if (overlay.vm != nullptr) {
                if (i[1] == -1) overlay.vm->script_cursor = -1;
                overlay.vm->flags2 &= ~1u;
            }
            overlay.layer = i[2];
            break;
        }
        case 16: break;
        case 18: start_vector(bg.up_interp, bg.up, f + 2, i[0], i[1]); break;
        case 19:
            for (BackgroundObject& object : bg.objects)
                for (BackgroundQuad& quad : object.quads)
                    anm_interrupt(quad.vm, i[0] + 7);
            for (BackgroundOverlay& overlay : bg.overlays)
                if (overlay.vm != nullptr) anm_interrupt(overlay.vm, i[0] + 7);
            break;
        case 21: {
            float* block = bg.fov_interp;
            std::memset(block, 0, sizeof(bg.fov_interp));
            block[0] = bg.fov;
            block[1] = f[2];
            int32_t* fields = reinterpret_cast<int32_t*>(block);
            fields[5] = -1;
            fields[9] = 1;
            fields[10] = i[0];
            fields[11] = i[1];
            break;
        }
        }
        bg.cursor += instruction->size;
    }
finished:
    if (advance) ++bg.time;
    evaluate_vector(bg.eye_interp, bg.eye);
    evaluate_vector(bg.look_interp, bg.look);
    evaluate_vector(bg.up_interp, bg.up);
    if (reinterpret_cast<int32_t*>(bg.fov_interp)[10] != 0)
        bg.fov = interp_eval1(reinterpret_cast<uint8_t*>(bg.fov_interp));
    if (bg.fog_duration != 0) {
        ++bg.fog_time;
        const float ratio = easing_ratio(bg.fog_mode, static_cast<float>(bg.fog_time), static_cast<float>(bg.fog_duration));
        for (int channel = 0; channel < 6; ++channel)
            bg.fog[channel] = bg.fog_source[channel] + (bg.fog_target[channel] - bg.fog_source[channel]) * ratio;
        if (bg.fog_time >= bg.fog_duration) {
            std::memcpy(bg.fog, bg.fog_target, sizeof(bg.fog));
            bg.fog_duration = 0;
        }
    }
}

void update_motion(Background& bg) {
    const float t = static_cast<float>(bg.motion_time);
    const float angle = static_cast<float>(bg.motion_time & 1023) * kTau / 1024.0f;
    switch (bg.motion) {
    case 1: {
        const float ramp = std::min(t / 512.0f, 1.0f);
        const float s = std::sin(angle);
        bg.eye_offset.x = -10.0f * s * ramp;
        bg.eye_offset.z = -10.0f * std::sin(angle * 2.0f) * ramp;
        bg.look.x = -s * 0.01f * ramp;
        bg.look_offset.x = -bg.eye_offset.x * 0.5f;
        bg.look_offset.z = -bg.eye_offset.z * 0.5f;
        break;
    }
    case 3:
        bg.eye_offset = {std::sin(angle) * std::min(t * 80.0f / 1024.0f, 80.0f), 0.0f, 0.0f};
        bg.look_offset.x = -bg.eye_offset.x / 10.0f;
        break;
    case 5:
        bg.eye_offset.z = std::sin(angle) * -50.0f * std::min(t / 512.0f, 1.0f);
        break;
    }
    if (bg.motion != 0 && ++bg.motion_time >= 2048)
        bg.motion_time = 1024;
}

D3DMATRIX identity() {
    D3DMATRIX result{};
    result._11 = result._22 = result._33 = result._44 = 1.0f;
    return result;
}

void setup_camera(Background& bg) {
    const Vec3 eye = add(bg.eye, bg.eye_offset);
    const Vec3 forward = bg.direction;
    bg.right = normalized(cross(bg.up, forward));
    bg.view_up = cross(forward, bg.right);
    D3DMATRIX& view = bg.view;
    view = identity();
    view._11 = bg.right.x; view._21 = bg.right.y; view._31 = bg.right.z;
    view._12 = bg.view_up.x; view._22 = bg.view_up.y; view._32 = bg.view_up.z;
    view._13 = forward.x; view._23 = forward.y; view._33 = forward.z;
    view._41 = -dot(bg.right, eye); view._42 = -dot(bg.view_up, eye); view._43 = -dot(forward, eye);
    D3DMATRIX& projection = bg.projection;
    projection = {};
    // 00454b20 / 0041f950 render the STD through a 408x472 viewport, then
    // retain its centred 384x448 gameplay rectangle. Fold that crop into
    // the projection while drawing directly to the gameplay viewport.
    projection._22 = (472.0f / 448.0f) / std::tan(bg.fov * 0.5f);
    projection._11 = projection._22 / (384.0f / 448.0f);
    projection._33 = 8000.0f / (8000.0f - 30.0f);
    projection._34 = 1.0f;
    projection._43 = -30.0f * projection._33;
    float shake_x, shake_y;
    gfx_get_camera_shake(3, shake_x, shake_y);
    projection._31 = 2.0f * shake_x / 384.0f;
    projection._32 = -2.0f * shake_y / 448.0f;
}

struct Vertex { float x, y, z; uint32_t color; float u, v; };

uint32_t vm_color(const AnmVm* vm) {
    uint32_t color = vm->color;
    const uint32_t mode = (vm->flags2 >> 17) & 7;
    if (mode == 1) color = vm->color_alt;
    if (mode == 4) {
        color = 0;
        for (int shift = 0; shift < 32; shift += 8)
            color |= (((vm->color >> shift) & 255) * ((vm->color_alt >> shift) & 255) / 255) << shift;
    }
    const AnmManager* manager = anm_manager();
    const uint8_t multiplier[4] = {manager->color_b, manager->color_g, manager->color_r, manager->color_a};
    uint32_t output = 0;
    for (int channel = 0; channel < 4; ++channel)
        output |= std::min(255u, ((color >> (8 * channel)) & 255) * multiplier[channel] >> 7) << (8 * channel);
    return output;
}

uint32_t billboard_color(const Background& bg, const AnmVm* vm, uint32_t color, Vec3 position) {
    const Vec3 eye = add(bg.eye, bg.eye_offset);
    const Vec3 difference{position.x - eye.x, position.y - eye.y, position.z - eye.z};
    const float distance = std::sqrt(dot(difference, difference));
    if (distance > bg.fog[0]) {
        const float amount = (bg.fog[0] - distance) / (bg.fog[0] - bg.fog[1]);
        if (amount >= 1.0f) return 0;
        uint32_t result = 0;
        for (int channel = 0; channel < 3; ++channel) {
            const int value = (color >> (channel * 8)) & 255;
            result |= (value - static_cast<int>((value - bg.fog[channel + 2]) * amount)) << (channel * 8);
        }
        const float alpha_factor = 1.0f - amount * amount * amount;
        color = result | (static_cast<uint32_t>(static_cast<float>(color >> 24) * alpha_factor) << 24);
    }
    if (vm->flags3 & 0x40000000u) {
        if (distance <= vm->near_alpha_end) return 0;
        if (distance < vm->near_alpha_start) {
            const float factor = 1.0f - (vm->near_alpha_start - distance) / (vm->near_alpha_start - vm->near_alpha_end);
            color = (color & 0xffffff) | (static_cast<uint32_t>(static_cast<float>(color >> 24) * factor) << 24);
        }
    }
    return color;
}

void draw_quad(Background& bg, BackgroundQuad& quad, const StdFace& instance) {
    AnmVm* vm = quad.vm;
    if ((vm->flags2 & 3) != 3 || (vm->flags3 & 0x180) != 0) return;
    const uint32_t type = (vm->flags2 >> 26) & 31;
    const bool billboard = type == 4 || type == 6;
    const StdQuad& definition = *quad.definition;
    const Vec3 position{definition.x + instance.x + vm->pos_x,
                        definition.y + instance.y + vm->pos_y,
                        definition.z + instance.z + vm->pos_z};
    const float width = definition.width != 0.0f ? definition.width : vm->sprite_w * vm->scale_x;
    const float height = definition.height != 0.0f ? definition.height : vm->sprite_h * vm->scale_y;
    const int anchor_x = (vm->flags2 >> 22) & 3;
    const int anchor_y = (vm->flags2 >> 24) & 3;
    const float left = anchor_x == 1 ? 0.0f : anchor_x == 2 ? -width : -width * 0.5f;
    const float top = anchor_y == 1 ? 0.0f : anchor_y == 2 ? -height : -height * 0.5f;
    uint32_t color = vm_color(vm);
    if (type == 6) color = billboard_color(bg, vm, color, position);
    const float u0 = vm->uv[0] + vm->uv_offset_u, u1 = vm->uv[2] + vm->uv_offset_u;
    const float v0 = vm->uv[1] + vm->uv_offset_v, v1 = vm->uv[3] + vm->uv_offset_v;
    Vertex vertices[4] = {{left, top, 0.0f, color, u0, v0}, {left + width, top, 0.0f, color, u1, v0},
        {left, top + height, 0.0f, color, u0, v1}, {left + width, top + height, 0.0f, color, u1, v1}};
    const float cz = std::cos(vm->rot_z), sz = std::sin(vm->rot_z);
    const float cx = std::cos(vm->rot_a), sx = std::sin(vm->rot_a);
    const float cy = std::cos(vm->rot_b), sy = std::sin(vm->rot_b);
    for (Vertex& vertex : vertices) {
        const float x = vertex.x, y = vertex.y;
        Vec3 transformed;
        if (billboard) {
            const float rotated_x = (x * cz - y * sz) * 0.5f;
            const float rotated_y = (x * sz + y * cz) * 0.5f;
            transformed = add(multiply(bg.right, rotated_x), multiply(bg.view_up, -rotated_y));
        } else {
            transformed = {x * cy * cz + y * (sx * sy * cz - cx * sz),
                           x * cy * sz + y * (sx * sy * sz + cx * cz),
                           -x * sy + y * sx * cy};
        }
        transformed = add(transformed, position);
        vertex.x = transformed.x; vertex.y = transformed.y; vertex.z = transformed.z;
    }
    IDirect3DDevice9* device = gfx_device();
    // 41cee0: type 8/24 selects fog; opcode 305's bit disables depth writes.
    device->SetRenderState(D3DRS_FOGENABLE, type == 8 || type == 24);
    device->SetRenderState(D3DRS_ZWRITEENABLE, (vm->flags2 & 0x2000) == 0);
    anm_apply_blend(vm);
    device->SetTexture(0, static_cast<IDirect3DTexture9*>(const_cast<void*>(vm->texture)));
    // 47d910: script 0=wrap, 1=clamp, 2=mirror (D3D 1/3/2).
    const D3DTEXTUREADDRESS address[] = {D3DTADDRESS_WRAP, D3DTADDRESS_CLAMP, D3DTADDRESS_MIRROR};
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, address[(vm->flags3 >> 2) & 3]);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, address[vm->flags3 & 3]);
    const DWORD filter = (vm->flags3 & 0x2000) ? D3DTEXF_POINT : D3DTEXF_LINEAR;
    device->SetSamplerState(0, D3DSAMP_MINFILTER, filter);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, filter);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex));
    ++anm_manager()->draw_count;
}

void draw_overlay(BackgroundOverlay& overlay) {
    IDirect3DDevice9* device = gfx_device();
    const D3DMATRIX view = identity();
    D3DMATRIX projection{};
    projection._11 = 2.0f / 768.0f; projection._22 = -2.0f / 896.0f;
    projection._33 = projection._44 = 1.0f; projection._41 = -1.0f; projection._42 = 1.0f;
    float shake_x, shake_y;
    gfx_get_camera_shake(3, shake_x, shake_y);
    projection._41 += 2.0f * shake_x / 384.0f;
    projection._42 -= 2.0f * shake_y / 448.0f;
    device->SetTransform(D3DTS_VIEW, &view);
    device->SetTransform(D3DTS_PROJECTION, &projection);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    // 0041d260 / 0041e350 pass the script position unchanged. Camera viewport
    // and screen-shake offsets are shared by all render types.
    anm_draw_vm(overlay.vm);
}

void create_depth(Background& bg) {
    gfx_device()->CreateDepthStencilSurface(640, 480, D3DFMT_D16, D3DMULTISAMPLE_NONE,
                                           0, TRUE, &bg.depth, nullptr);
}

} // namespace

void stage_background_create(const char* std_name) {
    g_background = new Background;
    Background& bg = *g_background;
    bg.direction = normalized(add(bg.look, bg.look_offset));
    size_t size;
    bg.data = static_cast<uint8_t*>(dat_read(std_name, &size));
    const auto& header = *reinterpret_cast<const StdHeader*>(bg.data);
    bg.script = bg.data + header.script_offset;
    bg.instances = reinterpret_cast<const StdFace*>(bg.data + header.faces_offset);
    // 41b850 uses (stage & 1) + 3: two slots keep the outgoing STD alive.
    bg.resource_index = 100 + (session().stage & 1);
    bg.anm = anm_get(bg.resource_index, header.anm_name);
    create_depth(bg);
    const auto* offsets = reinterpret_cast<const uint32_t*>(bg.data + sizeof(StdHeader));
    bg.objects.resize(header.nb_objects);
    for (int object_index = 0; object_index < header.nb_objects; ++object_index) {
        const auto* object = reinterpret_cast<const StdObjectHeader*>(bg.data + offsets[object_index]);
        BackgroundObject& output = bg.objects[object_index];
        output.layer = object->unknown & 255;
        const auto* definition = reinterpret_cast<const StdQuad*>(object + 1);
        while (definition->unknown != 0xffff) {
            AnmVm* vm = anm_vm_alloc(bg.anm);
            anm_vm_set_script(vm, bg.anm, definition->script_index);
            output.quads.push_back({definition, vm});
            definition = reinterpret_cast<const StdQuad*>(reinterpret_cast<const uint8_t*>(definition) + definition->size);
        }
    }
}

static void destroy_background(Background*& background) {
    for (BackgroundObject& object : background->objects)
        for (BackgroundQuad& quad : object.quads) anm_vm_free(quad.vm);
    for (BackgroundOverlay& overlay : background->overlays)
        if (overlay.vm != nullptr) anm_vm_free(overlay.vm);
    anm_release_resource(background->resource_index);
    // 显示设备正在重置时，深度面已经释放。
    if (background->depth != nullptr) background->depth->Release();
    std::free(background->data);
    delete background;
    background = nullptr;
}

static void update_background(Background& bg) {
    bg.eye_delta = {};
    // 41c0a0 clears camera +0xfc/+0x100 before A14 applies this frame's shake.
    gfx_set_camera_shake(3, 0, 0);
    // 41c0a0 caches camera +254 before the STD script changes its look vector.
    bg.direction = normalized(add(bg.look, bg.look_offset));
    for (BackgroundObject& object : bg.objects)
        for (BackgroundQuad& quad : object.quads) anm_vm_update(quad.vm);
    run_script(bg);
    // 41c0a0 publishes the camera before overlay updates and camera motion.
    const Vec3 eye = add(bg.eye, bg.eye_offset);
    const float values[] = {eye.x, eye.y, eye.z,
                            bg.direction.x, bg.direction.y, bg.direction.z};
    std::memcpy(g_camera_values, values, sizeof(values));
    g_camera_delta = bg.eye_delta;
    for (BackgroundOverlay& overlay : bg.overlays)
        if (overlay.vm != nullptr) anm_vm_update(overlay.vm);
    update_motion(bg);
}

float stage_background_anm_value(int variable) {
    return g_camera_values[variable - 10016];
}

void stage_background_apply_camera_delta(AnmVm* vm) {
    // 47b733: ANM 306 adds the STD's direct-move delta to the VM base position.
    vm->extra_x += g_camera_delta.x;
    vm->extra_y += g_camera_delta.y;
    vm->extra_z += g_camera_delta.z;
}

void stage_background_interrupt(int label) {
    Background& bg = *g_background;
    int cursor = 0;
    const auto* instruction = reinterpret_cast<const StdInstr*>(bg.script);
    while (instruction->size != 0xffff) {
        if (instruction->type == 16 && *reinterpret_cast<const int32_t*>(instruction + 1) == label) {
            bg.cursor = cursor;
            bg.time = instruction->time;
            return;
        }
        cursor += instruction->size;
        instruction = reinterpret_cast<const StdInstr*>(bg.script + cursor);
    }
}

static void render_background(Background& bg, int first_layer, int last_layer) {
    if (!bg.visible) return;
    IDirect3DDevice9* device = gfx_device();
    D3DVIEWPORT9 previous_viewport;
    D3DMATRIX previous_view, previous_projection, previous_world;
    IDirect3DSurface9* previous_depth = nullptr;
    DWORD previous_z, previous_zwrite, previous_zfunc, previous_fog;
    device->GetViewport(&previous_viewport);
    device->GetTransform(D3DTS_VIEW, &previous_view);
    device->GetTransform(D3DTS_PROJECTION, &previous_projection);
    device->GetTransform(D3DTS_WORLD, &previous_world);
    device->GetDepthStencilSurface(&previous_depth);
    device->GetRenderState(D3DRS_ZENABLE, &previous_z);
    device->GetRenderState(D3DRS_ZWRITEENABLE, &previous_zwrite);
    device->GetRenderState(D3DRS_ZFUNC, &previous_zfunc);
    device->GetRenderState(D3DRS_FOGENABLE, &previous_fog);
    device->SetDepthStencilSurface(bg.depth);
    const D3DVIEWPORT9 viewport{32, 16, 384, 448, 0.0f, 1.0f};
    device->SetViewport(&viewport);
    setup_camera(bg);
    const D3DMATRIX world = identity();
    uint32_t fog_color = 0;
    for (int channel = 0; channel < 4; ++channel)
        fog_color |= static_cast<uint32_t>(bg.fog[channel + 2]) << (channel * 8);
    // 41c461 clears the STD viewport with Background+0x390: the current fog
    // color. Fully fogged geometry must meet the same color outside its bounds.
    if (first_layer == 0)
        device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, fog_color, 1.0f, 0);
    DWORD fog_start, fog_end;
    std::memcpy(&fog_start, bg.fog, 4);
    std::memcpy(&fog_end, bg.fog + 1, 4);
    device->SetRenderState(D3DRS_FOGCOLOR, fog_color);
    device->SetRenderState(D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
    device->SetRenderState(D3DRS_FOGTABLEMODE, D3DFOG_NONE);
    device->SetRenderState(D3DRS_FOGSTART, fog_start);
    device->SetRenderState(D3DRS_FOGEND, fog_end);
    device->SetRenderState(D3DRS_RANGEFOGENABLE, FALSE);
    for (int layer = first_layer; layer < last_layer; ++layer) {
        for (BackgroundOverlay& overlay : bg.overlays)
            if (overlay.vm != nullptr && overlay.layer == layer) draw_overlay(overlay);
        device->SetTransform(D3DTS_WORLD, &world);
        device->SetTransform(D3DTS_VIEW, &bg.view);
        device->SetTransform(D3DTS_PROJECTION, &bg.projection);
        device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        device->SetRenderState(D3DRS_ZENABLE, TRUE);
        device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
        device->SetRenderState(D3DRS_ALPHAREF, 0);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        for (const StdFace* instance = bg.instances; instance->object_id != 0xffff; ++instance) {
            BackgroundObject& object = bg.objects[instance->object_id];
            if (object.layer == layer)
                for (BackgroundQuad& quad : object.quads) draw_quad(bg, quad, *instance);
        }
    }
    device->SetRenderState(D3DRS_FOGENABLE, previous_fog);
    device->SetRenderState(D3DRS_ZENABLE, previous_z);
    device->SetRenderState(D3DRS_ZWRITEENABLE, previous_zwrite);
    device->SetRenderState(D3DRS_ZFUNC, previous_zfunc);
    device->SetDepthStencilSurface(previous_depth);
    if (previous_depth != nullptr) previous_depth->Release();
    device->SetViewport(&previous_viewport);
    device->SetTransform(D3DTS_VIEW, &previous_view);
    device->SetTransform(D3DTS_PROJECTION, &previous_projection);
    device->SetTransform(D3DTS_WORLD, &previous_world);
}

void stage_background_set_visible(bool visible) { g_background->visible = visible; }

void stage_background_release_depth() {
    for (Background* bg : {g_background, g_previous}) {
        if (bg != nullptr) {
            bg->depth->Release();
            bg->depth = nullptr;
        }
    }
}

void stage_background_create_depth() {
    for (Background* bg : {g_background, g_previous})
        if (bg != nullptr) create_depth(*bg);
}

namespace {
ThreadStatus render_retained(void*) {
    if (g_background != nullptr) {
        g_retained_render = nullptr;
        return THREAD_DONE;
    }
    render_background(*g_previous, 0, 8);
    return THREAD_CONTINUE;
}

ThreadStatus render_retained_upper(void*) {
    if (g_background != nullptr) {
        g_retained_render_upper = nullptr;
        return THREAD_DONE;
    }
    render_background(*g_previous, 8, 12);
    return THREAD_CONTINUE;
}

ThreadStatus update_fade(void*) {
    // 475820: a 30-frame fade reaches black, and is removed at duration+2.
    if (g_fade_time >= 32.0f) {
        thread_remove(g_fade_render);
        g_fade_logic = g_fade_render = nullptr;
        return THREAD_DONE;
    }
    g_fade_alpha = g_fade_time >= 30.0f ? 255 : static_cast<int>(g_fade_time * 255.0f / 30.0f);
    g_fade_time += stage::g_speed_scale;
    return THREAD_CONTINUE;
}

ThreadStatus draw_fade(void*) {
    // 4758b0 -> 4754e0: the stage transition's original black screen object.
    struct Vertex { float x, y, z, rhw; uint32_t color; };
    const uint32_t color = static_cast<uint32_t>(g_fade_alpha) << 24;
    const Vertex vertices[] = {{0,0,0,1,color},{640,0,0,1,color},
                               {0,480,0,1,color},{640,480,0,1,color}};
    IDirect3DDevice9* device = gfx_device();
    gfx_select_camera(0);
    AnmVm blend{};
    anm_apply_blend(&blend);
    device->SetTexture(0, nullptr);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex));
    gfx_select_camera(2);
    return THREAD_CONTINUE;
}
}

void stage_background_destroy() {
    if (g_retained_render) { thread_remove(g_retained_render); g_retained_render = nullptr; }
    if (g_retained_render_upper) { thread_remove(g_retained_render_upper); g_retained_render_upper = nullptr; }
    if (g_fade_logic) {
        thread_remove(g_fade_logic); thread_remove(g_fade_render);
        g_fade_logic = g_fade_render = nullptr;
    }
    if (g_background) destroy_background(g_background);
    if (g_previous) destroy_background(g_previous);
}

void stage_background_retain_current() {
    g_previous = g_background;
    g_background = nullptr;
    // The original STD keeps its own B task while the stage session is replaced.
    g_retained_render = thread_create(render_retained);
    thread_set_priority(g_retained_render, 3);
    thread_enqueue_render(g_retained_render);
    g_retained_render_upper = thread_create(render_retained_upper);
    thread_set_priority(g_retained_render_upper, 6);
    thread_enqueue_render(g_retained_render_upper);
}

bool stage_background_has_previous() { return g_previous != nullptr; }
AnmResource* stage_background_resource() { return g_background->anm; }

void stage_background_begin_transition() {
    // 443e60: old STD flags|2/count30, new STD flags|4/count60.
    g_previous->retiring = true;
    g_previous->transition_time = 30.0f;
    g_background->incoming = true;
    g_background->transition_time = 60.0f;
    g_fade_time = 0.0f;
    g_fade_alpha = 0;
    g_fade_logic = thread_create(update_fade);
    thread_set_priority(g_fade_logic, 0x14);
    thread_enqueue_logic(g_fade_logic);
    g_fade_render = thread_create(draw_fade);
    thread_set_priority(g_fade_render, 0x10);
    thread_enqueue_render(g_fade_render);
}

void stage_background_update() {
    if (g_previous && g_previous->retiring) {
        if (static_cast<int>(g_previous->transition_time) < 1) destroy_background(g_previous);
        else update_background(*g_previous);
    }
    // 41c0a0: the incoming STD runs its script only below the 30-frame mark.
    if (!g_background->incoming || static_cast<int>(g_background->transition_time) < 30)
        update_background(*g_background);
}

void stage_background_render() {
    if (g_previous) render_background(*g_previous, 0, 8);
    if (g_background && (!g_background->incoming || static_cast<int>(g_background->transition_time) < 30))
        render_background(*g_background, 0, 8);
}

void stage_background_render_anm(int layer) {
    // B0x05/layer0 and B0x09/layer2 use the STD camera left by the background.
    // 4876b0 switches to the screen camera at B0x0a.
    Background& bg = *g_background;
    IDirect3DDevice9* device = gfx_device();
    D3DVIEWPORT9 viewport;
    D3DMATRIX view, projection, world;
    IDirect3DSurface9* depth = nullptr;
    DWORD z, zwrite, zfunc, fog;
    device->GetViewport(&viewport);
    device->GetTransform(D3DTS_VIEW, &view);
    device->GetTransform(D3DTS_PROJECTION, &projection);
    device->GetTransform(D3DTS_WORLD, &world);
    device->GetDepthStencilSurface(&depth);
    device->GetRenderState(D3DRS_ZENABLE, &z);
    device->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite);
    device->GetRenderState(D3DRS_ZFUNC, &zfunc);
    device->GetRenderState(D3DRS_FOGENABLE, &fog);
    const D3DVIEWPORT9 background_viewport{32, 16, 384, 448, 0.0f, 1.0f};
    setup_camera(bg);
    device->SetDepthStencilSurface(bg.depth);
    device->SetViewport(&background_viewport);
    device->SetTransform(D3DTS_VIEW, &bg.view);
    device->SetTransform(D3DTS_PROJECTION, &bg.projection);
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    anm_draw_layer(layer);
    device->SetRenderState(D3DRS_ZENABLE, z);
    device->SetRenderState(D3DRS_ZWRITEENABLE, zwrite);
    device->SetRenderState(D3DRS_ZFUNC, zfunc);
    device->SetRenderState(D3DRS_FOGENABLE, fog);
    device->SetDepthStencilSurface(depth);
    if (depth != nullptr) depth->Release();
    device->SetViewport(&viewport);
    device->SetTransform(D3DTS_VIEW, &view);
    device->SetTransform(D3DTS_PROJECTION, &projection);
    device->SetTransform(D3DTS_WORLD, &world);
}

void stage_background_render_upper() {
    if (g_previous) render_background(*g_previous, 8, 12);
    if (g_background && (!g_background->incoming || static_cast<int>(g_background->transition_time) < 30))
        render_background(*g_background, 8, 12);
    // 41c700 is the B0x06 pass: its timer advances after layers 8..11.
    if (g_previous && g_previous->retiring) g_previous->transition_time -= stage::g_speed_scale;
    if (g_background && g_background->incoming) {
        g_background->transition_time -= stage::g_speed_scale;
        if (static_cast<int>(g_background->transition_time) < 1) g_background->incoming = false;
    }
}
