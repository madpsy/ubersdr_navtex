#!/usr/bin/env bash
# Inner loop: fixed deterministic 20k-row sample of train (per-receiver chronological
# windows) + all of real, with proxy truth.  < 1 s once built.
#   test/ident/quick.sh <variant> [--tags]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
V="${1:?usage: quick.sh <variant> [--tags]}"; shift
exec "$HERE/run_eval.sh" "$V" "$HERE/data/train.csv@20000" "$HERE/data/real.csv" "$@"
