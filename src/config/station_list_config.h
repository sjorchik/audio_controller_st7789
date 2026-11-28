#pragma once

// [L1] Константи автозакриття списку станцій (Mode::Menu + MenuContext::StationList).
// Таймер перезапускається будь-якою дією користувача у списку; після kTimeoutMs без дій список
// закривається тим самим переходом, що й кнопка «назад» (closeStationList()). Станція не змінюється.

#include <stdint.h>

namespace station_list_cfg {

constexpr uint32_t kTimeoutMs = 10000;   // як eq_screen_cfg::kTimeoutMs

static_assert(kTimeoutMs >= 1000, "timeout is too short to use the station list");

}  // namespace station_list_cfg
