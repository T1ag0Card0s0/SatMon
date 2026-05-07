#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

port="${SATMON_PORT:-/dev/ttyUSB0}"
export_file="${IDF_EXPORT_FILE:-$HOME/.esp-idf/export.sh}"

if [[ ! -f "$export_file" ]]; then
    echo "ESP-IDF export script not found: $export_file" >&2
    echo "Set IDF_EXPORT_FILE or install ESP-IDF first." >&2
    exit 1
fi

# shellcheck source=/dev/null
. "$export_file" >/dev/null

idf.py reconfigure build
idf.py -p "$port" flash monitor
