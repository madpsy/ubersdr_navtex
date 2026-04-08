#!/bin/sh
# entrypoint.sh — translate environment variables into navtex_rx_from_ubersdr flags
#
# Environment variables:
#   UBERSDR_URL              UberSDR base URL (default: http://ubersdr:8080)
#   NAVTEX_FREQ_1            First NAVTEX carrier frequency in Hz  (default: 518000 — International)
#   NAVTEX_FREQ_2            Second NAVTEX carrier frequency in Hz (default: 490000 — National/coastal)
#   WEB_PORT                 Port for the web UI server (default: 6092)
#   NAVTEX_LOG_DIR           Directory to save decoded messages (default: /data/navtex_logs)
#                            Set to "" to disable logging.
#   NAVTEX_LOG_RETAIN_DAYS   Delete log files older than this many days (default: 90, 0=disabled)
#
# Both frequencies are always monitored simultaneously.

set -e

URL="${UBERSDR_URL:-http://ubersdr:8080}"
FREQ1="${NAVTEX_FREQ_1:-518000}"
FREQ2="${NAVTEX_FREQ_2:-490000}"
PORT="${WEB_PORT:-6092}"
LOG_DIR="${NAVTEX_LOG_DIR:-/data/navtex_logs}"
RETAIN_DAYS="${NAVTEX_LOG_RETAIN_DAYS:-90}"

# Build the log-dir argument (omit flag entirely if LOG_DIR is empty)
LOG_ARG=""
if [ -n "$LOG_DIR" ]; then
    LOG_ARG="--log-dir $LOG_DIR"
fi

# Build the log-retain-days argument
RETAIN_ARG=""
if [ -n "$LOG_DIR" ] && [ -n "$RETAIN_DAYS" ]; then
    RETAIN_ARG="--log-retain-days $RETAIN_DAYS"
fi

exec /usr/local/bin/navtex_rx_from_ubersdr \
    "$URL" \
    --freq "$FREQ1" \
    --freq "$FREQ2" \
    --web-port "$PORT" \
    $LOG_ARG \
    $RETAIN_ARG \
    "$@"
