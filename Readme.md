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

These are CPU cycles counted on the test emulator, which uses the 65816 datasheet's cycle counts.
"PVSnesLib 4.5" is its stock 816-tcc and 816-opt. "This fork" is this compiler and its 816-opt,
without `-mhx422`. Both builds give the same, checked answers. Lower is better.

### Loop bodies

64 passes of each pattern, inside one `for` loop in `main` (`tests/cases/bench_core.c`):

| Pattern | PVSnesLib 4.5 | This fork | Speed-up |
|---|---:|---:|---:|
| `u16` global array, read: `sum += arr16[i]` | 9230 | 2172 | 4.2× |
| `u16` global array, write: `arr16[i] = v` | 8078 | 1788 | 4.5× |
| `u8` global array, read: `sum += arr8[i]` | 9358 | 2232 | 4.2× |
| Struct array field: `act[i].x += act[i].dx` | 31886 | 2634 | 12.1× |
| Pointer walk: `sum += *p++` | 11214 | 2806 | 4.0× |
| A call with 3 `u16` args: `add3(a, b, c)` | 12430 | 7218 | 1.7× |
| 32-bit add: `acc32 += v32` | 12686 | 7414 | 1.7× |
| Local loop counter: `sum += l` | 7118 | 1654 | 4.3× |
| Global loop counter: `sum += g` | 7118 | 2228 | 3.2× |
| `u16` multiply: `a * b` | 16526 | 12594 | 1.3× |
| A fastcall with 3 `u16` args | 12430 | 6450 | 1.9× |

With `-mhx422`, the multiply row is 3958 (4.2×). PVSnesLib's compiler has no fastcall, so its last
row is the same call as the row above it.

### Whole functions

One call to a small function, call and return included (`tests/cases/bench_more.c`):

| Function | PVSnesLib 4.5 | This fork | Speed-up |
|---|---:|---:|---:|
| Copy 64 words: `while (n--) *d++ = *s++` | 12356 | 3498 | 3.5× |
| Fill 64 words: `for (...) buf[i] = v` | 8985 | 2063 | 4.4× |
| Sum 64 bytes through a pointer: `while (n--) t += *p++` | 9323 | 2985 | 3.1× |
| 8×8 block into a 32×32 tile map: `map[(y0 + y) * 32 + x0 + x] = tile` | 15091 | 6537 | 2.3× |
| Move 16 actors through a pointer: `p->x += p->dx; p->y += p->dy; p++` | 4570 | 3199 | 1.4× |
| `switch` on 64 opcodes, 4 cases | 13541 | 4824 | 2.8× |
| Clamp 64 signed values: `if (v > hi) v = hi; else if (v < lo) v = lo;` | 18900 | 7634 | 2.5× |
| Count the set bits of 16 words: `while (x) { c += x & 1; x >>= 1; }` | 20998 | 7994 | 2.6× |
| String length, 60 characters: `while (*s++) n++` | 8005 | 3049 | 2.6× |
| Table lookup through an index, 64 times: `t += tab[idx[i]]` | 12714 | 3425 | 3.7× |
| Divide 16 values by 10: `q += v[i] / 10` | 11119 | 9312 | 1.2× |
| 16 steps of a fixed-point ease: `x += (target - x) >> 2` | 2107 | 1674 | 1.3× |
| Recursive `fib(10)`, 177 calls | 25056 | 17102 | 1.5× |
| Count 16 `u32` values over a limit: `if (big[i] > limit) c++` | 4906 | 2673 | 1.8× |

Division still calls the library's divide routine, so that row barely moves.

### `while` and `do` loops over global arrays

One call, 64 passes (`tests/cases/bench_while.c`):

| Function | PVSnesLib 4.5 | This fork | Speed-up |
|---|---:|---:|---:|
| Sum 64 words: `while (i < n) { t += w[i]; i++; }` | 9693 | 2524 | 3.8× |
| Sum 64 words, counting down: `while (n--) t += w[n]` | 7632 | 2275 | 3.4× |
| Sum 64 words: `while (i < n) t += w[i++]` | 9693 | 2524 | 3.8× |
| Copy 64 words: `while (n--) dst[n] = w[n]` | 8364 | 1861 | 4.5× |
| Clear 64 bytes: `do { by[i] = 0; } while (++i < n)` | 6424 | 3245 | 2.0× |
| Move 64 actors: `while (n--) { acts[n].x += acts[n].dx; ... }` | 17836 | 3982 | 4.5× |
| Find a word, 64 reads: `while (w[i] != want) i++` | 7734 | 1709 | 4.5× |

To reproduce, run `sh tests/run.sh bench_core bench_more bench_while`. For PVSnesLib's numbers, point `TCC=` and
`OPT=` at its `816-tcc` and `816-opt`.

