#pragma once
#include <cstdint>

// 会话级状态。领域状态（自机、弹幕、卡牌等）由对应模块自行维护。
enum class GameMode : int32_t {
    Full = 0,
    Practice = 1,
    Replay = 2,
    SpellPractice = 0x20,
};

enum class Difficulty : int32_t {
    Easy = 0,
    Normal = 1,
    Hard = 2,
    Lunatic = 3,
    Extra = 4,
};

struct GameSession {
    GameMode mode;
    Difficulty difficulty;
    int32_t player;       // 0..3，对应 pl00..pl03
    int32_t stage;        // 1..6 正篇，Extra 另计
    uint32_t logic_frame; // 自进入会话起的逻辑帧
    uint32_t score;
};

GameSession& session();
