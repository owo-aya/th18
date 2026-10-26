#include "presentation/trophy.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cards/card_info.h"
#include "cards/card_profile.h"
#include "core/thread.h"
#include "presentation/anm.h"
#include "presentation/sound.h"
#include "presentation/text.h"
#include "resources/dat.h"

namespace {
struct Trophy {
    uint32_t pending[60];
    int count;
    int state;
    int timer;
    bool deferred_allowed;
    AnmResource* resource;
    AnmVm* animation;
    AnmVm* name;
    AnmVm* message;
    Thread* logic;
    Thread* render;
};
Trophy* g_trophy;
Trophy* g_achievement;

AnmVm* create_vm(AnmResource* resource, int script) {
    AnmVm* vm = anm_vm_alloc(resource);
    vm->flags3 |= 0x1000;
    vm->node_flags = 4;
    anm_vm_set_script(vm, resource, script);
    vm->flags3 &= ~0x30000u;
    anm_queue_add(vm);
    return vm;
}

void clear_animation(Trophy* trophy) {
    anm_vm_free(trophy->animation);
    anm_vm_free(trophy->name);
    anm_vm_free(trophy->message);
    trophy->animation = trophy->name = trophy->message = nullptr;
}

void destroy_queue(Trophy*& trophy) {
    clear_animation(trophy);
    thread_remove(trophy->logic);
    thread_remove(trophy->render);
    delete trophy;
    trophy = nullptr;
}

void achievement_name(int id, char* name) {
    // 0046e230: @decimal-id is followed by the original SJIS title line.
    size_t size;
    char* data = static_cast<char*>(dat_read("trophy.txt", &size));
    char* cursor = data;
    char* end = data + size;
    while (cursor < end && *cursor != '\\') {
        char* line = cursor;
        while (cursor < end && *cursor != '\r' && *cursor != '\n' && *cursor != '\f') ++cursor;
        while (cursor < end && (*cursor == '\r' || *cursor == '\n' || *cursor == '\f')) ++cursor;
        if (*line == '@' && std::strtol(line + 1, nullptr, 10) == id) {
            line = cursor;
            while (*cursor != '\r' && *cursor != '\n') ++cursor;
            std::memcpy(name, line, cursor - line);
            name[cursor - line] = 0;
            break;
        }
    }
    std::free(data);
}

// 00419350 is an empty B callback; its VMs use the ordinary ANM queues.
ThreadStatus render(void*) { return THREAD_CONTINUE; }

ThreadStatus update(void*) {
    Trophy* trophy = g_trophy;
    if (trophy->state == 2) {
        destroy_queue(g_trophy);
        return THREAD_CONTINUE;
    }
    if (trophy->state == 0 && trophy->count != 0) {
        if ((trophy->pending[0] & 0xff00u) != 0 && !trophy->deferred_allowed)
            return THREAD_CONTINUE;
        clear_animation(trophy);
        const uint32_t entry = trophy->pending[0];
        --trophy->count;
        std::memmove(trophy->pending, trophy->pending + 1, trophy->count * sizeof(uint32_t));
        trophy->state = 1;
        trophy->timer = 0;
        trophy->animation = create_vm(trophy->resource, (entry & 0xff00u) != 0 ? 2 : 1);
        trophy->name = create_vm(trophy->resource, 10);
        trophy->message = create_vm(trophy->resource, 11);
        const CardId id = static_cast<CardId>(entry & 0xffu);
        char title[80];
        std::snprintf(title, sizeof(title), "\x81\x77%s\x81\x78", card_name(id));
        text_write_texture_centered(trophy->name, 0x80c0c0, 0, 9, 0, title);
        text_write_texture_centered(trophy->message, 0x80c0c0, 0, 9, 0,
                                   "\x82\xf0\x8e\xe8\x82\xc9\x93\xfc\x82\xea\x82\xbd\x81\x49");
        AnmResource* cards = anm_get(0x1f, "abcard.anm");
        AnmVm* card = anm_find_child(trophy->animation, 3);
        anm_set_sprite(card, cards, card_info_by_id(id)->text_name);
        card->anm_table_index = cards->index;
        sound_play_se(0x52);
    } else {
        // The root ends at frame 45; its banner/card children finish at 180.
        // 00419170 keeps this notification until its 240-frame queue interval.
        if (trophy->animation != nullptr) anm_vm_update(trophy->animation);
        if (trophy->name != nullptr) anm_vm_update(trophy->name);
        if (trophy->message != nullptr) anm_vm_update(trophy->message);
    }
    if (trophy->timer > 0xef) {
        trophy->state = trophy->count == 0 ? 2 : 0;
        trophy->timer = 0;
    }
    ++trophy->timer;
    return THREAD_CONTINUE;
}

ThreadStatus update_achievement(void*) {
    Trophy* trophy = g_achievement;
    if (trophy->state == 2) {
        destroy_queue(g_achievement);
        return THREAD_CONTINUE;
    }
    if (trophy->state == 0 && trophy->count != 0) {
        clear_animation(trophy);
        const int id = trophy->pending[0];
        --trophy->count;
        std::memmove(trophy->pending, trophy->pending + 1, trophy->count * sizeof(uint32_t));
        trophy->state = 1;
        trophy->timer = 0;
        trophy->animation = create_vm(trophy->resource, 0);
        trophy->name = create_vm(trophy->resource, 7);
        trophy->message = create_vm(trophy->resource, 8);
        char name[256], title[264];
        achievement_name(id, name);
        std::snprintf(title, sizeof(title), "\x81\x77%s\x81\x78", name);
        text_write_texture_centered(trophy->name, 0x80c0c0, 0, 9, 0, title);
        text_write_texture_centered(trophy->message, 0x80c0c0, 0, 9, 0,
                                   "\x82\xcc\x8e\xc0\x90\xd1\x82\xf0\x8e\xe8\x82\xc9\x93\xfc\x82\xea\x82\xbd");
        sound_play_se(0x4f);
    } else {
        if (trophy->animation != nullptr) anm_vm_update(trophy->animation);
        if (trophy->name != nullptr) anm_vm_update(trophy->name);
        if (trophy->message != nullptr) anm_vm_update(trophy->message);
    }
    if (trophy->timer > 0xef) {
        trophy->state = trophy->count == 0 ? 2 : 0;
        trophy->timer = 0;
    }
    ++trophy->timer;
    return THREAD_CONTINUE;
}
} // namespace

