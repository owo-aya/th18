#include "presentation/music_room.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/input.h"
#include "gameplay/score_data.h"
#include "presentation/anm.h"
#include "presentation/sound.h"
#include "presentation/text.h"
#include "resources/dat.h"

struct MusicTrack {
    char filename[64];
    char title[66];
    char comments[8][66];
    AnmVm* row;
};

struct MusicRoom {
    MusicTrack tracks[32]{};
    int track_count = 0;
    AnmVm* heading = nullptr;
    AnmVm* ambient = nullptr;
    AnmVm* comments[8]{};
    int state = 0;
    int timer = 1;
    int selection = 0;
    int first_visible = 0;
    int comment_track = 0;
    int comment_line = 0;
    bool unheard_warning = false;
};

namespace {
// Original 004b7a78 strings, in the resource text's Shift-JIS encoding.
constexpr const char* kUnheardComments[] = {
    "\x81\x40",
    "\x81\x40\x81\x40\x81\x96\x81\x96\x91\x49\x91\xf0\x82\xb5\x82\xbd\x8b\xc8\x82\xcd\x82\xdc\x82\xbe\x83\x51\x81\x5b\x83\x80\x92\x86\x82\xc5\x8d\xc4\x90\xb6\x82\xb3\x82\xea\x82\xc4\x82\xa2\x82\xdc\x82\xb9\x82\xf1\x81\x96\x81\x96",
    " ",
    "\x81\x40\x81\x40\x81\x40\x81\x40\x8b\xc8\x82\xcc\x83\x52\x83\x81\x83\x93\x83\x67\x82\xaa\x83\x6c\x83\x5e\x83\x6f\x83\x8c\x82\xc9\x82\xc8\x82\xe9\x8b\xb0\x82\xea\x82\xaa\x82\xa0\x82\xe8\x82\xdc\x82\xb7\x81\x42",
    "\x81\x40\x81\x40\x81\x40\x81\x40\x81\x40\x81\x40\x81\x40\x81\x40\x81\x40\x82\xbb\x82\xea\x82\xc5\x82\xe0\x8d\xc4\x90\xb6\x82\xb5\x82\xdc\x82\xb7\x82\xa9\x81\x48",
    "\x81\x40",
    "\x81\x40\x81\x40\x81\x40\x8d\xc4\x90\xb6\x82\xb5\x82\xbd\x82\xa2\x95\xfb\x82\xcd\x81\x41\x82\xe0\x82\xa4\x88\xea\x93\x78\x8c\x88\x92\xe8\x83\x7b\x83\x5e\x83\x93\x82\xf0\x89\x9f\x82\xb5\x82\xc4\x82\xad\x82\xbe\x82\xb3\x82\xa2\x81\x42",
    "\x81\x40\x81\x40\x81\x40\x8d\xc4\x90\xb6\x82\xb5\x82\xbd\x82\xad\x82\xc8\x82\xa2\x95\xfb\x82\xcd\x81\x41\x83\x4a\x81\x5b\x83\x5c\x83\x8b\x82\xf0\x88\xda\x93\xae\x82\xb5\x82\xc4\x82\xad\x82\xbe\x82\xb3\x82\xa2\x81\x42",
};

// 0046bc30: lines are separated by CR/LF runs; a full-width space is a line.
void read_line(char*& cursor, const char* end, char* destination) {
    char* first = cursor;
    while (cursor < end && *cursor != '\r' && *cursor != '\n') ++cursor;
    const size_t length = cursor - first;
    std::memcpy(destination, first, length);
    destination[length] = '\0';
    while (cursor < end && (*cursor == '\r' || *cursor == '\n')) ++cursor;
}

void read_tracks(MusicRoom* room) {
    size_t size;
    char* data = static_cast<char*>(file_read("musiccmt.txt", &size, false));
    char* cursor = data;
    const char* end = data + size;
    while (cursor < end) {
        if (*cursor == '@') {
            MusicTrack& track = room->tracks[room->track_count++];
            ++cursor;
            read_line(cursor, end, track.filename);
            read_line(cursor, end, track.title);
            for (char (&line)[66] : track.comments) read_line(cursor, end, line);
        } else {
            while (cursor < end && *cursor != '\r' && *cursor != '\n') ++cursor;
            while (cursor < end && (*cursor == '\r' || *cursor == '\n')) ++cursor;
        }
    }
    std::free(data);
}

void create_vm(AnmResource* resource, int script, AnmVm** slot) {
    AnmVm* vm = anm_vm_alloc(resource);
    vm->flags3 |= 0x1000u;
    anm_vm_set_script(vm, resource, script);
    anm_queue_add(vm);
    anm_manage_vm(vm, slot);
}

bool heard(int track) {
    // Original Score+5f4de, relative to its ST chunk at +5f4b8.
    return score_data().state[0x26 + track] != 0;
}

void position_row(MusicRoom* room, int index, bool entering) {
    AnmVm* vm = room->tracks[index].row;
    if (index < room->first_visible || index >= room->first_visible + 10)
        vm->flags2 &= ~2u;
    else
        vm->flags2 |= 2u;

    const float x = index == room->selection ? 120.0f : 128.0f;
    const float y = 192.0f + (index - room->first_visible) * 40.0f;
    if (!entering && std::fabs(vm->pos_y - y) >= 80.0f) {
        vm->pos_x = x;
        vm->pos_y = y;
        vm->pos_z = 0;
    } else {
        // 00429ad0: four-frame linear position interpolation in the ANM VM.
        auto* block = reinterpret_cast<uint8_t*>(vm) + 0x90;
        std::memset(block, 0, 0x58);
        auto* values = reinterpret_cast<float*>(block);
        auto* fields = reinterpret_cast<int32_t*>(block);
        std::memcpy(values, &vm->pos_x, 12);
        values[3] = x;
        values[4] = y;
        fields[15] = -1;
        fields[19] = 1;
        fields[20] = 4;
    }
    anm_set_label(vm, index == room->selection ? 2 : 3);
}

void create_row(MusicRoom* room, int index) {
    MusicTrack& track = room->tracks[index];
    create_vm(anm_get(0x10, "title.anm"), 78 + index, &track.row);
    char hidden[66];
    std::sprintf(hidden, "No.%2d  \x81\x48\x81\x48\x81\x48\x81\x48\x81\x48\x81\x48\x81\x48\x81\x48\x81\x48\x81\x48\x81\x48", index + 1);
    text_write_texture(track.row, 0xffffff, 0, 0, 0, 0,
                       heard(index) ? track.title : hidden);
    position_row(room, index, true);
}

void update_comment(MusicRoom* room) {
    if ((room->timer & 1) != 0 || room->comment_line >= 8) return;
    const int line = room->comment_line++;
    const bool warning = !heard(room->comment_track) && room->unheard_warning;
    text_write_texture(room->comments[line], warning ? 0x8080ff : 0xffffff,
                       0, 0, 0, 0, warning ? kUnheardComments[line] :
                       room->tracks[room->comment_track].comments[line]);
    anm_set_label(room->comments[line], 2);
}
} // namespace

