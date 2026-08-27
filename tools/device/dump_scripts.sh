#!/usr/bin/env bash
#
# dump_scripts.sh — back up the Micropatterns scripts stored on an M5Paper.
#
# The firmware keeps scripts in the SPIFFS partition (see
# M5Paper_MicroPatterns/default_16MB.csv and src/script_manager.cpp):
#
#   /scripts/list.json           the script list / metadata
#   /scripts/content/<file_id>   one file per script body
#   /scripts/script_states.json  per-script persisted state
#
# These exist ONLY on the device. The repo tracks just one script
# (micropatterns_server/local-s3-storage/scripts/7qpkkx4dys/city.json), so
# losing the device's SPIFFS loses everything else. Run this before flashing
# anything to an M5Paper.
#
# READ-ONLY with respect to the device: it only ever calls `esptool read_flash`.
# It never writes, erases, or flashes.
#
# Usage:
#   tools/device/dump_scripts.sh [--port /dev/cu.usbserial-XXXX] [--out DIR] [--baud N]
#
# The CH9102 bridge on the M5Paper is unreliable above ~460800; if a read dies
# with "Unable to verify flash chip connection", retry with --baud 460800.
#
# If --port is omitted it tries to autodetect a single /dev/cu.usbserial-*.
#
# NOTE ON TIMING: the M5Paper firmware drops into light sleep after a short
# idle timeout and its CH9102 USB-serial bridge powers down with it, removing
# /dev/cu.usbserial-*. Run this promptly after connecting, or hold a button to
# keep the device awake.

set -euo pipefail

# SPIFFS partition geometry, from M5Paper_MicroPatterns/default_16MB.csv:
#   spiffs, data, spiffs, 0xc90000, 0x370000
SPIFFS_OFFSET=0xc90000
SPIFFS_SIZE=0x370000
# Arduino-ESP32 SPIFFS defaults.
BLOCK_SIZE=4096
PAGE_SIZE=256

PIO_PY="$HOME/.platformio/penv/bin/python"
ESPTOOL="$HOME/.platformio/packages/tool-esptoolpy/esptool.py"
MKSPIFFS="$HOME/.platformio/packages/tool-mkspiffs/mkspiffs_espressif32_arduino"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PORT=""
BAUD=921600
OUT="$REPO_ROOT/tools/device/backups/$(date +%Y-%m-%d-%H%M%S)"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --port) PORT="$2"; shift 2 ;;
    --baud) BAUD="$2"; shift 2 ;;
    --out)  OUT="$2";  shift 2 ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

for f in "$PIO_PY" "$ESPTOOL" "$MKSPIFFS"; do
  [[ -e "$f" ]] || { echo "missing required tool: $f" >&2; exit 1; }
done

if [[ -z "$PORT" ]]; then
  mapfile -t CANDIDATES < <(ls /dev/cu.usbserial-* 2>/dev/null || true)
  if [[ ${#CANDIDATES[@]} -eq 1 ]]; then
    PORT="${CANDIDATES[0]}"
    echo "autodetected port: $PORT"
  else
    echo "could not autodetect a unique port; found: ${CANDIDATES[*]:-none}" >&2
    echo "pass --port /dev/cu.usbserial-XXXX" >&2
    echo "(note: the Watchy and the M5Paper both enumerate as cu.usbserial-*)" >&2
    exit 1
  fi
fi

mkdir -p "$OUT"
IMG="$OUT/spiffs.bin"

echo "==> reading SPIFFS partition ($SPIFFS_SIZE bytes @ $SPIFFS_OFFSET) from $PORT"
"$PIO_PY" "$ESPTOOL" --port "$PORT" --baud "$BAUD" \
  read_flash "$SPIFFS_OFFSET" "$SPIFFS_SIZE" "$IMG"

echo "==> unpacking SPIFFS image"
mkdir -p "$OUT/files"
"$MKSPIFFS" -u "$OUT/files" -b "$BLOCK_SIZE" -p "$PAGE_SIZE" \
  -s "$((SPIFFS_SIZE))" "$IMG" || {
    echo "mkspiffs unpack reported an error; the raw image is still at $IMG" >&2
    echo "you can retry unpacking manually, or inspect with:" >&2
    echo "  $MKSPIFFS -l -b $BLOCK_SIZE -p $PAGE_SIZE -s $SPIFFS_SIZE $IMG" >&2
    exit 1
  }

echo
echo "==> backup complete: $OUT"
find "$OUT/files" -type f -print0 2>/dev/null | xargs -0 ls -l 2>/dev/null || true
echo
echo "scripts recovered:"
ls -1 "$OUT/files/scripts/content" 2>/dev/null || echo "  (no /scripts/content — check $OUT/files)"
