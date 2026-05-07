#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

port="${SATMON_FRONTEND_PORT:-5173}"

SATMON_FRONTEND_PORT="$port" python3 server.py
