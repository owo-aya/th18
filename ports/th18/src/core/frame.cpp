#include "core/frame.h"
#include "core/game_session.h"

static GameSession g_session;
static uint32_t g_logic_frame = 0;

GameSession& session() {
    return g_session;
}

uint32_t frame_count() {
    return g_logic_frame;
}

void frame_update(const FrameInput& input) {
    (void)input;
    ++g_logic_frame;
}
