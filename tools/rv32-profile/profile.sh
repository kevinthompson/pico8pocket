#!/usr/bin/env bash
# RV32 instruction profile of a cart, built with the Pocket's compiler flags
# and run under qemu-riscv32 (picolibc, no OS).  Counts the instructions the
# Pocket CPU would execute per frame, per function, and simulates its 8 KB
# instruction cache (code in .app_fasttext runs from BRAM and bypasses it).
#
#   tools/rv32-profile/profile.sh CART [SETUP_LUA] [WARM_FRAMES] [FRAMES]
#
# SETUP_LUA is evaluated after load, e.g. to jump to a level:
#   "(function() goto_level(2) level_intro=0 return 1 end)()"
# Needs riscv64-unknown-elf-gcc with picolibc, qemu-riscv32 and make deps.
# memset/memcpy come from picolibc here and are slower than the SDK's musl.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
P="$(cd "$HERE/../.." && pwd)"
CART="$(realpath "$1")"; SETUP="${2:-}"; WARM="${3:-30}"; FRAMES="${4:-20}"
Z="$P/.deps/fake-08/libs/z8lua"
OUT="$P/build/rv32-profile"
mkdir -p "$OUT/obj"
python3 -I - "$CART" "$OUT/cart_data.h" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
with open(sys.argv[2], "w") as out:
    out.write("static const unsigned char cart_data[] = {")
    out.write(",".join(str(b) for b in data))
    out.write("};\nstatic const unsigned cart_data_len = %d;\n" % len(data))
PY
ARCH="-march=rv32imafc -mabi=ilp32f"
SPECS="--specs=picolibc.specs"
COMMON="$ARCH $SPECS -O3 -flto -fomit-frame-pointer -ffunction-sections -fdata-sections -falign-functions=64 -falign-loops=64 -fno-tree-loop-distribute-patterns -D_GNU_SOURCE -w -I$P/include -I$P/.deps/openfpgaos-sdk/src/sdk/include -I$P/.deps/fake-08/libs/miniz -DP8P_RUNTIME_DEBUG -include $HERE/types_fix.h"
CXX="$COMMON -fwrapv -DLUA_USE_LONGJMP -std=c++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics -include $P/include/p8p/vm_fasttext.h -I$Z -I$P/src/compat/cxx"
setup_escaped="$(printf '%s' "$SETUP" | sed 's/\\/\\\\/g; s/"/\\"/g')"
rm -f "$OUT"/obj/*.o
{
for n in eris lapi lauxlib lbaselib lcode lcorolib lctype ldblib ldebug ldo ldump lfunc lgc linit llex lmem lobject lopcodes lparser lpico8lib lstate lstring lstrlib ltable ltablib ltm lundump lvm lzio; do
    riscv64-unknown-elf-g++ $CXX -x c++ -c "$Z/$n.c" -o "$OUT/obj/$n.o" &
done
riscv64-unknown-elf-g++ $CXX -c "$P/src/runtime/runtime.cpp" -o "$OUT/obj/runtime.o" &
# Match the production leaf boundary after the runtime source split.
riscv64-unknown-elf-g++ $CXX -fno-lto -c "$P/src/runtime/palette_fast.cpp" -o "$OUT/obj/palette_fast.o" &
riscv64-unknown-elf-g++ $CXX -fno-lto -c "$P/src/runtime/vm_delegate.cpp" -o "$OUT/obj/vm_delegate.o" &
riscv64-unknown-elf-g++ $CXX -I"$OUT" "-DSETUP=\"$setup_escaped\"" -DWARM="$WARM" -DFRAMES="$FRAMES" \
    -c "$HERE/main.cpp" -o "$OUT/obj/main.o" &
for n in audio cart cart_png; do
    riscv64-unknown-elf-g++ $CXX -x c++ -c "$P/src/runtime/$n.c" -o "$OUT/obj/$n.o" &
done
riscv64-unknown-elf-gcc $COMMON -std=c11 -c "$P/.deps/fake-08/libs/miniz/miniz.c" -o "$OUT/obj/miniz.o" &
riscv64-unknown-elf-gcc $ARCH $SPECS -O2 -c "$HERE/start.c" -o "$OUT/obj/start.o" &
wait
riscv64-unknown-elf-g++ $ARCH $SPECS -O3 -flto -nostartfiles -nostdlib++ -Wl,--no-relax -Wl,--gc-sections \
    -Wl,--defsym=__flash=0x10000000 -Wl,--defsym=__flash_size=0x01000000 \
    -Wl,--defsym=__ram=0x20000000 -Wl,--defsym=__ram_size=0x08000000 \
    -Wl,--defsym=__stack_size=0x100000 "$OUT"/obj/*.o -lm -lc -lgcc -o "$OUT/sim"
cc -O2 -o "$OUT/icsim" "$HERE/icsim.c"
} 2> "$OUT/build.log" || { cat "$OUT/build.log" >&2; exit 1; }
[[ -x "$OUT/sim" ]] || { cat "$OUT/build.log" >&2; exit 1; }
B="$OUT/sim"
riscv64-unknown-elf-nm -S -n --defined-only "$B" |
    awk 'NF==4 && ($3=="t"||$3=="T"){print $1, $2, $4}' > "$B.syms"
M="$(riscv64-unknown-elf-nm "$B" | awk '$3=="sim_marker"{print $1}')"
read -r SZ LO < <(riscv64-unknown-elf-objdump -h "$B" | awk '$2==".app_fasttext"{print $3, $4}')
HI="$(printf '%x' $((16#$LO + 16#$SZ)))"
qemu-riscv32 -d in_asm,exec,nochain -D /dev/stdout "$B" 2>/dev/null |
    "$OUT/icsim" "$M" "$LO" "$HI" "$B.syms" > "$OUT/report.txt"
echo "Per frame over $FRAMES frames (after $WARM warm-up frames):"
awk -v f="$FRAMES" '
    /^fetches/ { printf "  RV32 instructions: %d (BRAM %d)\n", $2/f, $4/f }
    /-way misses/ { printf "  I-cache %s misses: %d\n", $1, $3/f }
' "$OUT/report.txt"
echo "Top functions (instructions per frame):"
awk -v f="$FRAMES" '$2=="fetch" { printf "%9d  %s\n", $1/f, $5 }' "$OUT/report.txt" |
    sort -rn | head -25 | c++filt
