#!/usr/bin/env bash
# stop.sh — stop the ubersdr_navtex service
#
# Usage:
#   ./stop.sh

set -euo pipefail

INSTALL_DIR="${HOME}/ubersdr/navtex"

cd "${INSTALL_DIR}"
echo "Stopping ubersdr_navtex..."
docker compose down
echo "Done."
