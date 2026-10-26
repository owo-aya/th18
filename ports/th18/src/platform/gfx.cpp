#include <d3d9.h>

#include "platform/gfx.h"
#include "stage/stage.h"
#include "stage/background.h"

static IDirect3D9* g_d3d = nullptr;
static IDirect3DDevice9* g_dev = nullptr;
static HWND g_hwnd = nullptr;
static int g_camera = 2;
static float g_camera_shake[4][2];
static IDirect3DTexture9* g_scene_texture = nullptr;
static IDirect3DSurface9* g_scene_surface = nullptr;
static IDirect3DSurface9* g_backbuffer = nullptr;
static HMODULE g_d3dx = nullptr;
using LoadSurfaceFromSurface = HRESULT(WINAPI*)(IDirect3DSurface9*, const PALETTEENTRY*, const RECT*,
    IDirect3DSurface9*, const PALETTEENTRY*, const RECT*, DWORD, D3DCOLOR);
static LoadSurfaceFromSurface g_load_surface = nullptr;
static D3DPRESENT_PARAMETERS g_present;
static bool g_reset_pending = false;
static D3DCOLOR g_clear_color = 0xff000000; // DAT_004cd948

static void create_scene_surface() {
    g_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &g_backbuffer);
    g_dev->CreateTexture(640, 480, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                        D3DPOOL_DEFAULT, &g_scene_texture, nullptr);
    g_scene_texture->GetSurfaceLevel(0, &g_scene_surface);
}

static void release_scene_surface() {
    g_scene_surface->Release();
    g_scene_texture->Release();
    g_backbuffer->Release();
    g_scene_surface = nullptr;
    g_scene_texture = nullptr;
    g_backbuffer = nullptr;
}

bool gfx_init(HWND hwnd) {
    g_hwnd = hwnd;
    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d) {
        return false;
    }

    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    pp.BackBufferWidth = 640;
    pp.BackBufferHeight = 480;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    g_present = pp;

    HRESULT hr = g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &g_dev);
    if (SUCCEEDED(hr)) {
        // 原ccfa0是跨Present保留的场景surface，458480暂停底图从这里取图。
        create_scene_surface();
        g_d3dx = LoadLibraryA("d3dx9_43.dll");
        g_load_surface = reinterpret_cast<LoadSurfaceFromSurface>(
            GetProcAddress(g_d3dx, "D3DXLoadSurfaceFromSurface"));
    }
    return SUCCEEDED(hr);
}

static bool reset_device() {
    if (g_scene_surface != nullptr) {
        g_dev->SetRenderTarget(0, g_backbuffer);
        g_dev->SetDepthStencilSurface(nullptr);
        stage_background_release_depth();
        release_scene_surface();
    }
    // Reset改写宽高和窗口格式；下一次切全屏仍需原来的明确格式。
    D3DPRESENT_PARAMETERS parameters = g_present;
    if (FAILED(g_dev->Reset(&parameters))) return false;
    create_scene_surface();
    stage_background_create_depth();
    g_dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
    g_dev->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_ZERO);
    g_dev->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
    g_reset_pending = false;
    return true;
}

void gfx_set_display_mode(bool fullscreen, int width, int height) {
    g_present.Windowed = fullscreen ? FALSE : TRUE;
    g_present.BackBufferWidth = width;
    g_present.BackBufferHeight = height;
    g_reset_pending = true;
    reset_device();
}

bool gfx_ready() {
    const HRESULT state = g_dev->TestCooperativeLevel();
    if (state == D3DERR_DEVICELOST) return false;
    if (state == D3DERR_DEVICENOTRESET) g_reset_pending = true;
    return !g_reset_pending || reset_device();
}

void gfx_shutdown() {
    if (g_scene_surface != nullptr) release_scene_surface();
    FreeLibrary(g_d3dx);
    if (g_dev) {
        g_dev->Release();
        g_dev = nullptr;
    }
    if (g_d3d) {
        g_d3d->Release();
        g_d3d = nullptr;
    }
}

