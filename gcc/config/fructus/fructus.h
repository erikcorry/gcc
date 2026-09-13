/* Target definitions for Fructus, a 16-bit retrocomputer instruction set.
   Copyright (C) 2026 Free Software Foundation, Inc.

   This file is part of GCC.

   GCC is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published
   by the Free Software Foundation; either version 3, or (at your
   option) any later version.

   GCC is distributed in the hope that it will be useful, but WITHOUT
   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
   or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
   License for more details.

   You should have received a copy of the GNU General Public License
   along with GCC; see the file COPYING3.  If not see
   <http://www.gnu.org/licenses/>.  */

/* The ISA and the calling convention are defined outside GCC, in the fructus
   repository: isa/fructus.toml and isa/abi.s.  This port implements them; where
   the two disagree, those files win.  */

#ifndef GCC_FRUCTUS_H
#define GCC_FRUCTUS_H

/* --------------------------------------------------------------------------
   Driver
   -------------------------------------------------------------------------- */

#undef  STARTFILE_SPEC
#define STARTFILE_SPEC "crt0%O%s"

#undef  ENDFILE_SPEC
#define ENDFILE_SPEC ""

#undef  LIB_SPEC
#define LIB_SPEC "-lc"

#undef  LINK_SPEC
#define LINK_SPEC "%{h*} %{v:-V} %{static:-Bstatic}"

/* --------------------------------------------------------------------------
   Run-time target specification
   -------------------------------------------------------------------------- */

#define TARGET_CPU_CPP_BUILTINS()		\
  do						\
    {						\
      builtin_define_std ("fructus");		\
      builtin_define ("__FRUCTUS__");		\
    }						\
  while (0)

/* --------------------------------------------------------------------------
   Storage layout

   Sixteen-bit words, little endian, and NO ALIGNMENT ANYWHERE.  The ISA
   allows an unaligned 16-bit access with no fault and no penalty, so padding
   would cost bytes and buy nothing; isa/abi.s says the same of the stack.
   -------------------------------------------------------------------------- */

#define BITS_BIG_ENDIAN  0
#define BYTES_BIG_ENDIAN 0
#define WORDS_BIG_ENDIAN 0

#define UNITS_PER_WORD 2
#define POINTER_SIZE 16
#define Pmode HImode
#define FUNCTION_MODE QImode

#define PARM_BOUNDARY 8
#define STACK_BOUNDARY 8
#define FUNCTION_BOUNDARY 8
#define BIGGEST_ALIGNMENT 8
#define EMPTY_FIELD_BOUNDARY 8
#define STRUCTURE_SIZE_BOUNDARY 8
#define STRICT_ALIGNMENT 0
#undef  PCC_BITFIELD_TYPE_MATTERS
#define PCC_BITFIELD_TYPE_MATTERS 0
#define MAX_FIXED_MODE_SIZE 64

/* libgcc2 is built with 32-bit words, as msp430 does, so that the "double
   word" routines it provides are the 64-bit ones.  The 16- and 32-bit helpers
   come from libgcc/config/fructus.  */
#define LIBGCC2_UNITS_PER_WORD 4

/* --------------------------------------------------------------------------
   Layout of source language data types
   -------------------------------------------------------------------------- */

#define INT_TYPE_SIZE 16
#define SHORT_TYPE_SIZE 16
#define LONG_TYPE_SIZE 32
#define LONG_LONG_TYPE_SIZE 64

/* ld8 zero extends and there is no sign-extending byte load, so an unsigned
   char is the one that loads in one instruction.  */
#define DEFAULT_SIGNED_CHAR 0

#undef  SIZE_TYPE
#define SIZE_TYPE "unsigned int"
#undef  PTRDIFF_TYPE
#define PTRDIFF_TYPE "int"
#undef  WCHAR_TYPE
#define WCHAR_TYPE "int"
#undef  WCHAR_TYPE_SIZE
#define WCHAR_TYPE_SIZE 16

/* --------------------------------------------------------------------------
   Registers

   GCC'S REGISTER NUMBERS RUN BACKWARDS THROUGH THE MACHINE'S.  GCC regno N is
   machine register r(7-N):

     GCC   0    1    2    3    4    5    6    7    8     9
           lr   sp   r5   r4   r3   r2   r1   r0   ?fp   ?ap

   The reason is the ABI's register pairs.  A 32-bit value lives in two
   registers HIGH HALF FIRST - r0:r1 is r0 high, r1 low - while memory is little
   endian.  GCC puts a multi-register value in consecutive regnos with the LOW
   word at the lower number (WORDS_BIG_ENDIAN governs memory too, and memory
   must stay little endian), so with the natural numbering a long in r0:r1
   would have its low half in r0.  Reversed, the pair at GCC regno 6 is r1
   (low) then r0 (high), which is exactly isa/abi.s - and every pair,
   including the unaligned ones the ABI allows (r1:r2), comes out the same
   way.  Nothing else in GCC cares which way the numbers run.

   r5 is an ordinary caller-saved register.  It is the assembler's scratch
   only in the customasm tooling; gas never expands an immediate, so the
   compiler owns the whole file and must itself never print an immediate that
   no form can carry.

   lr is allocatable.  A call clobbers it, so no value lives in it across one,
   and a function that uses it saves it in the prologue exactly as a
   non-leaf function must anyway.  ?fp and ?ap are eliminated, always.  */

