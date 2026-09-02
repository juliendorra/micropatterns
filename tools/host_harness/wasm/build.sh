#!/usr/bin/env bash
# Builds the firmware renderer to WebAssembly.
#
# Same six core .cpp files the device runs, same shim the host harness uses --
# see ../Makefile, which compiles the identical list for native. Nothing here
# is a port; if this builds, it is because the core is already free of Arduino,
# FreeRTOS and hardware.
#
# Needs Emscripten. It is not on PATH by default on this machine; point EMSDK at
# an emsdk checkout, or let the fallback below find the one vendored in the
# qemu-ipod_touch_1g tree.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
HARNESS="$(dirname "$HERE")"
ROOT="$(cd "$HARNESS/../.." && pwd)"
FW_SRC="$HARNESS/../../M5Paper_MicroPatterns/src"

EMSDK="${EMSDK:-$HOME/Documents/GitHub/qemu-ipod_touch_1g/.wasm-toolchain/emsdk}"
EMCC="$EMSDK/upstream/emscripten/emcc"
if [ ! -x "$EMCC" ]; then
    echo "emcc not found at $EMCC" >&2
    echo "Set EMSDK to an emsdk checkout, or install one:" >&2
    echo "  git clone https://github.com/emscripten-core/emsdk && cd emsdk && ./emsdk install latest && ./emsdk activate latest" >&2
    exit 1
fi

OUT="${OUT:-$HERE/out}"
mkdir -p "$OUT"

# The glue is named .js, not .mjs, on purpose. It is an ES module either way
# (-sEXPORT_ES6), but the static host the editor deploys to serves .mjs with NO
# Content-Type and browsers refuse to execute a module script without a
# JavaScript MIME type: "Failed to fetch dynamically imported module". .js is
# served as application/javascript there. Found in production, not locally --
# the dev server sets types by extension and knew .mjs.
#
# Exceptions and RTTI stay OFF: the firmware builds that way (Arduino disables
# both), so leaving them on here would let code compile that cannot run on the
# device -- exactly the divergence this whole exercise exists to remove.
"$EMCC" \
    -std=c++17 -Os -fno-exceptions -fno-rtti \
    -ffile-prefix-map="$ROOT"=. \
    -I"$HARNESS/shim" -I"$HARNESS/src" -I"$FW_SRC" \
    -DHOST_LOG_LEVEL=0 \
    "$FW_SRC/micropatterns_parser.cpp" \
    "$FW_SRC/micropatterns_runtime.cpp" \
    "$FW_SRC/mp_program.cpp" \
    "$FW_SRC/micropatterns_drawing.cpp" \
    "$FW_SRC/display_list_renderer.cpp" \
    "$FW_SRC/occlusion_buffer.cpp" \
    "$FW_SRC/matrix_utils.cpp" \
    "$HARNESS/src/host_display_manager.cpp" \
    "$HARNESS/src/render_path.cpp" \
    "$HERE/mp_wasm.cpp" \
    -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=web,node \
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=32MB \
    -sEXPORTED_RUNTIME_METHODS='["cwrap","HEAPU8","UTF8ToString"]' \
    -sEXPORTED_FUNCTIONS='["_mp_parse","_mp_parse_error_count","_mp_parse_error_at","_mp_asset_count","_mp_asset_name","_mp_asset_original_name","_mp_asset_width","_mp_asset_height","_mp_asset_data","_mp_render","_mp_pixels","_mp_width","_mp_height","_mp_error","_mp_display_list_items","_mp_display_list_bytes","_mp_program_bytes","_mp_program_file_bytes","_mp_compile","_mp_compile_error","_mp_compile_ms","_mp_compile_program_bytes","_mp_compile_file_bytes","_mp_has_stored","_mp_ms_load","_mp_rendered_items","_mp_culled_offscreen","_mp_culled_occlusion","_mp_occupancy_map_used","_mp_ms_parse","_mp_ms_displaylist","_mp_ms_rasterize","_malloc","_free"]' \
    -o "$OUT/mp_render.js"

ls -lh "$OUT"/mp_render.wasm "$OUT"/mp_render.js | awk '{print "  " $9 "  " $5}'
