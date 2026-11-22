// Багатосмуговий еквалайзер (Prompt 30; P45 — екземпляри; P47d — processFloat; [E2a] 8 смуг: усе за eq_cfg::kBandCount;
// [E3] фасад processFloat + reportRadioBlock для float-ланцюга радіо).
// Опис і правила потоків — у eq.h.
//
// ВАЖЛИВО: файл НЕ включає Audio.h (як і vu_pcm_hook.cpp / output_trim.cpp). Залежності: математика,
// FreeRTOS-мʼютекс сеттерів і Serial-лог (лише при EQ_DEBUG).

#include "audio/eq.h"

#include <math.h>
#include <string.h>

#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#if EQ_DEBUG
#include <Arduino.h>
#define EQ_LOG(...) Serial.printf(__VA_ARGS__)
#else
#define EQ_LOG(...) \
    do {            \
    } while (0)
#endif

namespace {

constexpr uint8_t kN = eq_cfg::kBandCount;

// Найбільше float, що не перевищує int32 max (2147483647 у float не представляється).
constexpr float kOutMax = 2147483520.0f;
constexpr float kOutMin = -2147483648.0f;

// Одиничний біквад (0 дБ / смуга вимкнена): y = x.
constexpr eq::detail::Coef kIdentity = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};

int clampDb(int db) {
    if (db < eq_cfg::kGainMinDb) db = eq_cfg::kGainMinDb;
    if (db > eq_cfg::kGainMaxDb) db = eq_cfg::kGainMaxDb;
    return db;
}

#if EQ_DEBUG
// [E2a] Пресет одним рядком "a b c ..." (kBandCount значень) для логів: без літералів за кількістю смуг.
void formatGains(const int8_t* g, char* out, size_t cap) {
    if (cap == 0) {
        return;
    }
    out[0] = '\0';
    size_t pos = 0;
    for (uint8_t b = 0; b < kN; ++b) {
        const int n = snprintf(out + pos, cap - pos, b == 0 ? "%d" : " %d", static_cast<int>(g[b]));
        if (n < 0 || static_cast<size_t>(n) >= cap - pos) {
            break;  // не влізло: рядок обрізано (лише лог)
        }
        pos += static_cast<size_t>(n);
    }
}
#endif

// Peaking EQ (RBJ Audio EQ Cookbook). Рахується ЛИШЕ в сеттерах (контекст викликача), у double.
// У hot path (process) цієї функції немає.
eq::detail::Coef designPeaking(double f0, double dB, double fs) {
    const double kPi = 3.14159265358979323846;
    const double A = pow(10.0, dB / 40.0);
    const double w0 = 2.0 * kPi * f0 / fs;
    const double cw = cos(w0);
    const double alpha = sin(w0) / (2.0 * eq_cfg::kQ);
    const double a0 = 1.0 + alpha / A;
    eq::detail::Coef c;
    c.b0 = static_cast<float>((1.0 + alpha * A) / a0);
    c.b1 = static_cast<float>((-2.0 * cw) / a0);
    c.b2 = static_cast<float>((1.0 - alpha * A) / a0);
    c.a1 = static_cast<float>((-2.0 * cw) / a0);
    c.a2 = static_cast<float>((1.0 - alpha / A) / a0);
    return c;
}

// Насичення на вихід з лічильником. Округлення до найближчого.
inline int32_t toInt32(float y, uint32_t& clips) {
    if (y > kOutMax) {
        y = kOutMax;
        ++clips;
    } else if (y < kOutMin) {
        y = kOutMin;
        ++clips;
    }
    return static_cast<int32_t>(y >= 0.0f ? y + 0.5f : y - 0.5f);
}

// Довжина переходу коефіцієнтів у кадрах (мінімум 1).
uint32_t rampFramesFor(uint32_t rateHz) {
    const uint32_t f = rateHz * eq_cfg::kCoefRampMs / 1000u;
    return f > 0 ? f : 1;
}

eq::detail::Coef lerpCoef(const eq::detail::Coef& a, const eq::detail::Coef& b, float t) {
    eq::detail::Coef r;
    r.b0 = a.b0 + (b.b0 - a.b0) * t;
    r.b1 = a.b1 + (b.b1 - a.b1) * t;
    r.b2 = a.b2 + (b.b2 - a.b2) * t;
    r.a1 = a.a1 + (b.a1 - a.a1) * t;
    r.a2 = a.a2 + (b.a2 - a.a2) * t;
    return r;
}

}  // namespace

