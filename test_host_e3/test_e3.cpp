// Хост-тест E3 (лише ПК, не збірка прошивки): float-ланцюга радіо в audio/vu_pcm_hook.cpp.
//
// Збірка (з кореня репозиторію; stubs ПЕРЕД src, щоб підхопити заглушки відсутніх у промпті заголовків):
//   g++ -std=c++17 -O2 -Wall -Wextra -I test_host_e3/stubs -I src test_host_e3/test_e3.cpp src/audio/eq.cpp
//       src/audio/dsp_volume.cpp src/audio/output_trim.cpp src/audio/radio_volume.cpp src/audio/vu_pcm_hook.cpp
//       -o test_host_e3/test_e3 && test_host_e3/test_e3
// або sh test_host_e3/run.sh.
//
// Перевіряється РЕАЛЬНИЙ код прошивки (eq / dsp_volume / output_trim / radio_volume / хук). Еталон — ланцюг у double
// (RBJ peaking + DF2T, множення на коефіцієнти рівня станції й гучності, одне насичення). «Старий ланцюг» —
// eq::Processor::process() (насичення в int32 одразу після EQ) -> output_trim::process -> DspVolume::process.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "audio/dsp_volume.h"
#include "audio/eq.h"
#include "audio/output_trim.h"
#include "audio/radio_volume.h"
#include "audio/vu_source.h"
#include "config/eq_config.h"

extern void audio_process_i2s(int32_t* outBuff, int16_t validSamples, bool* continueI2S);

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = 48000.0;
constexpr double kMax = 2147483520.0;   // як kOutMax у прошивці
constexpr double kMin = -2147483648.0;
constexpr int32_t kLsb16 = 65536;       // 1 LSB int16 у шкалі int32 (дані вирівняні вліво)
constexpr int kBands = eq_cfg::kBandCount;

int g_fail = 0;
int g_pass = 0;

void check(bool ok, const char* name, const char* fmt = nullptr, double a = 0.0, double b = 0.0) {
    if (ok) {
        ++g_pass;
        printf("  PASS  %s", name);
    } else {
        ++g_fail;
        printf("  FAIL  %s", name);
    }
    if (fmt != nullptr) {
        printf("  (");
        printf(fmt, a, b);
        printf(")");
    }
    printf("\n");
}

// ---------------------------------------------------------------- еталон (double)
struct RefBand {
    double b0, b1, b2, a1, a2;
    double zl[2] = {0, 0}, zr[2] = {0, 0};
};

// roundCoefToFloat = true: коефіцієнти округлюються до float, як у прошивці (designPeaking у double -> float).
// Тоді різниця «прошивка − еталон» — лише арифметика float у станах і в множенні, тобто саме те, що міг би
// змінити E3. false: чисті double-коефіцієнти (різниця включає власну похибку квантування коефіцієнтів float
// у eq.cpp, яка від E3 не залежить і однакова в старому й новому ланцюзі).
std::vector<RefBand> makeRef(const int8_t* gains, bool roundCoefToFloat) {
    std::vector<RefBand> v;
    for (int b = 0; b < kBands; ++b) {
        if (gains[b] == 0) {
            continue;
        }
        const double f0 = eq_cfg::kBandFreqHz[b];
        const double A = pow(10.0, gains[b] / 40.0);
        const double w0 = 2.0 * kPi * f0 / kFs;
        const double cw = cos(w0);
        const double alpha = sin(w0) / (2.0 * eq_cfg::kQ);
        const double a0 = 1.0 + alpha / A;
        RefBand r;
        r.b0 = (1.0 + alpha * A) / a0;
        r.b1 = (-2.0 * cw) / a0;
        r.b2 = (1.0 - alpha * A) / a0;
        r.a1 = (-2.0 * cw) / a0;
        r.a2 = (1.0 - alpha / A) / a0;
        if (roundCoefToFloat) {
            r.b0 = static_cast<float>(r.b0);
            r.b1 = static_cast<float>(r.b1);
            r.b2 = static_cast<float>(r.b2);
            r.a1 = static_cast<float>(r.a1);
            r.a2 = static_cast<float>(r.a2);
        }
        v.push_back(r);
    }
    return v;
}