## What changed

### Fixes

- **Integer constants** get their C99 types (`int` and `long` are 16 bits here).
- **Pointer initialisers** and pointer-to-`long long` casts keep the bank byte. A `static` pointer,
  and `(u32)&var`, used to lose it.
- **`&&`, `||` and `?:`** produced wrong code in some forms: jump chains weren't merged, and a
  constant condition wasn't folded.
- **PVSnesLib's 816-opt**: two fixes, listed in [tools/816-opt/README.md](tools/816-opt/README.md).

### Code generation

- **Compares** branch on the flags, rather than building a 0 or 1 first. 816-opt also compares
  straight against memory, rather than copying the right-hand side through a register first.
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
  `counter × size` for the whole loop, in `for`, `while` and `do` loops alike. X follows every
  write to the counter, wherever it is: `++` and `--` step it, and any other write recomputes it
  from Y if the size is a power of 2 up to 16 (at other sizes such a loop goes without X). For
  `while` and `do` loops, a local that indexes a global array is chosen for Y over a busier one.
  `w[i++]` as a whole index steps at once and reads `w - 2,x`.
- **Big `const` tables** (4 KB or more) get a section of their own, so a HiROM link can put them in
  any bank, a whole 64 KB table included. Stock `.rodata` stays in bank 0's upper half, 32 KB at most
  shared with the vectors. Data lines break every 32 bytes, because 816-opt splits very long ones.
- **`p++` is delayed** to the next sequence point, for a pointer in direct-page slots. `*p++` then
  reads through `p`, and the step is one add.
- **`-mhx422`** uses the HX-422 cart's hardware multiplier for multiplies.
- **`__attribute__((fastcall))`** (or `regparm(n)`) passes up to four 2-byte parameters in
  direct-page slots (`tcc__a0`–`a3`) instead of on the stack.
- **Return marks.** Each `rtl` is marked with what the function returns (`; ret16`, `; ret32`, ...).
  Volatile accesses are marked `; volatile`. Both are for the optimiser.
