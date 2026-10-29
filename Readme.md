# 816-tcc, a faster fork

A fork of [Alekmaul's 816-tcc](https://github.com/alekmaul/tcc), the C compiler PVSnesLib uses
for the [WDC 65816](https://en.wikipedia.org/wiki/WDC_65C816) and the SNES, which itself derives
from [TinyCC](https://github.com/TinyCC/tinycc). It produces WLA-DX assembly the same way, so it
drops into a PVSnesLib build. The aims:

- **Correct code.** Bugs found while building a real cartridge project are fixed, each with a test.
- **Faster code.** Each change is measured on a 65816 emulator, and checked against expected answers.
- **A test framework.** A 65816 emulator runs compiled C and checks what it computes.

It was written for the HX-422, an FPGA SNES cartridge. Apart from `-mhx422`, nothing here depends
on that cart.

## How much faster

Cycles for 64 passes of each pattern (`tests/cases/bench_core.c`), on the emulator:

| Pattern | PVSnesLib 4.5 | This fork |
|---|---:|---:|
| `u16` global array, read: `arr16[i]` | 9230 | 2172 |
| `u16` global array, write | 8078 | 1788 |
| `u8` global array, read | 9358 | 2232 |
| Struct array field: `act[i].x += act[i].dx` | 31886 | 2634 |
| Pointer walk: `sum += *p++` | 11214 | 2806 |
| A call with 3 `u16` args | 12430 | 7218 |
| 32-bit add | 12686 | 7414 |
| Local loop counter | 7118 | 1654 |
| Global loop counter | 7118 | 2228 |
| `u16` multiply | 16526 | 12594 (3958 with `-mhx422`) |
| A fastcall with 3 `u16` args | 12430 | 6450 |

PVSnesLib's compiler has no fastcall, so its last row is the same call as the row above.

## What changed

### Fixes

- **Integer constants** get their C99 types (`int` and `long` are 16 bits here).
- **Pointer initialisers** and pointer-to-`long long` casts keep the bank byte. A `static` pointer,
  and `(u32)&var`, used to lose it.
- **`&&`, `||` and `?:`** produced wrong code in some forms: jump chains weren't merged, and a
  constant condition wasn't folded.
- **PVSnesLib's 816-opt**: two fixes, listed in [tools/816-opt/README.md](tools/816-opt/README.md).

### Code generation

- **Compares** branch on the flags, rather than building a 0 or 1 first.
- **Multiplies by a constant** use shifts and adds when that's cheaper.
- **Global arrays and struct fields** are read with `ldx` and `lda.l sym,x`, with any constant
  offset folded into the address.
- **`for` loops** test at the bottom: one branch per pass.
- **Each function is compiled twice.** The first pass measures and its code is thrown away: how
  often each local and parameter is used (weighted by loop depth), whether its address is taken,
  and what each loop uses. The second pass uses those measurements.
- **Direct-page slots.** The four busiest locals or parameters live in the direct page
  (`tcc__l0`–`l3`, callee-saved). A long pointer takes two adjacent slots, so `*p` is
  `lda [tcc__lK]`.
- **A loop's counter in Y.** In a loop with no call, no `goto` and no other use of Y, the counter
  lives in Y. For a `for` loop that's the variable its increment steps. For `while` and `do` loops
  it's the local the loop uses most.
- **A loop's index in X.** When the counter indexes global arrays of one element size, X holds
  `counter × size` for the whole loop and steps with the counter.
- **`p++` is delayed** to the next sequence point, for a pointer in direct-page slots. `*p++` then
  reads through `p`, and the step is one add.
- **`-mhx422`** uses the HX-422 cart's hardware multiplier for multiplies.
- **`__attribute__((fastcall))`** (or `regparm(n)`) passes up to four 2-byte parameters in
  direct-page slots (`tcc__a0`–`a3`) instead of on the stack.
- **Return marks.** Each `rtl` is marked with what the function returns (`; ret16`, `; ret32`, ...).
  Volatile accesses are marked `; volatile`. Both are for the optimiser.

### The optimiser

The fork carries its own copy of PVSnesLib's 816-opt in `tools/816-opt`. It has the two fixes, plus
passes that use liveness over the whole file: dead stores, operand forwarding, known register
contents, reused computations, and rules for the Y/X loops. They're described in
[tools/816-opt/README.md](tools/816-opt/README.md).

## Building

On Windows, use MSYS2's MINGW64 shell. Linux and macOS work the same way.

```sh
make            # 816-tcc
make opt        # tools/816-opt/816-opt (needs PCRE2: mingw-w64-x86_64-pcre2 on MSYS2)
```

## Using it

Use `816-tcc` and `tools/816-opt/816-opt` in place of PVSnesLib's
`devkitsnes/bin/816-tcc` and `devkitsnes/tools/816-opt`. Everything after them (constify,
wla-65816, wlalink, PVSnesLib's `libtcc.obj`) stays the same.

**Your start-up code must define the direct-page names the fork uses.** PVSnesLib's crt0 has only
the first block. Add `tcc__a0`–`a3` and `tcc__l0`–`l3`, in this order, at the end of the register
page:

```
.RAMSECTION ".registers" BANK 0 SLOT 1 ORGA 0 FORCE
tcc__registers dsb 0
tcc__r0 dsb 2
tcc__r0h dsb 2
; ... r1-r5, r9, r10 and their h halves, f2, f3, as in PVSnesLib ...
move_insn dsb 4
move_backwards_insn dsb 4
tcc__a0 dsb 2           ; fastcall parameter slots
tcc__a1 dsb 2
tcc__a2 dsb 2
tcc__a3 dsb 2
tcc__l0 dsb 2           ; direct-page locals: consecutive, a pointer spans two
tcc__l1 dsb 2
tcc__l2 dsb 2
tcc__l3 dsb 2
.ENDS
```

The page is 64 bytes. An interrupt handler that runs C code needs its own copy of it, as it does
with PVSnesLib's compiler. `tests/rt/crt0.asm` is a complete example.

```
816-tcc [-v] [-c] [-H] [-F] [-mhx422] [-o outfile] [-Idir] [-Wwarn] [infile ...]
  -H         HiROM (mode 21) memory map
  -F         FastROM
  -mhx422    multiplies on the HX-422 cart's hardware multiplier ($3C30)
```

### Things to know about this target

- `int` and `long` are 16 bits; `long long` is 32 bits.
- Pointer arithmetic doesn't carry into the bank byte, as in PVSnesLib.
- There is no inline assembly.
- A parameter in a fastcall slot has no address. Copy it to a local first if you need `&`.

## Tests

`tests/` holds a 65816 emulator and about twenty test programs. Each is a C file that computes
things and checks them with `CHECK(expr, want)`. `run.sh` builds each one the way a game is built,
runs it, and reports PASS or what went wrong. See [tests/README.md](tests/README.md).

```sh
sh tests/run.sh                    # every case
sh tests/run.sh xloop ptrwalk      # some
TCCFLAGS=-mhx422 sh tests/run.sh   # with the multiplier option
OPT=cat sh tests/run.sh            # without the optimiser: the compiler's own code
```

The tests need gcc (for the emulator) and PVSnesLib 4.5 at `$PVSNESLIB_HOME`. Run them from a path
without spaces, because wla-65816 fails on spaced paths. The emulator passes the
[Tom Harte 65816 single-step tests](https://github.com/SingleStepTests/65816) (257,177 of them).

## License

The compiler is TCC's: the GNU Lesser General Public License (see [COPYING](COPYING) and
[RELICENSING](RELICENSING)). 816-opt is PVSnesLib's, under the zlib licence (the notice is in each
source file). The fork's additions to it (`src/deadstore.c`), the emulator and the tests are public
domain.

## Acknowledgements

- All the community of the original TinyCC project.
- Ulrich Hecht, for the 65816 back end.
- [Alekmaul](https://github.com/alekmaul) and [Kobenairb](https://github.com/kobenairb), for
  816-tcc, 816-opt and PVSnesLib.
- This fork: John McAlarney.