inline double refStep(std::vector<RefBand>& bands, double x, bool left) {
    for (auto& r : bands) {
        double* z = left ? r.zl : r.zr;
        const double y = r.b0 * x + z[0];
        z[0] = r.b1 * x - r.a1 * y + z[1];
        z[1] = r.b2 * x - r.a2 * y;
        x = y;
    }
    return x;
}

// ---------------------------------------------------------------- сигнал
struct Tone {
    double ampL, freqL, phL;
    double ampR, freqR, phR;
};

// Дані як у бібліотеці: int16, вирівняні вліво в int32.
std::vector<int32_t> makeSignal(const Tone& t, int frames, int startFrame = 0) {
    std::vector<int32_t> v(static_cast<size_t>(frames) * 2);
    for (int i = 0; i < frames; ++i) {
        const double n = startFrame + i;
        const double l = t.ampL * 32767.0 * sin(2.0 * kPi * t.freqL * n / kFs + t.phL);
        const double r = t.ampR * 32767.0 * sin(2.0 * kPi * t.freqR * n / kFs + t.phR);
        v[2 * i] = static_cast<int32_t>(llround(l)) * 65536;
        v[2 * i + 1] = static_cast<int32_t>(llround(r)) * 65536;
    }
    return v;
}

// ---------------------------------------------------------------- «старий» ланцюг (до E3)
eq::Processor g_oldEq("old");
DspVolume g_oldVol;
// Еталон, що ізолює E3: той самий float-EQ прошивки (processFloat), далі множення й насичення в double.
eq::Processor g_refEq("ref");

void oldChain(int32_t* buf, int frames, double* sumL = nullptr, double* sumR = nullptr) {
    g_oldEq.process(buf, frames);  // насичення в int32 ОДРАЗУ після EQ
    if (sumL != nullptr) {
        double sl = 0, sr = 0;
        const int32_t* p = buf;
        for (int i = 0; i < frames; ++i) {
            const double l = static_cast<double>(p[0] >> 16);
            const double r = static_cast<double>(p[1] >> 16);
            p += 2;
            sl += l * l;
            sr += r * r;
        }
        *sumL = sl;
        *sumR = sr;
    }
    output_trim::process(buf, frames);
    g_oldVol.process(buf, frames);
}

// ---------------------------------------------------------------- налаштування ланцюгів
struct Setup {
    int8_t gains[kBands];
    int trimDb;       // рівень станції, −24..0
    float volAttDb;   // гучність радіо (послаблення), −120..0
    bool mute;
};

void feedSilence(int frames) {
    std::vector<int32_t> z(static_cast<size_t>(frames) * 2, 0);
    bool cont = false;
    int done = 0;
    while (done < frames) {
        const int n = (frames - done) < 256 ? (frames - done) : 256;
        audio_process_i2s(z.data() + 2 * done, static_cast<int16_t>(n * 2), &cont);
        std::vector<int32_t> z2(static_cast<size_t>(n) * 2, 0);
        oldChain(z2.data(), n);
        done += n;
    }
}

// Встановлює ОБИДВА ланцюги (новий — глобальні eq / radio_volume / output_trim; старий — власні екземпляри)
// і прогріває переходи (ramp гучності 100 мс, рівня станції) тишею. EQ: snap (перемикання fs), стан = 0.
void setup(const Setup& s) {
    eq::setAllDb(s.gains);
    eq::setSampleRate(44100);
    eq::setSampleRate(48000);
    g_oldEq.setAllDb(s.gains);
    g_oldEq.setSampleRate(44100);
    g_oldEq.setSampleRate(48000);
    g_refEq.setAllDb(s.gains);
    g_refEq.setSampleRate(44100);
    g_refEq.setSampleRate(48000);
    g_refEq.reset();
    output_trim::setTargetDb(static_cast<int8_t>(s.trimDb));
    radio_volume::setAttenuationDb(s.volAttDb);
    radio_volume::setMute(s.mute);
    g_oldVol.setAttenuationDb(s.volAttDb);
    g_oldVol.setMute(s.mute);
    eq::resetClipCount();
    g_oldEq.resetClipCount();
    feedSilence(12000);  // > 4800 кадрів ramp гучності, > ramp рівня станції
    eq::resetClipCount();
    g_oldEq.resetClipCount();
}

