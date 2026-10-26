#include <windows.h>

#include "platform/win32_app.h"

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int) {
    return platform_run(hinst);
}
