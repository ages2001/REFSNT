#!/bin/bash
# boottest.sh IMAGE...: a synthetic Windows tree + filler on copies of ReFS
# images, then refsboot; the FAT view (read with mtools) must equal the ReFS
# files, rfstool check / refschk must find the volume clean. Then a hive that
# grew and a new driver, refsboot again; then refsboot /U (plain ReFS again).
B=${B:-/tmp/refsbuild}; W=${W:-/tmp/refsbuild/boottest}
export MTOOLS_SKIP_CHECK=1
rm -rf $W; mkdir -p $W/t/WINNT/System32/drivers $W/t/WINNT/System32/config $W/t/WINNT/Fonts $W/t/WINNT/inf $W/fill
cd $W/t
head -c 156496 /dev/urandom > ntldr; head -c 26816 /dev/urandom > NTDETECT.COM
printf '[boot loader]\r\ndefault=multi(0)disk(0)rdisk(0)partition(1)\\WINNT\r\n' > boot.ini
for f in ntoskrnl.exe hal.dll kdcom.dll bootvid.dll c_1252.nls l_intl.nls; do head -c $((RANDOM*20+1000)) /dev/urandom > WINNT/System32/$f; done
for i in $(seq 1 150); do head -c $((RANDOM+100)) /dev/urandom > WINNT/System32/drivers/drv$i.sys; done
head -c 3000000 /dev/urandom > WINNT/System32/config/SYSTEM; head -c 2000000 /dev/urandom > WINNT/System32/config/system.alt
head -c 5000 /dev/urandom > WINNT/Fonts/vgaoem.fon; head -c 900 /dev/urandom > WINNT/inf/biosinfo.inf
for i in $(seq 1 40); do head -c $((RANDOM*30)) /dev/urandom > $W/fill/f$i.bin; done
# the tree as rfstool operations
ops() { # $1 local root, $2 volume root
  (cd "$1"; find . -mindepth 1 -type d | sort | while read d; do echo "mkdir:$2${d#.}"; done
   find . -type f | sort | while read f; do echo "put:$1/${f#./}:$2${f#.}"; done)
}
# the byte offset of the volume (MBR, GPT or none)
voloff() { python3 -c "
import struct,sys
f=open(sys.argv[1],'rb')
def vol(o):
    f.seek(o); b=f.read(1536)
    return b[3:11] in (b'ReFS\\0\\0\\0\\0', b'REFSNTB1')
c=[0]
m=f.read(1024)
if m[512:520]==b'EFI PART':
    for i in range(128):
        f.seek(1024+128*i); s=struct.unpack('<Q',f.read(48)[32:40])[0]
        if s: c.append(s*512)
elif m[510:512]==b'\\x55\\xaa':
    c+= [struct.unpack('<I',m[446+16*i+8:446+16*i+12])[0]*512 for i in range(4)]
print(next((o for o in c if vol(o)),0))" "$1"; }
rc=0
for img in "$@"; do
  cd $W; rm -f d.raw
  cp --sparse=always "$img" d.raw
  $B/rfstool d.raw do "mkdir:/fill" $(ops $W/fill /fill) commit >/dev/null || { echo "== $img: filler failed"; rc=1; continue; }
  $B/rfstool d.raw do $(ops $W/t "") commit >/dev/null || { echo "== $img: tree failed"; rc=1; continue; }
  out=$($B/refsboot d.raw 2>&1) || { echo "== $img: refsboot failed: $out"; rc=1; continue; }
  for pass in 1 2; do
    if [ $pass = 2 ]; then
      $B/rfstool d.raw do "mkdir:/fill2" $(ops $W/fill /fill2) commit >/dev/null
      head -c 4500000 /dev/urandom > t/WINNT/System32/config/SYSTEM
      head -c 70000 /dev/urandom > t/WINNT/System32/drivers/refsnt.sys
      $B/rfstool d.raw do "write:/WINNT/System32/config/SYSTEM:0:$W/t/WINNT/System32/config/SYSTEM" \
          "put:$W/t/WINNT/System32/drivers/refsnt.sys:/WINNT/System32/drivers/refsnt.sys" commit >/dev/null
      out=$($B/refsboot d.raw 2>&1) || { echo "== $img: refsboot (again) failed: $out"; rc=1; break; }
    fi
    rm -rf fv; mkdir fv; mcopy -s -n -i d.raw@@$(voloff d.raw) '::/*' fv/ 2>/dev/null
    bad=0; n=0
    for f in $(cd fv; find . -type f); do
      n=$((n+1)); src=$(cd t; find . -ipath "$f" | head -1)
      cmp -s "fv/$f" "t/$src" || { bad=$((bad+1)); echo "   differs: $f"; }
    done
    chk=$($B/rfstool d.raw check 2>&1 | tail -1)
    c=$($B/refschk d.raw >/dev/null 2>&1; echo $?)
    want=163; [ $pass = 2 ] && want=164
    echo "$chk" | grep -q " 0 not allocated, 0 errors, 0 checksum errors" && [ $n = $want ] && [ $bad = 0 ] && [ $c = 0 ] && s=OK || { s=FAIL; rc=1; }
    echo "== $(basename $img) pass $pass: $s ($n files in the view, $bad differ, refschk $c) $(echo "$out" | grep -o '[0-9]* KB clusters') $(echo "$out" | grep -o '[0-9]* moved near the start') $(echo "$out" | grep -o '[0-9]* files moved further out')"
    [ $s = OK ] || echo "   $chk"
  done
  out=$($B/refsboot d.raw /U 2>&1); c=$($B/refschk d.raw >/dev/null 2>&1; echo $?)
  dd if=d.raw bs=512 skip=$(($(voloff d.raw)/512)) count=1 2>/dev/null | head -c 16 | grep -q ReFS && [ $c = 0 ] && echo "== $(basename $img) undo: OK" || { echo "== $(basename $img) undo: FAIL $out (refschk $c)"; rc=1; }
  rm -f t/WINNT/System32/drivers/refsnt.sys d.raw
done
exit $rc
