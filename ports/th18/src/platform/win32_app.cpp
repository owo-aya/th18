#include <windows.h>
#include <shellapi.h>
#include <timeapi.h>

#include <algorithm>
#include <cstdio>

#include "core/app.h"
#include "core/frame.h"
#include "core/frame_input.h"
#include "core/input.h"
#include "core/thread.h"
#include "platform/gfx.h"
#include "platform/input.h"
#include "platform/win32_app.h"
#include "presentation/sound.h"
#include "presentation/text.h"
#include "resources/dat.h"
#include "storage/storage.h"
#include "gameplay/score_data.h"
#include "stage/stage.h"

static float g_frame_fps=60.0f;
float platform_frame_fps() {return g_frame_fps;}

static HWND g_window;
static int g_display_mode=3;
static int g_desktop_width,g_desktop_height;
static int g_backbuffer_height;
static float g_display_scale=1.0f;
static RECT g_window_rect;

int platform_display_mode() {return g_display_mode;}

void platform_set_display_mode(int mode) {
    // 4734e0: client size and D3D backbuffer size are separate, especially in DBD.
    static constexpr int widths[]={640,960,1280,640,960,1280,1920,2560};
    static constexpr int heights[]={480,720,960,480,720,960,1440,1920};
    int client_width,client_height,backbuffer_width,backbuffer_height;
    if(mode<8) {
        client_width=widths[mode];client_height=heights[mode];
        g_display_scale=(mode==0 || mode==3)?1.0f:(mode==1 || mode==4)?1.5f:2.0f;
        backbuffer_width=int(640*g_display_scale);backbuffer_height=int(480*g_display_scale);
    } else {
        client_width=g_desktop_width;client_height=g_desktop_height;
        const int base_width=int(640*g_display_scale),base_height=int(480*g_display_scale);
        const float aspect=float(g_desktop_width)/float(g_desktop_height);
        if(mode==9) {
            if(aspect>=float(base_width)/float(base_height)) {
                backbuffer_height=base_height;backbuffer_width=int(aspect*float(base_height));
            } else {
                backbuffer_width=base_width;backbuffer_height=int(float(g_desktop_height)/float(g_desktop_width)*float(base_width));
            }
        } else if(aspect>=float(base_width)/float(base_height)) {
            int height=base_height;const int half=height/2;
            while(height+half<g_desktop_height)height+=half;
            backbuffer_height=int(float(g_desktop_height)/float(height)*float(base_height));
            backbuffer_width=int(float(backbuffer_height)*aspect);
        } else {
            int width=base_width;const int half=width/2;
            while(width+half<g_desktop_width)width+=half;
            // 4737f7 reads the preceding 56ac9c value on this original branch.
            backbuffer_width=int(float(g_desktop_width)/float(width)*float(base_height));
            backbuffer_height=int(float(g_backbuffer_height)*(float(g_desktop_height)/float(g_desktop_width)));
        }
    }

    if(g_display_mode>=3 && g_display_mode<=7)GetWindowRect(g_window,&g_window_rect);
    if(mode<3) {
        SetWindowLongW(g_window,GWL_STYLE,0x90000000u);
        SetWindowPos(g_window,nullptr,0,0,client_width,client_height,SWP_FRAMECHANGED|SWP_SHOWWINDOW|SWP_NOZORDER);
    } else if(mode<8) {
        SetWindowLongW(g_window,GWL_STYLE,0x10cb0000);
        RECT bounds{0,0,client_width,client_height};AdjustWindowRectEx(&bounds,0x10cb0000,FALSE,0);
        const LONG width=bounds.right-bounds.left,height=bounds.bottom-bounds.top;
        MONITORINFO monitor{sizeof(MONITORINFO)};
        GetMonitorInfoW(MonitorFromRect(&g_window_rect,MONITOR_DEFAULTTONEAREST),&monitor);
        const RECT& work=monitor.rcWork;
        const LONG x=std::max(work.left,std::min(g_window_rect.left,work.right-width));
        const LONG y=std::max(work.top,std::min(g_window_rect.top,work.bottom-height));
        SetWindowPos(g_window,nullptr,x,y,width,height,
                     SWP_FRAMECHANGED|SWP_SHOWWINDOW|SWP_NOZORDER);
        ShowWindow(g_window,SW_SHOWNORMAL);
    } else {
        SetWindowLongW(g_window,GWL_STYLE,0);
        ShowWindow(g_window,SW_SHOW);
        SetWindowPos(g_window,nullptr,0,0,client_width,client_height,SWP_SHOWWINDOW|SWP_NOZORDER);
    }
    gfx_set_display_mode(mode<3,backbuffer_width,backbuffer_height);
    g_backbuffer_height=backbuffer_height;
    g_display_mode=mode;
    storage_config().data[0x47]=static_cast<uint8_t>(mode);
    GetWindowRect(g_window,&g_window_rect);
}

