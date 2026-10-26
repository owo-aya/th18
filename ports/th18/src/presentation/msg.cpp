#include "presentation/msg.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <d3d9.h>

#include "core/game_session.h"
#include "core/input.h"
#include "platform/gfx.h"
#include "presentation/anm.h"
#include "presentation/sound.h"
#include "presentation/text.h"
#include "resources/msg_file.h"
#include "stage/ecl_format.h"
#include "stage/enemy.h"
#include "stage/stage.h"

namespace {

struct Character {
    int animation;
    int portrait;
    int name_animation;
    int name;
};

// 004c9410 + stage*0xd4: 0043e550 uses +0x48/+0x4c for the name,
// +0x50/+0x54 for the portrait, and a 0x28-byte character stride.
constexpr Character kCharacters[7][4] = {
    {{3,9,3,15}, {-1,-1,-1,-1}, {0,0,0,0}, {0,0,0,0}},
    {{3,10,3,15}, {-1,-1,-1,-1}, {0,0,0,0}, {0,0,0,0}},
    {{3,9,3,14}, {-1,-1,-1,-1}, {0,0,0,0}, {0,0,0,0}},
    {{3,8,3,16}, {-1,-1,-1,-1}, {0,0,0,0}, {0,0,0,0}},
    {{3,8,3,15}, {4,8,4,9}, {0,0,0,0}, {0,0,0,0}},
    {{3,12,3,18}, {4,8,4,9}, {3,12,3,18}, {0,0,0,0}},
    {{3,9,3,16}, {4,8,-1,-1}, {5,11,5,16}, {5,8,5,16}},
};
constexpr int kPlayerPortrait[4] = {40, 46, 29, 28};

const uint8_t* g_file;
AnmResource* g_logo;
AnmResource* g_front;
AnmResource* g_text;
const MsgInstr* g_cursor;
AnmVm* g_left[4];
AnmVm* g_right[4];
AnmVm* g_words[4];
AnmVm* g_name;
AnmVm* g_bubble;
std::vector<AnmVm*> g_roots;
std::vector<AnmVm*> g_effects;
bool g_active;
bool g_paused;
bool g_skip_allowed;
bool g_skip_armed;
bool g_alt_font;
bool g_page_started;
int g_skip_delay;
float g_wait;
int g_release;
int g_line;
int g_side;
int g_style;
int g_bubble_script;
float g_time;
float g_x;
float g_y;
float g_width;
float g_fade_seconds;
uint32_t g_events;

int integer(const MsgInstr* instruction, int index = 0)
{
    int value;
    std::memcpy(&value, reinterpret_cast<const uint8_t*>(instruction + 1) + index * 4, 4);
    return value;
}

float real(const MsgInstr* instruction, int index = 0)
{
    float value;
    std::memcpy(&value, reinterpret_cast<const uint8_t*>(instruction + 1) + index * 4, 4);
    return value;
}

float& vm_float(AnmVm* vm, int offset)
{
    return *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(vm) + offset);
}

void label(AnmVm* vm, int id)
{
    if (vm != nullptr)
        anm_set_label(vm, id);
}

// 00488c60 executes the label immediately; 00488be0 only queues it.
void interrupt(AnmVm* vm, int id)
{
    if (vm != nullptr)
        anm_interrupt(vm, id);
}

AnmVm* create_vm(AnmResource* resource, int script)
{
    AnmVm* vm = anm_vm_alloc(resource);
    anm_vm_set_script(vm, resource, script);
    anm_queue_add(vm);
    g_roots.push_back(vm);
    return vm;
}

void create_effect(AnmResource* resource, int script)
{
    AnmVm* vm = anm_vm_alloc(resource);
    anm_vm_set_script(vm, resource, script);
    anm_queue_add(vm);
    g_effects.push_back(vm);
}

AnmResource* character_resource(int original_index)
{
    // Enemy ANM slots 2 onward are the ECL ANIM list in loading order.
    auto* resource = static_cast<ecl::EclResource*>(stage::g_enemy_manager->ecl_resource);
    return resource->animations[original_index - 2];
}

