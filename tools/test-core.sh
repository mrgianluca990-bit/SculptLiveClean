#!/usr/bin/env bash
set -euo pipefail

sculpt_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
sculpt_build="${SCULPT_LIVE_CLEAN_TEST_BUILD_DIR:-${sculpt_root}/build-tests}"
sculpt_cxx="${CXX:-c++}"
if ! command -v "$sculpt_cxx" >/dev/null 2>&1; then
  echo "Serve un compilatore C++17. Puoi selezionarlo con CXX=/percorso/del/compilatore." >&2
  exit 1
fi
mkdir -p "$sculpt_build"
"$sculpt_cxx" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
  "${sculpt_root}/Tests/live_clean_test.cpp" -o "${sculpt_build}/live_clean_test"
"${sculpt_build}/live_clean_test"

"$sculpt_cxx" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
  "${sculpt_root}/Tests/source_extractor_test.cpp" -o "${sculpt_build}/source_extractor_test"
"${sculpt_build}/source_extractor_test"
