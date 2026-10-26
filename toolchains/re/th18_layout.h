#pragma once
#include <cstdint>

constexpr uint32_t kCardRng = 0x4cf280;
constexpr uint32_t kRng = 0x4cf288;
constexpr uint32_t kStatus = 0x4cccdc;
constexpr uint32_t kInput = 0x4ca210;
constexpr uint32_t kApp = 0x4ccdf0;
constexpr uint32_t kPlayerPointer = 0x4cf410;

constexpr uint32_t kStatusCurrentStage = kStatus;
constexpr uint32_t kStatusStartStage = kStatus + 4;
constexpr uint32_t kStatusPlayer = kStatus + 0x18;
constexpr uint32_t kStatusShot = kStatus + 0x1c;
constexpr uint32_t kStatusScore = kStatus + 0x20;
constexpr uint32_t kStatusDifficulty = kStatus + 0x24;
constexpr uint32_t kStatusMoney = kStatus + 0x58;
constexpr uint32_t kStatusPower = kStatus + 0x5c;
constexpr uint32_t kStatusLives = kStatus + 0x6c;

constexpr uint32_t kAppState = kApp + 0x7f4;
constexpr uint32_t kAppNextState = kApp + 0x7f8;

constexpr uint32_t kPlayerX = 0x620;
constexpr uint32_t kPlayerY = 0x624;
constexpr uint32_t kPlayerState = 0x476ac;

constexpr uint32_t kInputUpdate = 0x404e90;
constexpr uint32_t kSchedulerReturn = 0x4013ed;