#define FRUCTUS_LR   0
#define FRUCTUS_SP   1
#define FRUCTUS_R5   2
#define FRUCTUS_R4   3
#define FRUCTUS_R3   4
#define FRUCTUS_R2   5
#define FRUCTUS_R1   6
#define FRUCTUS_R0   7
#define FRUCTUS_SFP  8
#define FRUCTUS_SAP  9

/* Machine register rN, as a GCC regno, and back.  */
#define FRUCTUS_REGNO(N) (7 - (N))

#define FIRST_PSEUDO_REGISTER 10

#define REGISTER_NAMES \
  { "lr", "sp", "r5", "r4", "r3", "r2", "r1", "r0", "?fp", "?ap" }

#define ADDITIONAL_REGISTER_NAMES { { "r7", 0 }, { "r6", 1 } }

#define FIXED_REGISTERS     { 0, 1, 0, 0, 0, 0, 0, 0, 1, 1 }

/* The widest of the three conventions, which is ABI 0: r0-r3, r5 and lr are
   clobbered.  Functions whose signature leaves r2 or r3 unused preserve them
   instead; see fructus_fntype_abi.  */
#define CALL_USED_REGISTERS { 1, 1, 1, 0, 1, 1, 1, 1, 1, 1 }

/* The argument and return registers first, since a value computed into the
   register it leaves in costs no move.  r4 is always callee saved and lr can
   only be used at the price of saving it, so they come last.  */
#define REG_ALLOC_ORDER \
  { FRUCTUS_R0, FRUCTUS_R1, FRUCTUS_R5, FRUCTUS_R2, FRUCTUS_R3, \
    FRUCTUS_R4, FRUCTUS_LR, FRUCTUS_SP, FRUCTUS_SFP, FRUCTUS_SAP }

enum reg_class
{
  NO_REGS,
  GENERAL_REGS,
  ALL_REGS,
  LIM_REG_CLASSES
};

#define N_REG_CLASSES LIM_REG_CLASSES

#define REG_CLASS_NAMES { "NO_REGS", "GENERAL_REGS", "ALL_REGS" }

#define REG_CLASS_CONTENTS \
{ { 0x000 },			\
  { 0x3ff },			\
  { 0x3ff } }

#define REGNO_REG_CLASS(R) ((R) < FIRST_PSEUDO_REGISTER ? GENERAL_REGS : NO_REGS)

#define BASE_REG_CLASS GENERAL_REGS
#define INDEX_REG_CLASS NO_REGS

#define FRUCTUS_BASE_REGNO_P(N) ((unsigned) (N) < FIRST_PSEUDO_REGISTER)

#ifdef REG_OK_STRICT
#define REGNO_OK_FOR_BASE_P(N) \
  (FRUCTUS_BASE_REGNO_P (N) || FRUCTUS_BASE_REGNO_P (reg_renumber[N]))
#else
#define REGNO_OK_FOR_BASE_P(N) \
  ((N) >= FIRST_PSEUDO_REGISTER || FRUCTUS_BASE_REGNO_P (N))
#endif

#define REGNO_OK_FOR_INDEX_P(N) 0

/* --------------------------------------------------------------------------
   The stack and the frame

   sp points at the last thing pushed.  The frame, from the top:

       stack arguments                 <- ?ap: sp on entry
       saved lr and callee-saved regs  push lr, r4, r3 ...
       locals                          <- ?fp, and sp after the prologue

   Nothing else lives below sp but outgoing arguments, which are pushed.  r4
   becomes a real frame pointer only when the frame has a variable size.
   -------------------------------------------------------------------------- */

#define STACK_GROWS_DOWNWARD 1
#define FRAME_GROWS_DOWNWARD 0
#define FIRST_PARM_OFFSET(F) 0
#define PUSH_ROUNDING(BYTES) (BYTES)
#define ACCUMULATE_OUTGOING_ARGS 0

#define STACK_POINTER_REGNUM FRUCTUS_SP
#define FRAME_POINTER_REGNUM FRUCTUS_SFP
#define ARG_POINTER_REGNUM FRUCTUS_SAP
#define HARD_FRAME_POINTER_REGNUM FRUCTUS_R4

/* The static chain of a nested function.  r5 carries no argument, so it is
   free at every call.  */
#define STATIC_CHAIN_REGNUM FRUCTUS_R5

#define ELIMINABLE_REGS						\
{ { ARG_POINTER_REGNUM,   STACK_POINTER_REGNUM },		\
  { ARG_POINTER_REGNUM,   HARD_FRAME_POINTER_REGNUM },		\
  { FRAME_POINTER_REGNUM, STACK_POINTER_REGNUM },		\
  { FRAME_POINTER_REGNUM, HARD_FRAME_POINTER_REGNUM } }