// ---------------------------------------------------------------------------
// Клас eq::Processor
// ---------------------------------------------------------------------------

eq::Processor::Processor(const char* tag)
    : tag_(tag),
      mutex_(nullptr),
      rate_(eq_cfg::kDefaultSampleRateHz),
      bypass_(false),
      clips_(0),
#if EQ_DEBUG
      dbgPeak_(0.0f),
#endif
      version_(0),
      seq_(0),
      appliedVer_(0),
      rampTotal_(0),
      rampDone_(0),
      bypassSeen_(false) {
    mutex_ = xSemaphoreCreateMutex();
    for (uint8_t b = 0; b < kN; ++b) {
        gains_[b] = 0;
        wantOn_[b] = 0;
        cur_[b] = kIdentity;
        from_[b] = kIdentity;
        to_[b] = kIdentity;
    }
    zeroStates();
    lock();
    publishLocked(true);  // початкова ціль: плоский пресет, миттєво
    unlock();
}

void eq::Processor::lock() const {
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
}

void eq::Processor::unlock() const {
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
}

// Розрахунок усіх цілей за поточним пресетом і fs; публікація через seqlock.
// Викликається ТІЛЬКИ з сеттерів, під мʼютексом.
void eq::Processor::publishLocked(bool snap) {
    detail::Target t{};
    ++version_;
    const uint32_t rate = rate_.load(std::memory_order_relaxed);
    for (uint8_t b = 0; b < kN; ++b) {
        const double f0 = static_cast<double>(eq_cfg::kBandFreqHz[b]);
        const bool on = (gains_[b] != 0) &&
                        (f0 <= eq_cfg::kMaxFreqRatio * static_cast<double>(rate));
        t.on[b] = on ? 1 : 0;
        t.c[b] = on ? designPeaking(f0, static_cast<double>(gains_[b]), static_cast<double>(rate))
                    : kIdentity;
    }
    t.snap = snap ? 1 : 0;
    t.version = version_;
    t.rateHz = rate;

    // Seqlock: непарний лічильник = запис триває.
    const uint32_t s = seq_.load(std::memory_order_relaxed);
    seq_.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    memcpy(&pub_, &t, sizeof(t));
    seq_.store(s + 2, std::memory_order_release);
}

// Копія опублікованої цілі. Повтор, якщо запис був у процесі (непарний лічильник або зміна).
void eq::Processor::readTarget(detail::Target& out) const {
    uint32_t s1;
    uint32_t s2;
    do {
        s1 = seq_.load(std::memory_order_acquire);
        if (s1 & 1u) {
            continue;
        }
        memcpy(&out, &pub_, sizeof(out));
        std::atomic_thread_fence(std::memory_order_acquire);
        s2 = seq_.load(std::memory_order_relaxed);
    } while ((s1 & 1u) || s1 != s2);
}

void eq::Processor::setSampleRate(uint32_t hz) {
    if (hz < eq_cfg::kMinSampleRateHz || hz > eq_cfg::kMaxSampleRateHz) {
        return;
    }
    // Швидкий шлях без мʼютекса: радіо викликає це з кожного Audio::loop().
    if (rate_.load(std::memory_order_relaxed) == hz) {
        return;
    }
    lock();
    if (rate_.load(std::memory_order_relaxed) != hz) {
        rate_.store(hz, std::memory_order_relaxed);
        publishLocked(true);  // нова fs: миттєва зміна коефіцієнтів, скидання стану
        EQ_LOG("[EQ] %s sample rate %lu Hz\n", tag_, static_cast<unsigned long>(hz));
    }
    unlock();
}

uint32_t eq::Processor::sampleRate() const {
    return rate_.load(std::memory_order_relaxed);
}

void eq::Processor::setBandDb(uint8_t band, int8_t db) {
    if (band >= kN) {
        return;
    }
    const int8_t v = static_cast<int8_t>(clampDb(db));
    lock();
    if (gains_[band] != v) {
        gains_[band] = v;
        publishLocked(false);
        EQ_LOG("[EQ] %s band %u (%u Hz) = %d dB\n", tag_, static_cast<unsigned>(band),
               static_cast<unsigned>(eq_cfg::kBandFreqHz[band]), static_cast<int>(v));
    }
    unlock();
}

