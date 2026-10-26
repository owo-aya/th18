#include "presentation/pause_pages.h"

#include <cstdio>
#include <cstdint>

#include "core/input.h"
#include "core/thread.h"
#include "platform/gfx.h"
#include "platform/win32_app.h"
#include "presentation/sound.h"
#include "presentation/text.h"
#include "stage/stage.h"
#include "storage/storage.h"

namespace {

// 004b42c0: the keyboard buttons accepted by 004019f0 / 00447700.
// The five trailing zero bytes pad the original table; VK 0 is not a button.
constexpr uint8_t kKeys[] = {
    1,2,4,5,6,8,9,16,17,18,27,32,33,34,35,36,45,46,
    48,49,50,51,52,53,54,55,56,57,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,
    186,187,188,189,190,191,192,219,220,221,222,223,226
};
constexpr int kMappingIndices[] = {0,1,2,8,9,3};
constexpr uint16_t kDefaultKeys[] = {'Z','X',VK_SHIFT,'C','D',VK_ESCAPE};
constexpr char const* kOptionNames[] = {"Screen","BGM Vol","SE Vol","Key Config","Default","Quit"};
constexpr char const* kKeyNames[] = {"Shot","Bomb","Slow","Item","Change","Pause","Default","Quit"};
// 0041b1c0 reads the first float of each pair at 004b6208.
constexpr float kNudge[] = {2,-1,0,3,-3,2,-2,0,2};

struct KeyConfigPage {
    int state = 0;
    int timer = 0;
    float time = 0;
    int selection = 0;
    float nudge_time = 0;
    uint16_t keys[6]{};
};

const char* key_name(uint16_t key, char* buffer) {
    // 004b4370: retain the original English names, including unnamed VK slots.
    if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z')) {
        buffer[0] = static_cast<char>(key);
        buffer[1] = '\0';
        return buffer;
    }
    if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) {
        std::sprintf(buffer, "Num%d", key - VK_NUMPAD0);
        return buffer;
    }
    if (key >= VK_F1 && key <= VK_F24) {
        std::sprintf(buffer, "F%d", key - VK_F1 + 1);
        return buffer;
    }
    switch (key) {
    case 1: return "MouseL";
    case 2: return "MouseR";
    case 4: return "MouseM";
    case 5: return "MouseX1";
    case 6: return "MouseX2";
    case 8: return "BackSpace";
    case 9: return "Tab";
    case 12: return "Clear";
    case 13: return "Enter";
    case 16: return "Shift";
    case 17: return "Ctrl";
    case 18: return "Alt";
    case 19: return "Pause";
    case 20: return "Caps";
    case 21: return "Kana";
    case 27: return "Esc";
    case 28: return "Convt";
    case 29: return "NonConv";
    case 32: return "Space";
    case 33: return "PageUp";
    case 34: return "PageDown";
    case 35: return "End";
    case 36: return "Home";
    case 37: return "Left";
    case 38: return "Up";
    case 39: return "Right";
    case 40: return "Down";
    case 41: return "Select";
    case 42: return "Print";
    case 43: return "Execute";
    case 44: return "SnapShot";
    case 45: return "Insert";
    case 46: return "Del";
    case 47: return "Help";
    case 91: return "LWin";
    case 92: return "RWin";
    case 93: return "App";
    case 95: return "Sleep";
    case 106: return "Num*";
    case 107: return "Num+";
    case 108: return "Separator";
    case 109: return "Num-";
    case 110: return "Num.";
    case 111: return "Num/";
    case 144: return "NumLock";
    case 145: return "Scroll";
    case 186: return "OEM1";
    case 187: return "PLUS";
    case 188: return "COMMA";
    case 189: return "MINUS";
    case 190: return "PERIOD";
    case 191: return "OEM2";
    case 192: return "OEM3";
    case 219: return "OEM4";
    case 220: return "OEM5";
    case 221: return "OEM6";
    case 222: return "OEM7";
    case 223: return "OEM8";
    case 226: return "OEM102";
    default: std::sprintf(buffer, "*%d", key); return buffer;
    }
}

