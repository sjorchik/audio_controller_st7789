// E2a: перевірка EQ_DEBUG = 1: компіляція, формат "[EQ] preset", звіт "[EQ] radio proc" через eq::setSampleRate().
#include <stdio.h>
#include <vector>
#include "audio/eq.cpp"
int main() {
    static_assert(EQ_DEBUG == 1, "debug build expected");
    eq::setSampleRate(48000);
    const int8_t g[eq_cfg::kBandCount] = {14, 2, -1, 1, -1, 2, 5, 12};
    eq::setAllDb(g);  // друкує "[EQ] radio preset ..."
    std::vector<int32_t> buf(256);
    for (int blk = 0; blk < 2000; ++blk) {
        for (int i = 0; i < 128; ++i) buf[2 * i] = buf[2 * i + 1] = (i % 2 ? 1 : -1) * (1 << 24);
        eq::process(buf.data(), 128);
    }
    // Період звіту kDebugReportMs = 1000 мс: чекаємо й викликаємо setSampleRate (як задача плеєра).
    const uint32_t t0 = millis();
    while (millis() - t0 < eq_cfg::kDebugReportMs + 50) {
        for (int i = 0; i < 128; ++i) buf[2 * i] = buf[2 * i + 1] = (i % 2 ? 1 : -1) * (1 << 24);
        eq::process(buf.data(), 128);
        eq::setSampleRate(48000);
    }
    return 0;
}
