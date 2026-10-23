#!/usr/bin/env bash
# Writes the box's saved settings (its NVS partition) in one step, so a box can be handed over in a known state.
#
#   scripts/provision.sh --mode helrig  -p /dev/cu.usbmodemXXXX     a Helrig Box: Helrig names, relay-only on
#   scripts/provision.sh --mode tonerelay -p /dev/ttyACM0            a plain ToneRelay box
#   scripts/provision.sh --mode helrig --dry-run                     build the image and show what it holds, flash nothing
#
# This REPLACES the whole NVS partition: the saved Wi-Fi network, the hotspot password, the relay setting and the mode all
# go, so the box starts like a new one (it raises its setup network on the next boot). The application, the catalog and
# the rest of the flash are not touched. Flash the application first (see README.md), then run this.
#
# Needs ESP-IDF's tools (set IDF_PATH, or run its export.sh first) and espflash.
set -euo pipefail

MODE="tonerelay"
PORT=""
DRY=0
NVS_OFFSET="0x9000" # partitions.csv
NVS_SIZE="0x6000"   # partitions.csv

usage() { sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --mode) MODE="${2:-}"; shift 2 ;;
    -p|--port) PORT="${2:-}"; shift 2 ;;
    --dry-run) DRY=1; shift ;;
    -h|--help) usage 0 ;;
    *) echo "unknown option: $1" >&2; usage 1 ;;
  esac
done

case "$MODE" in
  tonerelay|helrig) ;;
  *) echo "--mode must be tonerelay or helrig (got '$MODE')" >&2; exit 1 ;;
esac
if [[ $DRY -eq 0 && -z "$PORT" ]]; then
  echo "give the serial port with -p (or use --dry-run)" >&2; exit 1
fi

GEN="${IDF_PATH:-}/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
if [[ ! -f "$GEN" ]]; then
  echo "ESP-IDF's nvs_partition_gen.py not found: set IDF_PATH (or source ESP-IDF's export.sh)." >&2; exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# The namespace and keys are the ones the firmware reads: mode.c (`mode`) and relay.c (`relay_only`).
{
  echo "key,type,encoding,value"
  echo "tonerelay,namespace,,"
  if [[ "$MODE" == "helrig" ]]; then
    echo "mode,data,string,helrig"
    echo "relay_only,data,u8,1"
  fi
} > "$WORK/nvs.csv"

python3 "$GEN" generate "$WORK/nvs.csv" "$WORK/nvs.bin" "$NVS_SIZE" >/dev/null

echo "NVS image for mode '$MODE' ($(wc -c < "$WORK/nvs.bin" | tr -d ' ') bytes):"
sed 's/^/  /' "$WORK/nvs.csv"

if [[ $DRY -eq 1 ]]; then
  echo "dry run: nothing was written."
  exit 0
fi

echo "writing it to $NVS_OFFSET on $PORT (the saved Wi-Fi network and hotspot password are erased) ..."
espflash write-bin -p "$PORT" --chip esp32p4 "$NVS_OFFSET" "$WORK/nvs.bin"
echo "done. Power-cycle the board; it starts as a new '$MODE' box."
