# 816-opt (altered)

The peephole optimiser that runs on 816-tcc's output, from PVSnesLib
(`tools/816-opt`, version 2.0, July 2026) by Alekmaul and kobenairb, zlib licence (the notice is in
each source file). **This is an altered copy**, for the 816-tcc fork:

- An empty stack frame's `<name>_locals` is removed from every operand that adds it, not only
  `N + <name>_locals + 1,s`. 2.0 left it in `adc #(N + <name>_locals + 1) + 2` (a stack
  argument's address) after deleting its `.define`, so a function with no locals that passes a
  32-bit argument on failed to assemble ("Unresolved reference"). `tests/cases/opt_noloc.c`.
- `adc #X` followed by two `inc a` folds back into `adc #X + 2`, as 4.5.0's version did; 2.0's
  increment folding got there first and left the two `inc a` (4 cycles) on every stack argument's
  address.

With those, its output matches PVSnesLib 4.5.0's binary on every file of the HX-422 build and the
compiler tests, apart from the empty-frame code 2.0 removes, which assembled to nothing.

Added (`src/deadstore.c`, public domain), run after 2.0's passes and repeated with them until
nothing changes. They use liveness of the pseudo-registers over the whole file's control flow, not
a few lines of straight code. Lines 816-tcc marks `; volatile` are never dropped, moved or reused.

- **Dead stores:** a store to `tcc__rN` that no path reads goes. So does a load of a
  pseudo-register or slot that nothing reads.
- **Operand forwarding:**
  - A value parked in `tcc__rN` for one use goes straight to it: `lda M / sta rN / lda … / adc rN`
    becomes `lda … / adc M` (also `sbc and ora eor cmp`). That needs no store, call, branch, width
    change, or (for a stack M) stack move in between.
  - `sta rN / … / ldx rN` becomes `tax / …` (`ldy`: `tay`).
  - `stz rN / lda rN` becomes `lda.w #0`.
  - `sta rN / lda M / clc / adc rN` becomes `clc / adc M`, since the operation is commutative
    (also `and ora eor`).
- **Known registers:** a load of what A, X or Y already holds goes. A load of what another register
  holds becomes a transfer.
- **Reused blocks:** a computation repeated from the same sources (an `act[i]` index worked out
  twice) is done once.
- **Unsigned byte loads:** `lda.w #0 / sep / lda M / rep` becomes `lda M / and.w #$00ff`, 8 cycles
  instead of 13. It reads the byte after M too, so it only applies to named objects, never volatile
  ones.
- **Rules for 816-tcc's Y/X loops and direct-page pointers:**
  - `iny`/`dey` and `cpy`, with the counter in Y.
  - Shifts done in A.
  - `while (n--)` as `dey / cpy.w #$ffff`.
  - `[tcc__rN]` read as `[tcc__lK]` when `rN` is a copy of a slot pointer.

**Inline assembly is left alone.** 816-tcc puts every `asm` and `__asmraw__` block between
`; asm {` and `; asm }` lines. `tidyFile` lifts each block out as written and leaves one line,
`.asmblock_N:`, in its place. To `deadstore.c` that line is a directive that reads every register.
To the pattern passes it's control flow, because it ends in `:`. No pass moves code across it or
changes what's inside, and the block goes back unchanged when the file is written.

A rule may replace one line with two. `compact()` splits such a line before the next pass, which
would otherwise see only its first instruction.

`tests/cases/opt_forward.c`, `opt_known.c` and the loop cases hold the cases where a rule would be
wrong.

```sh
make            # needs PCRE2 (MSYS2: mingw-w64-x86_64-pcre2)
816-opt -q -i file.ps -o file.asp
```