int32_t trimQ16(int db) {
    return static_cast<int32_t>(llround(65536.0 * pow(10.0, db / 20.0)));
}

struct Result {
    double maxErrNew = 0;     // макс. |новий − еталон(з насиченням)|, еталон з float-коефіцієнтами, шкала int32
    double maxErrNewD = 0;    // те саме, але еталон з чистими double-коефіцієнтами (інформативно)
    double maxErrIso = 0;     // макс. |новий − (float-EQ прошивки, далі double-множення й насичення)|: лише внесок E3
    double maxDiffNewOld = 0; // макс. |новий − старий ланцюг|, шкала int32
    double maxErrOld = 0;     // макс. |старий − еталон|
    double refPeak = 0;       // макс. |еталон| ДО фінального насичення
    long refClips = 0;        // відліки еталона поза повною шкалою
    uint32_t newClips = 0;    // eq::clipCount() після прогону
    uint32_t oldEqClips = 0;  // клипи старого EQ (насичення одразу після EQ)
    double maxVuRelDiff = 0;  // макс. відносна різниця енергій VU (новий хук проти старого) за виклик
    double maxVuPerFrame = 0; // макс. (sumSq / frames) нового VU, int16²
};

// Один прогін сигналу блоками по blk кадрів через ОБИДВА ланцюги й еталон.
Result run(const Setup& s, const Tone& t, int totalFrames, int blk) {
    Result res;
    std::vector<RefBand> ref = makeRef(s.gains, true);
    std::vector<RefBand> refD = makeRef(s.gains, false);
    const double trimG = static_cast<double>(trimQ16(s.trimDb)) / 65536.0;
    const double volG = s.mute ? 0.0 : pow(10.0, static_cast<double>(s.volAttDb) / 20.0);

    int done = 0;
    while (done < totalFrames) {
        const int n = (totalFrames - done) < blk ? (totalFrames - done) : blk;
        std::vector<int32_t> in = makeSignal(t, n, done);
        std::vector<int32_t> nw = in;
        std::vector<int32_t> od = in;

        bool cont = false;
        VuSourceDecodedPcm::lastFrames = 0;
        audio_process_i2s(nw.data(), static_cast<int16_t>(n * 2), &cont);
        if (!cont) {
            check(false, "continueI2S must be true");
        }
        double oSumL = 0, oSumR = 0;
        oldChain(od.data(), n, &oSumL, &oSumR);

        std::vector<float> fe(in.size());
        for (size_t k = 0; k < in.size(); ++k) {
            fe[k] = static_cast<float>(in[k]);
        }
        g_refEq.processFloat(fe.data(), n);

        for (int i = 0; i < n; ++i) {
            for (int c = 0; c < 2; ++c) {
                const double x = static_cast<double>(in[2 * i + c]);
                {
                    double yi = static_cast<double>(fe[2 * i + c]) * trimG * volG;
                    if (yi > kMax) yi = kMax;
                    if (yi < kMin) yi = kMin;
                    const double ei = fabs(static_cast<double>(nw[2 * i + c]) - yi);
                    if (ei > res.maxErrIso) res.maxErrIso = ei;
                    const double dno = fabs(static_cast<double>(nw[2 * i + c]) - static_cast<double>(od[2 * i + c]));
                    if (dno > res.maxDiffNewOld) res.maxDiffNewOld = dno;
                }
                double y = refStep(ref, x, c == 0) * trimG * volG;
                double yd = refStep(refD, x, c == 0) * trimG * volG;
                if (yd > kMax) yd = kMax;
                if (yd < kMin) yd = kMin;
                const double e3 = fabs(static_cast<double>(nw[2 * i + c]) - yd);
                if (e3 > res.maxErrNewD) res.maxErrNewD = e3;
                const double a = fabs(y);
                if (a > res.refPeak) res.refPeak = a;
                if (y > kMax || y < kMin) ++res.refClips;
                if (y > kMax) y = kMax;
                if (y < kMin) y = kMin;
                const double e1 = fabs(static_cast<double>(nw[2 * i + c]) - y);
                const double e2 = fabs(static_cast<double>(od[2 * i + c]) - y);
                if (e1 > res.maxErrNew) res.maxErrNew = e1;
                if (e2 > res.maxErrOld) res.maxErrOld = e2;
            }
        }
        // VU: новий проти старого (енергія у шкалі int16²; старий міряє після EQ, як і раніше)
        const double nl = VuSourceDecodedPcm::lastSumL;
        const double nr = VuSourceDecodedPcm::lastSumR;
        auto rel = [](double a, double b) {
            const double d = fabs(a - b);
            const double m = fmax(fabs(a), fabs(b));
            return m > 0 ? d / m : 0.0;
        };
        res.maxVuRelDiff = fmax(res.maxVuRelDiff, fmax(rel(nl, oSumL), rel(nr, oSumR)));
        if (VuSourceDecodedPcm::lastFrames != 0) {
            const double pf = fmax(nl, nr) / static_cast<double>(VuSourceDecodedPcm::lastFrames);
            res.maxVuPerFrame = fmax(res.maxVuPerFrame, pf);
        }
        done += n;
    }
    res.newClips = eq::clipCount();
    res.oldEqClips = g_oldEq.clipCount();
    return res;
}