#define INITIAL_ELIMINATION_OFFSET(FROM, TO, OFFSET) \
  ((OFFSET) = fructus_initial_elimination_offset ((FROM), (TO)))

/* lr is live at the ret, but only once the epilogue exists to have restored
   it.  Before that, saying so would make it live through the whole function
   and the allocator could never use it.  */
#define EPILOGUE_USES(R) (epilogue_completed && (R) == FRUCTUS_LR)

#define INCOMING_RETURN_ADDR_RTX gen_rtx_REG (Pmode, FRUCTUS_LR)
#define DWARF_FRAME_RETURN_COLUMN DWARF_FRAME_REGNUM (FRUCTUS_LR)

/* The debugger sees the machine's numbering, not GCC's.  */
#define DEBUGGER_REGNO(N) ((N) < 8 ? 7 - (N) : INVALID_REGNUM)

/* --------------------------------------------------------------------------
   Passing arguments

   r0, r1, r2, r3 in that order, then the stack; no pair alignment, no
   splitting, no backfilling, unnamed arguments always on the stack.  See
   isa/abi.s for the reasons.
   -------------------------------------------------------------------------- */

struct fructus_cumulative_args
{
  /* Machine argument registers consumed so far, 0-4.  */
  int nregs;
  /* Set once an argument has gone to the stack; everything after it follows.  */
  bool stack;
  /* No prototype: the call gets the widest convention, ABI 0.  */
  bool full;
  /* Registers the return value occupies, 0-4, or -1 when not yet known.  */
  int ret_regs;
};

#define CUMULATIVE_ARGS struct fructus_cumulative_args

#define INIT_CUMULATIVE_ARGS(CUM, FNTYPE, LIBNAME, FNDECL, N_NAMED_ARGS) \
  fructus_init_cumulative_args (&(CUM), (FNTYPE), VOIDmode)

#define INIT_CUMULATIVE_LIBCALL_ARGS(CUM, MODE, LIBNAME) \
  fructus_init_cumulative_args (&(CUM), NULL_TREE, (MODE))

#define FUNCTION_ARG_REGNO_P(R) ((R) >= FRUCTUS_R3 && (R) <= FRUCTUS_R0)

/* An aggregate comes back in registers when its fields fit in four of them,
   which is TARGET_RETURN_IN_MEMORY's business.  GCC's default is to return
   every aggregate through memory before asking, which is the older PCC
   convention and not this ABI's.  */
#define DEFAULT_PCC_STRUCT_RETURN 0

/* --------------------------------------------------------------------------
   Addressing
   -------------------------------------------------------------------------- */

#define MAX_REGS_PER_ADDRESS 1

#define MOVE_MAX 2
#define SLOW_BYTE_ACCESS 0

/* ld8 and pop8 zero extend.  */
#define LOAD_EXTEND_OP(MODE) ZERO_EXTEND

#define CASE_VECTOR_MODE HImode

#define STORE_FLAG_VALUE 1

#define SHIFT_COUNT_TRUNCATED 0

/* --------------------------------------------------------------------------
   Costs
   -------------------------------------------------------------------------- */

#define BRANCH_COST(SPEED_P, PREDICTABLE_P) 1
#define NO_FUNCTION_CSE 1

/* --------------------------------------------------------------------------
   Assembler output
   -------------------------------------------------------------------------- */

#define ASM_COMMENT_START ";"
#define ASM_APP_ON "#APP\n"
#define ASM_APP_OFF "#NO_APP\n"

#define TEXT_SECTION_ASM_OP "\t.text"
#define DATA_SECTION_ASM_OP "\t.data"
#define BSS_SECTION_ASM_OP  "\t.section\t.bss"

#undef  GLOBAL_ASM_OP
#define GLOBAL_ASM_OP "\t.globl\t"

#define ASM_OUTPUT_ALIGN(STREAM, POWER) \
  fprintf ((STREAM), "\t.p2align\t%d\n", (POWER))

#define ASM_OUTPUT_ADDR_VEC_ELT(STREAM, VALUE) \
  fprintf ((STREAM), "\t.short\t.L%d\n", (VALUE))

#define ASM_OUTPUT_ADDR_DIFF_ELT(STREAM, BODY, VALUE, REL) \
  fprintf ((STREAM), "\t.short\t.L%d-.L%d\n", (VALUE), (REL))

#define JUMP_TABLES_IN_TEXT_SECTION 0

/* --------------------------------------------------------------------------
   Miscellaneous
   -------------------------------------------------------------------------- */

#define FUNCTION_PROFILER(FILE, LABELNO) \
  sorry ("profiling is not supported on fructus")

/* Nested functions whose address is taken need a trampoline, which is not
   written yet; the default TARGET_TRAMPOLINE_INIT reports that.  */
#define TRAMPOLINE_SIZE 8
#define TRAMPOLINE_ALIGNMENT 8

#define HAS_LONG_UNCOND_BRANCH true
#define HAS_LONG_COND_BRANCH false

#endif /* GCC_FRUCTUS_H */
