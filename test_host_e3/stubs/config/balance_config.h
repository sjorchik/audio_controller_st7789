#pragma once
// ЗАГЛУШКА для хост-тесту E3 (лише ПК): справжній src/config/balance_config.h не прикріплювався.
// Значення — з MASTER SPEC (розділ 11/13): ±20 дБ, крок 1, ramp 30 мс, kSwapChannels = false.
#include <stdint.h>

namespace balance_cfg {
constexpr int kMinDb = -20;
constexpr int kMaxDb = 20;
constexpr int kStepDb = 1;
constexpr int kDefaultDb = 0;
constexpr uint32_t kRampMs = 30;
constexpr bool kSwapChannels = false;
}  // namespace balance_cfg
