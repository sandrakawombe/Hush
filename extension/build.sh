#!/usr/bin/env bash
# Rebuild extension/hush.wasm from the C++ engine (needs clang + lld).
# Run after changing the engine or retraining the model (train/export_cpp.py).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[ -d "$ROOT/engine/wasm/.wasi" ] || "$ROOT/engine/wasm/get_wasi.sh"
"$ROOT/engine/wasm/build_wasm.sh" "$ROOT/extension/hush.wasm"
