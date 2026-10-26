cmake_minimum_required(VERSION 3.24)
project(th18 LANGUAGES CXX)
if(MSVC)
    set(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "注意: 包含文件:")
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "注意: 包含文件:")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 原版为 32 位 PE32，重建目标同为 Win32/x86。
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "需要 32 位工具链：使用 vcvarsall.bat x86 + Ninja")
endif()

set(TH18_SOURCES
    src/main.cpp
    src/core/app.cpp
    src/core/frame.cpp
    src/core/input.cpp
    src/core/rng.cpp
    src/core/thread.cpp
    src/platform/win32_app.cpp
    src/platform/input.cpp
    src/platform/gfx.cpp
    src/resources/dat.cpp
    src/resources/anm_file.cpp
    src/projectiles/laser.cpp
    src/presentation/anm.cpp
    src/presentation/title.cpp
    src/presentation/music_room.cpp
    src/presentation/ending.cpp
    src/presentation/text.cpp
    src/presentation/sound.cpp
    src/storage/storage.cpp
    src/cards/card_select.cpp
    src/cards/card_menu.cpp
    src/cards/card_profile.cpp
    src/cards/card_manager.cpp
    src/cards/card_table.cpp
    src/cards/card_passive.cpp
    src/cards/card_options.cpp
    src/cards/card_active.cpp
    src/replay/replay.cpp
    src/presentation/msg.cpp
    src/presentation/ingame_menu.cpp
    src/presentation/practice_menu.cpp
    src/presentation/pause_help.cpp
    src/presentation/pause_options.cpp
    src/replay/replay_menu.cpp
    src/presentation/trophy.cpp
    src/gameplay/shop.cpp
    src/gameplay/spell.cpp
    src/player/player.cpp
    src/player/bomb.cpp
    src/stage/ecl_vm.cpp
    src/stage/extra_effects.cpp
    src/stage/stage.cpp
    src/stage/hud.cpp
    src/stage/background.cpp
)

add_executable(th18 WIN32 ${TH18_SOURCES})
target_include_directories(th18 PRIVATE src)
target_compile_definitions(th18 PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(th18 PRIVATE /utf-8)
target_link_libraries(th18 PRIVATE d3d9 dsound winmm user32 gdi32 shell32)
