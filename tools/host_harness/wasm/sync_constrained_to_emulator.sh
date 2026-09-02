#!/usr/bin/env bash
# Copy the generated constrained ES modules into the static emulator bundle.
# Build first so checked-in artifacts can never silently lag their C++ sources.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
EMULATOR_WASM="$HERE/../../../micropatterns_emulator/wasm"

"$HERE/build_constrained.sh"
mkdir -p "$EMULATOR_WASM"

for profile in watchy m5paper; do
    install -m 0644 "$HERE/out/mp_render_$profile.js" \
        "$EMULATOR_WASM/mp_render_$profile.js"
    install -m 0644 "$HERE/out/mp_render_$profile.wasm" \
        "$EMULATOR_WASM/mp_render_$profile.wasm"
done

echo "Synced constrained WASM profiles to $EMULATOR_WASM"
