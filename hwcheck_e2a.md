# hwcheck E2a — EQ на 8 смуг

1. **Патч:** з кореня проєкту `python apply_e2a.py` → очікуй `OK src/audio/line_path.cpp 8 замін`, `OK src/core/app_controller.cpp 2 замін`, `OK src/config/settings_config.h 1 замін`. Помилка = нічого не змінено; надішли текст помилки.
2. Заміни файли з архіву (`eq_config.h`, `eq_screen_config.h`, `eq.cpp`, `settings.cpp`, `settings_test.cpp`).
3. Перевір літерали, що лишились: `grep -rn "eqGainsDb\[5\]\|\[5\]\|%d %d %d %d %d" src` — надішли вивід.
4. `pio run` — без помилок/попереджень. `pio run -t erase`, потім `pio run -t upload`, `pio device monitor`.
5. Старт: у логу `[SET] defaults saved`; екран EQ: 8 стовпчиків + Bal, усі 0.
6. Екран «EQ + Balance» (утримання енкодера): клік/RIGHT проходить 50→125→350→1k→2.8k→5.6k→10k→15k→Bal→50; ±1 дБ, межі ±18; підписи читаються, «+18» не налізає на сусідів.
7. API: `GET /api/eq?input=0` → масиви по 8; `POST /api/eq {"gainsDb":[14,2,0,0,0,2,5,12]}` → 200; з 5 значеннями → 400 `invalid_value` (field gainsDb); `[19,0,0,0,0,0,0,0]` → 400 `out_of_range`.
8. Перезапуск: значення 8 смуг збереглись.
9. Вхід Computer, `LINE_PATH_TEST`=1: `lp.reset`, 30–60 с музики, `lp.stat`. Повтори для двох пресетів: A `lp.eqall 14 2 0 0 0 2 5 12` (5 активних), B (найгірший) `lp.eqall 14 2 -1 1 -1 2 5 12` (8 активних). Надішли `maxProcess`, `maxReadWait`, `stackFreeMin`, вільну RAM, `clipCount` (база 5 смуг: 515 / 2445 / 2564 / 69128 / 0).
10. `EQ_DEBUG`=1 на радіо: рядки `[EQ] radio proc: ... avg/max us`; надішли 3–5 рядків (пресет B).
11. Стек задачі бібліотеки: `WEB_TRACE`=1, таблиця `[TASK]` → мін. вільний стек `PeriodicTask` (було 1040–1904 Б; очікується мінус ≈176 Б).
12. На слух (радіо й Computer): пресет A ≈ AIMP; крок ±1 дБ без клацань; різкий перехід 0→+14 на низьких смугах (через `lp.eqall`) — чи чути «бульк» ≈10 мс.
