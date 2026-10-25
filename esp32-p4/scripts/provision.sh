#!/usr/bin/env bash
# Clears the box's saved settings (its NVS partition) in one step, so it starts like a new one: no saved Wi-Fi network or hotspot password.
#
#   scripts/provision.sh -p /dev/ttyACM0       clear the saved settings
#   scripts/provision.sh --dry-run             build the image and show what it holds, flash nothing
#
# This REPLACES the whole NVS partition. The application, the catalog and the rest of the flash are not touched. Flash the application
# first (see README.md), then run this.
#
# Needs ESP-IDF's tools (set IDF_PATH, or run its export.sh first) and espflash.
set -euo pipefail

PORT=""
DRY=0
NVS_OFFSET="0x9000" # partitions.csv
NVS_SIZE="0x6000"   # partitions.csv

usage() { sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    -p|--port) PORT="${2:-}"; shift 2 ;;
    --dry-run) DRY=1; shift ;;
    -h|--help) usage 0 ;;
    *) echo "unknown option: $1" >&2; usage 1 ;;
  esac
done

if [[ $DRY -eq 0 && -z "$PORT" ]]; then
  echo "give the serial port with -p (or use --dry-run)" >&2; exit 1
fi

GEN="${IDF_PATH:-}/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
if [[ ! -f "$GEN" ]]; then
  echo "ESP-IDF's nvs_partition_gen.py not found: set IDF_PATH (or source ESP-IDF's export.sh)." >&2; exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# An empty namespace: the firmware reads `ssid`, `pass` and `ap_pass` from "tonerelay" and finds nothing.
{
  echo "key,type,encoding,value"
  echo "tonerelay,namespace,,"
} > "$WORK/nvs.csv"

python3 "$GEN" generate "$WORK/nvs.csv" "$WORK/nvs.bin" "$NVS_SIZE" >/dev/null

echo "NVS image ($(wc -c < "$WORK/nvs.bin" | tr -d ' ') bytes):"
sed 's/^/  /' "$WORK/nvs.csv"

if [[ $DRY -eq 1 ]]; then
  echo "dry run: nothing was written."
  exit 0
fi

echo "writing it to $NVS_OFFSET on $PORT (the saved Wi-Fi network and hotspot password are erased) ..."
espflash write-bin -p "$PORT" --chip esp32p4 "$NVS_OFFSET" "$WORK/nvs.bin"
echo "done. Power-cycle the board; it starts like a new box."