- **Inline assembly** with operands: GCC-style `asm`, described under [Using it](#inline-assembly).
- **`static inline` functions are expanded** where they're called. Each argument goes into a local
  and the body is compiled in place, with no call or return, and with no call left in a loop that
  calls one, so the loop can keep its counter in Y. Some functions stay ordinary calls:
  - recursive ones;
  - ones with `goto`, a `static` local or `__asmraw__`;
  - long ones (over 160 tokens);
  - ones with struct arguments or results, `...`, or an old-style declaration;
  - one whose address is taken. It's also compiled as a normal function for that use.
- **`-mcartram`** puts the globals in RAM in the cart's ROM area, a bank DB points at, so they're
  reached DB-relative at FastROM speed. Described under [Using it](#globals-in-cart-ram).

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
wla-65816, wlalink) stays the same.

**Link `lib816/libtcc.asm` instead of PVSnesLib's `libtcc.obj`.** It's the fork's copy of the
compiler's helper routines (multiply, divide, 32-bit operations), with one fix: the divide routine
read the direct page through DB, which was right only while DB was `$7E`. Assemble it with your
project's memory map, as `tests/run.sh` does:

```
wla-65816 -d -s -x -DHIROM -DFASTROM -I <your hdr.asm folder> -o libtcc.obj lib816/libtcc.asm
```

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
  -mcartram[=bank]  globals in RAM in the cart's ROM area, that bank ($C8 by default)
```

### Globals in cart RAM

On a cart with RAM in its ROM area (the HX-422's PSRAM, writable from `$C8`),
`-mcartram[=bank]` moves every global there. Your start-up code points DB at that bank, and the
compiler reaches the globals DB-relative (`lda.w`). FastROM-speed memory beats WRAM:

| Where the variable is | Instruction | Master cycles (16-bit load) |
|---|---|---:|
| WRAM `$7E` (uninitialised, without `-mcartram`) | `lda.w` | 34 |
| WRAM `$7F` (initialised, without `-mcartram`) | `lda.l` | 40 |
| Cart RAM, DB pointing at it | `lda.w` | 30 |

- **What goes where:**
  - `.bss` goes into a section called `cartram.bss`, which your crt0 anchors in that bank. So does
    the RAM copy of initialised data (`globram.data`).
  - `__TCC816_CARTRAM__` is defined as the bank.
  - A global the file defines, and doesn't make `const`, is reached `.w`. Externs and `const`
    data stay long, because they may be in WRAM or ROM.
- **Keeping a variable in WRAM:** `__attribute__((section(".wram")))` keeps it in bank `$7E`,
  reached long. Use it for HDMA tables and anything else that must stay there. It takes no
  initial value; crt0 clears WRAM.
- **crt0's part:**
  - anchor both sections in the bank, using slot 3 (any bank's `$0000`–`$FFFF`) so the memory map
    stays the same as everything else's;
  - point DB at the bank;
  - clear the area, then copy `glob.data` in;
  - in any interrupt handler that calls C, set DB the same way.

  `tests/rt/crt0.asm` does all of it under `CARTRAM`.

The test emulator counts CPU cycles, not master cycles, so the benchmark tables above don't show
this gain. Built with `-mcartram`, they stay within 4% of the numbers above, and the small
differences come from dropping the long-address cycle on globals. The real gain is on the bus: the
table above, 30 master cycles against 34 or 40 for each global access.

### Inline assembly

GCC-style `asm` works, with operands, so assembly can work on C variables without knowing where the
compiler keeps them:

```c
u16 sum, v;
asm("lda.b %1\n clc\n adc.b %2\n sta.b %0" : "=r"(sum) : "r"(a), "r"(b));
asm volatile("xba" : "=a"(v) : "a"(x));                 /* swap a word's bytes */
asm("lda.l %1,x" : "=a"(v) : "m"(table[0]), "x"(i * 2));
asm("ldx.w #8\nl%=:\n asl a\n dex\n bne l%=" ::: "a", "x");
```

| Constraint | The operand | `%N` in the template |
|---|---|---|
| `r` | in a pseudo-register | `tcc__rK` (`%HN`: `tcc__rKh`, a pointer's high word) |
| `a`, `x`, `y` | loaded into A, X or Y before the template, or stored from it after | the pseudo-register |
| `i`, `n` | a constant, or a global's address | the number, or `sym + offset` |
| `m` | a global, in memory | `sym + offset` (write `lda.l %N`) |
| `0`–`9` | an input in the same register as that output | |

- **Outputs** start with `=` (written) or `+` (read and written). A local is an `r` operand, not `m`.
- **Template codes:** `%[name]` refers to a named operand (`[name] "r"(x)`), `%%` is a `%`, and `%=`
  is a number unique to the statement, for labels.
- **Clobbers:** `"x"` and `"y"` keep a loop's counter and index out of those registers. `"a"`,
  `"cc"` and `"memory"` are accepted; no value lives in A, the flags or a register across a
  statement anyway.
- **No operands:** an `asm` with no `:` is copied as written, `%` included, as GCC does.
- **Outside a function:** `asm("...")` gets a section of its own, so a whole routine can live in a C
  file. `tests/cases/asm.c` has one, called from C.

The code must leave A, X and Y 16 bits wide, and D, DB and S as it found them. The fork's 816-opt
leaves every `asm` block, and every `__asmraw__` block, exactly as written. PVSnesLib's own 816-opt
doesn't know about them.

### Things to know about this target

- `int` and `long` are 16 bits; `long long` is 32 bits.
- Pointer arithmetic doesn't carry into the bank byte, as in PVSnesLib.
- A parameter in a fastcall slot has no address. Copy it to a local first if you need `&`.

## Tests

`tests/` holds a 65816 emulator and about twenty test programs. Each is a C file that computes
things and checks them with `CHECK(expr, want)`. `run.sh` builds each one the way a game is built,
runs it, and reports PASS or what went wrong. See [tests/README.md](tests/README.md).

```sh
sh tests/run.sh                    # every case
sh tests/run.sh xloop ptrwalk      # some
TCCFLAGS=-mhx422 sh tests/run.sh   # with the multiplier option
TCCFLAGS=-mcartram sh tests/run.sh # every case's globals in cart RAM ($C8)
OPT=cat sh tests/run.sh            # without the optimiser: the compiler's own code
```

The tests need gcc (for the emulator) and PVSnesLib 4.5 at `$PVSNESLIB_HOME`. Run them from a path
without spaces, because wla-65816 fails on spaced paths. The emulator passes the
[Tom Harte 65816 single-step tests](https://github.com/SingleStepTests/65816) (257,177 of them).

## License

The compiler is TCC's: the GNU Lesser General Public License (see [COPYING](COPYING) and
[RELICENSING](RELICENSING)). 816-opt is PVSnesLib's, under the zlib licence (the notice is in each
source file). `lib816/libtcc.asm` is PVSnesLib's, under the MIT licence (the notice is in the
file). The fork's additions to 816-opt (`src/deadstore.c`), the emulator and the tests are public
domain.

## Acknowledgements

- All the community of the original TinyCC project.
- Ulrich Hecht, for the 65816 back end.
- [Alekmaul](https://github.com/alekmaul) and [Kobenairb](https://github.com/kobenairb), for
  816-tcc, 816-opt and PVSnesLib.
