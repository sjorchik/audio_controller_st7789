// Перехоплення PCM (гарячий шлях аудіо, ядро 1): VU-метр (Prompt 17), рівень виходу
// декодера по станціях (Prompt 25), еквалайзер радіо (Prompt 30) і ЦИФРОВА ГУЧНІСТЬ радіо (Prompt 47a).
//
// ВАЖЛИВО: цей файл НЕ включає Audio.h. Там audio_process_i2s оголошено як
// extern __attribute__((weak)); визначення, що йде після такого оголошення, теж стає
// слабким, і лінкер залишав порожню 5-байтову заглушку з Audio.cpp (хук не викликався).
// Без цього оголошення наше визначення сильне й перекриває слабку заглушку.
//
// Сигнатуру звірено з Audio.h/Audio.cpp збірки (символ _Z17audio_process_i2sPlsPb):
//   void audio_process_i2s(int32_t* outBuff, int16_t validSamples, bool* continueI2S)
// Викликається з Audio::playChunk() ПІСЛЯ гучності/gain бібліотеки і ПЕРЕД i2s_channel_write().
//   - outBuff — інтерліврований стерео L,R,L,R...; кожне слово int32 = один відлік
//     каналу, 16-бітні дані вирівняні вліво (повна шкала 2^31);
//   - validSamples — кількість СЛІВ int32 (обох каналів разом): кадрів = /2;
//   - *continueI2S = true ОБОВʼЯЗКОВО: false = бібліотека пропускає запис блоку -> тиша.
//
// [E3] ПОРЯДОК ОБРОБКИ БЛОКУ (float до самого виходу; порядок і параметри ті самі, що були):
//   int32 -> float (точно: 16-бітні дані вирівняні вліво) -> eq::processFloat -> вимір VU (на значеннях,
//   обмежених до діапазону int16) -> output_trim::processFloat (рівень станції) -> radio_volume::processFloat
//   (гучність, баланс, мʼют, ramp) -> ОДНЕ округлення / насичення в int32 -> outBuff.
// До E3 EQ насичував результат в int32 ОДРАЗУ (до рівня станції й гучності): буст на смузі обрізав гучний трек,
// навіть якщо далі сигнал послаблювався. Тепер проміжний сигнал може бути > 0 дБFS і не обрізається; насичення
// (і його лічильник eq::clipCount()) — лише на виході, як у line_path.
//
// Блок ділиться на проходи по radio_chain_cfg::kChunkFrames кадрів через ОДИН статичний float-буфер (не стек: у
// задачі бібліотеки мінімум вільного стека ≈ 1 КБ). Стан EQ / гучності / рівня станції зберігається між
// проходами, тож результат не залежить від розбиття (крім дрібнішого кроку інтерполяції коефіцієнтів EQ).
// Хук викликається з ОДНІЄЇ задачі (PeriodicTask бібліотеки, ядро 1): статичний буфер без мʼютекса.
// Без логування й блокувань; millis() немає (при EQ_DEBUG = 1 лише esp_timer_get_time()).
//
// [VU1] VU_MEASURE_MODE == 1 (RMS, за замовчуванням): замість піка за блок міряється СУМА КВАДРАТІВ окремо для
// L і R у шкалі int16 (відлік >> 16, float-множення-додавання, без double / sqrtf / log10f / malloc) і
// публікується VuSourceDecodedPcm::publishEnergy(sumL, sumR, frames). Корінь, дБ і усереднення за інтервал між
// кадрами UI робить display-задача. VU_MEASURE_MODE == 0 — піковий шлях.
// [E3] VU міряється, як і раніше, ПІСЛЯ EQ і ДО рівня станції. Значення спершу округлюються й обмежуються до
// діапазону int32 (ЛИШЕ для вимірювання; сигнал далі не обрізається), тому VU бачить те саме, що бачив до E3
// (бітово, де EQ не насичував), а шкала не виходить за 0 дБFS при бусті.
//
// [Prompt 47a] Без хука гучності радіо НЕ керується (повна шкала ЦАП на підсилювач), тому
// VU_PCM_HOOK_STYLE != 1 -> #error (раніше #warning). Компілюється незалежно від ENABLE_VU
// (публікація рівнів — лише при ENABLE_VU).

