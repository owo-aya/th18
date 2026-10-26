#pragma once
#include <cstddef>
#include <cstdint>

constexpr size_t kCompareFieldCount = 24;

enum CompareField {
    FieldTick,
    FieldState,
    FieldNextState,
    FieldStage,
    FieldCurrentStage,
    FieldStartStage,
    FieldPlayer,
    FieldShot,
    FieldDifficulty,
    FieldInputHeld,
    FieldInputPressed,
    FieldBattleHeld,
    FieldBattlePressed,
    FieldRngState,
    FieldRngCounter,
    FieldCardRngState,
    FieldCardRngCounter,
    FieldScore,
    FieldMoney,
    FieldPower,
    FieldLives,
    FieldPlayerX,
    FieldPlayerY,
    FieldPlayerState,
};

struct Snapshot {
    uint32_t forced_bits;
    uint32_t field_count;
    uint64_t fields[kCompareFieldCount];
    char text[256];
};
