#pragma once

// Константи екрана «EQ + Balance» (режим редагування еквалайзера й балансу АКТИВНОГО входу).
// [Prompt 49a] Створено: таймаут, крок, початковий вибір, тимчасова текстова розкладка.
// [Prompt 49b] Тимчасова розкладка прибрана; додано справжню розкладку вертикальних стовпчиків
// (смуги EQ + баланс). Логіка (AppController) читає лише kTimeoutMs / kStepDb / kDefaultSelected —
// їх значення НЕ змінено.
// [E2a] Смуг EQ стало 8 (eq_cfg::kBandCount): колонок 9 (8 смуг + баланс). Розкладку розраховано ФОРМУЛОЮ від
// ширини екрана й кількості колонок (kColW, kLeft, colX): зміна eq_cfg::kBandCount перебудовує екран сама.
// Це мінімально-робочий вигляд; шліфування — окремий крок.
// Усі числа екрана тут; screens.cpp не містить «магічних» координат.
// Розкладка (320×170; [E2a] ширина колонки ≈ 33 px, підписи й значення — Tiny / Small):
//   y 0..23   шапка: назва «EQ + Balance» (Small) зліва, назва входу справа; лінія при kHeaderLineY
//   y 28..150 область стовпчиків (kAreaTop..kAreaBottom), нульова лінія при kZeroY
//   y 154..167 підписи частот і «Bal» (Tiny, kLabelY)

#include <stdint.h>

#include "config/balance_config.h"
#include "config/display_config.h"
#include "config/eq_config.h"
#include "config/screens_config.h"

