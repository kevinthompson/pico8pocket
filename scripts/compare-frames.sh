#!/usr/bin/env bash
# Frame-by-frame comparison of two builds of the runtime over many carts.
#
#   scripts/compare-frames.sh build OUT         build a deterministic scanner
#                                               of the working tree into OUT
#   scripts/compare-frames.sh diff OLD NEW CART...   compare two such scanners
#
# Typical use: build OUT=/tmp/old on the base commit (git stash or a
# checkout, then make deps), build OUT=/tmp/new with your change, then diff
# them over a folder of carts.  Each cart runs FRAMES (default 1500) frames
# of scripted input with seed 1; a cart is "same" when every frame's
# framebuffer hash matches.
#
# The scanner fixes Lua's string-hash seed; run under setarch -R so heap
# addresses repeat too.  Even so, a few carts (Kiloman, Grippy) iterate
# tables in address order and change their frames when only the binary's
# layout changes; check such DIFFs by padding static data or diffing the
# frame where they diverge before calling them regressions.
#
# diff exits 1 when any cart differs, and 2 when a scanner cannot run.  A
# cart that errors is compared like any other output: the same error at the
# same frame in both builds counts as "same".
set -euo pipefail
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FRAMES="${FRAMES:-1500}"

case "${1:-}" in
build)
    out="$(realpath -m "$2")"
    Z="$PROJECT_ROOT/.deps/fake-08/libs/z8lua"
    M="$PROJECT_ROOT/.deps/fake-08/libs/miniz"
    [[ -d "$Z" ]] || { echo "run make deps first" >&2; exit 1; }
    sources=()
    for n in eris lapi lauxlib lbaselib lcode lcorolib lctype ldblib ldebug ldo ldump \
             lfunc lgc linit llex lmem lobject lopcodes lparser lpico8lib lstate \
             lstring lstrlib ltable ltablib ltm lundump lvm lzio; do
        sources+=("$Z/$n.c")
    done
    cd "$PROJECT_ROOT"
    g++ -x c++ -std=c++17 -O2 -fwrapv -DP8P_RUNTIME_DEBUG -w '-Dluai_makeseed()=0u' \
        -Iinclude -I"$Z" -I"$M" tools/compat_scan.cpp src/runtime/runtime.cpp \
        src/runtime/palette_fast.cpp \
        src/runtime/audio.c src/runtime/cart.c src/runtime/cart_png.c \
        "${sources[@]}" "$M/miniz.c" -lm -o "$out"
    echo "built $out"
    ;;
diff)
    old="$2"; new="$3"; shift 3
    same=0; differ=0
    # Scanner exit codes 0, 3 (load error) and 4 (runtime error) are results
    # to compare; anything else (missing binary, setarch failure, bad cart
    # path) means the run itself failed.
    scan() {
        local output status=0
        output="$(P8P_EVERY_FRAME=1 setarch -R "$1" "$2" "$FRAMES" 1 2>&1)" ||
            status=$?
        case "$status" in
        0|3|4) ;;
        *) echo "scan error: $1 $2 exited $status: ${output:0:200}" >&2
           exit 2 ;;
        esac
        printf '%s\nexit=%s\n' "$output" "$status" | grep -v 'avg=' || true
    }
    for cart in "$@"; do
        a="$(scan "$old" "$cart")"
        b="$(scan "$new" "$cart")"
        if [[ "$a" == "$b" ]]; then
            same=$((same + 1))
        else
            differ=$((differ + 1))
            first="$(diff <(printf '%s\n' "$a") <(printf '%s\n' "$b") | grep -m1 '^<' || true)"
            echo "DIFF $(basename "$cart"): first differing line: ${first#< }"
        fi
    done
    echo "same=$same differ=$differ"
    [[ "$differ" -eq 0 ]] || exit 1
    ;;
*)
    sed -n '2,19p' "$0" >&2
    exit 2
    ;;
esac