void destroy_scene()
{
    for (AnmVm* word : g_words) {
        if (word != nullptr && word->texture != nullptr)
            static_cast<IDirect3DTexture9*>(const_cast<void*>(word->texture))->Release();
    }
    for (AnmVm* vm : g_roots)
        anm_vm_free(vm);
    g_roots.clear();
    std::memset(g_left, 0, sizeof(g_left));
    std::memset(g_right, 0, sizeof(g_right));
    std::memset(g_words, 0, sizeof(g_words));
    g_bubble = g_name = nullptr;
    g_active = false;
}

void remove_bubble()
{
    if (g_bubble != nullptr) {
        auto it = std::find(g_roots.begin(), g_roots.end(), g_bubble);
        g_roots.erase(it);
        anm_vm_free(g_bubble);
        g_bubble = nullptr;
    }
}

void decode_text(const MsgInstr* instruction, char* output)
{
    const uint8_t* source = reinterpret_cast<const uint8_t*>(instruction + 1);
    uint8_t key = 0x77;
    uint8_t step = 7;
    int i = 0;
    do {
        output[i] = static_cast<char>(source[i] ^ key);
        key += step;
        step += 0x10;
    } while (output[i++] != 0);
    for (auto* p = reinterpret_cast<uint8_t*>(output); *p != 0; ++p) {
        if ((*p >= 0x81 && *p <= 0x9f) || (*p >= 0xe0 && *p <= 0xfc))
            ++p;
        else if (*p == '_')
            *p = ' ';
    }
}

void position_words()
{
    if (g_bubble == nullptr)
        return;
    AnmVm* child = anm_find_child(g_bubble, g_bubble_script + 0xaa);
    if (child == nullptr)
        return;
    float x = child->pos_x + child->extra_x + vm_float(child, 0x4e4);
    float y = child->pos_y + child->extra_y + vm_float(child, 0x4e8);
    for (auto* parent = static_cast<AnmVm*>(child->parent); parent != nullptr;
         parent = static_cast<AnmVm*>(parent->parent)) {
        x += parent->pos_x + parent->extra_x + vm_float(parent, 0x4e4);
        y += parent->pos_y + parent->extra_y + vm_float(parent, 0x4e8);
    }
    // 00441130 is the text VM's pre-draw callback.
    if (g_side == 0)
        x -= 36.0f;
    else if (g_side == 1)
        x += child->scale_x >= 1.0f ? 24.0f : (child->scale_x + 0.125f) * 32.0f - 8.0f;
    else if (g_side == 2)
        x += (child->scale_x + 0.125f) * 16.0f - 8.0f;
    for (AnmVm* word : g_words) {
        word->extra_x = x;
        word->extra_y = y;
    }
}

void show_bubble(int line)
{
    remove_bubble();
    g_bubble_script = (g_style + line * 8) * 3 + g_side;
    g_bubble = create_vm(g_front, g_bubble_script + 0x112);
    g_bubble->extra_x = g_x;
    g_bubble->extra_y = g_y;
    // 004416d0/00441900 write the width into every direct child (float var 10004).
    for(int queue=0;queue<2;++queue)
    for (AnmVm* vm = static_cast<AnmVm*>(queue==0?anm_manager()->queue_head:anm_manager()->queue_head2); vm != nullptr;
         vm = static_cast<AnmVm*>(vm->node_next)) {
        if (vm->parent == g_bubble)
            vm_float(vm, 0x4b4) = g_width + 16.0f;
    }
}

void reset_page(int side)
{
    g_side = side;
    g_line = 0;
    g_page_started = false;
    g_alt_font = false;
    for (AnmVm* word : g_words)
        vm_float(word, 0x4e8) = 0.0f;
}

