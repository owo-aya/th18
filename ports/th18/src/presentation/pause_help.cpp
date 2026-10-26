#include "presentation/pause_pages.h"

#include <cstdio>
#include <cstdlib>

#include "core/input.h"
#include "presentation/anm.h"
#include "presentation/sound.h"
#include "resources/dat.h"
#include "stage/stage.h"

struct PauseHelp {
    AnmResource* resource;
    AnmVm* choices[7]{};
    AnmVm* image = nullptr;
    void* image_data = nullptr;
    size_t image_size = 0;
    int state = 0;
    int page_state = 0;
    int selection = 0;
    float time = -1.0f;
};

namespace {
AnmVm* create_vm(PauseHelp* page, int script, AnmVm** slot) {
    // Returning from a picture can precede the old directory's staggered exit.
    if (*slot != nullptr) anm_manage_vm(*slot, nullptr);
    AnmVm* vm = anm_vm_alloc(page->resource);
    anm_vm_bind_script(vm, page->resource, script);
    vm->flags3 |= 0x1000u;
    vm->extra_x = 32.0f;
    anm_vm_update(vm);
    vm->node_flags = 4;
    anm_queue_add(vm);
    anm_manage_vm(vm, slot);
    return vm;
}

void show_choices(PauseHelp* page) {
    for (int index = 0; index < 7; ++index) {
        create_vm(page, index, &page->choices[index]);
        anm_interrupt(page->choices[index], index == page->selection ? 2 : 3);
    }
}

void leave_choices(PauseHelp* page) {
    for (AnmVm* vm : page->choices)
        if (vm != nullptr) anm_set_label(vm, 1);
}

void open_image(PauseHelp* page) {
    page->page_state = 2;
    page->time = 0.0f;
    char name[16];
    std::snprintf(name, sizeof(name), "help_%02d.png", page->selection + 1);
    page->image_data = file_read(name, &page->image_size, false);
    leave_choices(page);
}
} // namespace

PauseHelp* pause_help_create() {
    auto* page = new PauseHelp;
    page->resource = anm_get(0x13, "help.anm");
    return page;
}

void pause_help_destroy(PauseHelp* page) {
    for (AnmVm* vm : page->choices)
        if (vm != nullptr) anm_vm_free(vm);
    if (page->image != nullptr) anm_vm_free(page->image);
    std::free(page->image_data);
    anm_release_resource(0x13);
    delete page;
}

// 004451c0: directory, image opening, image browsing and twenty-frame page turns.
bool pause_help_update(PauseHelp* page) {
    const int timer = static_cast<int>(page->time);
    if (page->state == 0) {
        page->state = 1;
    } else if (page->state == 2) {
        if (timer > 29) return true;
    } else {
        const FrameInput input = input_frame();
        if (page->page_state == 0) {
            page->selection = 0;
            show_choices(page);
            page->page_state = 1;
        }
        if (page->page_state == 1 && timer > 19) {
            const uint32_t movement = input.pressed | input.repeat;
            const int previous = page->selection;
            if (movement & ACTION_UP) page->selection = (page->selection + 6) % 7;
            if (movement & ACTION_DOWN) page->selection = (page->selection + 1) % 7;
            if (previous != page->selection) {
                sound_play_se(10);
                for (int index = 0; index < 7; ++index)
                    anm_interrupt(page->choices[index], index == page->selection ? 2 : 3);
            }
            if (input.pressed & (ACTION_SHOT | ACTION_MENU_OK)) {
                sound_play_se(7);
                open_image(page);
            } else if (input.pressed & (ACTION_BOMB | ACTION_MENU_CANCEL)) {
                sound_play_se(9);
                leave_choices(page);
                page->state = 2;
                page->page_state = 0;
                page->time = 0.0f;
            }
        } else if (page->page_state == 2) {
            anm_load_image(page->resource, 1, page->image_data, page->image_size);
            std::free(page->image_data);
            page->image_data = nullptr;
            create_vm(page, 9, &page->image);
            page->page_state = 4;
            page->time = 0.0f;
        } else if (page->page_state == 4 && timer >= 20) {
            if ((input.pressed & ACTION_DOWN) && page->selection < 6) {
                ++page->selection;
                sound_play_se(7);
                page->page_state = 5;
                page->time = 0.0f;
                anm_interrupt(page->image, 7);
            } else if ((input.pressed & ACTION_UP) && page->selection > 0) {
                --page->selection;
                sound_play_se(7);
                page->page_state = 5;
                page->time = 0.0f;
                anm_interrupt(page->image, 8);
            } else if (input.pressed & (ACTION_SHOT | ACTION_BOMB | ACTION_MENU_OK | ACTION_MENU_CANCEL)) {
                sound_play_se(9);
                page->page_state = 1;
                page->time = 0.0f;
                anm_set_label(page->image, 1);
                show_choices(page);
            }
        } else if (page->page_state == 5 && timer >= 20) {
            open_image(page);
        }
    }
    page->time += stage::g_speed_scale;
    return false;
}