namespace {

void save_window_position() {
    // 最小化时GetWindowRect会返回(-32000,-32000)，不能存为下次启动位置。
    if(g_display_mode>2 && !IsIconic(g_window)) {
        GetWindowRect(g_window,&g_window_rect);
        *reinterpret_cast<int32_t*>(storage_config().data+0x54)=g_window_rect.left;
        *reinterpret_cast<int32_t*>(storage_config().data+0x58)=g_window_rect.top;
    }
}

// exe 所在目录（不带末尾反斜杠）。
void exe_dir(wchar_t* buf, size_t cap) {
    GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(cap));
    wchar_t* slash = wcsrchr(buf, L'\\');
    if (slash != nullptr) {
        *slash = L'\0';
    }
}

void to_ansi(const wchar_t* src, char* dst, size_t cap) {
    WideCharToMultiByte(CP_ACP, 0, src, -1, dst, static_cast<int>(cap), nullptr, nullptr);
}

// 原版按 exe 目录找资源；重建版支持 --game-dir / --user-dir（相对路径按 exe 目录展开）。
void resolve_dirs(char* game_dir, size_t game_cap, char* user_dir, size_t user_cap) {
    wchar_t base[520];
    exe_dir(base, 520);
    to_ansi(base, game_dir, game_cap);
    to_ansi(base, user_dir, user_cap);

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return;
    }
    for (int i = 1; i + 1 < argc; ++i) {
        if (wcscmp(argv[i], L"--game-dir") == 0) {
            to_ansi(argv[i + 1], game_dir, game_cap);
        } else if (wcscmp(argv[i], L"--user-dir") == 0) {
            to_ansi(argv[i + 1], user_dir, user_cap);
        }
    }
    LocalFree(argv);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
        // 原版 FUN_00453460：窗口关闭请求（DAT_004cd624 无 0x180 时）置请求状态 3，
        // 由状态机走清理流程后 app 线程返回 STOP_ZERO 退出主循环。
        app_context()->next_state = 3;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

}  // namespace

