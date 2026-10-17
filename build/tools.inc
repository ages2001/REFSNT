# shared by build_cross.sh and build_amd64.sh: the tools
TOOLCRT="-include $HERE/toolcrt.h \
 -D_INC_STDIO -D_INC_STDLIB -D_INC_STRING -D_INC_CTYPE -D_INC_TIME -D_INC_STDARG \
 -D_STDIO_H_=1 -D_STDLIB_H=1 -D_STRING_H_=1 -D_CTYPE_H_=1 -D_TIME_H_=1 \
 -Dmain=appmain -Dprintf=ext_printf -Dfprintf=ext_fprintf -Dsprintf=ext_sprintf -Dsnprintf=ext_snprintf -Dvsnprintf=ext_vsnprintf \
 -Dvprintf=ext_vprintf -Dmalloc=ext_malloc -Dfree=ext_free -Dcalloc=ext_calloc -Drealloc=ext_realloc -Dstrdup=ext_strdup \
 -Dstrtoul=ext_strtoul -Dtime=ext_time -Dlocaltime=ext_localtime -Dtoupper=ext_toupper -D_stricmp=ext_stricmp"
TINC="-I$SRC -I$ROOT/tools -I$ROOT/fmt -I$ROOT/chk -I$ROOT/boot -I$OUT"
ENGSRC="$SRC/rfssup.c $SRC/rfsupcase.c $SRC/rfsvol.c $SRC/rfstree.c $SRC/rfsfile.c $SRC/rfswrite.c $SRC/rfsalloc.c \
 $SRC/rfscommit.c $SRC/rfsops.c $SRC/rfsv1.c $SRC/rfsshim.c $ROOT/tools/rfsimg.c"
if [ -f "$ROOT/boot/refsboot.S" ]; then
    echo "Assembling the boot loader..."
    gcc -E -P -x assembler-with-cpp "$ROOT/boot/refsboot.S" > "$OUT/refsboot.s"
    as --32 -o "$OUT/refsboot.o" "$OUT/refsboot.s"
    ld -m elf_i386 -Ttext 0x7C00 --oformat binary -o "$OUT/refsboot.bin" "$OUT/refsboot.o"
    rm -f "$OUT/refsboot.o" "$OUT/refsboot.s"
    python3 "$HERE/bin2h.py" "$OUT/refsboot.bin" RfsBootCode > "$OUT/refsbootbin.h"
fi
echo "Linking the tools..."
$GCC $EXEBASE $TOOLCRT $TINC -o "$OUT/refschk.exe" "$ROOT/chk/refschk.c" $ENGSRC "$HERE/toolcrt.c" -lkernel32 -lgcc
[ -f "$ROOT/fmt/refsfmt.c" ] && $GCC $EXEBASE $TOOLCRT $TINC -o "$OUT/refsfmt.exe" "$ROOT/fmt/refsfmt.c" "$ROOT/fmt/rfsmkfs.c" $ENGSRC "$HERE/toolcrt.c" -lkernel32 -lgcc
[ -f "$ROOT/fmt/refsboot.c" ] && $GCC $EXEBASE $TOOLCRT $TINC -o "$OUT/refsboot.exe" "$ROOT/fmt/refsboot.c" $ENGSRC "$HERE/toolcrt.c" -lkernel32 -lgcc
$GCC $EXEBASE $TOOLCRT -o "$OUT/refsinst.exe" "$ROOT/inst/refsinst.c" "$HERE/toolcrt.c" -ladvapi32 -lkernel32 -lgcc
