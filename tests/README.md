# Compiler tests

Each `cases/*.c` is a small program that computes things and checks them. `run.sh` builds it the
way a game is built (816-tcc, 816-opt, constify, wla-65816, wlalink with PVSnesLib's `libtcc.obj`),
runs the ROM on a 65816 emulator, and reports PASS or what went wrong.

```sh
make test                      # every case
sh tests/run.sh fix_logic      # one
TCC=/path/to/other/816-tcc sh tests/run.sh    # another compiler
```

It needs gcc (for the emulator) and PVSnesLib 4.5 at `$PVSNESLIB_HOME`. Run it from a path without
spaces: wla-65816 fails on spaced paths.

## Writing a case

```c
#include "test.h"

volatile u16 a = 1234;          /* inputs in volatile globals, or it all folds */

int main(void)
{
    CHECK(a * 3, 3702);         /* got and want, as 32-bit values */
    return 0;
}
```

- `CHECK(expr, want)`: a mismatch prints the line, what it got and what it wanted.
- `t_puts`, `t_putc`: print. If `cases/<name>.expect` exists, the output must match it exactly.
- `t_timer_start()`, `t_timer_stop(id)`: print the cycles between them, for the speed work.
- `t_fail(code)`: fail at once.

A case fails on a bad check, a BRK (running into empty ROM), a write to the code region (the
first 512 KB of PSRAM, read-only on the cart), or running past 200 million cycles.

For every bug fixed in the compiler, add a case that fails without the fix: run it with
`TCC=` pointing at the old compiler to see it fail.

## The pieces

| | |
|---|---|
| `emu/emu816.c` | The 65816 core: every opcode, native and emulation mode, decimal mode, cycle counts. |
| `emu/run816.c` | Runs a test ROM with the HX-422 lean core's memory map; `-x` traces each instruction, `-t` prints cycles. |
| `emu/sst_fetch.py`, `emu/sst_run.c` | Check the core against Tom Harte's single-step tests ([SingleStepTests/65816](https://github.com/SingleStepTests/65816), MIT): `make test-emu` fetches the first 256 KB of each of the 512 files once (about 257,000 tests) and runs them. All pass, registers, memory and cycles. |
| `rt/` | The test runtime: memory map, header, start-up, and the hooks (`WDM`, a 2-byte no-op on a real 65816, which run816 traps). |

The emulator, runtime and tests are public domain (CC0); `rt/crt0.asm` is derived from PVSnesLib's
(zlib licence, notice kept).
