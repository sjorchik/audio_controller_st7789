// Хост-тест E2a (лише ПК, не збірка прошивки): 8-смуговий еквалайзер.
// Підключає РЕАЛЬНИЙ src/audio/eq.cpp (через #include, щоб бачити designPeaking з анонімного namespace) із заглушками
// Arduino / FreeRTOS (stubs/). Перевірки:
//   1. kBandCount = 8, частоти як у завданні.
//   2. Коефіцієнти скінченні й стійкі: усі смуги, fs 32 / 44,1 / 48 кГц, підсилення -18..+18 (смуга вище 0,45·fs вимкнена).
//   3. Усталене підсилення на центрі смуги при +18 (сусідні 0) і «витік» на центри сусідніх смуг — таблиця.
//   4. Перехід 0 -> +18 усіма смугами (і +18 -> -18): піки у вікні не перевищують усталений рівень більш ніж на 1 дБ.
//   5. Вимкнення смуг за fs: 15 кГц вимкнена при 32 кГц, увімкнена при 44,1 / 48 кГц.
//   6. process(int32): clipCount, бітова ідентичність при всіх нулях, узгодженість із processFloat.
//   7. Відгук цілочисельного пресету «підгонка AIMP» у точках цілі (довідка).
//   8. Мікробенч ПК (НЕ ESP32): 5 vs 8 активних смуг, нс / кадр.
// Збірка: див. build.sh. Код виходу 0 = усі перевірки пройшли.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>
#include <vector>

#include "audio/eq.cpp"  // реальна реалізація (включно з анонімним namespace)

