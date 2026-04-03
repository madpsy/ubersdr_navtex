#!/bin/sh
# entrypoint.sh — translate environment variables into navtex_rx_from_ubersdr flags
#
# Environment variables:
#   UBERSDR_URL    UberSDR base URL (default: http://172.20.0.1:8080)
#   NAVTEX_FREQ    NAVTEX carrier frequency in Hz (default: 518000)
#   WEB_PORT       Port for the web UI server (default: 6092)

set -e

URL="${UBERSDR_URL:-http://172.20.0.1:8080}"
FREQ="${NAVTEX_FREQ:-518000}"
PORT="${WEB_PORT:-6092}"

exec /usr/local/bin/navtex_rx_from_ubersdr \
    "$URL" \
    "$FREQ" \
    --web-port "$PORT" \
    "$@"
