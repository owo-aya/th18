#pragma once
#include <windows.h>

// Win32 入口、窗口创建与固定 60Hz 主循环。
int platform_run(HINSTANCE hinst);
float platform_frame_fps();
int platform_display_mode();
float platform_display_scale();
void platform_set_display_mode(int mode);
