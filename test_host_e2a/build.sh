#!/bin/sh
# Хост-тест E2a (лише ПК). Запуск з папки test_host_e2a/:  sh build.sh
# Збирає test_eq_e2a двічі: EQ_DEBUG = 0 (реліз) і EQ_DEBUG = 1 (звіт про вартість; лише перевірка компіляції та виводу).
set -e
cd "$(dirname "$0")"
SRC=../src
CXX=${CXX:-g++}
FLAGS="-std=c++17 -O2 -Wall -Wextra -Wno-unused-function"

echo "== build EQ_DEBUG=0"
$CXX $FLAGS -Istubs -I$SRC -o test_eq_e2a test_eq_e2a.cpp
./test_eq_e2a

echo "== build EQ_DEBUG=1 (compile + timing report check)"
rm -rf build_dbg && mkdir -p build_dbg/config
sed 's/^#define EQ_DEBUG 0/#define EQ_DEBUG 1/' $SRC/config/eq_config.h > build_dbg/config/eq_config.h
grep -q '^#define EQ_DEBUG 1' build_dbg/config/eq_config.h
$CXX $FLAGS -Ibuild_dbg -Istubs -I$SRC -o test_eq_dbg test_eq_dbg.cpp
./test_eq_dbg
