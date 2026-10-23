#!/usr/bin/env bash
# Build engine/wasm/hush.wasm with clang + the WASI SDK sysroot.
#   WASI_SYSROOT   path to wasi-sysroot (default: ./.wasi/wasi-sysroot-24.0)
#   WASI_BUILTINS  path to libclang_rt.builtins-wasm32.a
# Run engine/wasm/get_wasi.sh once to download both (~70 MB, from GitHub).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ENGINE="$HERE/.."
SYSROOT="${WASI_SYSROOT:-$HERE/.wasi/wasi-sysroot-24.0}"
BUILTINS="${WASI_BUILTINS:-$HERE/.wasi/libclang_rt.builtins-wasm32-wasi-24.0/libclang_rt.builtins-wasm32.a}"
CXX="${CXX:-clang++}"
OUT="${1:-$HERE/hush.wasm}"

"$CXX" --target=wasm32-wasi --sysroot="$SYSROOT" -mexec-model=reactor \
  -std=c++17 -O3 -DNDEBUG -fno-exceptions -fno-rtti \
  -I"$ENGINE/include" -I"$ENGINE/src" \
  "$ENGINE/src/engine.cpp" "$ENGINE/src/rules_detector.cpp" "$ENGINE/src/model_detector.cpp" \
  "$ENGINE/src/c_api.cpp" "$HERE/hush_wasm.cpp" \
  -nodefaultlibs -lc++ -lc++abi -lc "$BUILTINS" \
  -Wl,--strip-all -Wl,--gc-sections \
  -o "$OUT"
echo "built $OUT ($(wc -c < "$OUT") bytes)"
