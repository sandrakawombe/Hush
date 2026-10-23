#!/usr/bin/env bash
# Download part of the Coswara dataset (CC BY 4.0, Indian Institute of Science):
# two days of recordings, 237 people, ~1.1 GB download, ~500 MB after conversion.
# Each person recorded coughs, counting out loud, breathing and vowels.
#
# Uses a sparse checkout so only those two folders are fetched; the full
# repository is many gigabytes.
set -euo pipefail
cd "$(dirname "$0")/../data"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
DAYS="20200413 20200415"

git clone -q --filter=blob:none --no-checkout --depth 1 https://github.com/iiscleap/Coswara-Data "$TMP/repo"
git -C "$TMP/repo" sparse-checkout set --no-cone $(for d in $DAYS; do echo "/$d/"; done)
git -C "$TMP/repo" checkout -q
mkdir -p "$TMP/x"
for d in $DAYS; do cat "$TMP/repo/$d/$d.tar.gz."* | tar -xz -C "$TMP/x"; done
python3 "$(dirname "$0")/prep_coswara.py" "$TMP/x" raw/coswara