Setup mk(std::initializer_list<int> g, int trimDb, float volDb, bool mute = false) {
    Setup s{};
    int i = 0;
    for (int v : g) {
        if (i < kBands) s.gains[i++] = static_cast<int8_t>(v);
    }
    s.trimDb = trimDb;
    s.volAttDb = volDb;
    s.mute = mute;
    return s;
}

}  // namespace

int main() {
    printf("E3 host test: %d EQ bands, chunk %d frames\n", kBands, 128);

    const Tone t1k = {1.0, 1000.0, 0.0, 1.0, 1000.0, 1.0};  // 0 dBFS, L / R зі зсувом фази

    // ------------------------------------------------------------------ T1
    // 0 dBFS, +18 дБ на смузі 1 кГц, рівень станції −20 дБ, гучність −6 дБ: еталон не перевищує повну шкалу.
    {
        printf("\n[T1] tone 0 dBFS, +18 dB @1k, trim -20 dB, volume -6 dB (reference stays below full scale)\n");
        setup(mk({0, 0, 0, 18, 0, 0, 0, 0}, -20, -6.0f));
        const Result r = run(mk({0, 0, 0, 18, 0, 0, 0, 0}, -20, -6.0f), t1k, 24000, 256);
        printf("      ref peak %.1f dBFS, new err max %.0f (%.3f LSB16), old err max %.0f (%.1f LSB16), old EQ clips %u, new final clips %u\n",
               20 * log10(r.refPeak / 2147483648.0), r.maxErrNew, r.maxErrNew / kLsb16, r.maxErrOld, r.maxErrOld / kLsb16,
               r.oldEqClips, r.newClips);
        printf("      E3-isolating reference err max %.4f LSB16; (info) vs pure-double-coefficient reference %.3f LSB16\n",
               r.maxErrIso / kLsb16, r.maxErrNewD / kLsb16);
        check(r.refClips == 0, "reference never exceeds full scale");
        check(r.maxErrIso <= kLsb16, "E3 stages (float trim/volume/final rounding) == double reference within 1 LSB16",
              "err %.0f LSB32 (limit %.0f)", r.maxErrIso, kLsb16);
        check(r.maxErrNew <= kLsb16, "whole chain == full double reference within 1 LSB16", "err %.0f LSB32 (limit %.0f)",
              r.maxErrNew, kLsb16);
        check(r.newClips == 0, "new chain: no saturation at all");
        check(r.oldEqClips > 0, "old chain clipped after EQ", "old EQ clips %.0f", r.oldEqClips);
        check(r.maxErrOld > 100.0 * kLsb16, "old chain deviates strongly from reference (clipped before trim)", "old err %.0f LSB32", r.maxErrOld);
    }

    // ------------------------------------------------------------------ T2
    // Справжнє перевантаження: рівень станції 0 дБ, гучність 0 дБ: еталон перевищує повну шкалу -> фінальне насичення.
    {
        printf("\n[T2] tone 0 dBFS, +18 dB @1k, trim 0 dB, volume 0 dB (reference really exceeds full scale)\n");
        const Setup s = mk({0, 0, 0, 18, 0, 0, 0, 0}, 0, 0.0f);
        setup(s);
        const Result r = run(s, t1k, 24000, 256);
        const double d = fabs(static_cast<double>(r.newClips) - static_cast<double>(r.refClips));
        printf("      ref peak %+.1f dBFS, ref clipped samples %ld, new final clips %u, err max %.0f (%.3f LSB16)\n",
               20 * log10(r.refPeak / 2147483648.0), r.refClips, r.newClips, r.maxErrNew, r.maxErrNew / kLsb16);
        printf("      E3-isolating reference err max %.4f LSB16; (info) vs pure-double-coefficient reference %.3f LSB16\n",
               r.maxErrIso / kLsb16, r.maxErrNewD / kLsb16);
        check(r.refClips > 1000, "reference really exceeds full scale", "ref clips %.0f", static_cast<double>(r.refClips));
        check(r.maxErrIso <= kLsb16, "E3 stages == saturated double reference within 1 LSB16", "err %.0f LSB32 (limit %.0f)",
              r.maxErrIso, kLsb16);
        // Повний еталон у double: цей сигнал на виході EQ у 7,9 раза вище повної шкали, а float32-EQ має власну
        // відносну похибку (стани DF2T) ~1e-7..1e-4 від рівня сигналу -> допуск 1 LSB16 + 1e-3 від min(пік, повна шкала).
        const double tolFull = kLsb16 + 1e-3 * fmin(r.refPeak, 2147483648.0);
        check(r.maxErrNew <= tolFull, "whole chain == saturated full-double reference (1 LSB16 + 1e-3 of level)",
              "err %.0f LSB32 (limit %.0f)", r.maxErrNew, tolFull);
        check(d <= 0.01 * static_cast<double>(r.refClips) + 2.0, "final clip count matches reference count (+-1%)",
              "new %.0f vs ref %.0f", static_cast<double>(r.newClips), static_cast<double>(r.refClips));
    }

    // ------------------------------------------------------------------ T3
    // Преcет власника при реальних рівнях: пік EQ вище 0 дБFS, але після рівня станції й гучності вкладається.
    {
        printf("\n[T3] owner preset {+14,+2,-1,+1,-1,+2,+5,+12}, 60 Hz + 1 kHz tones, trim -20 dB, volume -12 dB\n");
        const Setup s = mk({14, 2, -1, 1, -1, 2, 5, 12}, -20, -12.0f);
        setup(s);
        const Tone t = {0.9, 60.0, 0.3, 0.9, 1000.0, 0.7};
        const Result r = run(s, t, 24000, 256);
        printf("      ref peak %+.1f dBFS, new err max %.0f (%.3f LSB16), old err max %.1f LSB16, old EQ clips %u, new final clips %u\n",
               20 * log10(r.refPeak / 2147483648.0), r.maxErrNew, r.maxErrNew / kLsb16, r.maxErrOld / kLsb16, r.oldEqClips,
               r.newClips);
        printf("      E3-isolating reference err max %.4f LSB16; (info) vs pure-double-coefficient reference %.3f LSB16\n",
               r.maxErrIso / kLsb16, r.maxErrNewD / kLsb16);
        check(r.refClips == 0, "reference below full scale");
        check(r.maxErrIso <= kLsb16, "E3 stages == double reference within 1 LSB16", "err %.0f LSB32 (limit %.0f)",
              r.maxErrIso, kLsb16);
        // Повний еталон у double: float32-EQ на 50–60 Гц (+14 дБ) має власну похибку, однакову в старому й новому
        // ланцюзі (не від E3): допуск 1 LSB16 + 1e-3 від рівня виходу (−60 дБ).
        const double tolFull = kLsb16 + 1e-3 * r.refPeak;
        check(r.maxErrNew <= tolFull, "whole chain == full double reference (1 LSB16 + 1e-3 of level)",
              "err %.0f LSB32 (limit %.0f)", r.maxErrNew, tolFull);
        check(r.newClips == 0, "no final saturation");
        check(r.oldEqClips > 0, "old chain did saturate after EQ", "old EQ clips %.0f", r.oldEqClips);
    }

    // ------------------------------------------------------------------ T4
    // Бітова ідентичність в нейтральному режимі: EQ плоский, рівень станції 0 дБ, гучність 0 дБ.
    {
        printf("\n[T4] neutral chain (EQ flat, trim 0 dB, volume 0 dB): output must equal input bit for bit\n");
        const Setup s = mk({0, 0, 0, 0, 0, 0, 0, 0}, 0, 0.0f);
        setup(s);
        const Tone t = {0.8, 440.0, 0.0, 0.5, 3000.0, 0.0};
        std::vector<int32_t> in = makeSignal(t, 4096);
        std::vector<int32_t> out = in;
        bool cont = false;
        for (int off = 0; off < 4096; off += 256) {
            audio_process_i2s(out.data() + 2 * off, 512, &cont);
        }
        check(memcmp(in.data(), out.data(), in.size() * sizeof(int32_t)) == 0, "bit-exact passthrough");
        check(eq::clipCount() == 0, "no clips");
    }

    // ------------------------------------------------------------------ T5
    // VU: ті самі значення, що в старому ланцюзі (де старий не насичував) і обмеження до int16 (де насичував).
    {
        printf("\n[T5] VU energy: new hook vs old chain\n");
        {
            const Setup s = mk({0, 0, 0, 6, 0, 0, 0, 0}, -20, -6.0f);
            setup(s);
            const Tone t = {0.25, 1000.0, 0.0, 0.25, 1000.0, 0.4};  // −12 dBFS, +6 дБ -> без насичення
            const Result r = run(s, t, 24000, 256);
            printf("      no EQ saturation: old EQ clips %u, max rel VU diff %.3g\n", r.oldEqClips, r.maxVuRelDiff);
            check(r.oldEqClips == 0, "old chain did not saturate here");
            check(r.maxVuRelDiff <= 1e-6, "VU energy identical to old chain", "rel diff %.3g (limit %.0g)", r.maxVuRelDiff, 1e-6);
            printf("      output new vs old chain: max difference %.0f LSB32 (%.4f LSB16)\n", r.maxDiffNewOld, r.maxDiffNewOld / kLsb16);
            check(r.maxDiffNewOld <= kLsb16, "where the old chain did not saturate, output is the same within 1 LSB16",
                  "diff %.0f LSB32", r.maxDiffNewOld);
        }
        {
            const Setup s = mk({0, 0, 0, 18, 0, 0, 0, 0}, -20, -6.0f);
            setup(s);
            const Result r = run(s, t1k, 24000, 256);
            const double lim = 1073741824.0 * (1.0 + 1e-6);  // 2^30 = (32768)^2 на кадр
            printf("      EQ overshoot (+18 dB): max sumSq/frame %.4g (limit 2^30 = %.4g), rel diff vs old %.3g\n",
                   r.maxVuPerFrame, 1073741824.0, r.maxVuRelDiff);
            check(r.maxVuPerFrame <= lim, "VU input stays within int16 range despite EQ overshoot");
            check(r.maxVuRelDiff <= 1e-6, "VU energy identical to old chain (old clipped at EQ == VU clamp)", "rel diff %.3g", r.maxVuRelDiff, 0);
        }
    }

    // ------------------------------------------------------------------ T6
    // Незалежність від розміру блоку (в тому числі не кратного розміру проходу, 1 кадр, > kMaxFramesPerBlock).
    {
        printf("\n[T6] block size independence\n");
        const Setup s = mk({6, 0, 0, 12, 0, 0, 0, 4}, -20, -10.0f);
        const Tone t = {0.5, 1000.0, 0.0, 0.5, 220.0, 0.0};
        const int total = 9000;
        std::vector<std::vector<int32_t>> outs;
        const int blks[] = {256, 77, 1, 1000, 3000};
        for (int blk : blks) {
            setup(s);
            std::vector<int32_t> all(static_cast<size_t>(total) * 2);
            int done = 0;
            while (done < total) {
                const int n = (total - done) < blk ? (total - done) : blk;
                std::vector<int32_t> in = makeSignal(t, n, done);
                bool cont = false;
                audio_process_i2s(in.data(), static_cast<int16_t>(n * 2), &cont);
                memcpy(all.data() + 2 * done, in.data(), static_cast<size_t>(n) * 2 * sizeof(int32_t));
                done += n;
            }
            outs.push_back(all);
        }
        double maxd = 0;
        for (size_t k = 1; k < outs.size(); ++k) {
            for (size_t i = 0; i < outs[0].size(); ++i) {
                const double d = fabs(static_cast<double>(outs[k][i]) - static_cast<double>(outs[0][i]));
                if (d > maxd) maxd = d;
            }
        }
        printf("      max difference between block sizes 256 / 77 / 1 / 1000 / 3000: %.0f LSB32\n", maxd);
        check(maxd <= kLsb16, "output independent of block size (within 1 LSB16)", "diff %.0f", maxd);

        // 3000 кадрів > kMaxFramesPerBlock (2048): VU сканує не більше 2048
        setup(s);
        std::vector<int32_t> in = makeSignal(t, 3000);
        bool cont = false;
        audio_process_i2s(in.data(), 6000, &cont);
        check(VuSourceDecodedPcm::lastFrames == 2048, "VU frame count capped at kMaxFramesPerBlock",
              "reported %.0f", static_cast<double>(VuSourceDecodedPcm::lastFrames));
    }

    // ------------------------------------------------------------------ T7
    // Межі буфера й непарна кількість слів.
    {
        printf("\n[T7] buffer bounds\n");
        const Setup s = mk({0, 0, 0, 18, 0, 0, 0, 0}, 0, 0.0f);
        setup(s);
        std::vector<int32_t> buf(2 + 2 * 301 + 2 + 1, 0x5A5A5A5A);
        const Tone t = {1.0, 1000.0, 0.0, 1.0, 1000.0, 0.0};
        std::vector<int32_t> sig = makeSignal(t, 301);
        memcpy(buf.data() + 2, sig.data(), sig.size() * sizeof(int32_t));
        const int32_t guardHead0 = buf[0], guardHead1 = buf[1];
        const size_t tail = 2 + sig.size();
        bool cont = false;
        audio_process_i2s(buf.data() + 2, static_cast<int16_t>(2 * 301 + 1), &cont);  // непарна кількість слів
        check(buf[0] == guardHead0 && buf[1] == guardHead1, "no write before buffer");
        check(buf[tail] == 0x5A5A5A5A && buf[tail + 1] == 0x5A5A5A5A && buf[tail + 2] == 0x5A5A5A5A,
              "no write after buffer; the odd trailing word is untouched");
        audio_process_i2s(nullptr, 100, &cont);
        check(cont, "null buffer: continueI2S set, no crash");
        audio_process_i2s(buf.data() + 2, 1, &cont);
        check(cont, "validSamples < 2: no crash");
    }

    // ------------------------------------------------------------------ T8
    // Мʼют: тиша, жодних насичень.
    {
        printf("\n[T8] mute\n");
        const Setup s = mk({0, 0, 0, 18, 0, 0, 0, 0}, 0, 0.0f, true);
        setup(s);
        std::vector<int32_t> in = makeSignal(t1k, 2048);
        bool cont = false;
        audio_process_i2s(in.data(), 4096, &cont);
        bool allZero = true;
        for (int32_t v : in) {
            if (v != 0) allZero = false;
        }
        check(allZero, "muted: output is silence");
        check(eq::clipCount() == 0, "muted: no clips even with +18 dB EQ");
        check(radio_volume::isSilent(), "isSilent() true after mute ramp");
    }

    printf("\nRESULT: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
