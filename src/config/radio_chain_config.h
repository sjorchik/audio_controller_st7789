#pragma once

// [E3] Константи float-ланцюга радіо в хуку audio_process_i2s (audio/vu_pcm_hook.cpp).
// Ланцюг: int32 -> float -> EQ -> VU -> output_trim -> radio_volume -> ОДНЕ насичення -> int32.
// Проміжний сигнал між EQ, рівнем станції й гучністю лишається у float (без обрізання).

#include <stdint.h>

namespace radio_chain_cfg {

// Скільки стереокадрів хук обробляє за один прохід через float-буфер. Буфер статичний (внутрішня RAM,
// НЕ стек задачі бібліотеки): 2 канали * 4 байти * kChunkFrames = 1 КБ при 128. Блок бібліотеки (≈ 256
// кадрів) ділиться на кілька проходів; EQ / гучність / рівень станції переходять між проходами без
// розривів (їхній стан зберігається), інтерполяція коефіцієнтів EQ іде дрібнішими кроками.
constexpr int32_t kChunkFrames = 128;

static_assert(kChunkFrames >= 16 && kChunkFrames <= 512, "chunk size out of sane range");

}  // namespace radio_chain_cfg
