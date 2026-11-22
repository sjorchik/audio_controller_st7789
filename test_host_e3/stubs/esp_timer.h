#pragma once
// ЗАГЛУШКА для хост-тесту E3 (лише ПК): esp_timer_get_time() у мкс.
#include <chrono>
#include <stdint.h>

inline int64_t esp_timer_get_time() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}
