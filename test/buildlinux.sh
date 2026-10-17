#!/bin/bash
# Builds the Linux tools: rfstool (engine test tool) and, when present, refsfmt and refschk.
D=$(cd "$(dirname "$0")/.." && pwd); OUT=${OUT:-/tmp/refsbuild}; mkdir -p "$OUT"
ENG="$D/src/rfssup.c $D/src/rfsupcase.c $D/src/rfsvol.c $D/src/rfstree.c $D/src/rfsfile.c"
for f in rfsv1 rfsalloc rfswrite rfscommit rfsops rfsshim; do [ -f "$D/src/$f.c" ] && ENG="$ENG $D/src/$f.c"; done
ENG="$ENG $D/fmt/rfsmk1.c"
CF="-I$OUT -O1 -g -Wall -Wno-unused-function -Wno-pointer-sign -I$D/src -I$D/tools -I$D/fmt ${CFLAGS:-}"
set -e
if [ -f "$D/boot/refsboot.S" ]; then
    gcc -E -P -x assembler-with-cpp "$D/boot/refsboot.S" > "$OUT/refsboot.s"
    as --32 -o "$OUT/refsboot.o" "$OUT/refsboot.s"
    ld -m elf_i386 -Ttext 0x7C00 --oformat binary -o "$OUT/refsboot.bin" "$OUT/refsboot.o"
    python3 "$D/build/bin2h.py" "$OUT/refsboot.bin" RfsBootCode > "$OUT/refsbootbin.h"
fi
cc $CF $ENG "$D/tools/rfstool.c" "$D/tools/rfsimg.c" -o "$OUT/rfstool"
[ -f "$D/fmt/refsfmt.c" ] && cc $CF "$D/fmt/refsfmt.c" "$D/fmt/rfsmkfs.c" "$D/tools/rfsimg.c" $ENG -o "$OUT/refsfmt"
[ -f "$D/fmt/refsboot.c" ] && cc $CF "$D/fmt/refsboot.c" "$D/tools/rfsimg.c" $ENG -o "$OUT/refsboot"
[ -f "$D/chk/refschk.c" ] && cc $CF "$D/chk/refschk.c" "$D/tools/rfsimg.c" $ENG -o "$OUT/refschk"
true
# driver-layer tests (engine + rfx.c)
cc $CF $ENG "$D/src/rfx.c" "$D/test/rfxtest.c"   "$D/tools/rfsimg.c" -o "$OUT/rfxtest"   2>/dev/null || true
cc $CF $ENG "$D/src/rfx.c" "$D/test/rfxstress.c" "$D/tools/rfsimg.c" -o "$OUT/rfxstress" 2>/dev/null || true
cc $CF $ENG "$D/src/rfx.c" "$D/test/enumtest.c"  "$D/tools/rfsimg.c" -o "$OUT/enumtest"  2>/dev/null || true
