#pragma once
// ЗАГЛУШКА для хост-тесту E3 (лише ПК): мінімум Arduino API, який потрібен eq.cpp при EQ_DEBUG = 1.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

inline uint32_t& fakeMillisRef() {
    static uint32_t m = 0;
    return m;
}
inline uint32_t millis() {
    return fakeMillisRef();
}

struct SerialStub {
    int printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        va_list ap;
        va_start(ap, fmt);
        const int n = vprintf(fmt, ap);
        va_end(ap);
        return n;
    }
};
inline SerialStub Serial;
