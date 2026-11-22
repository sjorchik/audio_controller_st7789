#pragma once
// ЗАГЛУШКА для хост-тесту E3 (лише ПК): справжній src/config/station_level_config.h не прикріплювався.
// Значення kLevelMinDb / kLevelMaxDb / kDefaultStationLevelDb — з MASTER SPEC (розділ 11/13); kTrimRampFrames
// НЕ відомий чату (на тест не впливає: перевірки йдуть в усталеному режимі) — взято 2400 (≈ 50 мс при 48 кГц).
#include <stdint.h>

namespace station_level_cfg {
constexpr int kLevelMinDb = -24;
constexpr int kLevelMaxDb = 0;
constexpr int kDefaultStationLevelDb = -20;
constexpr int32_t kTrimRampFrames = 2400;
}  // namespace station_level_cfg
