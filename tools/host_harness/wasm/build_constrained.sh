#!/usr/bin/env bash
# Build versioned device-resource simulators. Pixel correctness still comes from
# the same firmware C++ files; allocation placement comes from each firmware's
# exact ESP-IDF multi_heap/TLSF version.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
HARNESS="$(dirname "$HERE")"
FW_SRC="$HARNESS/../../M5Paper_MicroPatterns/src"
CONSTRAINTS="$HARNESS/device_constraints"
COMPAT="$HARNESS/device_compat"
EMSDK_DIR="${EMSDK:-$HOME/Documents/GitHub/qemu-ipod_touch_1g/.wasm-toolchain/emsdk}"
EMCC="$EMSDK_DIR/upstream/emscripten/emcc"
EMXX="$EMSDK_DIR/upstream/emscripten/em++"
OUT="${OUT:-$HERE/out}"

if [ ! -x "$EMCC" ]; then
    echo "emcc not found at $EMCC" >&2
    exit 1
fi

build_profile() {
    local profile="$1"
    local version arduino_version define board_define tlsf_source tlsf_dir output
    case "$profile" in
        watchy)
            version="5.3.2"
            arduino_version="3.1.3"
            define="MP_DEVICE_WATCHY"
            board_define=""
            tlsf_source="tlsf/tlsf.c"
            tlsf_dir="$COMPAT/esp-idf-$version/tlsf"
            output="mp_render_watchy"
            ;;
        m5paper)
            version="4.4.1"
            arduino_version="2.0.4"
            define="MP_DEVICE_M5PAPER"
            board_define="-DBOARD_HAS_PSRAM=1"
            tlsf_source="heap_tlsf.c"
            tlsf_dir="$COMPAT/esp-idf-$version"
            output="mp_render_m5paper"
            ;;
        *)
            echo "unknown profile: $profile" >&2
            return 2
            ;;
    esac

    local src="$COMPAT/esp-idf-$version"
    local arduino="$COMPAT/arduino-esp32-$arduino_version"
    local arduino_host="$COMPAT/arduino_host"
    local obj="$OUT/obj-$profile"
    mkdir -p "$OUT" "$obj"
    local cflags=(
        -std=c11 -Os -Wno-format
        -DCONFIG_HEAP_POISONING_LIGHT=1
        -DCONFIG_HEAP_TLSF_USE_ROM_IMPL=0
        -I"$CONSTRAINTS/compat" -I"$src/include" -I"$src" -I"$tlsf_dir"
    )
    "$EMCC" "${cflags[@]}" -c "$src/$tlsf_source" -o "$obj/tlsf.o"
    "$EMCC" "${cflags[@]}" -c "$src/multi_heap.c" -o "$obj/multi_heap.o"
    "$EMCC" "${cflags[@]}" -c "$src/multi_heap_poisoning.c" -o "$obj/poison.o"
    "$EMCC" -std=c11 -Os \
        -I"$arduino" -I"$arduino_host" \
        -c "$arduino/stdlib_noniso.c" -o "$obj/stdlib_noniso.o"
    "$EMCC" -std=c11 -Os \
        -I"$arduino" -I"$arduino_host" \
        -c "$arduino_host/stdlib_itoa.c" -o "$obj/stdlib_itoa.o"
    "$EMXX" -std=c++17 -Os -fexceptions -fno-rtti \
        -DMP_DEVICE_CONSTRAINTS=1 -D"$define"=1 $board_define \
        -I"$arduino" -I"$arduino_host" -I"$HARNESS/shim" -I"$CONSTRAINTS" \
        -include "$CONSTRAINTS/device_string_alloc_redirect.h" \
        -DHOST_LOG_LEVEL=0 \
        -c "$arduino/WString.cpp" -o "$obj/WString.o"

    "$EMXX" -std=c++17 -O2 -fexceptions -fno-rtti \
        -DMP_DEVICE_CONSTRAINTS=1 -D"$define"=1 $board_define \
        -I"$arduino" -I"$arduino_host" -I"$CONSTRAINTS" -I"$src/include" \
        "$CONSTRAINTS/device_allocator.cpp" \
        "$CONSTRAINTS/device_new.cpp" \
        "$CONSTRAINTS/string_smoke.cpp" \
        "$obj/WString.o" "$obj/stdlib_noniso.o" "$obj/stdlib_itoa.o" \
        "$obj/tlsf.o" "$obj/multi_heap.o" "$obj/poison.o" \
        -sEXIT_RUNTIME=1 -o "$obj/string_smoke.js"
    node "$obj/string_smoke.js"

    "$EMXX" \
        -std=c++17 -Os -fexceptions -fno-rtti \
        -DMP_DEVICE_CONSTRAINTS=1 -DARDUINO_ARCH_ESP32=1 -D"$define"=1 $board_define \
        -I"$arduino" -I"$arduino_host" -I"$HARNESS/shim" -I"$HARNESS/src" -I"$FW_SRC" \
        -I"$CONSTRAINTS" -I"$src/include" \
        -DHOST_LOG_LEVEL=0 \
        "$FW_SRC/micropatterns_parser.cpp" \
        "$FW_SRC/micropatterns_runtime.cpp" \
        "$FW_SRC/micropatterns_drawing.cpp" \
        "$FW_SRC/display_list_renderer.cpp" \
        "$FW_SRC/occlusion_buffer.cpp" \
        "$FW_SRC/matrix_utils.cpp" \
        "$HARNESS/src/host_display_manager.cpp" \
        "$HARNESS/src/render_path.cpp" \
        "$CONSTRAINTS/device_allocator.cpp" \
        "$CONSTRAINTS/device_new.cpp" \
        "$HERE/mp_wasm.cpp" \
        "$obj/WString.o" "$obj/stdlib_noniso.o" "$obj/stdlib_itoa.o" \
        "$obj/tlsf.o" "$obj/multi_heap.o" "$obj/poison.o" \
        -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=web,node \
        -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=32MB \
        -sEXPORTED_RUNTIME_METHODS='["cwrap","HEAPU8","UTF8ToString"]' \
        -sEXPORTED_FUNCTIONS='["_mp_parse","_mp_parse_error_count","_mp_parse_error_at","_mp_asset_count","_mp_asset_name","_mp_asset_original_name","_mp_asset_width","_mp_asset_height","_mp_asset_data","_mp_render","_mp_pixels","_mp_width","_mp_height","_mp_error","_mp_display_list_items","_mp_rendered_items","_mp_culled_offscreen","_mp_culled_occlusion","_mp_ms_parse","_mp_ms_displaylist","_mp_ms_rasterize","_mp_set_device_state","_mp_device_profile","_mp_device_arduino","_mp_device_idf","_mp_device_profile_calibrated","_mp_device_state_calibrated","_mp_mem_allocation_calls","_mp_mem_realloc_calls","_mp_mem_free_calls","_mp_mem_initial_internal_free","_mp_mem_initial_internal_largest","_mp_mem_current_internal_free","_mp_mem_current_internal_largest","_mp_mem_peak_internal_used","_mp_mem_initial_psram_free","_mp_mem_current_psram_free","_mp_mem_peak_psram_used","_mp_mem_failure_valid","_mp_mem_failure_request","_mp_mem_failure_phase","_mp_mem_failure_source","_mp_mem_failure_capability","_mp_mem_failure_internal_free","_mp_mem_failure_internal_largest","_mp_mem_failure_psram_free","_mp_mem_failure_psram_largest","_malloc","_free"]' \
        -o "$OUT/$output.js"

    # Emscripten emits a whitespace-only line in its ES-module wrapper. Keep
    # committed static artifacts friendly to repository whitespace checks.
    sed 's/[[:blank:]]*$//' "$OUT/$output.js" > "$OUT/$output.js.tmp"
    mv "$OUT/$output.js.tmp" "$OUT/$output.js"

    ls -lh "$OUT/$output.wasm" "$OUT/$output.js" | awk '{print "  " $9 "  " $5}'
}

if [ "$#" -eq 0 ]; then
    build_profile watchy
    build_profile m5paper
else
    for profile in "$@"; do build_profile "$profile"; done
fi