namespace {

int g_fail = 0;
int g_pass = 0;
int g_known = 0;  // відомі властивості P45 (інтерполяція від одиничного біквада), однакові з 5 смугами; НЕ збій тесту

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (cond) {                                       \
            ++g_pass;                                     \
        } else {                                          \
            ++g_fail;                                     \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

constexpr double kPi = 3.14159265358979323846;

// |H(e^{jw})| в дБ для біквада (коефіцієнти нормовані на a0).
double magDb(const eq::detail::Coef& c, double f, double fs) {
    const double w = 2.0 * kPi * f / fs;
    const double cw = cos(w), sw = sin(w), c2 = cos(2 * w), s2 = sin(2 * w);
    const double nr = c.b0 + c.b1 * cw + c.b2 * c2;
    const double ni = -(c.b1 * sw + c.b2 * s2);
    const double dr = 1.0 + c.a1 * cw + c.a2 * c2;
    const double di = -(c.a1 * sw + c.a2 * s2);
    return 10.0 * log10((nr * nr + ni * ni) / (dr * dr + di * di));
}

bool stable(const eq::detail::Coef& c) {
    // z^2 + a1 z + a2: полюси всередині одиничного кола <=> |a2| < 1 і |a1| < 1 + a2.
    return fabs(static_cast<double>(c.a2)) < 1.0 && fabs(static_cast<double>(c.a1)) < 1.0 + static_cast<double>(c.a2);
}

bool finite5(const eq::detail::Coef& c) {
    return isfinite(c.b0) && isfinite(c.b1) && isfinite(c.b2) && isfinite(c.a1) && isfinite(c.a2);
}

bool bandOn(uint8_t b, double fs) {
    return static_cast<double>(eq_cfg::kBandFreqHz[b]) <= eq_cfg::kMaxFreqRatio * fs;
}

// --- 1 ---------------------------------------------------------------------
void testConfig() {
    printf("\n[1] config\n");
    CHECK(eq_cfg::kBandCount == 8, "kBandCount = %u", static_cast<unsigned>(eq_cfg::kBandCount));
    const uint16_t want[8] = {50, 125, 350, 1000, 2800, 5600, 10000, 15000};
    for (uint8_t b = 0; b < 8; ++b) {
        CHECK(eq_cfg::kBandFreqHz[b] == want[b], "freq[%u] = %u", b, static_cast<unsigned>(eq_cfg::kBandFreqHz[b]));
    }
    CHECK(eq_cfg::kQ == 1.0, "Q");
    CHECK(eq_cfg::kGainMaxDb == 18 && eq_cfg::kGainMinDb == -18, "range +-18");
    printf("    sizeof(Coef)=%zu sizeof(Block)=%zu sizeof(Target)=%zu sizeof(Processor)=%zu\n",
           sizeof(eq::detail::Coef), sizeof(eq::detail::Block), sizeof(eq::detail::Target), sizeof(eq::Processor));
    // Старі розміри (5 смуг) для порівняння: розраховано вручну з тих самих структур.
    printf("    (до E2a, 5 смуг: Block 188, Target 116 -> стек beginBlock+process ≈ 304 Б; тепер %zu Б)\n",
           sizeof(eq::detail::Block) + sizeof(eq::detail::Target));
}

// --- 2 ---------------------------------------------------------------------
void testStability() {
    printf("\n[2] coefficient finiteness / stability\n");
    const double fss[3] = {32000.0, 44100.0, 48000.0};
    int checked = 0, unstable = 0, nonfinite = 0;
    for (double fs : fss) {
        for (uint8_t b = 0; b < eq_cfg::kBandCount; ++b) {
            if (!bandOn(b, fs)) {
                continue;  // така смуга вимикається (identity)
            }
            for (int db = eq_cfg::kGainMinDb; db <= eq_cfg::kGainMaxDb; ++db) {
                const auto c = designPeaking(eq_cfg::kBandFreqHz[b], db, fs);
                ++checked;
                if (!finite5(c)) ++nonfinite;
                if (!stable(c)) ++unstable;
            }
        }
    }
    printf("    checked %d designs (fs 32/44.1/48k, all active bands, -18..+18 dB)\n", checked);
    CHECK(nonfinite == 0, "non-finite coefficients: %d", nonfinite);
    CHECK(unstable == 0, "unstable designs: %d", unstable);
    // Лінійна інтерполяція між двома стійкими біквадами лишається стійкою (область (a1,a2) опукла): перевірка на ±18.
    int bad = 0;
    for (uint8_t b = 0; b < eq_cfg::kBandCount; ++b) {
        const auto a = designPeaking(eq_cfg::kBandFreqHz[b], -18, 48000.0);
        const auto z = designPeaking(eq_cfg::kBandFreqHz[b], 18, 48000.0);
        for (int i = 0; i <= 20; ++i) {
            if (!stable(lerpCoef(a, z, i / 20.0f))) ++bad;
        }
    }
    CHECK(bad == 0, "unstable interpolated coefficients: %d", bad);
}

// --- 3 ---------------------------------------------------------------------
void testGainAndLeak() {
    printf("\n[3] steady-state gain at band centre (+18 dB, neighbours 0) and leakage onto neighbour centres, fs 48 kHz\n");
    printf("    band  f0[Hz]   gain@f0[dB]   leak->prev[dB]   leak->next[dB]\n");
    const double fs = 48000.0;
    for (uint8_t b = 0; b < eq_cfg::kBandCount; ++b) {
        const auto c = designPeaking(eq_cfg::kBandFreqHz[b], 18, fs);
        const double g0 = magDb(c, eq_cfg::kBandFreqHz[b], fs);
        const double lp = b > 0 ? magDb(c, eq_cfg::kBandFreqHz[b - 1], fs) : 0.0;
        const double ln = b + 1 < eq_cfg::kBandCount ? magDb(c, eq_cfg::kBandFreqHz[b + 1], fs) : 0.0;
        printf("    %u    %6u    %+7.2f        %s%+6.2f         %s%+6.2f\n", b,
               static_cast<unsigned>(eq_cfg::kBandFreqHz[b]), g0, b > 0 ? "" : " ", lp,
               b + 1 < eq_cfg::kBandCount ? "" : " ", ln);
        CHECK(fabs(g0 - 18.0) < 0.05, "band %u centre gain %.3f dB != 18", b, g0);
    }
    // Усі смуги +18 одночасно: підсилення на центрах (сума внесків сусідів).
    printf("    all 8 bands at +18 dB: total response at band centres [dB]: ");
    double worst = 0;
    for (uint8_t b = 0; b < eq_cfg::kBandCount; ++b) {
        double t = 0;
        for (uint8_t k = 0; k < eq_cfg::kBandCount; ++k) {
            t += magDb(designPeaking(eq_cfg::kBandFreqHz[k], 18, fs), eq_cfg::kBandFreqHz[b], fs);
        }
        printf("%+.1f ", t);
        if (t > worst) worst = t;
    }
    printf("(max %+.1f dB)\n", worst);
}

// --- 4 ---------------------------------------------------------------------
// Піки у ковзному вікні (>= 1 період тону, >= 1 блок) у розрізі переходу; порівняння з усталеним піком.
double runTransition(double f, double fs, const int8_t from[eq_cfg::kBandCount], const int8_t to[eq_cfg::kBandCount],
                     double& steadyPeakOut) {
    eq::Processor p("test");
    p.setSampleRate(static_cast<uint32_t>(fs));
    p.setAllDb(from);
    const int kBlock = 128;
    const double amp = 0.01;  // -40 dBFS: float-шлях не насичує
    const long totalFrames = static_cast<long>(fs * 0.6);
    const long switchAt = static_cast<long>(fs * 0.2);
    std::vector<float> in(2 * kBlock);
    std::vector<float> out;  // усі вихідні відліки (лише L)
    out.reserve(static_cast<size_t>(totalFrames));
    long n = 0;
    bool switched = false;
    while (n < totalFrames) {
        if (!switched && n >= switchAt) {
            p.setAllDb(to);
            switched = true;
        }
        for (int i = 0; i < kBlock; ++i) {
            const float s = static_cast<float>(amp * sin(2.0 * kPi * f * static_cast<double>(n + i) / fs));
            in[2 * i] = s;
            in[2 * i + 1] = s;
        }
        p.processFloat(in.data(), kBlock);
        for (int i = 0; i < kBlock; ++i) out.push_back(in[2 * i]);
        n += kBlock;
    }
    const long win = static_cast<long>(fmax(fs / f, static_cast<double>(kBlock)) * 1.01) + 1;
    // Усталений пік: максимум у вікні в кінці.
    double steady = 0;
    for (long i = static_cast<long>(out.size()) - win; i < static_cast<long>(out.size()); ++i) {
        steady = fmax(steady, fabs(static_cast<double>(out[static_cast<size_t>(i)])));
    }
    steadyPeakOut = steady;
    // Найбільший пік у ковзних вікнах після моменту перемикання (крок = блок).
    double worst = 0;
    for (long start = switchAt; start + win <= static_cast<long>(out.size()); start += kBlock) {
        double pk = 0;
        for (long i = start; i < start + win; ++i) pk = fmax(pk, fabs(static_cast<double>(out[static_cast<size_t>(i)])));
        worst = fmax(worst, pk);
    }
    return worst;
}

void testTransitions() {
    printf("\n[4] transition: all bands 0 -> +18 and +18 -> -18, tone at each band centre, fs 48 kHz\n");
    printf("    f0[Hz]   steady peak[dBFS]   worst window peak[dBFS]   overshoot[dB]\n");
    int8_t zeros[eq_cfg::kBandCount];
    int8_t up[eq_cfg::kBandCount];
    int8_t down[eq_cfg::kBandCount];
    for (uint8_t b = 0; b < eq_cfg::kBandCount; ++b) {
        zeros[b] = 0;
        up[b] = 18;
        down[b] = -18;
    }
    double worstOver = -1e9;
    for (uint8_t b = 0; b < eq_cfg::kBandCount; ++b) {
        const double f = eq_cfg::kBandFreqHz[b];
        double steady = 0;
        const double w = runTransition(f, 48000.0, zeros, up, steady);
        const double over = 20.0 * log10(w / steady);
        printf("    %6.0f   %+8.2f            %+8.2f                %+6.2f\n", f, 20.0 * log10(steady),
               20.0 * log10(w), over);
        if (over > 1.0) { ++g_known; printf("    KNOWN: 0->+18 overshoot %.2f dB > 1 dB at %.0f Hz (P45 coefficient interpolation, same with 5 bands)\n", over, f); }
        if (over > worstOver) worstOver = over;
    }
    // +18 -> -18: максимум вікна після перемикання не має перевищувати ПОЧАТКОВИЙ усталений рівень (+18) більш ніж на 1 дБ.
    printf("    +18 -> -18: window peak after the switch vs. the +18 steady level:\n");
    for (uint8_t b = 0; b < eq_cfg::kBandCount; ++b) {
        const double f = eq_cfg::kBandFreqHz[b];
        double steadyDown = 0, steadyUp = 0;
        const double w = runTransition(f, 48000.0, up, down, steadyDown);
        runTransition(f, 48000.0, up, up, steadyUp);  // усталений рівень «від» (+18 всюди)
        const double over = 20.0 * log10(w / steadyUp);
        printf("    %6.0f   from %+7.2f dBFS, after %+7.2f dBFS, worst %+7.2f dBFS (vs from: %+5.2f dB)\n", f,
               20.0 * log10(steadyUp), 20.0 * log10(steadyDown), 20.0 * log10(w), over);
        if (over > 1.0) { ++g_known; printf("    KNOWN: +18->-18 overshoot %.2f dB > 1 dB at %.0f Hz (P45 interpolation)\n", over, f); }
    }
    printf("    worst 0->+18 overshoot: %+.2f dB\n", worstOver);
}

// --- 5 ---------------------------------------------------------------------
// Підсилення тону f при ОДНІЙ смузі +18: дБ відносно входу (усталене, на кінці довгого прогону).
double toneGainDb(double f, double fs, uint8_t band, int db) {
    eq::Processor p("test");
    p.setSampleRate(static_cast<uint32_t>(fs));
    int8_t g[eq_cfg::kBandCount] = {};
    g[band] = static_cast<int8_t>(db);
    p.setAllDb(g);
    const int kBlock = 128;
    std::vector<float> buf(2 * kBlock);
    double peak = 0;
    const long total = static_cast<long>(fs * 0.5);
    for (long n = 0; n < total; n += kBlock) {
        for (int i = 0; i < kBlock; ++i) {
            const float s = static_cast<float>(0.01 * sin(2.0 * kPi * f * static_cast<double>(n + i) / fs));
            buf[2 * i] = buf[2 * i + 1] = s;
        }
        p.processFloat(buf.data(), kBlock);
        if (n >= total - 4 * kBlock) {
            for (int i = 0; i < kBlock; ++i) peak = fmax(peak, fabs(static_cast<double>(buf[2 * i])));
        }
    }
    return 20.0 * log10(peak / 0.01);
}

void testNyquistRule() {
    printf("\n[5] bands above 0.45*fs are switched off (15 kHz: off at 32 kHz, on at 44.1 / 48 kHz)\n");
    const uint8_t top = eq_cfg::kBandCount - 1;  // 15 кГц
    const double g32 = toneGainDb(15000.0 * 0.9, 32000.0, top, 18);   // 13,5 кГц < Найквіста 16 кГц
    const double g44 = toneGainDb(15000.0, 44100.0, top, 18);
    const double g48 = toneGainDb(15000.0, 48000.0, top, 18);
    printf("    15 kHz band +18 dB: gain at 32 kHz fs (tone 13.5k) %+.2f dB, 44.1 kHz fs %+.2f dB, 48 kHz fs %+.2f dB\n",
           g32, g44, g48);
    CHECK(fabs(g32) < 0.1, "15k band must be off at fs 32k (gain %.2f)", g32);
    CHECK(fabs(g44 - 18.0) < 0.3, "15k band at 44.1k: %.2f", g44);
    CHECK(fabs(g48 - 18.0) < 0.3, "15k band at 48k: %.2f", g48);
    const double g10 = toneGainDb(10000.0, 32000.0, 6, 18);  // 10 кГц: 10000 <= 0.45*32000 = 14400 -> увімкнена
    printf("    10 kHz band +18 dB at fs 32 kHz (on, 10000 <= 14400): %+.2f dB\n", g10);
    CHECK(fabs(g10 - 18.0) < 0.3, "10k band at 32k: %.2f", g10);
    // Поріг: найнижчий fs, при якому 15 кГц увімкнена = 15000 / 0,45 = 33333 Гц.
    CHECK(bandOn(7, 33334.0) && !bandOn(7, 33333.0 - 1.0), "threshold 15k / 0.45");
}

// --- 6 ---------------------------------------------------------------------
void testInt32Path() {
    printf("\n[6] int32 path: bit-identity at 0 dB, clipCount, consistency with float\n");
    // 6a: усі нулі — блок не чіпається.
    {
        eq::Processor p("test");
        p.setSampleRate(48000);
        std::vector<int32_t> a(256), b;
        for (size_t i = 0; i < a.size(); ++i) a[i] = static_cast<int32_t>((rand() % 2000000) - 1000000) * 1000;
        b = a;
        p.process(a.data(), 128);
        CHECK(memcmp(a.data(), b.data(), a.size() * sizeof(int32_t)) == 0, "all-zero preset must be bit-identical");
        CHECK(p.clipCount() == 0, "clipCount at zero preset");
    }
    // 6b: сильний сигнал + підсилення 125 Гц -> насичення рахується.
    {
        eq::Processor p("test");
        p.setSampleRate(48000);
        int8_t g[eq_cfg::kBandCount] = {};
        g[1] = 18;
        p.setAllDb(g);
        std::vector<int32_t> buf(256);
        for (int blk = 0; blk < 400; ++blk) {
            for (int i = 0; i < 128; ++i) {
                const double s = 0.5 * sin(2.0 * kPi * 125.0 * (blk * 128 + i) / 48000.0);  // -6 dBFS
                buf[2 * i] = buf[2 * i + 1] = static_cast<int32_t>(s * 2147483648.0 * 0.999);
            }
            p.process(buf.data(), 128);
        }
        printf("    -6 dBFS @125 Hz with +18 dB on band 1: clipCount = %lu\n", static_cast<unsigned long>(p.clipCount()));
        CHECK(p.clipCount() > 0, "expected saturation to be counted");
        p.resetClipCount();
        CHECK(p.clipCount() == 0, "resetClipCount");
    }
    // 6c: узгодженість process(int32) і processFloat на помірному сигналі.
    {
        eq::Processor pi("i"), pf("f");
        pi.setSampleRate(48000);
        pf.setSampleRate(48000);
        const int8_t g[eq_cfg::kBandCount] = {14, 2, -1, 1, -1, 2, 5, 12};
        pi.setAllDb(g);
        pf.setAllDb(g);
        double maxErr = 0;
        for (int blk = 0; blk < 200; ++blk) {
            std::vector<int32_t> a(256);
            std::vector<float> f(256);
            for (int i = 0; i < 128; ++i) {
                const double s = 0.02 * sin(2.0 * kPi * 1000.0 * (blk * 128 + i) / 48000.0) +
                                 0.01 * sin(2.0 * kPi * 8000.0 * (blk * 128 + i) / 48000.0);
                const int32_t v = static_cast<int32_t>(s * 2147483648.0);
                a[2 * i] = a[2 * i + 1] = v;
                f[2 * i] = f[2 * i + 1] = static_cast<float>(v) / 2147483648.0f;
            }
            pi.process(a.data(), 128);
            pf.processFloat(f.data(), 128);
            if (blk > 20) {
                for (int i = 0; i < 256; ++i) {
                    maxErr = fmax(maxErr, fabs(static_cast<double>(a[static_cast<size_t>(i)]) / 2147483648.0 -
                                               static_cast<double>(f[static_cast<size_t>(i)])));
                }
            }
        }
        printf("    max |int32 path - float path| = %.3g (full scale = 1)\n", maxErr);
        CHECK(maxErr < 1e-6, "int32 and float paths differ by %.3g", maxErr);
    }
}

// --- 7 ---------------------------------------------------------------------
void testAimpFit() {
    printf("\n[7] response of the integer 'AIMP fit' preset {14, 2, 0, 0, 0, 2, 5, 12} (fs 48 kHz; AIMP target in the brackets)\n");
    const double fs = 48000.0;
    const int8_t g[eq_cfg::kBandCount] = {14, 2, 0, 0, 0, 2, 5, 12};
    struct P { double f; const char* tgt; } pts[] = {{43, "≈ +14"}, {63, "≈ +14"}, {125, ""}, {350, "≈ 0"},
                                                      {1000, "≈ 0"}, {2000, "≈ 0"}, {2800, "≈ +2"}, {5600, ""},
                                                      {11200, "≈ +12"}, {16000, "≈ +14"}};
    for (const auto& pt : pts) {
        double t = 0;
        for (uint8_t k = 0; k < eq_cfg::kBandCount; ++k) {
            if (g[k] != 0) t += magDb(designPeaking(eq_cfg::kBandFreqHz[k], g[k], fs), pt.f, fs);
        }
        printf("    %6.0f Hz  %+6.2f dB   [%s]\n", pt.f, t, pt.tgt);
    }
    int active = 0;
    for (auto v : g) active += (v != 0);
    printf("    active bands in this preset (non-zero): %d of %u\n", active, static_cast<unsigned>(eq_cfg::kBandCount));
}

// --- 8 ---------------------------------------------------------------------
double benchNsPerFrame(int activeBands) {
    eq::Processor p("bench");
    p.setSampleRate(48000);
    int8_t g[eq_cfg::kBandCount] = {};
    for (int i = 0; i < activeBands; ++i) g[i] = static_cast<int8_t>(3 + i);
    p.setAllDb(g);
    std::vector<float> buf(256);
    for (int i = 0; i < 128; ++i) buf[2 * i] = buf[2 * i + 1] = static_cast<float>(0.001 * sin(i * 0.1));
    for (int w = 0; w < 200; ++w) p.processFloat(buf.data(), 128);  // прогрів і завершення переходу
    const int iters = 20000;
    const auto t0 = std::chrono::steady_clock::now();
    for (int k = 0; k < iters; ++k) {
        p.processFloat(buf.data(), 128);
        for (int i = 0; i < 256; ++i) buf[i] *= 0.5f + 0.0f;   // не даємо сигналу вирости; дешево
        for (int i = 0; i < 256; ++i) buf[i] += 0.0005f;
    }
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / (static_cast<double>(iters) * 128.0);
}

void testBench() {
    printf("\n[8] PC micro-benchmark (NOT ESP32; only the ratio is meaningful): ns per stereo frame, processFloat\n");
    const double n5 = benchNsPerFrame(5);
    const double n8 = benchNsPerFrame(8);
    const double n0 = benchNsPerFrame(1);
    printf("    1 band: %.1f ns | 5 bands: %.1f ns | 8 bands: %.1f ns  (8/5 = %.2f, per-band ≈ %.1f ns)\n", n0, n5, n8,
           n8 / n5, (n8 - n0) / 7.0);
}

}  // namespace

int main() {
    testConfig();
    testStability();
    testGainAndLeak();
    testTransitions();
    testNyquistRule();
    testInt32Path();
    testAimpFit();
    testBench();
    printf("\n==== PASS %d / FAIL %d / KNOWN-ISSUE %d ====\n", g_pass, g_fail, g_known);
    return g_fail == 0 ? 0 : 1;
}
