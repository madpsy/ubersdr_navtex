#!/usr/bin/env bash
# start.sh — start the ubersdr_navtex service
#
# Usage:
#   ./start.sh

set -euo pipefail

INSTALL_DIR="${HOME}/ubersdr/navtex"

cd "${INSTALL_DIR}"
echo "Starting ubersdr_navtex..."
docker compose up -d --remove-orphans
echo "Done."
echo "  View logs : docker compose logs -f"
