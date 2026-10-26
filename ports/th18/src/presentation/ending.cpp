#include "presentation/ending.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/app.h"
#include "core/game_session.h"
#include "core/input.h"
#include "core/thread.h"
#include "gameplay/score_data.h"
#include "platform/gfx.h"
#include "presentation/anm.h"
#include "presentation/sound.h"
#include "presentation/text.h"
#include "resources/dat.h"
#include "stage/stage.h"

namespace {
struct Ending {
    Thread* logic;
    Thread* fade_logic;
    Thread* fade_render;
    uint8_t* file;
    const uint8_t* cursor;
    AnmVm* words[5];
    AnmVm* pictures[16];
    AnmResource* resources[4];
    int line;
    float time;
    float wait;
    int iterations;
    uint32_t color;
    bool first_ending;
    bool first_staff;
    bool staff;
    bool fading;
    bool fade_out;
    int fade_duration;
    float fade_time;
    uint32_t fade_alpha;
};
Ending* g_ending;

void load_message(Ending* ending, const char* name) {
    std::free(ending->file);
    size_t size;
    ending->file = static_cast<uint8_t*>(dat_read(name, &size));
    ending->cursor = ending->file + *reinterpret_cast<const uint32_t*>(ending->file + 4);
    ending->time = ending->wait = 0;
    ending->line = 0;
    ending->color = 0xffffff;
}

AnmVm* create_vm(AnmResource* resource, int script, AnmVm** owner) {
    AnmVm* vm = anm_vm_alloc(resource);
    vm->flags3 |= 0x1000;
    anm_vm_set_script(vm, resource, script);
    anm_queue_add(vm);
    *owner = vm;
    anm_manage_vm(vm, owner);
    return vm;
}

// 4419f0: rolling XOR, with underscores replaced only outside SJIS pairs.
void decode_line(const uint8_t* input, char* output) {
    uint8_t key = 0x77;
    uint8_t step = 7;
    int length = 0;
    do {
        output[length] = static_cast<char>(input[length] ^ key);
        key += step;
        step += 0x10;
    } while (output[length++] != 0);
    for (int i = 0; output[i] != 0; ++i) {
        const uint8_t c = static_cast<uint8_t>(output[i]);
        if ((c >= 0x81 && c <= 0x9f) || (c >= 0xe0 && c <= 0xfc)) ++i;
        else if (c == '_') output[i] = ' ';
    }
}

// 42be70: return true only when the script itself ends.
bool execute_message(Ending* ending) {
    for (;;) {
        const uint8_t* instruction = ending->cursor;
        const int time = *reinterpret_cast<const uint16_t*>(instruction);
        if (static_cast<int>(ending->time) < time) {
            ending->time += stage::g_speed_scale;
            return false;
        }
        const int opcode = instruction[2];
        const uint8_t* payload = instruction + 4;
        const auto* args = reinterpret_cast<const int32_t*>(payload);
        const uint8_t* next = payload + instruction[3];
        switch (opcode) {
        case 0:
            return true;
        case 3: {
            if (ending->line == 0) {
                for (AnmVm* word : ending->words) {
                    text_write_texture(word, 0xffffff, 0, 0, 0, 0, " ");
                    anm_set_label(word, 3);
                }
            }
            char text[256];
            decode_line(payload, text);
            AnmVm* word = ending->words[ending->line];
            text_write_texture(word, ending->color, 0, 0, 0, 0, text);
            anm_set_label(word, 2);
            if (++ending->line == 5) ending->line = 0;
            break;
        }
        case 4:
            for (AnmVm* word : ending->words) anm_set_label(word, 3);
            break;
        case 5:
        case 6: {
            if (static_cast<int>(ending->wait) < 1) ending->wait = static_cast<float>(args[0]);
            ending->wait -= stage::g_speed_scale;
            if (opcode == 5 && args[0] < 0) ending->wait = 999;
            const InputState& input = input_state();
            const bool pressed = (input.pressed & (ACTION_SHOT | ACTION_MENU_OK)) != 0;
            if (!pressed && static_cast<int>(ending->wait) > 0) {
                const bool held_skip = (input.held & 0x200) != 0 ||
                    ((input.held & ACTION_SHOT) != 0 && input.extra[0] >= 20);
                if (ending->first_ending || !held_skip || static_cast<int>(ending->wait) % 6 != 0)
                    return false;
            } else sound_play_se(0);
            ending->wait = 0;
            if (opcode == 6) ending->line = 0;
            break;
        }
        case 7: {
            const int slot = args[0];
            text_loading_start(480, 392);
            if (ending->resources[slot] != nullptr) {
                for (AnmVm* picture : ending->pictures)
                    if (picture != nullptr && picture->anm_table_index == 20 + slot)
                        anm_vm_free(picture);
                anm_release_resource(20 + slot);
            }
            ending->resources[slot] = anm_get(20 + slot, reinterpret_cast<const char*>(payload + 4));
            text_loading_stop();
            ending->cursor = next;
            // 42cb10 resumes the interpreter after the resource-loader step.
            return false;
        }
        case 8:
        case 15:
        case 16:
        case 17:
            if (opcode == 8 || static_cast<int>(session().difficulty) == opcode - 14) {
                anm_vm_free(ending->pictures[args[0]]);
                create_vm(ending->resources[args[1]], args[2], &ending->pictures[args[0]]);
            }
            break;
        case 9:
            ending->color = static_cast<uint32_t>(args[0]);
            break;
        case 10:
            // Original scripts name bgm/th18_15 (ending) or bgm/th18_16 (staff).
            bgm_play(std::strcmp(reinterpret_cast<const char*>(payload), "bgm/th18_15") == 0 ? 13 : 14);
            break;
        case 11:
            bgm_fade(stage::g_speed_scale > 0 && stage::g_speed_scale <= 1 ?
                3.0f / stage::g_speed_scale : 3.0f);
            break;
        case 12: {
            for (AnmVm*& word : ending->words) {
                anm_vm_free(word);
                word = nullptr;
            }
            char name[20];
            std::sprintf(name, "staff%d.msg", static_cast<int>(session().difficulty) + 1);
            load_message(ending, name);
            ending->staff = true;
            continue;
        }
        case 13:
        case 14:
            ending->fading = true;
            ending->fade_out = opcode == 14;
            ending->fade_time = 0;
            ending->fade_duration = args[0];
            ending->fade_alpha = 0;
            break;
        }
        ending->cursor = next;
    }
}

ThreadStatus update(void* context) {
    auto* ending = static_cast<Ending*>(context);
    if (execute_message(ending)) {
        app_context()->next_state = 0x10;
        return THREAD_CONTINUE;
    }
    ++ending->iterations;
    const InputState& input = input_state();
    const bool skipping = (input.held & 0x200) != 0 ||
        ((input.held & ACTION_SHOT) != 0 && input.extra[0] >= 20);
    return ending->staff && !ending->first_staff && skipping && ending->iterations % 12 != 0 ?
        THREAD_RESCAN : THREAD_CONTINUE;
}

ThreadStatus update_fade(void* context) {
    auto* ending = static_cast<Ending*>(context);
    if (!ending->fading) return THREAD_CONTINUE;
    const int duration = ending->fade_duration;
    if (duration != 0) {
        const float progress = ending->fade_time * 255.0f / duration;
        ending->fade_alpha = ending->fade_out ?
            (ending->fade_time >= duration ? 255u : static_cast<uint32_t>(progress)) :
            (ending->fade_time >= duration ? 0u : static_cast<uint32_t>(255.0f - progress));
    }
    if (static_cast<int>(ending->fade_time) >= duration + (ending->fade_out ? 2 : 0))
        ending->fading = false;
    else ending->fade_time += stage::g_speed_scale;
    return THREAD_CONTINUE;
}

ThreadStatus draw_fade(void* context) {
    const auto* ending = static_cast<const Ending*>(context);
    if (!ending->fading) return THREAD_CONTINUE;
    gfx_select_camera(2);
    IDirect3DDevice9* device = gfx_device();
    struct Vertex { float x, y, z, rhw; uint32_t color; };
    const uint32_t color = ending->fade_alpha << 24;
    const Vertex vertices[] = {{0,0,0,1,color}, {640,0,0,1,color},
                              {0,480,0,1,color}, {640,480,0,1,color}};
    device->SetTexture(0, nullptr);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex));
    return THREAD_CONTINUE;
}
} // namespace

