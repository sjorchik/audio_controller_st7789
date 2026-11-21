#pragma once

// Константи багатосмугового еквалайзера (Prompt 30; P45 — екземпляри, плавні коефіцієнти; [E2a] 8 смуг).
// Еквалайзер — цифровий каскад peaking-biquad (RBJ) у хуку audio_process_i2s
// (audio/vu_pcm_hook.cpp) для декодованого потоку радіо, а також у проході line_path (P45).
// Зовнішні входи (TV Box / Computer / Aux) через радіо-хук не проходять (аналоговий тракт).
// Пресет EQ — окремий для кожного входу (Settings::profiles[i].eqGainsDb). Піни й I2S цей модуль не чіпає.

#include <stdint.h>

// ---------------------------------------------------------------------------
// Прапорці умовної компіляції
// ---------------------------------------------------------------------------

// Serial-лог еквалайзера ("[EQ] ..."): зміни смуг, пресету, частоти дискретизації.
// [E1] Те саме для вимірювання насичення в хуку радіо: process() додатково запамʼятовує найбільший
// модуль відліку ПЕРЕД насиченням, а звіт "[EQ] radio sat: ..." друкується не з хука, а з
// eq::setSampleRate() (задача плеєра), не частіше за eq_cfg::kDebugReportMs і лише коли були насичення.
// У гарячому шляху (process) логів немає. У релізі 0 (прибирає й додаткові обчислення в process()).
#define EQ_DEBUG 0

// 1 = задача плеєра після кожного Audio::loop() читає Audio::getSampleRate() і передає її в
// eq::setSampleRate() (частота потоку змінна: 44.1 / 48 / 32 кГц ...).
// 0 = частота лишається eq_cfg::kDefaultSampleRateHz.
#define EQ_SAMPLE_RATE_FROM_LIB 1

namespace eq_cfg {

// ---------------------------------------------------------------------------
// Смуги
// ---------------------------------------------------------------------------
// [E2a] 8 смуг (було 5). Усе, що залежить від кількості смуг (профіль входу й NVS, AppController, /api/eq,
// line_path, екран «EQ + Balance»), бере її звідси: без літералів 5 / 6 / 8 / 9 в інших файлах.
constexpr uint8_t kBandCount = 8;

// Центральні частоти, Гц (строго за зростанням; інтервал ≈ 1,3–1,5 октави). Підібрано під криву AIMP
// (МНК-підгонка peaking RBJ, Q = 1,0, fs 48 кГц).
constexpr uint16_t kBandFreqHz[kBandCount] = {50, 125, 350, 1000, 2800, 5600, 10000, 15000};

// Підсилення смуги, дБ (цілі кроки). 0 дБ = смуга прозора (identity-біквад).
// [E1] ЄДИНИЙ параметр діапазону — kGainRangeDb (симетричний, ± дБ). Від нього залежать: clamp у сеттерах
// eq::Processor, sanitize() профілів у NVS, перевірки /api/eq (поля minDb / maxDb / min / max у відповідях),
// LinePath (lp.eq*), AppController (екран EQ, IR, енкодер), масштаб стовпчика екрана «EQ + Balance».
// Було ±12; ±18 обрано за аудитом насичення (див. звіт E1). Формат NVS (int8_t) не міняється.
constexpr int kGainRangeDb = 18;
constexpr int kGainMinDb  = -kGainRangeDb;
constexpr int kGainMaxDb  = kGainRangeDb;
constexpr int kGainStepDb = 1;

// Добротність peaking-фільтра.
constexpr double kQ = 1.0;

// Тривалість переходу коефіцієнтів після зміни пресету або частоти дискретизації, мс.
// Коефіцієнти інтерполюються лінійно (на кожен блок); стан фільтрів не скидається.
// Значення 20 мс — компроміс між зникненням клацань і помітною затримкою зміни смуги.
constexpr uint32_t kCoefRampMs = 20;

// [E1] Період звіту про насичення радіо-EQ (лише EQ_DEBUG = 1), мс.
constexpr uint32_t kDebugReportMs = 1000;

// ---------------------------------------------------------------------------
// Частота дискретизації потоку
// ---------------------------------------------------------------------------
// Використовується, доки плеєр не повідомив справжню (і якщо EQ_SAMPLE_RATE_FROM_LIB == 0).
constexpr uint32_t kDefaultSampleRateHz = 44100;

// Значення поза цими межами ігноруються.
constexpr uint32_t kMinSampleRateHz = 8000;
constexpr uint32_t kMaxSampleRateHz = 96000;

// Смуга з центром вище kMaxFreqRatio * fs вимикається (біля Найквіста АЧХ біквада спотворюється).
// Правило однакове для ВСІХ смуг (eq.cpp, publishLocked): [E2a] 10 кГц працює від fs >= 22,2 кГц,
// 15 кГц — від fs >= 33,3 кГц (тобто при fs = 32 кГц смуга 15 кГц вимкнена, при 44,1 / 48 кГц усі 8 працюють).
constexpr double kMaxFreqRatio = 0.45;

// Частоти смуг строго зростають і додатні (перевірка в compile-time для будь-якого kBandCount).
constexpr bool bandsAscending() {
    if (kBandFreqHz[0] == 0) {
        return false;
    }
    for (uint8_t i = 1; i < kBandCount; ++i) {
        if (kBandFreqHz[i - 1] >= kBandFreqHz[i]) {
            return false;
        }
    }
    return true;
}

static_assert(kBandCount >= 1, "need at least one band");
static_assert(kGainMinDb < 0 && kGainMaxDb > 0, "range must contain 0 dB");
static_assert(kGainMinDb == -kGainMaxDb, "range must be symmetric (EQ screen scale and web sliders assume it)");
static_assert(kGainMinDb >= -127 && kGainMaxDb <= 127, "gain must fit int8_t");
static_assert(kGainStepDb >= 1, "step must be positive");
static_assert((kGainMaxDb - kGainMinDb) % kGainStepDb == 0, "range must be a multiple of step");
static_assert(bandsAscending(), "band frequencies must be positive and strictly ascending");
static_assert(kBandCount <= 16, "EQ per-block state (eq::detail::Block) is sized for a small band count");
static_assert(static_cast<double>(kBandFreqHz[kBandCount - 1]) <= kMaxFreqRatio * 48000.0,
              "the top band must be active at the fixed 48 kHz line path rate");
static_assert(kDebugReportMs >= 100, "debug report period too short");
static_assert(kQ > 0.1 && kQ < 10.0, "Q out of sane range");
static_assert(kCoefRampMs >= 1 && kCoefRampMs <= 500, "coefficient ramp out of range");
static_assert(kMinSampleRateHz > 0 && kMinSampleRateHz < kMaxSampleRateHz, "sample rate range");
static_assert(kDefaultSampleRateHz >= kMinSampleRateHz && kDefaultSampleRateHz <= kMaxSampleRateHz,
              "default sample rate must be inside the range");
static_assert(kMaxFreqRatio > 0.0 && kMaxFreqRatio < 0.5, "ratio must be below Nyquist");

}  // namespace eq_cfg