int8_t eq::Processor::getBandDb(uint8_t band) const {
    if (band >= kN) {
        return 0;
    }
    lock();
    const int8_t v = gains_[band];
    unlock();
    return v;
}

void eq::Processor::setAllDb(const int8_t gains[eq_cfg::kBandCount]) {
    if (gains == nullptr) {
        return;
    }
    lock();
    bool changed = false;
    for (uint8_t b = 0; b < kN; ++b) {
        const int8_t v = static_cast<int8_t>(clampDb(gains[b]));
        if (gains_[b] != v) {
            gains_[b] = v;
            changed = true;
        }
    }
    if (changed) {
        publishLocked(false);
#if EQ_DEBUG
        {
            char s[kN * 5 + 1];  // «-18 » = 4 символи на смугу + нуль-термінатор
            formatGains(gains_, s, sizeof(s));
            EQ_LOG("[EQ] %s preset %s dB\n", tag_, s);
        }
#endif
    }
    unlock();
}

void eq::Processor::getAllDb(int8_t out[eq_cfg::kBandCount]) const {
    if (out == nullptr) {
        return;
    }
    lock();
    for (uint8_t b = 0; b < kN; ++b) {
        out[b] = gains_[b];
    }
    unlock();
}

void eq::Processor::setBypass(bool on) {
    bypass_.store(on, std::memory_order_relaxed);
}

bool eq::Processor::bypass() const {
    return bypass_.load(std::memory_order_relaxed);
}

uint32_t eq::Processor::clipCount() const {
    return clips_.load(std::memory_order_relaxed);
}

void eq::Processor::resetClipCount() {
    clips_.store(0, std::memory_order_relaxed);
}

void eq::Processor::addClips(uint32_t n) {
    if (n != 0) {
        clips_.fetch_add(n, std::memory_order_relaxed);
    }
}

#if EQ_DEBUG
float eq::Processor::debugTakePeak() {
    return dbgPeak_.exchange(0.0f, std::memory_order_relaxed);
}
#endif

void eq::Processor::zeroBand(uint8_t b) {
    z_[0][b][0] = z_[0][b][1] = 0.0f;
    z_[1][b][0] = z_[1][b][1] = 0.0f;
}

void eq::Processor::zeroStates() {
    memset(z_, 0, sizeof(z_));
}

void eq::Processor::reset() {
    zeroStates();
}

// Підготовка блоку (спільна для process / processFloat): bypass, нова ціль, інтерполяція коефіцієнтів,
// список активних смуг, локальні копії станів. false = блок не чіпати (бітова ідентичність).
bool eq::Processor::beginBlock(int32_t frames, detail::Block& b) {
    if (bypass_.load(std::memory_order_relaxed)) {
        bypassSeen_ = true;
        return false;  // bypass: блок не чіпаємо
    }
    if (bypassSeen_) {
        // Вихід з bypass: стан фільтрів застарів, починаємо з нуля.
        zeroStates();
        bypassSeen_ = false;
    }

    // 1. Нова ціль? Перехід починається з поточних (інтерпольованих) коефіцієнтів.
    detail::Target t;
    readTarget(t);
    if (t.version != appliedVer_) {
        appliedVer_ = t.version;
        for (uint8_t k = 0; k < kN; ++k) {
            wantOn_[k] = t.on[k];
        }
        if (t.snap) {
            for (uint8_t k = 0; k < kN; ++k) {
                cur_[k] = t.c[k];
            }
            rampTotal_ = 0;
            rampDone_ = 0;
            zeroStates();
        } else {
            for (uint8_t k = 0; k < kN; ++k) {
                from_[k] = cur_[k];
                to_[k] = t.c[k];
            }
            rampTotal_ = rampFramesFor(t.rateHz);
            rampDone_ = 0;
        }
    }

    // 2. Інтерполяція коефіцієнтів: один крок на блок (лінійно за кадрами переходу).
    if (rampTotal_ > 0) {
        rampDone_ += static_cast<uint32_t>(frames);
        if (rampDone_ >= rampTotal_) {
            rampTotal_ = 0;
            rampDone_ = 0;
            for (uint8_t k = 0; k < kN; ++k) {
                cur_[k] = to_[k];
                if (!wantOn_[k]) {
                    zeroBand(k);  // смуга згасла: стан обнуляємо, щоб не було «хвоста»
                }
            }
        } else {
            const float a = static_cast<float>(rampDone_) / static_cast<float>(rampTotal_);
            for (uint8_t k = 0; k < kN; ++k) {
                cur_[k] = lerpCoef(from_[k], to_[k], a);
            }
        }
    }

    // 3. Активні смуги: увімкнені або ще в переході (згасання).
    b.n = 0;
    for (uint8_t k = 0; k < kN; ++k) {
        if (wantOn_[k] || rampTotal_ > 0) {
            b.act[b.n++] = k;
        }
    }
    if (b.n == 0) {
        return false;  // усі смуги вимкнені: блок не чіпаємо (бітова ідентичність)
    }
    for (uint8_t i = 0; i < b.n; ++i) {
        const uint8_t k = b.act[i];
        b.c[i] = cur_[k];
        b.zl[i][0] = z_[0][k][0];
        b.zl[i][1] = z_[0][k][1];
        b.zr[i][0] = z_[1][k][0];
        b.zr[i][1] = z_[1][k][1];
    }
    return true;
}