void key_state(KeyConfigPage& page, int state) {
    page.state = state;
    page.timer = 0;
    page.time = 0;
}

bool update_keys(KeyConfigPage& page) {
    const FrameInput input = input_frame();
    const uint32_t movement = input.pressed | input.repeat;
    auto* mappings = reinterpret_cast<uint16_t*>(storage_config().data + 0x2c);
    switch (page.state) {
    case 0:
        page.selection = 0;
        for (int i = 0; i < 6; ++i) page.keys[i] = mappings[kMappingIndices[i]];
        key_state(page, 1);
        [[fallthrough]];
    case 1:
        if (page.timer > 6) key_state(page, 2);
        break;
    case 2: {
        const int old = page.selection;
        if (movement & ACTION_UP) page.selection = (page.selection + 7) % 8;
        if (movement & ACTION_DOWN) page.selection = (page.selection + 1) % 8;
        if (page.selection != old) {
            sound_play_se(10);
            page.nudge_time = 8;
        }
        // 004019f0 clears the raw keyboard buffer before sampling it.
        BYTE keyboard[256]{};
        GetKeyboardState(keyboard);
        for (uint8_t key : kKeys) {
            if ((keyboard[key] & 0x80) == 0) continue;
            if (page.selection < 6 && page.keys[page.selection] != key) {
                // 00447be0 swaps an occupied button with the selected action's old one.
                const uint16_t previous = page.keys[page.selection];
                for (int i = 0; i < 6; ++i)
                    if (i != page.selection && page.keys[i] == key) page.keys[i] = previous;
                page.keys[page.selection] = key;
                sound_play_se(7);
                page.nudge_time = 8;
            }
            break;
        }
        const bool confirm = (input.pressed & (ACTION_SHOT | ACTION_MENU_OK)) != 0;
        const bool quit = page.selection == 7 && (input.pressed & 0xd0a) != 0;
        if (confirm && page.selection == 6) {
            for (int i = 0; i < 6; ++i) page.keys[i] = kDefaultKeys[i];
            sound_play_se(7);
        } else if (quit || (confirm && page.selection == 7)) {
            for (int i = 0; i < 6; ++i) mappings[kMappingIndices[i]] = page.keys[i];
            sound_play_se(9);
            key_state(page, 4);
        }
        break;
    }
    case 4:
        if (page.timer > 9) {
            storage_config_save();
            return true;
        }
        break;
    }
    page.time += stage::g_speed_scale;
    page.timer = static_cast<int>(page.time);
    if (page.nudge_time > 0) page.nudge_time -= stage::g_speed_scale;
    return false;
}

void draw_text(float x, float y, int alignment, const char* text) {
    text_set_alignment(alignment, 1);
    const TextPos position{x * 2, y * 2, 0};
    text_draw_fmt(text_manager(), &position, "%s", text);
}

float row_color(int item, int selected, float nudge_time, int blink) {
    if (item != selected) {
        text_set_color(0xff606030);
        text_set_secondary_color(0xffffffff);
        return 0;
    }
    if (blink == 0) {
        text_set_color(0xff000000);
        text_set_secondary_color(0xff20d0ff);
    } else if ((blink & 3) < 2) {
        text_set_color(0xff000000);
        text_set_secondary_color(0xffffff00);
    } else {
        text_set_color(0xffffff00);
        text_set_secondary_color(0xffffffff);
    }
    const int nudge = static_cast<int>(nudge_time);
    return nudge > 0 ? kNudge[nudge] * 0.5f : 0;
}

void render_keys(const KeyConfigPage& page) {
    text_set_color(0xffffffff);
    text_set_secondary_color(0xff000000);
    draw_text(228, 100, 0, "Key Config");
    text_set_color(0xff808080);
    draw_text(228, 128, 0, "KeyBoard");
    for (int i = 0; i < 8; ++i) {
        const float nudge = row_color(i, page.selection, page.nudge_time, 0);
        const float y = 178.0f + i * 28.0f + nudge;
        draw_text(88 + nudge, y, 1, kKeyNames[i]);
        if (i < 6) {
            char buffer[16];
            draw_text(348 + nudge, y, 0, key_name(page.keys[i], buffer));
        }
    }
}

} // namespace

