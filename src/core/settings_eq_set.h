#pragma once

// [E4] Захист від незбігу набору частот EQ (чиста логіка без заліза: хост-тестується в test_host_e4/).
//
// Профілі входів зберігають лише підсилення смуг (дБ) за індексом, а не частоти. Якщо після прошивки змінився дефайн
// EQ_BAND_SET (config/eq_config.h), ті самі індекси означали б інші частоти. Тому в Settings лежить id набору, з яким
// пресети записано (Settings::eqBandSet), і при завантаженні з NVS він порівнюється з поточним.
//
// Викликається ЛИШЕ з SettingsStore::load() (один раз при старті, не в гарячому шляху).

#include <stdint.h>

#include "config/defaults.h"
#include "config/eq_config.h"
#include "core/settings.h"

namespace settings_eq_set {

// Якщо s.eqBandSet != currentId: усі eq_cfg::kBandCount смуг УСІХ входів -> 0 дБ, s.eqBandSet = currentId.
// Решта полів (гучність, баланс, VU, назви, маска входів, остання станція тощо) НЕ чіпається.
// Повертає true, коли скидання було; тоді *oldId (якщо не nullptr) = збережений раніше id. Збіг -> false, без змін.
inline bool reconcile(Settings& s, uint8_t currentId, uint8_t* oldId = nullptr) {
    if (s.eqBandSet == currentId) {
        return false;
    }
    if (oldId != nullptr) {
        *oldId = s.eqBandSet;
    }
    for (uint8_t i = 0; i < defaults::kInputCount; ++i) {
        for (uint8_t b = 0; b < eq_cfg::kBandCount; ++b) {
            s.profiles[i].eqGainsDb[b] = 0;
        }
    }
    s.eqBandSet = currentId;
    return true;
}

}  // namespace settings_eq_set