void ending_create(int ending_index) {
    auto* ending = new Ending{};
    g_ending = ending;
    uint8_t* state = score_data().state;
    ending->first_ending = state[0x16 + ending_index] == 0;
    ending->first_staff = state[0x22] == 0;
    state[0x16 + ending_index] |= static_cast<uint8_t>(1u << static_cast<int>(session().difficulty));
    state[0x16 + ending_index] |= 1;
    if (ending_index < 8) state[0x22] = 1;
    char name[16];
    std::sprintf(name, "e%02d.msg", ending_index + 1);
    load_message(ending, name);
    AnmResource* text = anm_get(0, "text.anm");
    for (int i = 0; i < 5; ++i) {
        AnmVm* word = create_vm(text, 53 + i, &ending->words[i]);
        auto* raw = reinterpret_cast<uint8_t*>(word);
        raw[0x530] = raw[0x531] = 0x10;
        word->flags3 &= ~0x4000u;
    }
    ending->logic = thread_create(update);
    thread_set_ctx(ending->logic, ending);
    ending->logic->flags |= 2;
    thread_set_priority(ending->logic, 0x24);
    thread_enqueue_logic(ending->logic);
    ending->fade_logic = thread_create(update_fade);
    thread_set_ctx(ending->fade_logic, ending);
    ending->fade_logic->flags |= 2;
    thread_set_priority(ending->fade_logic, 0x14);
    thread_enqueue_logic(ending->fade_logic);
    ending->fade_render = thread_create(draw_fade);
    thread_set_ctx(ending->fade_render, ending);
    thread_set_priority(ending->fade_render, 0x5b);
    thread_enqueue_render(ending->fade_render);
}

void ending_destroy() {
    Ending* ending = g_ending;
    thread_remove(ending->logic);
    thread_remove(ending->fade_logic);
    thread_remove(ending->fade_render);
    for (AnmVm* word : ending->words) anm_vm_free(word);
    for (AnmVm* picture : ending->pictures) anm_vm_free(picture);
    for (int slot = 0; slot < 4; ++slot)
        if (ending->resources[slot] != nullptr) anm_release_resource(20 + slot);
    std::free(ending->file);
    delete ending;
    g_ending = nullptr;
}
