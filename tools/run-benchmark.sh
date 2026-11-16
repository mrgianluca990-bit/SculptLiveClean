#!/usr/bin/env bash
set -euo pipefail
sculpt_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
sculpt_build="${SCULPT_LIVE_CLEAN_BENCH_DIR:-${sculpt_root}/build-benchmark}"
sculpt_cxx="${CXX:-c++}"
mkdir -p "$sculpt_build"
"$sculpt_cxx" -std=c++17 -O3 -DNDEBUG -Wall -Wextra -Wpedantic -Werror \
  "$sculpt_root/tools/benchmark.cpp" -o "$sculpt_build/benchmark"
"$sculpt_build/benchmark" "${1:-${sculpt_build}/cpu-benchmark.json}"