void eq::Processor::endBlock(const detail::Block& b) {
    for (uint8_t i = 0; i < b.n; ++i) {
        const uint8_t k = b.act[i];
        z_[0][k][0] = b.zl[i][0];
        z_[0][k][1] = b.zl[i][1];
        z_[1][k][0] = b.zr[i][0];
        z_[1][k][1] = b.zr[i][1];
    }
}

// Гаряча частина (int32, радіо-хук). Викликається лише з задачі-власника.
void eq::Processor::process(int32_t* buf, int32_t frames) {
    if (buf == nullptr || frames <= 0) {
        return;
    }
    detail::Block bl;
    if (!beginBlock(frames, bl)) {
        return;
    }
    const uint8_t n = bl.n;

    uint32_t clips = 0;
#if EQ_DEBUG
    float peak = 0.0f;  // [E1] макс. |відлік| ПЕРЕД насиченням у цьому блоці
#endif
    int32_t* p = buf;
    for (int32_t f = 0; f < frames; ++f) {
        float xl = static_cast<float>(p[0]);
        float xr = static_cast<float>(p[1]);
        for (uint8_t i = 0; i < n; ++i) {
            const detail::Coef& k = bl.c[i];
            const float yl = k.b0 * xl + bl.zl[i][0];
            bl.zl[i][0] = k.b1 * xl - k.a1 * yl + bl.zl[i][1];
            bl.zl[i][1] = k.b2 * xl - k.a2 * yl;
            xl = yl;
            const float yr = k.b0 * xr + bl.zr[i][0];
            bl.zr[i][0] = k.b1 * xr - k.a1 * yr + bl.zr[i][1];
            bl.zr[i][1] = k.b2 * xr - k.a2 * yr;
            xr = yr;
        }
#if EQ_DEBUG
        {
            const float al = fabsf(xl);
            const float ar = fabsf(xr);
            if (al > peak) peak = al;
            if (ar > peak) peak = ar;
        }
#endif
        p[0] = toInt32(xl, clips);
        p[1] = toInt32(xr, clips);
        p += 2;
    }

    endBlock(bl);
    if (clips != 0) {
        clips_.fetch_add(clips, std::memory_order_relaxed);
    }
#if EQ_DEBUG
    // Єдина задача-власник пише максимум; звітувач лише скидає його (exchange) — для діагностики цього досить.
    if (peak > dbgPeak_.load(std::memory_order_relaxed)) {
        dbgPeak_.store(peak, std::memory_order_relaxed);
    }
#endif
}