namespace eq_screen_cfg {

// Вихід з екрана без дій: від ОСТАННЬОЇ дії в екрані (UP / DOWN / LEFT / RIGHT / обертання / клік / VOL_*), мс.
constexpr uint32_t kTimeoutMs = 10000;

// Крок зміни вибраного параметра (смуга EQ і баланс), дБ на подію. UP / DOWN / обертання енкодера.
constexpr int kStepDb = 1;

// Параметр, вибраний при відкритті екрана: 0..kBandCount - 1 = смуги EQ, kBandCount = баланс.
constexpr uint8_t kDefaultSelected = 0;

// ---------------------------------------------------------------------------
// Шапка
// ---------------------------------------------------------------------------
constexpr int16_t kHeaderTitleY   = 4;     // Small: заголовок і назва входу (одна лінія)
constexpr int16_t kHeaderLineY    = 24;    // розділова лінія під шапкою
constexpr int16_t kHeaderNameGap  = 12;    // мінімальний проміжок між заголовком і назвою входу
constexpr uint16_t kColorHeaderName = display_cfg::kColorYellow;   // назва входу — акцент (що саме редагується)

// ---------------------------------------------------------------------------
// Колонки (kBandCount смуг EQ + баланс = kBandCount + 1)
// ---------------------------------------------------------------------------
constexpr uint8_t kColumnCount = eq_cfg::kBandCount + 1;
constexpr uint8_t kBalanceColumn = eq_cfg::kBandCount;

constexpr int16_t kSideMargin = 4;    // [E2a] мінімальне поле зліва / справа від колонок
constexpr int16_t kGapEq    = 0;      // проміжок між смугами EQ (комірки впритул; бари розділяє поле навколо бару)
constexpr int16_t kGapBal   = 12;     // проміжок перед балансом (з розділювальною лінією посередині)
// [E2a] Ширина комірки — з ширини екрана: (320 - 2·4 - 12 - 0) / 9 = 33 px при 8 смугах + баланс.
constexpr int16_t kColW     = static_cast<int16_t>((display_cfg::kWidth - 2 * kSideMargin - kGapBal -
                                                    (eq_cfg::kBandCount - 1) * kGapEq) / kColumnCount);
constexpr int16_t kBarW     = 20;     // ширина самого стовпчика (між барами лишається ≈ 13 px)
constexpr int16_t kTotalW   = kColumnCount * kColW + (eq_cfg::kBandCount - 1) * kGapEq + kGapBal;
constexpr int16_t kLeft     = (display_cfg::kWidth - kTotalW) / 2;   // центрування по горизонталі

// Лівий край i-ї колонки. Перед балансом — (kBandCount - 1) проміжків EQ + kGapBal.
constexpr int16_t colX(uint8_t i) {
    return static_cast<int16_t>(kLeft + i * kColW +
                                (i < eq_cfg::kBandCount ? i * kGapEq
                                                        : (eq_cfg::kBandCount - 1) * kGapEq + kGapBal));
}

// Вертикальна розділова лінія між смугами EQ і балансом: по центру проміжку kGapBal.
constexpr int16_t kDividerX = static_cast<int16_t>(colX(kBalanceColumn) - kGapBal / 2);

// ---------------------------------------------------------------------------
// Вертикальна область і шкала
// ---------------------------------------------------------------------------
constexpr int16_t kAreaTop    = 28;                              // верх області стовпчиків
constexpr int16_t kAreaBottom = 150;                             // низ області (вище за підписи)
constexpr int16_t kZeroY      = (kAreaTop + kAreaBottom) / 2;    // нульова лінія (y=89)
// Піввисота області: повна шкала (±kEqFullDb для EQ, ±kBalFullDb для балансу — однакова висота).
constexpr int16_t kHalfH      = 40;
constexpr int     kEqFullDb   = eq_cfg::kGainMaxDb;    // [E1] межа з eq_cfg (симетричний діапазон); висота бару = |дБ| / kEqFullDb × kHalfH
constexpr int     kBalFullDb  = balance_cfg::kMaxDb;   // 20

// Значення над / під стовпчиком: відступ від кінця бару, px.
constexpr int16_t kValueGap   = 2;
constexpr uint8_t kValueSmallPx = display_cfg::kFontSmallPx;   // вибране значення (жирніше за Tiny)
constexpr uint8_t kValueTinyPx  = display_cfg::kFontTinyPx;    // решта значень

// Підписи частот / «Bal» під областю.
constexpr int16_t kLabelY     = 154;

// ---------------------------------------------------------------------------
// Кольори
// ---------------------------------------------------------------------------
constexpr uint16_t kColorEqBar     = display_cfg::rgb565(90, 200, 255);   // EQ, вибране — холодний
constexpr uint16_t kColorEqBarDim  = display_cfg::rgb565(40, 80, 105);    // EQ, решта — приглушено
constexpr uint16_t kColorEqSel     = screens_cfg::kColorSelectionBg;     // підсвітка вибраної EQ-колонки
constexpr uint16_t kColorBalBar    = display_cfg::rgb565(255, 170, 0);    // баланс, вибране — амбер
constexpr uint16_t kColorBalBarDim = display_cfg::rgb565(110, 72, 0);     // баланс, решта
constexpr uint16_t kColorBalSel    = display_cfg::rgb565(80, 50, 0);      // підсвітка вибраної колонки балансу
constexpr uint16_t kColorBalValue  = display_cfg::rgb565(200, 140, 0);    // значення балансу, не вибране
constexpr uint16_t kColorZero      = screens_cfg::kColorDim;             // нульова лінія
constexpr uint16_t kColorDivider   = display_cfg::rgb565(60, 60, 60);    // розділювач EQ / Bal
constexpr uint16_t kColorValueDim  = screens_cfg::kColorDim;             // значення EQ, не вибране
constexpr uint16_t kColorValueSel  = display_cfg::kColorFg;              // значення вибраного
constexpr uint16_t kColorLabelDim  = screens_cfg::kColorDim;             // підписи, не вибрані
constexpr uint16_t kColorLabelSel  = display_cfg::kColorFg;              // підпис вибраного

// ---------------------------------------------------------------------------
// Перевірки розкладки (збій компіляції, якщо щось виходить за межі)
// ---------------------------------------------------------------------------
static_assert(kStepDb >= 1, "step must be at least 1 dB");
static_assert(kTimeoutMs >= 1000, "timeout is too short to use the screen");
static_assert(kDefaultSelected <= eq_cfg::kBandCount, "default selection out of range (0..bands = balance)");
static_assert(kHeaderLineY > kHeaderTitleY + display_cfg::kFontSmallPx, "header line overlaps the title");
static_assert(kAreaTop > kHeaderLineY, "columns area starts above the header line");
static_assert(kLeft >= 4 && kLeft + kTotalW <= display_cfg::kWidth - 4, "columns do not fit the screen width");
static_assert(kBarW >= 2 && kBarW < kColW, "bar must be narrower than its column");
static_assert(kColW >= 24, "column is too narrow for the frequency labels / values (check eq_cfg::kBandCount)");
static_assert(kGapBal >= 2, "gap before the balance column must fit the divider");
static_assert(kDividerX > colX(eq_cfg::kBandCount - 1) + kColW && kDividerX < colX(kBalanceColumn),
              "divider must sit inside the gap before the balance column");
static_assert(kHalfH > 0, "half height must be positive");
static_assert(kZeroY - kHalfH - kValueGap - kValueSmallPx >= kAreaTop,
              "top value label (selected, full scale) leaves the columns area");
static_assert(kZeroY + kHalfH + kValueGap + kValueSmallPx <= kAreaBottom,
              "bottom value label (selected, full scale) leaves the columns area");
static_assert(kAreaBottom + 2 <= kLabelY && kLabelY + display_cfg::kFontTinyPx <= display_cfg::kHeight,
              "frequency labels leave the screen");
static_assert(kEqFullDb > 0 && kBalFullDb > 0, "full scale must be positive");
static_assert(kEqFullDb == -eq_cfg::kGainMinDb, "EQ column scale assumes a symmetric range (see eq_cfg::kGainRangeDb)");

}  // namespace eq_screen_cfg
