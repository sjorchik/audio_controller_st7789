#pragma once
// ЗАГЛУШКА для хост-тесту E3 (лише ПК): справжній src/audio/vu_source.h не прикріплювався.
// Хук викликає лише VuSourceDecodedPcm::publishEnergy / publishPeaks; заглушка запамʼятовує останній виклик.
#include <stdint.h>

class VuSourceDecodedPcm {
public:
    static inline float    lastSumL = 0.0f;
    static inline float    lastSumR = 0.0f;
    static inline uint32_t lastFrames = 0;
    static inline uint32_t lastPeakL = 0;
    static inline uint32_t lastPeakR = 0;
    static inline uint32_t calls = 0;

    static void publishEnergy(float sumSqL, float sumSqR, uint32_t frames) {
        lastSumL = sumSqL;
        lastSumR = sumSqR;
        lastFrames = frames;
        ++calls;
    }
    static void publishPeaks(uint32_t peakL, uint32_t peakR, uint32_t frames) {
        lastPeakL = peakL;
        lastPeakR = peakR;
        lastFrames = frames;
        ++calls;
    }
};
