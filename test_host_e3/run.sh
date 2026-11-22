#!/bin/sh
# Хост-тест E3 (лише ПК). Запускати з КОРЕНЯ репозиторію: sh test_host_e3/run.sh
set -e
OUT=test_host_e3/test_e3
g++ -std=c++17 -O2 -Wall -Wextra -I test_host_e3/stubs -I src test_host_e3/test_e3.cpp \
    src/audio/eq.cpp src/audio/dsp_volume.cpp src/audio/output_trim.cpp src/audio/radio_volume.cpp \
    src/audio/vu_pcm_hook.cpp -o "$OUT"
"$OUT"