void trophy_notify(CardId id, int kind) {
    if (g_trophy == nullptr) {
        g_trophy = new Trophy{};
        g_trophy->resource = anm_get(0x1b, "trophy.anm");
        g_trophy->logic = thread_create(update);
        thread_set_priority(g_trophy->logic, 0xe);
        thread_set_paused(g_trophy->logic, true);
        thread_enqueue_logic(g_trophy->logic);
        g_trophy->render = thread_create(render);
        thread_set_priority(g_trophy->render, 0x4f);
        thread_set_paused(g_trophy->render, true);
        thread_enqueue_render(g_trophy->render);
    }
    if (g_trophy->count < 60)
        g_trophy->pending[g_trophy->count++] = static_cast<uint32_t>(kind << 8) | id;
}

void trophy_allow_deferred() {
    if (g_trophy != nullptr) g_trophy->deferred_allowed = true;
}

void trophy_achievement(int id) {
    if (g_achievement == nullptr) {
        g_achievement = new Trophy{};
        g_achievement->resource = anm_get(0x1b, "trophy.anm");
        g_achievement->logic = thread_create(update_achievement);
        thread_set_priority(g_achievement->logic, 0xe);
        thread_set_paused(g_achievement->logic, true);
        thread_enqueue_logic(g_achievement->logic);
        g_achievement->render = thread_create(render);
        thread_set_priority(g_achievement->render, 0x4f);
        thread_set_paused(g_achievement->render, true);
        thread_enqueue_render(g_achievement->render);
    }
    if (g_achievement->count < 60)
        g_achievement->pending[g_achievement->count++] = id;
}

void trophy_shutdown() {
    if (g_trophy != nullptr) destroy_queue(g_trophy);
    if (g_achievement != nullptr) destroy_queue(g_achievement);
}