int platform_run(HINSTANCE hinst) {
    timeBeginPeriod(1);

    char game_dir[520];
    char user_dir[520];
    resolve_dirs(game_dir, sizeof(game_dir), user_dir, sizeof(user_dir));
    storage_set_game_dir(game_dir);
    storage_set_user_dir(user_dir);
    storage_config_init();
    score_init();
    SetCurrentDirectoryA(game_dir);

    // 原版 FUN_00471270 启动时打开 th18.dat（FUN_0046eb80）。
    char dat_path[600];
    std::snprintf(dat_path, sizeof(dat_path), "%s\\th18.dat", game_dir);
    if (!dat_open(dat_path)) {
        MessageBoxW(nullptr, L"找不到 th18.dat", L"东方虹龙洞", MB_ICONERROR);
        return 0;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"TH18";
    RegisterClassExW(&wc);

    DEVMODEW desktop{};desktop.dmSize=sizeof(desktop);EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&desktop);
    g_desktop_width=desktop.dmPelsWidth;g_desktop_height=desktop.dmPelsHeight;
    const int display_mode=storage_config().data[0x47];
    if(display_mode>=8) {
        auto& scale=storage_config().data[0x4e];
        if(g_desktop_width>=2560 && g_desktop_height>=1920)scale=4;
        else if(g_desktop_width>=1920 && g_desktop_height>=1440)scale=3;
        else if(g_desktop_width>=1280 && g_desktop_height>=960)scale=2;
        else if(g_desktop_width>=960 && g_desktop_height>=720)scale=1;
        else scale=0;
        g_display_scale=scale==0?1.0f:scale==1?1.5f:2.0f;
    }
    RECT rc{0,0,640,480};AdjustWindowRectEx(&rc,0x00cb0000,FALSE,0);
    const int window_x=*reinterpret_cast<const int32_t*>(storage_config().data+0x54);
    const int window_y=*reinterpret_cast<const int32_t*>(storage_config().data+0x58);
    HWND hwnd=CreateWindowExW(0,L"TH18",L"东方虹龙洞",0x00cb0000,window_x,window_y,
        rc.right-rc.left,rc.bottom-rc.top,nullptr,nullptr,hinst,nullptr);
    g_window=hwnd;GetWindowRect(hwnd,&g_window_rect);

    gfx_init(hwnd);
    platform_set_display_mode(display_mode);
    storage_log("init: gfx ok");
    input_reset();
    thread_manager_init();
    app_context_init();
    stage_dispatchers_init();

    // 原版 FUN_00471270：malloc 0x39724b8 建游戏数据对象（DAT_0051f65c）；
    // FUN_00453460 每帧还会推进文字系统（DAT_004cf2ac）与 BGM（CSound）。
    game_data_init();
    storage_log("init: game_data ok");
    text_create();
    storage_log("init: text ok");
    sound_init();
    sound_set_se_enabled(storage_config().data[0x46]!=0);
    sound_set_se_volume(storage_config().data[0x4b]);
    bgm_set_volume(storage_config().data[0x4a]);
    bgm_set_enabled(!(storage_config().data[0x50]&0x10) || storage_config().data[0x45]!=0);
    storage_log("init: sound ok");

    // 原版 FUN_00453db0：app 主线程进入 A 队列，优先级 1；创建时处于暂停标记。
    Thread* app_thread = thread_create(app_context_step);
    thread_set_priority(app_thread, 1);
    thread_set_ctx(app_thread, app_context());
    thread_set_paused(app_thread, true);
    thread_enqueue_logic(app_thread);

    const double step = 1.0 / LOGIC_FPS;
    double acc = 0.0;
    LARGE_INTEGER freq, prev;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);
    LARGE_INTEGER previous_frame=prev;

    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                app_context_shutdown();
                stage_dispatchers_shutdown();
                save_window_position();
                storage_config_save();
                sound_shutdown();
                text_destroy(text_manager());
                game_data_shutdown();
                dat_close();
                gfx_shutdown();
                timeEndPeriod(1);
                return (int)msg.wParam;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        acc += (double)(now.QuadPart - prev.QuadPart) / (double)freq.QuadPart;
        prev = now;

        if (acc >= step) {
            // 全屏失焦或切模式期间停止逻辑，也不累计恢复后的补帧。
            // 关闭请求仍交给app状态机清理，即使设备尚未恢复。
            if (app_context()->next_state != 3 && !gfx_ready()) {
                acc = 0;
                previous_frame = now;
                Sleep(10);
                continue;
            }
            g_frame_fps=float(double(freq.QuadPart)/double(now.QuadPart-previous_frame.QuadPart));
            previous_frame=now;
            uint32_t raw_bits = input_sample_device();
            {
                static uint32_t last_bits = 0;
                if (raw_bits != last_bits) {
                    storage_log("input raw=0x%x", raw_bits);
                    last_bits = raw_bits;
                }
            }
            input_update(raw_bits);
            frame_update(input_frame());
            int32_t rc = thread_run_logic();
            if (rc <= 0) {
                app_context_shutdown();
                stage_dispatchers_shutdown();
                save_window_position();
                storage_config_save();
                // 原版主循环：FUN_004012e0 返回 0=退出（STOP_ZERO）、-1=重启，
                // 其余为正常帧（执行过的线程数）。
                sound_shutdown();
                text_destroy(text_manager());
                game_data_shutdown();
                dat_close();
                gfx_shutdown();
                timeEndPeriod(1);
                return rc == 0 ? 0 : 1;
            }
            if (gfx_begin()) {
                gfx_clear();
                thread_run_render();
                gfx_end();
            }
            acc -= step;
        }
        Sleep(1);
    }
}