struct PauseOptions {
    int state = 0;
    int timer = 0;
    int previous_timer = -1;
    float time = 0;
    int selection = 0;
    float nudge_time = 0;
    float blink_time = 0;
    int pending_mode = 0;
    bool keys_active = false;
    KeyConfigPage keys;
    Thread* black_render = nullptr;
    float black_time = 0;
};

namespace {

void set_state(PauseOptions& page, int state) {
    page.state = state;
    page.timer = 0;
    page.previous_timer = -1;
    page.time = 0;
}

ThreadStatus render_black(void*) {
    // 00476060(9,10,0,0,0,0x5b) -> 004759e0 / 00475a70.
    gfx_select_camera(2);
    IDirect3DDevice9* device = gfx_device();
    D3DVIEWPORT9 viewport;
    device->GetViewport(&viewport);
    const float x = static_cast<float>(viewport.X);
    const float y = static_cast<float>(viewport.Y);
    const float right = x + viewport.Width;
    const float bottom = y + viewport.Height;
    struct Vertex { float x, y, z, rhw; uint32_t color; };
    const Vertex vertices[] = {{x,y,0,1,0xff000000},{right,y,0,1,0xff000000},
        {x,bottom,0,1,0xff000000},{right,bottom,0,1,0xff000000}};
    device->SetTexture(0, nullptr);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex));
    return THREAD_CONTINUE;
}

void change_screen(PauseOptions& page, int direction) {
    int mode = page.state == 4 ? page.pending_mode : platform_display_mode();
    const int scale = storage_config().data[0x4e];
    constexpr int window_modes[] = {3,4,5,6,7,5};
    constexpr int fullscreen_modes[] = {0,1,2,2,2,2};
    if (direction < 0) {
        if (mode <= 2) mode = 9;
        else if (mode == 3) mode = 0;
        else if (mode == 4) mode = 1;
        else if (mode <= 7) mode = 2;
        else if (mode == 8) mode = window_modes[scale];
        else mode = 8;
    } else {
        if (mode == 0) mode = 3;
        else if (mode == 1) mode = 4;
        else if (mode == 2) mode = window_modes[scale];
        else if (mode <= 7) mode = 8;
        else if (mode == 8) mode = 9;
        else mode = fullscreen_modes[scale];
    }
    page.pending_mode = mode;
    page.black_time = 0;
    if (page.black_render == nullptr) {
        page.black_render = thread_create(render_black);
        page.black_render->flags |= 2;
        thread_set_priority(page.black_render, 0x5b);
        thread_enqueue_render(page.black_render);
    }
    set_state(page, 4);
}

void change_volume(int selection, int direction) {
    uint8_t& volume = storage_config().data[0x49 + selection];
    if (direction < 0) volume = volume < 5 ? 0 : volume - 5;
    else volume = volume + 5 > 100 ? 100 : volume + 5;
    // 00457100 applies both volumes after either row changes.
    bgm_set_volume(storage_config().data[0x4a]);
    sound_set_se_volume(storage_config().data[0x4b]);
}

} // namespace

PauseOptions* pause_options_create() {
    // 00456880, with the pause caller's (60,100,0) and +0x130 = 1.
    return new PauseOptions;
}

void pause_options_destroy(PauseOptions* page) {
    if (page->black_render != nullptr) thread_remove(page->black_render);
    storage_config_save();
    delete page;
}

