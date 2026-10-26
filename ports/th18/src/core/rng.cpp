#include "core/rng.h"

static Rng g_rng;
static Rng g_card_rng;

Rng& rng() {
    return g_rng;
}

Rng& card_rng() {
    return g_card_rng;
}

void rng_reset(uint16_t seed) {
    g_card_rng.state = seed;
    g_card_rng.pad = 0;
    g_card_rng.counter = 0;
    g_rng = g_card_rng;
}

uint32_t rng_next() {
    return rng_next(g_rng);
}

uint32_t rng_next(Rng& random) {
    uint32_t s = random.state;
    uint32_t a = ((s ^ 0x9630u) + 0x9aadu) & 0xffffu;
    uint32_t b = ((((a >> 14) + a * 4) ^ 0x9630u) + 0x9aadu) & 0xffffu;
    random.state = (uint16_t)(((b >> 14) + b * 4) & 0xffffu);
    random.counter += 2;
    return (a << 16) | b;
}

uint16_t rng_next16() {
    const uint32_t value=((uint32_t(g_rng.state)^0x9630u)+0x9aadu)&0xffffu;
    g_rng.state=uint16_t(((value>>14)+value*4)&0xffffu);++g_rng.counter;
    return uint16_t(value);
}

float rng_float01() {
    return (float)rng_next() * 2.3283064e-10f;  // 2^-32
}

float rng_float_pm1() {
    return (float)rng_next() * 4.6566129e-10f - 1.0f;  // 2^-31 - 1
}