bool gfx_begin() {
    // 设置页在本帧逻辑中切模式；Reset未完成时不进入绘制。
    if (g_reset_pending) return false;
    g_dev->SetRenderTarget(0, g_scene_surface);
    g_dev->BeginScene();
    gfx_select_camera(2);
    return true;
}

void gfx_select_camera(int camera, bool clipped) {
    g_camera = camera;
    // 保留端口既有1280x960画布和world对象显式原点，不再用viewport重缩一次。
    const D3DVIEWPORT9 viewport{0, 0, 640, 480, 0, 1};
    g_dev->SetViewport(&viewport);
    D3DMATRIX view{};
    view._11 = view._22 = view._33 = view._44 = 1;
    view._41 = 2.0f * g_camera_shake[camera][0];
    view._42 = 2.0f * g_camera_shake[camera][1];
    D3DMATRIX projection{};
    projection._11 = 2.0f / 1280;
    projection._22 = -2.0f / 960;
    projection._33 = 1;
    projection._41 = -1;
    projection._42 = 1;
    projection._44 = 1;
    g_dev->SetTransform(D3DTS_VIEW, &view);
    g_dev->SetTransform(D3DTS_PROJECTION, &projection);
    const bool game_clip = clipped || (stage::g_stage != nullptr && camera != 2);
    const RECT clip = game_clip ? RECT{32, 16, 416, 464} : RECT{0, 0, 640, 480};
    g_dev->SetScissorRect(&clip);
    g_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, game_clip ? TRUE : FALSE);
    g_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
    g_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
}

int gfx_current_camera() { return g_camera; }

void gfx_set_camera_shake(int camera, float x, float y) {
    g_camera_shake[camera][0] = x;
    g_camera_shake[camera][1] = y;
}

void gfx_get_camera_shake(int camera, float& x, float& y) {
    x = g_camera_shake[camera][0];
    y = g_camera_shake[camera][1];
}

void gfx_set_clear_color(D3DCOLOR color) { g_clear_color = color; }

void gfx_clear() {
    g_dev->SetRenderTarget(0, g_scene_surface);
    const D3DVIEWPORT9 viewport{0, 0, 640, 480, 0, 1};
    g_dev->SetViewport(&viewport);
    g_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    g_dev->Clear(0, nullptr, D3DCLEAR_TARGET, g_clear_color, 1.0f, 0);
}

void gfx_end() {
    g_dev->EndScene();
    g_dev->SetRenderTarget(0, g_backbuffer);
    const D3DVIEWPORT9 viewport{0, 0, g_present.BackBufferWidth, g_present.BackBufferHeight, 0, 1};
    g_dev->SetViewport(&viewport);
    g_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    g_dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1, 0);
    int width = g_present.BackBufferWidth;
    int height = g_present.BackBufferHeight;
    if (width * 3 > height * 4) width = height * 4 / 3;
    else height = width * 3 / 4;
    const int x = (g_present.BackBufferWidth - width) / 2;
    const int y = (g_present.BackBufferHeight - height) / 2;
    const RECT rectangle{x, y, x + width, y + height};
    g_dev->StretchRect(g_scene_surface, nullptr, g_backbuffer, &rectangle, D3DTEXF_NONE);
    g_dev->Present(nullptr, nullptr, nullptr, nullptr);
}

void gfx_copy_game_surface(IDirect3DSurface9* destination, const RECT& rectangle) {
    // 458480的源RECT；原调用两处右/下端都减1，D3DX过滤参数为2(POINT)。
    const RECT source{32, 16, 415, 463};
    g_load_surface(destination, nullptr, &rectangle, g_scene_surface, nullptr, &source, 2, 0);
}

IDirect3DDevice9* gfx_device() {
    return g_dev;
}

// 声音模块（DirectSound8 需要 HWND 调 SetCooperativeLevel/SetTimer）用。
HWND gfx_hwnd() {
    return g_hwnd;
}
