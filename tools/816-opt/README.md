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

```sh
make            # needs PCRE2 (MSYS2: mingw-w64-x86_64-pcre2)
816-opt -q -i file.ps -o file.asp
```
