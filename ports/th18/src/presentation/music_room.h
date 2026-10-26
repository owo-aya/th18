#pragma once

struct MusicRoom;
struct AnmVm;

// 0046aff0: title state 14. The title owns the A8 callback; ANM owns drawing.
MusicRoom* music_room_create(AnmVm* title_ambient);
bool music_room_update(MusicRoom* room);
void music_room_destroy(MusicRoom* room);
