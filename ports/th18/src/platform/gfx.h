#pragma once
#include <d3d9.h>
#include <windows.h>

// D3D9 设备与绘制帧边界。
bool gfx_init(HWND hwnd);
void gfx_set_display_mode(bool fullscreen, int width, int height);
void gfx_shutdown();
bool gfx_ready();   // 恢复丢失设备；未恢复时暂停帧推进
bool gfx_begin();   // BeginScene；设置页切模式未完成时不绘制
void gfx_clear();   // 清屏
void gfx_set_clear_color(D3DCOLOR color); // 原 DAT_004cd948，STD 13 设置的场景底色
void gfx_end();     // EndScene + Present
IDirect3DDevice9* gfx_device();
// 487580/458480：把上一完整场景的游戏区复制到暂停底图精灵的texture区域。
void gfx_copy_game_surface(IDirect3DSurface9* destination, const RECT& rectangle);
// Boss distortion samples the current full scene (@R), including edge texels.
void gfx_copy_scene_surface(IDirect3DSurface9* destination);
HWND gfx_hwnd();
// 当前1280虚拟画布的原相机pass；clipped对应camera2的+124裁剪viewport。
void gfx_select_camera(int camera, bool clipped = false);
int gfx_current_camera();
// 原相机 +0xfc/+0x100，以640x480画布像素为单位。
void gfx_set_camera_shake(int camera, float x, float y);
void gfx_get_camera_shake(int camera, float& x, float& y);