MusicRoom* music_room_create(AnmVm* title_ambient) {
    auto* room = new MusicRoom;
    room->ambient = title_ambient;
    return room;
}

bool music_room_update(MusicRoom* room) {
    switch (room->state) {
    case 0:
        if (room->timer == 1) {
            if (room->ambient == nullptr)
                create_vm(anm_get(2, "ascii.anm"), 19, &room->ambient);
            else
                anm_manage_vm(room->ambient, &room->ambient);
            create_vm(anm_get(0x10, "title.anm"), 42, &room->heading);
            read_tracks(room);
            for (int line = 0; line < 8; ++line)
                create_vm(anm_get(0, "text.anm"), 26 + line, &room->comments[line]);
        }
        if (room->timer < 10) {
            for (int index = room->timer * 2 - 2;
                 index < room->timer * 2 && index < room->track_count; ++index)
                create_row(room, index);
        } else {
            room->state = 1;
            room->timer = 0;
        }
        break;
    case 1:
        update_comment(room);
        if (room->timer > 4) {
            room->state = 2;
            room->timer = 0;
        }
        break;
    case 2: {
        update_comment(room);
        const FrameInput input = input_frame();
        const uint32_t movement = input.pressed | input.repeat;
        const int previous = room->selection;
        if (movement & ACTION_UP)
            room->selection = (room->selection + room->track_count - 1) % room->track_count;
        if (movement & ACTION_DOWN)
            room->selection = (room->selection + 1) % room->track_count;
        if (previous != room->selection) {
            sound_play_se(10);
            if (room->selection < room->first_visible)
                room->first_visible = room->selection;
            else if (room->selection >= room->first_visible + 10)
                room->first_visible = room->selection - 9;
            for (int index = 0; index < room->track_count; ++index)
                position_row(room, index, false);
            if (room->comment_line >= 8) room->unheard_warning = false;
        }
        if (input.pressed & (ACTION_MENU_OK | ACTION_SHOT)) {
            for (AnmVm* comment : room->comments) anm_set_label(comment, 3);
            room->comment_line = 0;
            room->comment_track = room->selection;
            room->timer = 0;
            if (!heard(room->comment_track) && !room->unheard_warning) {
                bgm_stop();
                room->unheard_warning = true;
            } else {
                bgm_preview(room->tracks[room->selection].filename);
                room->unheard_warning = false;
            }
        } else if (input.pressed & (ACTION_MENU_CANCEL | ACTION_BOMB)) {
            for (int index = 0; index < room->track_count; ++index)
                anm_set_label(room->tracks[index].row, 1);
            for (AnmVm* comment : room->comments) anm_set_label(comment, 1);
            sound_play_se(9);
            room->state = 3;
            room->timer = 0;
        }
        break;
    }
    case 3:
        if (room->timer > 9) {
            anm_set_label(room->heading, 1);
            anm_manage_vm(room->heading, nullptr);
            anm_set_label(room->ambient, 1);
            anm_manage_vm(room->ambient, nullptr);
            bgm_play(0);
            return true;
        }
        break;
    }
    ++room->timer;
    return false;
}

void music_room_destroy(MusicRoom* room) {
    for (int index = 0; index < room->track_count; ++index)
        anm_vm_free(room->tracks[index].row);
    for (AnmVm* comment : room->comments) anm_vm_free(comment);
    anm_vm_free(room->heading);
    anm_vm_free(room->ambient);
    delete room;
}