void show_text(const MsgInstr* instruction)
{
    if (!g_page_started && g_line == 0) {
        g_width = 0.0f;
        for (int i = 0; i < 4; ++i) {
            text_write_texture(g_words[i], 0, 0, g_alt_font ? 1 : 0, 0, i >= 2 ? 1 : 0, "  ");
            label(g_words[i], 3);
        }
        g_page_started = true;
    }
    char text[256];
    decode_text(instruction, text);
    if (text[0] == '|') {
        // Original MSG ruby: |horizontal offset,character spacing,text.
        const int x = std::atoi(text + 1);
        char* comma = std::strchr(text + 1, ',');
        const int spacing = std::atoi(comma + 1);
        comma = std::strchr(comma + 1, ',');
        AnmVm* word = g_words[g_line + 2];
        word->flags3 |= 0x4000;
        text_write_texture(word, 0, 0xa0a0a0, 2, x, spacing, comma + 1);
        interrupt(word, 2);
    } else {
        const int pixels = (static_cast<int>(std::strlen(text)) * 8 & ~15) - 28;
        g_width = std::max(g_width, static_cast<float>(pixels * 2));
        show_bubble(g_line);
        AnmVm* word = g_words[g_line];
        text_write_texture(word, 0, 0, g_alt_font ? 1 : 0, 0, 0, text);
        interrupt(word, 2);
        if (g_line == 0)
            g_line = 1;
        else {
            g_line = 0;
            g_page_started = false;
        }
    }
    for (AnmVm* word : g_words) {
        word->extra_x = g_x;
        word->extra_y = g_y;
    }
}

void fade_music(float seconds)
{
    g_fade_seconds = seconds;
    if (stage::g_speed_scale != 0.0f && stage::g_speed_scale <= 1.0f)
        g_fade_seconds /= stage::g_speed_scale;
    g_events |= MSG_BGM_FADE;
}

} // namespace

void msg_load(const void* file_data, AnmResource* logo)
{
    g_file = static_cast<const uint8_t*>(file_data);
    g_logo = logo;
    g_front = anm_get(5, "front.anm");
    g_text = anm_get(0, "text.anm");
    g_paused = false;
    g_events = 0;
}

void msg_start(int entry)
{
    // Negative 0043e360 entries are stage/boss music and stage completion.
    if (entry == -1 || entry == -3) {
        const int boss = entry == -1 ? 1 : 0;
        bgm_play((session().stage == 7 ? 15 : (session().stage - 1) * 2 + 1) + boss);
        create_effect(g_logo, boss + 1);
        return;
    }
    if (entry == -2) {
        g_events |= MSG_STAGE_END;
        return;
    }
    destroy_scene();
    const auto* entries = reinterpret_cast<const MsgEntry*>(g_file + 4);
    g_cursor = reinterpret_cast<const MsgInstr*>(g_file + entries[entry].offset);
    g_active = true;
    g_paused = g_skip_allowed = g_skip_armed = g_alt_font = g_page_started = false;
    g_skip_delay = g_release = g_line = g_side = g_style = 0;
    g_wait = 0.0f;
    g_time = 0.0f;
    g_x = 384.0f;
    g_y = 640.0f;
    g_width = 320.0f;
    for (int i = 0; i < 4; ++i) {
        g_words[i] = create_vm(g_text, i < 2 ? 0 : 1);
        g_words[i]->flags3 |= 0x4000;
        if ((i & 1) != 0)
            anm_interrupt(g_words[i], 7);
        // text.anm's four dialogue sprites are 1024x40 writable surfaces.
        IDirect3DTexture9* texture;
        gfx_device()->CreateTexture(1024, 40, 1, 0, D3DFMT_A4R4G4B4,
                                    D3DPOOL_MANAGED, &texture, nullptr);
        g_words[i]->texture = texture;
        g_words[i]->uv[0] = g_words[i]->uv[1] = 0.0f;
        g_words[i]->uv[2] = g_words[i]->uv[3] = 1.0f;
    }
}

void msg_unload()
{
    destroy_scene();
    for (AnmVm* vm : g_effects)
        anm_vm_free(vm);
    g_effects.clear();
    g_file = nullptr;
    g_events = 0;
}