#include <stdint.h>
#include <math.h>

#include "audio/eq.h"
#include "audio/output_trim.h"
#include "audio/radio_volume.h"
#include "audio/vu_source.h"
#include "config/features.h"
#include "config/radio_chain_config.h"
#include "config/vu_config.h"

#if EQ_DEBUG
#include <esp_timer.h>  // esp_timer_get_time(): вимір вартості етапів (лише EQ_DEBUG)
#endif

#if VU_PCM_HOOK_STYLE != 1
#error "VU_PCM_HOOK_STYLE must be 1: audio_process_i2s hook carries radio digital volume (P47a), station level (P25) and EQ (P30)"
#endif

namespace {

// Найбільше float, що не перевищує int32 max (2147483647 у float не представляється).
constexpr float kOutMax = 2147483520.0f;
constexpr float kOutMin = -2147483648.0f;

constexpr int32_t kChunk = radio_chain_cfg::kChunkFrames;

// Єдиний проміжний буфер float (стерео L,R,...). Статичний: ні стек, ні куча.
float s_buf[kChunk * 2];

// Округлення до найближчого з насиченням БЕЗ лічильника (для VU).
inline int32_t roundSat(float y) {
    if (y > kOutMax) {
        y = kOutMax;
    } else if (y < kOutMin) {
        y = kOutMin;
    }
    return static_cast<int32_t>(y >= 0.0f ? y + 0.5f : y - 0.5f);
}

// Фінальне округлення з насиченням і лічильником (ЄДИНЕ насичення ланцюга радіо).
inline int32_t finalSat(float y, uint32_t& clips) {
    if (y > kOutMax) {
        y = kOutMax;
        ++clips;
    } else if (y < kOutMin) {
        y = kOutMin;
        ++clips;
    }
    return static_cast<int32_t>(y >= 0.0f ? y + 0.5f : y - 0.5f);
}

}  // namespace

