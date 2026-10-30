#!/bin/sh
# ============================================================
#  run.sh -- builds each tests/cases/*.c with this tree's 816-tcc and
#  PVSnesLib's tools, the same steps as a game build (816-tcc, 816-opt,
#  constify, wla-65816, wlalink + libtcc.obj), runs it on run816 and
#  reports. A case with a .expect file must also print exactly that.
#
#    tests/run.sh [case ...]        all cases, or the named ones
#    TCCFLAGS=-mhx422 tests/run.sh  every case with these compiler flags
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
TCCFLAGS=${TCCFLAGS:-}
# 816-opt: ours (tools/816-opt, built by make) when it's there, else PVSnesLib's;
# $OPT picks one. Ours takes -i/-o, PVSnesLib 4.5's writes to stdout.
OPT=${OPT:-}
if [ -z "$OPT" ]; then
    if [ -x ../tools/816-opt/816-opt ] || [ -x ../tools/816-opt/816-opt.exe ]; then OPT=../tools/816-opt/816-opt; else OPT="$PV/devkitsnes/tools/816-opt"; fi
fi
CTF="$PV/devkitsnes/tools/constify"
WLA="$PV/devkitsnes/bin/wla-65816"
LINK="$PV/devkitsnes/bin/wlalink"
LIBTCC="$PV/pvsneslib/lib/HiROM_FastROM/libtcc.obj"
B=build

mkdir -p $B
# the optimiser, whichever its command line
run_opt() {
    if "$OPT" -h 2>&1 | grep -q -- '--file-input'; then "$OPT" -q -i "$1" -o "$2"; else "$OPT" "$1" > "$2"; fi
}
[ -x "$TCC" ] || [ -x "$TCC.exe" ] || { echo "no $TCC: run make first"; exit 2; }
gcc -O2 -Wall -o $B/run816 emu/run816.c emu/emu816.c || exit 2
# the helper routines (multiply, divide, ...): the fork's copy of PVSnesLib's
# libtcc.asm (../lib816), fixed for -mcartram, assembled for this map
"$WLA" -d -s -x -I rt -DHIROM -DFASTROM -o $B/libtcc.obj ../lib816/libtcc.asm >> $B/rt.log 2>&1 || { cat $B/rt.log; exit 2; }
"$WLA" -d -s -x -I rt -o $B/crt0.obj rt/crt0.asm > $B/rt.log 2>&1 || { cat $B/rt.log; exit 2; }
# the start-up for code compiled with -mcartram: globals in cart RAM, bank $C8
"$WLA" -d -s -x -I rt -DCARTRAM=\$C8 -o $B/crt0_cart.obj rt/crt0.asm >> $B/rt.log 2>&1 || { cat $B/rt.log; exit 2; }
"$WLA" -d -s -x -I rt -o $B/test.obj rt/test.asm >> $B/rt.log 2>&1 || { cat $B/rt.log; exit 2; }

if [ $# -gt 0 ]; then cases="$*"; else cases=$(cd cases && ls *.c | sed 's/\.c$//'); fi
pass=0
fail=0
for n in $cases; do
    log=$B/$n.log
    : > $log
    ok=1
    # extra compiler flags: $TCCFLAGS for every case, and a case's own on a
    # "/* tccflags: ... */" line
    flags="$TCCFLAGS $(sed -n 's|^/\* tccflags: \(.*\) \*/.*$|\1|p' cases/$n.c | head -1)"
    "$TCC" -Wall $flags -I rt -c cases/$n.c -H -F -o $B/$n.ps >> $log 2>&1 &&
        run_opt $B/$n.ps $B/$n.asp >> $log 2>&1 &&
        "$CTF" cases/$n.c $B/$n.asp $B/$n.asm >> $log 2>&1 &&
        "$WLA" -d -s -x -I rt -o $B/$n.obj $B/$n.asm >> $log 2>&1 &&
        crt0=$B/crt0.obj &&
        case " $flags " in *" -mcartram"*) crt0=$B/crt0_cart.obj;; esac &&
        printf '[objects]\n%s\n%s\n%s\n%s\n' $crt0 $B/test.obj $B/$n.obj $B/libtcc.obj > $B/$n.link &&
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
# 816-opt on hand-written input: a volatile read is never dropped (and the
# same read unmarked is, so the check can fail)
if "$OPT" -h 2>&1 | grep -q -- '--file-input'; then
    run_opt opt/vol_marked.ps $B/vol_marked.asp > /dev/null 2>&1
    run_opt opt/vol_unmarked.ps $B/vol_unmarked.asp > /dev/null 2>&1
    m=$(grep -c '^lda.l 16128' $B/vol_marked.asp)
    u=$(grep -c '^lda.l 16128' $B/vol_unmarked.asp)
    if [ "$m" = 2 ] && [ "$u" = 1 ]; then
        echo "opt volatile: PASS (marked reads kept $m, unmarked $u)"
        pass=$((pass + 1))
    else
        echo "opt volatile: FAIL (marked reads kept $m of 2, unmarked $u, want 1)"
        fail=$((fail + 1))
    fi
fi
echo "$pass passed, $fail failed"
[ $fail = 0 ]