bool msg_active() { return g_active; }
bool msg_wait_released() { return !g_active || g_release != 0; }
float msg_bgm_fade_seconds() { return g_fade_seconds; }

uint32_t msg_take_events()
{
    const uint32_t events = g_events;
    g_events = 0;
    return events;
}

void msg_set_paused(bool paused)
{
    g_paused = paused;
    g_skip_armed = false;
    g_skip_delay = 2;
    for(int queue=0;queue<2;++queue)
    for (AnmVm* vm = static_cast<AnmVm*>(queue==0?anm_manager()->queue_head:anm_manager()->queue_head2); vm != nullptr;
         vm = static_cast<AnmVm*>(vm->node_next)) {
        AnmVm* root = vm;
        while (root->parent != nullptr)
            root = static_cast<AnmVm*>(root->parent);
        if (std::find(g_roots.begin(), g_roots.end(), root) != g_roots.end()) {
            if (paused)
                vm->flags2 &= ~2u;
            else
                vm->flags2 |= 2u;
        }
    }
}

void msg_update()
{
    if (g_paused)
        return;
    // A stopped parent can still own animated children and receive interrupts.
    for (AnmVm* vm : g_roots)
        anm_vm_update(vm);
    for (AnmVm* vm : g_effects)
        anm_vm_update(vm);
    if (!g_active)
        return;
    if (g_release > 0)
        --g_release;
    const InputState& input = input_battle_state();
    if (g_skip_delay == 0) {
        if ((input.pressed & 0x201) != 0)
            g_skip_armed = true;
    } else
        --g_skip_delay;
    const bool skipping = g_skip_allowed && g_skip_armed &&
        (((input.held & 0x200) != 0 && input.extra[9] >= 20) ||
         ((input.held & ACTION_SHOT) != 0 && input.extra[0] >= 20));
    if (skipping)
        g_time = static_cast<float>(g_cursor->time);
    bool advance_time = true;
    while (g_cursor->time <= static_cast<int>(g_time)) {
        const MsgInstr* instruction = g_cursor;
        const auto* next = reinterpret_cast<const MsgInstr*>(
            reinterpret_cast<const uint8_t*>(instruction + 1) + instruction->length);
        switch (instruction->type) {
        case MSG_END:
            destroy_scene();
            g_events |= MSG_FINISHED;
            return;
        case MSG_LEFT_CREATE:
        case MSG_RIGHT_CREATE: {
            const int slot = integer(instruction);
            const Character& character = kCharacters[session().stage - 1][slot];
            const bool player = instruction->type == MSG_LEFT_CREATE && slot == 0;
            AnmResource* resource = player ? anm_manager()->resources[40 + session().player] :
                character_resource(character.animation);
            AnmVm* vm = create_vm(resource, player ? kPlayerPortrait[session().player] : character.portrait);
            (instruction->type == MSG_LEFT_CREATE ? g_left : g_right)[slot] = vm;
            break;
        }
        case MSG_LEFT_REMOVE:
        case MSG_RIGHT_REMOVE: {
            AnmVm*& vm = (instruction->type == MSG_LEFT_REMOVE ? g_left : g_right)[integer(instruction)];
            label(vm, 1);
            vm = nullptr;
            if (instruction->type == MSG_RIGHT_REMOVE)
                label(g_name, 1);
            break;
        }
        case MSG_TEXT_REMOVE:
            for (AnmVm* word : g_words) label(word, 1);
            remove_bubble();
            break;
        case MSG_LEFT_SPEAK:
        case MSG_RIGHT_SPEAK:
        case MSG_CENTER_SPEAK: {
            const int side = instruction->type == MSG_LEFT_SPEAK ? 0 :
                             instruction->type == MSG_RIGHT_SPEAK ? 1 : 2;
            const int slot = side == 2 ? -1 : integer(instruction);
            for (int i = 0; i < 4; ++i) {
                interrupt(g_left[i], side == 0 && slot == i ? 2 : 3);
                interrupt(g_right[i], side == 1 && slot == i ? 2 : 3);
            }
            reset_page(side);
            break;
        }
        case MSG_ALLOW_SKIP:
            g_skip_allowed = (integer(instruction) & 1) != 0;
            break;
        case MSG_WAIT:
            if (static_cast<int>(g_wait) < 1)
                g_wait = static_cast<float>(integer(instruction));
            g_wait -= stage::g_speed_scale;
            if ((input.pressed & (ACTION_SHOT | ACTION_MENU_OK)) != 0 || static_cast<int>(g_wait) <= 0)
                sound_play_se(0);
            else if (!skipping) {
                advance_time = false;
                goto done;
            }
            g_wait = 0.0f;
            g_line = 0;
            g_page_started = false;
            break;
        case MSG_ECL_SIGNAL:
            g_release = 1;
            break;
        case MSG_LEFT_FACE:
        case MSG_RIGHT_FACE:
            interrupt((instruction->type == MSG_LEFT_FACE ? g_left : g_right)[integer(instruction, 1)],
                      integer(instruction) + 17);
            break;
        case MSG_FIRST_LINE:
        case MSG_SECOND_LINE: {
            char text[256];
            decode_text(instruction, text);
            AnmVm* word = g_words[instruction->type == MSG_FIRST_LINE ? 0 : 1];
            text_write_texture(word, 0, 0, (g_alt_font ? 1 : 0) + 4, 0, 0, text);
            label(word, 2);
            break;
        }
        case MSG_TEXT:
            show_text(instruction);
            break;
        case MSG_TEXT_HIDE:
            remove_bubble();
            for (AnmVm* word : g_words) label(word, 3);
            break;
        case MSG_BOSS_MUSIC:
            bgm_play(session().stage == 7 ? 16 : session().stage * 2);
            create_effect(g_logo, 2);
            break;
        case MSG_BOSS_NAME: {
            const Character& character = kCharacters[session().stage - 1][integer(instruction)];
            g_name = create_vm(character_resource(character.name_animation), character.name);
            break;
        }
        case MSG_STAGE_FINISH:
            g_events |= MSG_STAGE_END;
            break;
        case MSG_MUSIC_FADE:
            fade_music(session().stage == 6 ? 2.0f : 8.0f);
            break;
        case MSG_LEFT_EXIT:
        case MSG_RIGHT_EXIT: {
            AnmVm** portraits = instruction->type == MSG_LEFT_EXIT ? g_left : g_right;
            label(portraits[0], 7);
            label(portraits[1], 7);
            break;
        }
        case MSG_TEXT_Y:
            for (AnmVm* word : g_words) vm_float(word, 0x4e8) = static_cast<float>(integer(instruction));
            break;
        case MSG_ALT_FONT:
            g_alt_font = true;
            break;
        case MSG_MUSIC_FADE_TIME:
            fade_music(real(instruction));
            break;
        case MSG_TEXT_POSITION:
            g_x = real(instruction) * 2.0f;
            g_y = real(instruction, 1) * 2.0f;
            break;
        case MSG_BUBBLE_STYLE:
            g_style = integer(instruction) & 15;
            break;
        case MSG_SECOND_BOSS_CREATE: {
            const Character& character = kCharacters[session().stage - 1][1];
            g_right[1] = create_vm(character_resource(character.animation), character.portrait);
            break;
        }
        case MSG_SPEAKER:
            reset_page(integer(instruction));
            break;
        case MSG_PORTRAIT_DIM:
        case MSG_PORTRAIT_LIGHT:
            interrupt((integer(instruction) == 0 ? g_left : g_right)[integer(instruction, 1)],
                      instruction->type == MSG_PORTRAIT_DIM ? 3 : 2);
            break;
        case MSG_STAGE_OVERLAY:
            create_effect(g_front, 0x30);
            break;
        case MSG_OPEN_SHOP:
            g_events |= MSG_SHOP;
            g_cursor = next;
            return;
        }
        g_cursor = next;
    }
done:
    if (advance_time)
        g_time += stage::g_speed_scale;
    position_words();
}

