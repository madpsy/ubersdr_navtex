#!/usr/bin/env bash
# Build (incrementally) and run the evaluator for one variant.
#
#   test/ident/run_eval.sh <variant> all [--tags]      train + holdout (if present) + real (+ proxy)
#   test/ident/run_eval.sh <variant> <csv>[@N] ... [--tags] [--proxy F] [--proxy-min-conf X]
#
# <variant> names test/ident/variants/<variant>.h.  Builds into build/<variant>/
# (one directory per variant, so parallel agents never clash).  The evaluator core
# is compiled once into build/_core/ and shared; only the variant is recompiled
# when its header changes.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
V="${1:?usage: run_eval.sh <variant> all|<csv>... [--tags]}"; shift
VH="$HERE/variants/$V.h"
[ -f "$VH" ] || { echo "no such variant: $VH" >&2; exit 2; }
CXX="${CXX:-g++}"
FLAGS=(-std=c++11 -O2)
SCRATCH="${NAVTEX_IDENT_WORK:-$HERE/work}/ident"   # working data outside git, see README
PROXY="${IDENT_PROXY:-$SCRATCH/real_proxy.csv}"
STATIONS="$HERE/../../src/navtex_stations.h"

newer() {   # newer TARGET DEP... : true if TARGET is missing or older than any DEP
    local t="$1"; shift
    [ -e "$t" ] || return 0
    local d; for d in "$@"; do [ "$d" -nt "$t" ] && return 0; done
    return 1
}

CORE="$HERE/build/_core/eval_core.o"
mkdir -p "$HERE/build/_core" "$HERE/build/$V"
if newer "$CORE" "$HERE/eval.cpp" "$HERE/ident_api.h" "$STATIONS"; then
    tmp="$CORE.$$.tmp.o"
    "$CXX" "${FLAGS[@]}" -DEVAL_CORE_ONLY -c "$HERE/eval.cpp" -o "$tmp"
    mv -f "$tmp" "$CORE"                  # atomic: parallel builders never see a half-written object
fi
BIN="$HERE/build/$V/eval"
VO="$HERE/build/$V/variant.o"
if newer "$VO" "$VH" "$HERE/ident_api.h" "$STATIONS"; then
    printf '#include "%s"\n' "$VH" > "$HERE/build/$V/variant.cpp"
    "$CXX" "${FLAGS[@]}" -c "$HERE/build/$V/variant.cpp" -o "$VO.tmp" && mv -f "$VO.tmp" "$VO"
fi
if newer "$BIN" "$VO" "$CORE"; then
    "$CXX" "${FLAGS[@]}" "$VO" "$CORE" -o "$BIN.tmp" && mv -f "$BIN.tmp" "$BIN"
fi

ARGS=()
HAVE_PROXY=0
for a in "$@"; do
    case "$a" in
        all)
            ARGS+=("$HERE/data/train.csv")
            [ -f "$HERE/data/holdout.csv" ] && ARGS+=("$HERE/data/holdout.csv")
            [ -f "$HERE/data/real.csv" ] && ARGS+=("$HERE/data/real.csv")
            ;;
        --proxy) HAVE_PROXY=1; ARGS+=("$a") ;;
        *) ARGS+=("$a") ;;
    esac
done
if [ $HAVE_PROXY = 0 ] && [ -f "$PROXY" ]; then ARGS=(--proxy "$PROXY" "${ARGS[@]}"); fi
exec "$BIN" "${ARGS[@]}"
