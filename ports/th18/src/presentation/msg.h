#pragma once
#include <cstdint>

struct AnmResource;

// 0043e360 / 0043d720: the stage owns file_data. MSG borrows it until unload.
void msg_load(const void* file_data, AnmResource* logo);
void msg_start(int entry);
void msg_unload();
void msg_update();
bool msg_active();
// ECL 519 passes on MSG 12's one-frame pulse, or after the scene ends.
bool msg_wait_released();
// 004412b0 / 004414c0: pause hides the scene and suppresses skip for two frames.
void msg_set_paused(bool paused);

enum MsgEvent : uint32_t {
    MSG_FINISHED = 1,
    MSG_SHOP = 2,
    MSG_STAGE_END = 4,
    MSG_BGM_FADE = 8,
};
uint32_t msg_take_events();
float msg_bgm_fade_seconds();
