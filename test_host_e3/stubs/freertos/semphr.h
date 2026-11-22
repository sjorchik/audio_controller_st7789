#pragma once
// ЗАГЛУШКА для хост-тесту E3 (лише ПК): мʼютекс на std::mutex.
#include <mutex>

#include "freertos/FreeRTOS.h"

typedef void* SemaphoreHandle_t;

inline SemaphoreHandle_t xSemaphoreCreateMutex() {
    return new std::mutex();
}
inline int xSemaphoreTake(SemaphoreHandle_t h, TickType_t) {
    static_cast<std::mutex*>(h)->lock();
    return 1;
}
inline int xSemaphoreGive(SemaphoreHandle_t h) {
    static_cast<std::mutex*>(h)->unlock();
    return 1;
}