bool pause_options_update(PauseOptions* pointer) {
    PauseOptions& page = *pointer;
    if (page.black_render != nullptr) {
        page.black_time += stage::g_speed_scale;
        if (static_cast<int>(page.black_time) >= 10) {
            thread_remove(page.black_render);
            page.black_render = nullptr;
        }
    }
    if (page.keys_active) {
        if (update_keys(page.keys)) page.keys_active = false;
        return false;
    }
    const FrameInput input = input_frame();
    const uint32_t movement = input.pressed | input.repeat;
    switch (page.state) {
    case 0:
        page.selection = 0;
        set_state(page, 1);
        [[fallthrough]];
    case 1:
        if (page.timer > 6) set_state(page, 2);
        break;
    case 2: {
        const int old = page.selection;
        if (movement & ACTION_UP) page.selection = (page.selection + 5) % 6;
        if (movement & ACTION_DOWN) page.selection = (page.selection + 1) % 6;
        if (old != page.selection) {
            sound_play_se(10);
            page.nudge_time = 8;
        }
        if (page.selection == 2 && page.timer != page.previous_timer && page.timer % 60 == 0)
            sound_play_se(2);
        if (movement & ACTION_LEFT) {
            if (page.selection == 0) change_screen(page, -1);
            else if (page.selection == 1 || page.selection == 2) change_volume(page.selection, -1);
        }
        if (movement & ACTION_RIGHT) {
            if (page.selection == 0) change_screen(page, 1);
            else if (page.selection == 1 || page.selection == 2) change_volume(page.selection, 1);
        }
        const bool cancel = (input.pressed & (ACTION_BOMB | ACTION_MENU_CANCEL)) != 0;
        const bool confirm = (input.pressed & (ACTION_SHOT | ACTION_MENU_OK)) != 0;
        const int cancel_selection = page.selection;
        if (cancel && page.selection != 5) page.selection = 5;
        if (cancel && ((input.pressed & ACTION_MENU_CANCEL) || cancel_selection == 5)) {
            sound_play_se(9);
            set_state(page, 5);
        } else if (confirm) {
            if (page.selection == 3) {
                set_state(page, 3);
                page.blink_time = 30;
                sound_play_se(7);
            } else if (page.selection == 4) {
                // 00456b60 writes these defaults without sending 00457100.
                storage_config().data[0x4a] = 100;
                storage_config().data[0x4b] = 80;
                sound_play_se(7);
            } else if (page.selection == 5) {
                sound_play_se(9);
                set_state(page, 5);
            }
        }
        break;
    }
    case 3:
        if (page.timer > 30) {
            page.keys = KeyConfigPage{};
            page.keys_active = true;
            page.state = 0;
        }
        break;
    case 4:
        if (page.timer > 3) {
            platform_set_display_mode(page.pending_mode);
            storage_config().data[0x47] = static_cast<uint8_t>(page.pending_mode);
            set_state(page, 2);
        }
        break;
    case 5:
        if (page.timer > 9) return true;
        break;
    }
    page.previous_timer = page.timer;
    page.time += stage::g_speed_scale;
    page.timer = static_cast<int>(page.time);
    if (page.nudge_time > 0) page.nudge_time -= stage::g_speed_scale;
    if (page.blink_time > 0) page.blink_time -= stage::g_speed_scale;
    return false;
}

void pause_options_render(PauseOptions* pointer) {
    const PauseOptions& page = *pointer;
    text_set_group(0);
    text_set_life(0);
    text_set_size(1, 1);
    text_set_style(7);
    if (page.keys_active) render_keys(page.keys);
    else {
        text_set_color(0xffffffff);
        text_set_secondary_color(0xff000000);
        draw_text(228, 100, 0, "Option");
        for (int i = 0; i < 6; ++i) {
            const float nudge = row_color(i, page.selection, page.nudge_time,
                                          static_cast<int>(page.blink_time));
            const float y = 140.0f + i * 28.0f + nudge;
            draw_text(60 + nudge, y, 1, kOptionNames[i]);
            if (i == 0) {
                const int mode = platform_display_mode();
                const char* name = mode <= 2 ? "FullScreen" : mode <= 7 ? "Window" :
                    mode == 8 ? "BorderlessDBD" : "Borderless";
                draw_text(300 + nudge, y, 0, name);
            } else if (i == 1 || i == 2) {
                char value[16];
                std::sprintf(value, "%d%%", storage_config().data[0x49 + i]);
                draw_text(300 + nudge, y, 0, value);
            }
        }
    }
    text_set_style(0);
    text_set_color(0xffffffff);
}
