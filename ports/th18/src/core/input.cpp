#include "core/input.h"

static InputState g_input;
static InputState g_battle_input;

void input_reset() {
    g_input = InputState{};
    g_battle_input=InputState{};
}

static void advance_input(InputState& g_input,uint32_t raw_bits) {
    g_input.prev = g_input.held;
    g_input.held = raw_bits;

    g_input.repeat = 0;
    g_input.first_repeat = 0;

    uint32_t mask = 1;
    for (int i = 0; i < 32; i++) {
        if ((raw_bits & mask) == 0) {
            g_input.hold[i] = 0;
            g_input.extra[i] = 0;
        } else {
            g_input.hold[i] += 1;
            uint32_t frames = (uint32_t)g_input.hold[i];
            g_input.extra[i] += 1;
            if (frames > 7) {
                g_input.first_repeat |= mask;
            }
            if (frames > 0x19) {
                g_input.repeat |= mask;
                g_input.hold[i] -= 8;
            }
        }
        mask <<= 1;
    }

    g_input.pressed = (g_input.prev ^ g_input.held) & g_input.held;
    g_input.released = ~g_input.held & (g_input.prev ^ g_input.held);
}
void input_update(uint32_t raw_bits) {advance_input(g_input,raw_bits);}
void input_battle_update(uint32_t raw_bits) {advance_input(g_battle_input,raw_bits);}
void input_battle_reset() {g_battle_input=InputState{};}
void input_battle_clear() {g_battle_input.held=g_battle_input.pressed=g_battle_input.released=0;}
const InputState& input_battle_state() {return g_battle_input;}
FrameInput input_battle_frame() {return {g_battle_input.held,g_battle_input.pressed,g_battle_input.released,g_battle_input.repeat};}

const InputState& input_state() {
    return g_input;
}

FrameInput input_frame() {
    FrameInput out;
    out.held = g_input.held;
    out.pressed = g_input.pressed;
    out.released = g_input.released;
    out.repeat = g_input.repeat;
    return out;
}
