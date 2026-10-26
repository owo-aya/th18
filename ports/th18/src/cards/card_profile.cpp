#include "cards/card_profile.h"

#include <cstdlib>
#include <cstring>

#include "cards/card_info.h"
#include "gameplay/score_data.h"
#include "resources/dat.h"

namespace {
char g_card_text[58][7][64];
bool g_loaded;

const CardId g_order[57] = {
    Blank, Extend, Bomb, Extend2, Bomb2, Pendulum, Dango, Mokou,
    ReimuOp, ReimuOp2, MarisaOp, MarisaOp2, SakuyaOp, SakuyaOp2, SanaeOp, SanaeOp2,
    YoumuOp, AliceOp, CirnoOp, OkinaOp, NueOp, Magatama, ItemCatch, ItemLine,
    AutoBomb, DeadBombExtend, MainShotPU, MagicScroll, Koishi, MainShotSP, SpeedQueen,
    OptionBR, DeadSpell, PowerMax, Yuyuko, Money, Rokumon, Narumi, Pache, Maneki,
    Yamawaro, Kiseru, Mukade, Warp, Kozuchi, Kaname, Moon, MikoFlash, Vampire,
    Sun, Lily, Bassdrum, Psyco, Cylinder, Riceball, Magatama2, Null
};

void read_card_text() {
    size_t size;
    char* text = static_cast<char*>(dat_read("ability.txt", &size));
    char* cursor = text;
    char* end = text + size;
    int id = Null;
    int line_index = 0;
    while (cursor < end && *cursor != '\\') {
        char* start = cursor;
        while (cursor < end && *cursor != '\r' && *cursor != '\n' && *cursor != '\f')
            ++cursor;
        const size_t length = cursor - start;
        while (cursor < end && (*cursor == '\r' || *cursor == '\n' || *cursor == '\f'))
            ++cursor;
        if (length == 0 || *start == '#')
            continue;
        if (*start == '@') {
            id = Null;
            for (const CardInfo& info : g_card_info)
                if (std::strlen(info.key) == length - 1 &&
                    std::memcmp(info.key, start + 1, length - 1) == 0)
                    id = info.id;
            line_index = 0;
        } else if (line_index < 7) {
            std::memcpy(g_card_text[id][line_index++], start, length);
        }
    }
    std::free(text);
}
} // namespace

void card_profile_init() {
    if (g_loaded)
        return;
    score_init();
    read_card_text();
    g_loaded = true;
}

bool card_profile_unlocked(CardId id) { return score_data().state[0xd0 + id] != 0; }
bool card_profile_mark_acquired(CardId id) {
    uint8_t* state = score_data().state;
    if (state[0xd0 + id] != 0)
        return false;
    state[0xd0 + id] = 1;
    for (int card = 0; card < 56; ++card)
        if (state[0xd0 + card] == 0)
            return true;
    score_mark_achievement(0x1d); // FUN_00418de0 -> FUN_0046e490, all 56 cards
    return true;
}
uint64_t card_profile_record_clear(int player, int difficulty, bool extra,
                                   bool continued, bool has_blank, int miss_count) {
    uint8_t* state = score_data().state;
    if (extra) {
        const uint64_t acquired = card_profile_mark_acquired(Mukade) ? (uint64_t{1} << Mukade) : 0;
        score_mark_achievement(player + 0xc);
        if (miss_count == 0)
            score_mark_achievement(0x1b);
        return acquired;
    }

    int32_t& slots = *reinterpret_cast<int32_t*>(state + 0x1c0);
    // 00442450(1/2/3) sets the corresponding notice flag; its temporary
    // notification queue belongs to the result/title presentation owner.
    if (slots == 1 && state[0x181] == 0) {
        state[0x181] = 1;
        slots = 2;
    }
    const int ending = continued ? player + 8 : player * 2 + (has_blank ? 0 : 1);
    score_mark_achievement(ending);
    if (!continued && has_blank) {
        score_mark_achievement(0x18);
        if (slots == 2 && state[0x182] == 0) {
            state[0x182] = 1;
            slots = 3;
        }
    }
    score_mark_achievement(difficulty + 0x10);
    if (!continued)
        score_mark_achievement(difficulty + 0x14);
    if (miss_count == 0) {
        score_mark_achievement(0x19);
        if (difficulty == 3)
            score_mark_achievement(0x1a);
    }

    const CardId second_options[4] = {ReimuOp2, MarisaOp2, SakuyaOp2, SanaeOp2};
    const CardId option = second_options[player];
    uint64_t acquired = 0;
    if (card_profile_mark_acquired(option))
        acquired |= uint64_t{1} << option;
    if (!continued) {
        if (card_profile_mark_acquired(Magatama2))
            acquired |= uint64_t{1} << Magatama2;
        if (card_profile_mark_acquired(Blank))
            acquired |= uint64_t{1} << Blank;
        state[0x183] = 1;
    }
    return acquired;
}
int card_profile_deck_count(int bank) {
    return reinterpret_cast<const int32_t*>(score_data().state + 0x1c0)[bank];
}
CardId card_profile_deck_id(int bank, int slot) {
    return static_cast<CardId>(score_data().state[0x150 + bank * 16 + slot]);
}
void card_profile_set_deck_card(int bank, int slot, CardId id) {
    score_data().state[0x150 + bank * 16 + slot] = static_cast<uint8_t>(id);
}
const CardId* card_profile_order() { return g_order; }
bool card_profile_cleared(int player, int difficulty) {
    return reinterpret_cast<const int32_t*>(score_data().character[player] + 0x64ec)[difficulty] != 0;
}
uint64_t card_profile_high_score(int player, int difficulty) {
    const uint8_t* record = score_data().character[player] + 0x10 + difficulty * 0x140;
    return static_cast<uint64_t>(*reinterpret_cast<const uint32_t*>(record)) * 10 + record[5];
}
const char* card_name(CardId id) { return g_card_text[id][0]; }
const char* card_description(CardId id, int line) { return g_card_text[id][line + 1]; }
