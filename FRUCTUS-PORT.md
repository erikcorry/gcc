# The Fructus port of GCC: where it stands

A C compiler for Fructus, the 16-bit ISA in the parent repository: `int` is
16 bits, `long` 32, pointers 16. Built with `tools/build-gcc.sh` (or
`just build-gcc`), used through `tools/fcc`, and run on the simulator with
`tools/fcc-run.mjs`.

## Results

GCC's C execute torture suite, 1692 tests, run by `tools/torture.mjs`:

| | pass | n/a | wrong result | can't build here |
|---|---|---|---|---|
| `-O2` | 1588 | 94 | 0 | 10 |
| `-Os` | 1585 | 94 | 0 | 13 |
| `-O0` | 1582 | 94 | 2 | 14 |

- **n/a** is a test requiring something a 16-bit, C-only target lacks —
  mostly `int32plus`, and a few needing trampolines, `__int128` or
  `__builtin_apply`.
- **Can't build here:** arrays over 32K, no `math.h`/`setjmp.h`/`signal.h`
  yet, `__int128`, an x87 register in inline asm, and (at `-O0`) one test too
  big for 64K.
- **Known open failures:**
  - `-Os`: `builtin-bitops-1` needs `__clrsbhi2`, which dropped out of libgcc
    when msp430's `lib2shift.c` was replaced.
  - `-O0`: `pr58574` produces a jump-table entry gas rejects ("too large for
    field of 2 bytes") — a real bug, not yet looked at.
  - `-O0`: `pr17377` gets `__builtin_return_address (0)` wrong.
  - `-O0`: `20010122-1` wants `__builtin_return_address (1)`, which needs
    frame pointers; the test itself says to skip it without them.

A hand-written caller, `tests/abi-caller.s` in the parent repository, checks
that compiled callees keep exactly the registers `isa/abi.s` says they keep.
`npm test` runs it.

## How it is built

- **The ABI's high:low pairs.** GCC's hard register numbers run backwards
  through the machine's: GCC regno N is machine register r(7−N). GCC puts the
  low word of a multi-register value at the lower regno, so reversed, every
  pair — `r0:r1`, and the unaligned `r1:r2` the ABI allows — comes out
  high:low, while memory stays little endian (`WORDS_BIG_ENDIAN` is 0).
- **The sliding convention** uses GCC's per-function ABIs: three ABI ids,
  chosen by how many of r0–r3 the signature uses for arguments or the return
  value. `TARGET_FNTYPE_ABI` gives a function its own; each call pattern
  carries its callee's in an `UNSPEC_CALLEE_CC`, from the cookie
  `function_arg` returns for the end marker, so indirect calls and libcalls
  get it too. Callee saves test `crtl->abi`, never the static
  `call_used_regs`.
- **Structs go by their fields**, as `isa/abi.s` says: each field is assigned
  independently, so a field of any size takes a whole register and the limit
  is four REGISTERS rather than eight bytes. Arguments and return values are
  both PARALLELs of register and offset, and `DEFAULT_PCC_STRUCT_RETURN` is 0
  so GCC does not return every aggregate in memory before the hook is asked.
  All or nothing: an aggregate whose fields do not all fit goes on the stack
  whole rather than straddling.
- **Tail calls**, and the sliding convention is what decides them. A tail
  call hands the callee our caller's return address, so the callee's clobbers
  become ours: `TARGET_FUNCTION_OK_FOR_SIBCALL` is the test that the callee
  clobbers no more than this function was already entitled to, which is a
  subset test because the three ABIs nest. It refuses exactly the case
  `libc/memset.s` documents by hand — bzero (two arguments, r2 callee saved)
  may not tail call memset (three, r2 an argument and destroyed). An indirect
  tail call is staged through r5, which no epilogue touches.
- **Immediates.** gas picks the encoding and never builds a constant, so the
  compiler prints only constants some form carries. `tools/gen-gcc.js`
  generates `config/fructus/fructus-isa.h` — imm3, shift3, immbit5, immask5
  and the condimm5 table — from `isa/fructus.toml`. Branch constants are
  matched against condimm5 by truth set, as gas does, and printed in the
  entry's own spelling. There is one constraint per comparison code and width,
  because a constraint cannot see the operator.
- **Branches** reach ±127 and gas does not relax them, so lengths mirror gas's
  form selection and a far branch becomes `br !c, 1f; jmp L; 1:`.
- **Registers.** r5 is an ordinary caller-saved register (gas never borrows
  it). lr is allocatable and saved whenever used. An indirect jump is
  `jmp r5`, one byte, and touches lr not at all — so a leaf function with a
  switch table no longer saves it. r4 is a frame pointer only when the frame
  has a variable size.
- **No alignment anywhere**, as the ABI says: every type is byte aligned.
- **Pushes never push sp.** GCC's `(set (mem (pre_dec sp)) (reg sp))` stores
  the old sp; the ISA stores the new one.
- **libgcc** uses the parent repository's hand-written `lib1funcs.s` for the
  multiplies and for 16-bit division, msp430's generic C division for 32-bit,
  32-bit shifts on 16-bit halves, and libgcc2 built with 32-bit words, so it
  supplies the 64-bit routines. No unwinder: C only. `lib1funcs.S` here is
  GENERATED from that file by `tools/gen-lib1funcs.js`, which gives each
  routine a `.globl` and a section of its own so `--gc-sections` can drop the
  ones a program does not call; `npm test` checks it is not stale. Before the
  split, one int multiply linked the 32-bit helper, the 64-bit helper and the
  whole of division — 528 bytes in a program whose only use of any of it was
  a single `calloc`.
- **Division returns both results.** `__udivmodhi4` gives the quotient in r0
  and the remainder in r1, which is a 32-bit return here, and it is
  registered as the udivmod libfunc with `TARGET_EXPAND_DIVMOD_LIBFUNC` to
  unpack it - so `a / b` and `a % b` together cost one call. The quotient is
  the HIGH word, unlike every other port doing this, because r0 is the high
  half of a pair. Only for a VARIABLE divisor:
  `divmod_candidate_p` in tree-ssa-math-opts declines a constant one, since
  constant division normally becomes a multiply-high, which this machine
  lacks. So `x / 10` and `x % 10` are still two calls, at 46 cycles each.

## Not done yet

- **No trampolines**, so no nested functions whose address is taken.
- **`long long` works but is slow** — software, through libgcc2.
- **Code size is close to hand-written but not level.** `digits3` in C is
  43 bytes against the hand version's 41: the compiler finds the same
  multiply-by-41 and `>> 11`, but keeps its scratch in r5 where the hand
  version gets one-byte r0/r1 forms.

## Found along the way, outside the compiler

- **`iseq` with a negative immediate** compared an unsigned register with the
  signed immediate in the spec's semantics, so `iseq r0, r0, #-205` was false
  for r0 = -205. Fixed in `isa/fructus.toml` to compare 16-bit patterns.
- **gas did not scope customasm's `.label` names**, so `lib1funcs.s`, both
  libc files and `snippets/memcpy.s` failed to assemble — and gas-check had
  been reporting every such failure as "needs the scratch register" and
  skipping it. tc-fructus now scopes them as customasm does, all four are
  byte-identical to customasm, and gas-check names customasm-only directives
  (`#res`) as the reason when it skips.
