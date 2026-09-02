#!/usr/bin/env bash
# Rebuild and copy every generated WASM module consumed by the static editor.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
EMULATOR_WASM="$HERE/../../../micropatterns_emulator/wasm"

"$HERE/build.sh"
"$HERE/build_constrained.sh"
mkdir -p "$EMULATOR_WASM"

for file in \
    mp_render.js mp_render.wasm \
    mp_render_watchy.js mp_render_watchy.wasm \
    mp_render_m5paper.js mp_render_m5paper.wasm; do
    install -m 0644 "$HERE/out/$file" "$EMULATOR_WASM/$file"
done

echo "Synced all WASM renderer builds to $EMULATOR_WASM"
