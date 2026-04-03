#!/usr/bin/env bash
# restart.sh — restart the ubersdr_navtex service
#
# Usage:
#   ./restart.sh

set -euo pipefail

INSTALL_DIR="${HOME}/ubersdr/navtex"

cd "${INSTALL_DIR}"
echo "Stopping ubersdr_navtex..."
docker compose down
echo "Starting ubersdr_navtex..."
docker compose up -d --remove-orphans
echo "Done."
echo "  View logs : docker compose logs -f"