void audio_process_i2s(int32_t* outBuff, int16_t validSamples, bool* continueI2S) {
    if (continueI2S != nullptr) {
        *continueI2S = true;
    }
    if (outBuff == nullptr || validSamples < 2) {
        return;
    }
    const int32_t frames = static_cast<int32_t>(validSamples) / 2;

#if EQ_DEBUG
    const int64_t tStart = esp_timer_get_time();
    int64_t eqTicks = 0;
    float peakEq = 0.0f;
    float peakFinal = 0.0f;
#endif
#if ENABLE_VU
    // [Prompt 35] VU міряється ПІСЛЯ еквалайзера, але ДО рівня станції і гучності користувача:
    // 0 дБ індикатора = повна шкала сигналу декодера.
    int32_t scanLeft = frames;
    if (scanLeft > vu_cfg::kMaxFramesPerBlock) {
        scanLeft = vu_cfg::kMaxFramesPerBlock;  // захист від виходу за буфер
    }
    const int32_t scanTotal = scanLeft;
#if VU_MEASURE_MODE == 1
    float sumSqL = 0.0f;
    float sumSqR = 0.0f;
#else
    uint32_t peakL = 0;
    uint32_t peakR = 0;
#endif
#endif  // ENABLE_VU
    uint32_t clips = 0;

    int32_t* dst = outBuff;
    int32_t left = frames;
    while (left > 0) {
        const int32_t n = left < kChunk ? left : kChunk;
        const int32_t words = n * 2;

        // 1. int32 -> float (шкала зберігається: 2^31 = 0 дБFS; 16-бітні дані вирівняні вліво -> перетворення точне).
        for (int32_t i = 0; i < words; ++i) {
            s_buf[i] = static_cast<float>(dst[i]);
        }

        // 2. Еквалайзер радіо у float, без насичення (усі смуги 0 дБ — блок не чіпається).
#if EQ_DEBUG
        const int64_t tEq0 = esp_timer_get_time();
#endif
        eq::processFloat(s_buf, n);
#if EQ_DEBUG
        eqTicks += esp_timer_get_time() - tEq0;
        for (int32_t i = 0; i < words; ++i) {
            const float a = fabsf(s_buf[i]);
            if (a > peakEq) peakEq = a;
        }
#endif

#if ENABLE_VU
        // 3. VU на значеннях, обмежених до діапазону int32 (як було), у шкалі int16 (>> 16).
        if (scanLeft > 0) {
            const int32_t scan = n < scanLeft ? n : scanLeft;
            const float* q = s_buf;
#if VU_MEASURE_MODE == 1
            // [VU1] RMS: сума квадратів у шкалі int16 (|відлік >> 16| <= 32768, квадрат <= 2^30, точний у float).
            for (int32_t i = 0; i < scan; ++i) {
                const float l = static_cast<float>(roundSat(q[0]) >> 16);
                const float r = static_cast<float>(roundSat(q[1]) >> 16);
                q += 2;
                sumSqL += l * l;
                sumSqR += r * r;
            }
#else
            for (int32_t i = 0; i < scan; ++i) {
                const int32_t l = roundSat(q[0]);
                const int32_t r = roundSat(q[1]);
                q += 2;
                // Модуль без UB для INT32_MIN: 0u - (uint32_t)l = 2^31.
                const uint32_t al = (l < 0) ? (0u - static_cast<uint32_t>(l)) : static_cast<uint32_t>(l);
                const uint32_t ar = (r < 0) ? (0u - static_cast<uint32_t>(r)) : static_cast<uint32_t>(r);
                if (al > peakL) peakL = al;
                if (ar > peakR) peakR = ar;
            }
#endif  // VU_MEASURE_MODE
            scanLeft -= scan;
        }
#endif  // ENABLE_VU

        // 4. [Prompt 25] Рівень станції (float, той самий коефіцієнт і ramp).
        output_trim::processFloat(s_buf, n);
        // 5. [Prompt 47a] Гучність користувача, баланс і мʼют: останній крок ланцюга (float, без насичення).
        radio_volume::processFloat(s_buf, n);

        // 6. ЄДИНЕ округлення / насичення в int32 (+ лічильник насичень).
        for (int32_t i = 0; i < words; ++i) {
            const float y = s_buf[i];
#if EQ_DEBUG
            const float a = fabsf(y);
            if (a > peakFinal) peakFinal = a;
#endif
            dst[i] = finalSat(y, clips);
        }

        dst += words;
        left -= n;
    }

#if ENABLE_VU
#if VU_MEASURE_MODE == 1
    // [VU1] Енергія блоку (шкала int16²) — одна публікація на блок.
    VuSourceDecodedPcm::publishEnergy(sumSqL, sumSqR, static_cast<uint32_t>(scanTotal));
#else
    // 2^31 -> 2^15: шкала vu_cfg::kFullScale (32768).
    VuSourceDecodedPcm::publishPeaks(peakL >> 16, peakR >> 16, static_cast<uint32_t>(scanTotal));
#endif  // VU_MEASURE_MODE
#endif  // ENABLE_VU

    // [E3] Підсумок для eq::clipCount() і (при EQ_DEBUG) звіту; без printf і блокувань.
    eq::RadioBlockReport rep{};
    rep.clips = clips;
    rep.frames = static_cast<uint32_t>(frames);
#if EQ_DEBUG
    rep.peakEq = peakEq;
    rep.peakFinal = peakFinal;
    rep.eqUs = eqTicks > 0 ? static_cast<uint32_t>(eqTicks) : 0u;
    const int64_t tAll = esp_timer_get_time() - tStart;
    rep.chainUs = tAll > 0 ? static_cast<uint32_t>(tAll) : 0u;
#endif
    eq::reportRadioBlock(rep);
}
