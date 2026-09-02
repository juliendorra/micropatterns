#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
HARNESS="$(dirname "$HERE")"
COMPAT="$HARNESS/device_compat"
EMSDK_DIR="${EMSDK:-$HOME/Documents/GitHub/qemu-ipod_touch_1g/.wasm-toolchain/emsdk}"
EMCC="$EMSDK_DIR/upstream/emscripten/emcc"
EMXX="$EMSDK_DIR/upstream/emscripten/em++"
OUT="${OUT:-$HARNESS/out/device-allocator}"

if [ ! -x "$EMCC" ]; then
    echo "emcc not found at $EMCC" >&2
    exit 1
fi

build_profile() {
    local profile="$1"
    local version define tlsf_source tlsf_dir
    case "$profile" in
        watchy)
            version="5.3.2"
            define="MP_DEVICE_WATCHY"
            tlsf_source="tlsf/tlsf.c"
            tlsf_dir="$COMPAT/esp-idf-$version/tlsf"
            ;;
        m5paper)
            version="4.4.1"
            define="MP_DEVICE_M5PAPER"
            tlsf_source="heap_tlsf.c"
            tlsf_dir="$COMPAT/esp-idf-$version"
            ;;
        *)
            echo "unknown profile: $profile" >&2
            return 2
            ;;
    esac

    local src="$COMPAT/esp-idf-$version"
    local dst="$OUT/$profile"
    mkdir -p "$dst"

    local cflags=(
        -std=c11 -O2 -Wno-format
        -DCONFIG_HEAP_POISONING_LIGHT=1
        -DCONFIG_HEAP_TLSF_USE_ROM_IMPL=0
        -I"$HERE/compat" -I"$src/include" -I"$src" -I"$tlsf_dir"
    )
    "$EMCC" "${cflags[@]}" -c "$src/$tlsf_source" -o "$dst/tlsf.o"
    "$EMCC" "${cflags[@]}" -c "$src/multi_heap.c" -o "$dst/multi_heap.o"
    "$EMCC" "${cflags[@]}" -c "$src/multi_heap_poisoning.c" -o "$dst/poison.o"

    "$EMXX" -std=c++17 -O2 -D"$define"=1 \
        -I"$HERE" -I"$src/include" \
        "$HERE/device_allocator.cpp" "$HERE/allocator_smoke.cpp" \
        "$dst/tlsf.o" "$dst/multi_heap.o" "$dst/poison.o" \
        -sEXIT_RUNTIME=1 -o "$dst/smoke.js"

    node "$dst/smoke.js"
}

if [ "$#" -eq 0 ]; then
    build_profile watchy
    build_profile m5paper
else
    for profile in "$@"; do build_profile "$profile"; done
fi