// [P47d] Те саме ядро на float-буфері, без перетворення й насичення.
void eq::Processor::processFloat(float* buf, int32_t frames) {
    if (buf == nullptr || frames <= 0) {
        return;
    }
    detail::Block bl;
    if (!beginBlock(frames, bl)) {
        return;
    }
    const uint8_t n = bl.n;

    float* p = buf;
    for (int32_t f = 0; f < frames; ++f) {
        float xl = p[0];
        float xr = p[1];
        for (uint8_t i = 0; i < n; ++i) {
            const detail::Coef& k = bl.c[i];
            const float yl = k.b0 * xl + bl.zl[i][0];
            bl.zl[i][0] = k.b1 * xl - k.a1 * yl + bl.zl[i][1];
            bl.zl[i][1] = k.b2 * xl - k.a2 * yl;
            xl = yl;
            const float yr = k.b0 * xr + bl.zr[i][0];
            bl.zr[i][0] = k.b1 * xr - k.a1 * yr + bl.zr[i][1];
            bl.zr[i][1] = k.b2 * xr - k.a2 * yr;
            xr = yr;
        }
        p[0] = xl;
        p[1] = xr;
        p += 2;
    }

    endBlock(bl);
}

// ---------------------------------------------------------------------------
// Фасад: глобальний екземпляр радіо
// ---------------------------------------------------------------------------

namespace {
eq::Processor g_radio{"radio"};
}  // namespace

void eq::setBandDb(uint8_t band, int8_t db) {
    g_radio.setBandDb(band, db);
}

int8_t eq::getBandDb(uint8_t band) {
    return g_radio.getBandDb(band);
}

void eq::setAllDb(const int8_t gains[eq_cfg::kBandCount]) {
    g_radio.setAllDb(gains);
}

void eq::getAllDb(int8_t out[eq_cfg::kBandCount]) {
    g_radio.getAllDb(out);
}

#if EQ_DEBUG
namespace {

// [E2a]/[E3] Статистика хука радіо за період звіту. Пише ЛИШЕ хук (reportRadioBlock, задача бібліотеки, ядро 1), звіт
// друкує задача плеєра (інша задача) — тому атомарні лічильники без мʼютекса; значення беруться exchange().
// Зайві інструкції — лише при EQ_DEBUG = 1; у релізі цього немає.
std::atomic<uint32_t> g_procCalls{0};
std::atomic<uint32_t> g_procFrames{0};
std::atomic<uint32_t> g_procEqSumUs{0};
std::atomic<uint32_t> g_procEqMaxUs{0};
std::atomic<uint32_t> g_procChainSumUs{0};
std::atomic<uint32_t> g_procChainMaxUs{0};
std::atomic<float>    g_peakEq{0.0f};     // макс. |відлік| на виході EQ (шкала int32)
std::atomic<float>    g_peakFinal{0.0f};  // макс. |відлік| після гучності, перед фінальним насиченням

constexpr float kFullScaleF = 2147483648.0f;  // 2^31 = 0 дБFS

// Максимум єдиного писача (хук); звітувач лише скидає (exchange).
inline void storeMax(std::atomic<uint32_t>& a, uint32_t v) {
    if (v > a.load(std::memory_order_relaxed)) {
        a.store(v, std::memory_order_relaxed);
    }
}
inline void storeMax(std::atomic<float>& a, float v) {
    if (v > a.load(std::memory_order_relaxed)) {
        a.store(v, std::memory_order_relaxed);
    }
}

float toDbFs(float peak) {
    return (peak > 0.0f) ? 20.0f * log10f(peak / kFullScaleF) : -200.0f;
}

// [E1] Звіт радіо-EQ. Викликається з eq::setSampleRate() (задача плеєра, НЕ з хука): не частіше за
// eq_cfg::kDebugReportMs. Максимуми скидаються щоразу, тож peak стосується саме періоду звіту.
// [E2a] Рядок \"[EQ] radio proc: ...\" друкується щоразу, коли за період були виклики хука.
// [E3] eq = сумарний час eq::processFloat() за ОДИН виклик хука (порівнювати зі старим eq::process() на ≈ 256
// кадрів: ≈ 800 мкс), chain = час усього хука. \"[EQ] radio sat\" тепер про ФІНАЛЬНЕ насичення (після гучності);
// друкується, якщо були насичення АБО пік на виході EQ перевищив 0 дБFS (тоді видно запас, який раніше обрізався).
void debugReportRadio() {
    static uint32_t s_lastMs = 0;
    static uint32_t s_lastClips = 0;
    const uint32_t now = millis();
    if (now - s_lastMs < eq_cfg::kDebugReportMs) {
        return;
    }
    const uint32_t periodMs = now - s_lastMs;
    s_lastMs = now;

    const uint32_t calls = g_procCalls.exchange(0, std::memory_order_relaxed);
    const uint32_t frames = g_procFrames.exchange(0, std::memory_order_relaxed);
    const uint32_t eqSum = g_procEqSumUs.exchange(0, std::memory_order_relaxed);
    const uint32_t eqMax = g_procEqMaxUs.exchange(0, std::memory_order_relaxed);
    const uint32_t chSum = g_procChainSumUs.exchange(0, std::memory_order_relaxed);
    const uint32_t chMax = g_procChainMaxUs.exchange(0, std::memory_order_relaxed);
    if (calls != 0) {
        int8_t g[kN];
        g_radio.getAllDb(g);
        unsigned nz = 0;
        for (uint8_t b = 0; b < kN; ++b) {
            if (g[b] != 0) {
                ++nz;
            }
        }
        Serial.printf("[EQ] radio proc: %lu calls in %lu ms, eq avg %lu us max %lu us per call, chain avg %lu us max %lu us per call, "
                      "avg %lu frames/call, %u of %u bands non-zero, fs %lu Hz\n",
                      static_cast<unsigned long>(calls), static_cast<unsigned long>(periodMs),
                      static_cast<unsigned long>(eqSum / calls), static_cast<unsigned long>(eqMax),
                      static_cast<unsigned long>(chSum / calls), static_cast<unsigned long>(chMax),
                      static_cast<unsigned long>(frames / calls), nz, static_cast<unsigned>(kN),
                      static_cast<unsigned long>(g_radio.sampleRate()));
    }

    const uint32_t clips = g_radio.clipCount();
    const float peakEq = g_peakEq.exchange(0.0f, std::memory_order_relaxed);
    const float peakFin = g_peakFinal.exchange(0.0f, std::memory_order_relaxed);
    const uint32_t delta = clips - s_lastClips;
    s_lastClips = clips;
    if (delta == 0 && !(peakEq > kFullScaleF)) {
        return;
    }
    Serial.printf("[EQ] radio sat: +%lu samples in %lu ms (total %lu), peak after EQ %+.1f dBFS, final peak before saturation %+.1f dBFS\n",
                  static_cast<unsigned long>(delta), static_cast<unsigned long>(periodMs),
                  static_cast<unsigned long>(clips), static_cast<double>(toDbFs(peakEq)),
                  static_cast<double>(toDbFs(peakFin)));
}

}  // namespace
#endif

