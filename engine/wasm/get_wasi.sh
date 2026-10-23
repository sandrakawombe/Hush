#!/usr/bin/env bash
# Download the WASI SDK sysroot and compiler builtins (version 24) from GitHub.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$HERE/.wasi" && cd "$HERE/.wasi"
B=https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-24
curl -sSfL "$B/wasi-sysroot-24.0.tar.gz" | tar xz
curl -sSfL "$B/libclang_rt.builtins-wasm32-wasi-24.0.tar.gz" | tar xz
echo "WASI SDK ready in $HERE/.wasi"
