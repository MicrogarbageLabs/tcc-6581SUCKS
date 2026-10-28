#!/bin/sh
# ============================================================
#  run.sh -- builds each tests/cases/*.c with this tree's 816-tcc and
#  PVSnesLib's tools, the same steps as a game build (816-tcc, 816-opt,
#  constify, wla-65816, wlalink + libtcc.obj), runs it on run816 and
#  reports. A case with a .expect file must also print exactly that.
#
#    tests/run.sh [case ...]        all cases, or the named ones
#    TCC=path tests/run.sh          another compiler (e.g. PVSnesLib's, to see
#                                   a regression test fail where the bug is)
#
#  Needs: ../816-tcc (make), gcc for the emulator, and PVSnesLib 4.5 at
#  $PVSNESLIB_HOME (its devkitsnes\ and pvsneslib\ folders). Run it from
#  a path without spaces: wla fails on spaced paths.
#
#  Public domain (CC0). No warranty.
# ============================================================
set -u
cd "$(dirname "$0")"
T=$(pwd)
PV=${PVSNESLIB_HOME:-"/c/Users/IP Freely/Documents/Source/pvsneslib_450_64b_windows/pvsneslib"}
TCC=${TCC:-../816-tcc}
OPT="$PV/devkitsnes/tools/816-opt"
CTF="$PV/devkitsnes/tools/constify"
WLA="$PV/devkitsnes/bin/wla-65816"
LINK="$PV/devkitsnes/bin/wlalink"
LIBTCC="$PV/pvsneslib/lib/HiROM_FastROM/libtcc.obj"
B=build

mkdir -p $B
[ -x "$TCC" ] || [ -x "$TCC.exe" ] || { echo "no $TCC: run make first"; exit 2; }
gcc -O2 -Wall -o $B/run816 emu/run816.c emu/emu816.c || exit 2
cp "$LIBTCC" $B/libtcc.obj
"$WLA" -d -s -x -I rt -o $B/crt0.obj rt/crt0.asm > $B/rt.log 2>&1 || { cat $B/rt.log; exit 2; }
"$WLA" -d -s -x -I rt -o $B/test.obj rt/test.asm >> $B/rt.log 2>&1 || { cat $B/rt.log; exit 2; }

if [ $# -gt 0 ]; then cases="$*"; else cases=$(cd cases && ls *.c | sed 's/\.c$//'); fi
pass=0
fail=0
for n in $cases; do
    log=$B/$n.log
    : > $log
    ok=1
    "$TCC" -Wall -I rt -c cases/$n.c -H -F -o $B/$n.ps >> $log 2>&1 &&
        "$OPT" $B/$n.ps > $B/$n.asp 2>> $log &&
        "$CTF" cases/$n.c $B/$n.asp $B/$n.asm >> $log 2>&1 &&
        "$WLA" -d -s -x -I rt -o $B/$n.obj $B/$n.asm >> $log 2>&1 &&
        printf '[objects]\n%s\n%s\n%s\n%s\n' $B/crt0.obj $B/test.obj $B/$n.obj $B/libtcc.obj > $B/$n.link &&
        "$LINK" -d -s -v -A -c $B/$n.link $B/$n.sfc >> $log 2>&1 || ok=0
    if [ $ok = 0 ]; then
        echo "$n: BUILD FAILED"
        tail -5 $log | sed 's/^/    /'
        fail=$((fail + 1))
        continue
    fi
    $B/run816 $B/$n.sfc > $B/$n.out
    r=$?
    if [ $r = 0 ] && [ -f cases/$n.expect ]; then
        sed '$d' $B/$n.out | diff -u cases/$n.expect - > $B/$n.diff || { r=1; echo "    output differs: $B/$n.diff"; }
    fi
    if [ $r = 0 ]; then
        echo "$n: $(tail -1 $B/$n.out)"
        case $n in bench_*) grep '^cycles' $B/$n.out | sed 's/^/    /';; esac
        pass=$((pass + 1))
    else
        echo "$n:"
        sed 's/^/    /' $B/$n.out | grep -E 'BAD|FAIL' | head -20
        fail=$((fail + 1))
    fi
done
echo "$pass passed, $fail failed"
[ $fail = 0 ]