void eq::setSampleRate(uint32_t hz) {
    g_radio.setSampleRate(hz);
#if EQ_DEBUG
    debugReportRadio();
#endif
}

uint32_t eq::sampleRate() {
    return g_radio.sampleRate();
}

void eq::process(int32_t* buf, int32_t frames) {
    g_radio.process(buf, frames);
}

// [E3] Float-шлях радіо: без перетворення й насичення (його робить хук після гучності).
void eq::processFloat(float* buf, int32_t frames) {
    g_radio.processFloat(buf, frames);
}

// [E3] Підсумок виклику хука: фінальні насичення в лічильник; при EQ_DEBUG — статистика для звіту (без printf).
void eq::reportRadioBlock(const RadioBlockReport& r) {
    g_radio.addClips(r.clips);
#if EQ_DEBUG
    g_procCalls.fetch_add(1, std::memory_order_relaxed);
    g_procFrames.fetch_add(r.frames, std::memory_order_relaxed);
    g_procEqSumUs.fetch_add(r.eqUs, std::memory_order_relaxed);
    g_procChainSumUs.fetch_add(r.chainUs, std::memory_order_relaxed);
    storeMax(g_procEqMaxUs, r.eqUs);
    storeMax(g_procChainMaxUs, r.chainUs);
    storeMax(g_peakEq, r.peakEq);
    storeMax(g_peakFinal, r.peakFinal);
#endif
}

// [E1]/[E3] Лічильник насичень радіо (тепер фінальних, після гучності): див. eq.h.
uint32_t eq::clipCount() {
    return g_radio.clipCount();
}

void eq::resetClipCount() {
    g_radio.resetClipCount();
}

