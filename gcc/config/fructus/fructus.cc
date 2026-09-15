/* Target code for Fructus, a 16-bit retrocomputer instruction set.
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

#define IN_TARGET_CODE 1

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "backend.h"
#include "target.h"
#include "rtl.h"
#include "tree.h"
#include "stringpool.h"
#include "attribs.h"
#include "df.h"
#include "memmodel.h"
#include "tm_p.h"
#include "regs.h"
#include "emit-rtl.h"
#include "diagnostic-core.h"
#include "output.h"
#include "stor-layout.h"
#include "varasm.h"
#include "calls.h"
#include "expr.h"
#include "builtins.h"
#include "function-abi.h"
#include "insn-config.h"
#include "recog.h"
#include "insn-attr.h"
#include "optabs.h"
#include "optabs-libfuncs.h"
#include "explow.h"
#include "opts.h"
#include "tm-constrs.h"
#include "config/fructus/fructus-isa.h"

/* This file should be included last.  */
#include "target-def.h"

/* ==========================================================================
   Immediates

   gas never builds a constant: it has no scratch register, so an immediate
   that no form of the instruction carries is an error.  Every constant this
   port prints therefore has to pass one of these first.
   ========================================================================== */

static bool
fructus_signed_fits (HOST_WIDE_INT v, int bits)
{
  return IN_RANGE (v, -(HOST_WIDE_INT_1 << (bits - 1)),
		   (HOST_WIDE_INT_1 << (bits - 1)) - 1);
}

static bool
fructus_in_table (const int *table, int n, HOST_WIDE_INT v)
{
  for (int i = 0; i < n; i++)
    if (table[i] == v)
      return true;
  return false;
}

bool
fructus_imm5_p (HOST_WIDE_INT v)
{
  return fructus_signed_fits (v, FRUCTUS_IMM5_BITS);
}

bool
fructus_imm10_p (HOST_WIDE_INT v)
{
  return fructus_signed_fits (v, FRUCTUS_IMM10_BITS);
}

bool
fructus_imm3_p (HOST_WIDE_INT v)
{
  return fructus_in_table (fructus_imm3, ARRAY_SIZE (fructus_imm3), v);
}

bool
fructus_shift3_p (HOST_WIDE_INT v)
{
  return fructus_in_table (fructus_shift3, ARRAY_SIZE (fructus_shift3), v);
}

/* immbit5 or immask5.  The tables hold 16-bit patterns and a constant arrives
   sign extended, so compare the low 16 bits.  */

bool
fructus_mask5_p (HOST_WIDE_INT v)
{
  v &= 0xffff;
  return (fructus_in_table (fructus_immbit5, ARRAY_SIZE (fructus_immbit5), v)
	  || fructus_in_table (fructus_immask5, ARRAY_SIZE (fructus_immask5),
			       v));
}

/* Whether an ALU instruction with operation CODE can take V as its
   immediate.  imm10 covers imm3 and imm5 and needs no tie; the mask tables
   exist only on add, and, or and xor, and only in the tied forms.  */

bool
fructus_alu_imm_p (enum rtx_code code, HOST_WIDE_INT v, bool tied)
{
  if (fructus_imm10_p (v))
    return true;
  if (!tied)
    return false;
  switch (code)
    {
    case PLUS: case AND: case IOR: case XOR:
      return fructus_mask5_p (v);
    default:
      return false;
    }
}

/* ==========================================================================
   The compare-and-branch constants

   `br ra, #k' packs a condition and a constant into one five-bit index, so
   whether k can be tested depends on the comparison.  A comparison matches a
   condimm5 entry when the two have the same TRUTH SET - `x <= 3' is `x < 4' -
   which is how gas accepts spellings the table does not list.  The port does
   the same, and then prints the entry's own spelling, so it never relies on
   gas deriving the same rewrite.

   Every one of these predicates is true on a CIRCULAR interval of the 2^W
   values, so a truth set is a start and a length.
   ========================================================================== */

static bool
fructus_truth_set (enum rtx_code code, int width, HOST_WIDE_INT k,
		   HOST_WIDE_INT *start, HOST_WIDE_INT *len)
{
  const HOST_WIDE_INT m = HOST_WIDE_INT_1 << width, h = m / 2;
  const HOST_WIDE_INT u = k & (m - 1);
  const HOST_WIDE_INT s = u >= h ? u - m : u;
  HOST_WIDE_INT st, ln;

  switch (code)
    {
    case EQ:  st = u;     ln = 1;          break;
    case NE:  st = u + 1; ln = m - 1;      break;
    case LTU: st = 0;     ln = u;          break;
    case GEU: st = u;     ln = m - u;      break;
    case LEU: st = 0;     ln = u + 1;      break;
    case GTU: st = u + 1; ln = m - u - 1;  break;
    case LT:  st = h;     ln = s + h;      break;
    case GE:  st = u;     ln = h - s;      break;
    case LE:  st = h;     ln = s + h + 1;  break;
    case GT:  st = u + 1; ln = h - s - 1;  break;
    default:
      return false;
    }

  if (ln <= 0)
    st = ln = 0;
  else if (ln >= m)
    st = 0, ln = m;
  *start = st & (m - 1);
  *len = ln;
  return true;
}

static enum rtx_code
fructus_cond3_code (const char *name)
{
  static const struct { const char *name; enum rtx_code code; } names[] = {
    { "eq", EQ }, { "ne", NE }, { "lt", LT }, { "ge", GE }, { "le", LE },
    { "gt", GT }, { "lo", LTU }, { "hs", GEU }, { "ls", LEU }, { "hi", GTU }
  };
  for (unsigned i = 0; i < ARRAY_SIZE (names); i++)
    if (strcmp (names[i].name, name) == 0)
      return names[i].code;
  gcc_unreachable ();
}

/* The condimm5 entry that tests CODE against K at the width of MODE, or -1.  */

static int
fructus_condimm_index (enum rtx_code code, machine_mode mode, HOST_WIDE_INT k)
{
  int width = mode == QImode ? 8 : 16;
  HOST_WIDE_INT s0, l0, s1, l1;

  if (!fructus_truth_set (code, width, k, &s0, &l0))
    return -1;
  for (unsigned i = 0; i < ARRAY_SIZE (fructus_condimm5); i++)
    if (fructus_truth_set (fructus_cond3_code (fructus_condimm5[i].cond),
			   width, fructus_condimm5[i].imm, &s1, &l1)
	&& s0 == s1 && l0 == l1)
      return i;
  return -1;
}

bool
fructus_cbranch_imm_p (enum rtx_code code, machine_mode mode, HOST_WIDE_INT k)
{
  return fructus_condimm_index (code, mode, k) >= 0;
}

/* Whether a branch on CODE with second operand OP has a single instruction.
   Two registers always do.  */

bool
fructus_cbranch_ok_p (rtx op, enum rtx_code code, machine_mode mode)
{
  return !CONST_INT_P (op) || fructus_cbranch_imm_p (code, mode, INTVAL (op));
}

static const char *
fructus_cond_name (enum rtx_code code)
{
  switch (code)
    {
    case EQ:  return "eq";
    case NE:  return "ne";
    case LT:  return "lt";
    case LE:  return "le";
    case GT:  return "gt";
    case GE:  return "ge";
    case LTU: return "lo";
    case LEU: return "ls";
    case GTU: return "hi";
    case GEU: return "hs";
    default:  gcc_unreachable ();
    }
}

/* The operands of a compare-and-branch testing CODE, as assembler text with
   %0 and %1 left for output_asm_insn.  False when no single instruction
   tests it.  */

static bool
fructus_cond_text (char *buf, size_t size, enum rtx_code code,
		   machine_mode mode, rtx op)
{
  if (!CONST_INT_P (op))
    {
      snprintf (buf, size, "%s, %%0, %%1", fructus_cond_name (code));
      return true;
    }
  int i = fructus_condimm_index (code, mode, INTVAL (op));
  if (i < 0)
    return false;
  snprintf (buf, size, "%s, %%0, #%d", fructus_condimm5[i].cond,
	    fructus_condimm5[i].imm);
  return true;
}

/* Output a compare-and-branch to %l2 taken when CODE holds of %0 and %1, or
   when it does not if INVERT.  LENGTH is what the length attribute worked
   out, which has already decided between the three shapes:

     3   br c, a, b, L                        in reach, c encodable
     6   br !c, a, b, 1f; jmp L; 1:           !c encodable
     8   br c, a, b, 1f; jmpr 2f; 1: jmp L; 2:
					      only c encodable, out of reach

   The conditional branch has an 8-bit displacement and gas does not relax it,
   so the choice has to be made here.  */

const char *
fructus_output_cbranch (rtx *operands, enum rtx_code code, machine_mode mode,
			bool invert, int length)
{
  static char buf[160];
  char c[40], nc[40];
  const char *mn = mode == QImode ? "br8" : "br";
  enum rtx_code cc = invert ? reverse_condition (code) : code;
  bool c_ok = fructus_cond_text (c, sizeof c, cc, mode, operands[1]);
  bool nc_ok = fructus_cond_text (nc, sizeof nc, reverse_condition (cc), mode,
				  operands[1]);

  if (c_ok && length == 3)
    snprintf (buf, sizeof buf, "%s\t%s, %%l2", mn, c);
  else if (nc_ok)
    snprintf (buf, sizeof buf, "%s\t%s, .Lb%%=\n\tjmp\t%%l2\n.Lb%%=:", mn, nc);
  else
    {
      gcc_assert (c_ok);
      snprintf (buf, sizeof buf,
		"%s\t%s, .Lt%%=\n\tjmpr\t.Lf%%=\n.Lt%%=:\n\tjmp\t%%l2\n.Lf%%=:",
		mn, c);
    }
  return buf;
}

/* brset / brclear: taken when %0 & %1 is non-zero if SET, zero otherwise.  */

const char *
fructus_output_bitbranch (rtx *operands, bool set, int length)
{
  if (length == 3)
    return set ? "brset\t%0, #%1, %l2" : "brclear\t%0, #%1, %l2";
  return (set ? "brclear\t%0, #%1, .Lb%=\n\tjmp\t%l2\n.Lb%=:"
	  : "brset\t%0, #%1, .Lb%=\n\tjmp\t%l2\n.Lb%=:");
}

/* ==========================================================================
   Instruction lengths

   These mirror gas's form selection - shortest form whose constraints the
   operands meet - so that the lengths branch shortening adds up are the
   lengths gas will emit.  Being an overestimate is safe; being an
   underestimate can put a branch out of reach.  They are only consulted
   after register allocation, when every register is a hard one.
   ========================================================================== */

/* Whether X is machine register rN.  */

static bool
fructus_reg_is (rtx x, int n)
{
  return REG_P (x) && REGNO (x) == (unsigned) FRUCTUS_REGNO (n);
}

bool
fructus_same_reg_p (rtx a, rtx b)
{
  return REG_P (a) && REG_P (b) && REGNO (a) == REGNO (b);
}

/* A load into or store from REG through MEM.  */

static int
fructus_mem_length (rtx reg, rtx mem, bool load)
{
  rtx addr = XEXP (mem, 0), base = addr;
  HOST_WIDE_INT off = 0;

  if (GET_CODE (addr) == PLUS && CONST_INT_P (XEXP (addr, 1)))
    base = XEXP (addr, 0), off = INTVAL (XEXP (addr, 1));

  if (load && off == 0 && fructus_reg_is (reg, 0) && fructus_reg_is (base, 0))
    return 1;
  if (load && fructus_same_reg_p (reg, base) && fructus_imm5_p (off))
    return 2;
  if (fructus_imm3_p (off))
    return 2;
  return 3;
}

int
fructus_move_length (rtx *operands)
{
  rtx d = operands[0], s = operands[1];

  if (MEM_P (d))
    return fructus_mem_length (s, d, false);
  if (MEM_P (s))
    return fructus_mem_length (d, s, true);
  if (REG_P (s))
    return ((fructus_reg_is (d, 0) && fructus_reg_is (s, 1))
	    || (fructus_reg_is (d, 1) && fructus_reg_is (s, 0))) ? 1 : 2;
  if (CONST_INT_P (s))
    {
      HOST_WIDE_INT v = INTVAL (s);
      if (v == 0 && fructus_reg_is (d, 0))
	return 1;
      if (fructus_imm5_p (v) || fructus_mask5_p (v))
	return 2;
    }
  return 3;
}

int
fructus_double_move_length (rtx *operands)
{
  machine_mode mode = GET_MODE (operands[0]);
  int len = 0;

  for (int w = 0; w < 2; w++)
    {
      rtx ops[2];
      ops[0] = simplify_gen_subreg (HImode, operands[0], mode, 2 * w);
      ops[1] = simplify_gen_subreg (HImode, operands[1], mode, 2 * w);
      len += (ops[0] && ops[1]) ? fructus_move_length (ops) : 3;
    }
  return len;
}

int
fructus_alu_length (enum rtx_code code, rtx *operands)
{
  rtx d = operands[0], a = operands[1], b = operands[2];

  if (!CONST_INT_P (b))
    {
      /* add r0, r0, r1 and add r1, r1, r0 have one-byte encodings, and
	 fructus_output_alu puts a commutative pair in whichever order
	 reaches them.  */
      if (code == PLUS
	  && ((fructus_reg_is (d, 0)
	       && ((fructus_reg_is (a, 0) && fructus_reg_is (b, 1))
		   || (fructus_reg_is (a, 1) && fructus_reg_is (b, 0))))
	      || (fructus_reg_is (d, 1)
		  && ((fructus_reg_is (a, 1) && fructus_reg_is (b, 0))
		      || (fructus_reg_is (a, 0) && fructus_reg_is (b, 1))))))
	return 1;
      return 2;
    }

  HOST_WIDE_INT v = INTVAL (b);
  bool tied = fructus_same_reg_p (d, a);

  switch (code)
    {
    case ASHIFT: case ASHIFTRT: case LSHIFTRT:
      return 2;
    case PLUS:
      if (tied && fructus_reg_is (d, 0) && (v == 1 || v == -1 || v == 2))
	return 1;
      break;
    case XOR:
      if (tied && fructus_reg_is (d, 0) && v == 1)
	return 1;
      break;
    default:
      break;
    }

  if (tied && (fructus_imm5_p (v) || fructus_mask5_p (v)))
    return 2;
  if (fructus_imm3_p (v))
    return 2;
  return 3;
}

/* rsb %0, %2, %1: d = %1 - %2, the constant if any in %1.  */

int
fructus_rsb_length (rtx *operands)
{
  if (!CONST_INT_P (operands[1]))
    return 2;
  HOST_WIDE_INT v = INTVAL (operands[1]);
  if (fructus_same_reg_p (operands[0], operands[2]) && fructus_imm5_p (v))
    return 2;
  return fructus_imm3_p (v) ? 2 : 3;
}

/* iseq, and for NE the xor that inverts it.  */

int
fructus_iseq_length (rtx *operands, bool ne)
{
  int len = 2;
  if (CONST_INT_P (operands[2]))
    {
      HOST_WIDE_INT v = INTVAL (operands[2]);
      if (!((fructus_same_reg_p (operands[0], operands[1])
	     && fructus_imm5_p (v))
	    || fructus_imm3_p (v)))
	len = 3;
    }
  if (ne)
    len += fructus_reg_is (operands[0], 0) ? 1 : 2;
  return len;
}

/* ==========================================================================
   Output templates that depend on the operands
   ========================================================================== */

const char *
fructus_output_alu (enum rtx_code code, rtx *operands)
{
  static char buf[40];
  const char *mn;

  switch (code)
    {
    case PLUS: mn = "add"; break;
    case AND:  mn = "and"; break;
    case IOR:  mn = "or";  break;
    case XOR:  mn = "xor"; break;
    default:   gcc_unreachable ();
    }

  if (CONST_INT_P (operands[2]))
    {
      snprintf (buf, sizeof buf, "%s\t%%0, %%1, #%%2", mn);
      return buf;
    }

  /* Commutative, so name the destination first when it is the second source:
     that is the shape the one-byte adds have.  */
  if (fructus_same_reg_p (operands[0], operands[2])
      && !fructus_same_reg_p (operands[0], operands[1]))
    snprintf (buf, sizeof buf, "%s\t%%0, %%2, %%1", mn);
  else
    snprintf (buf, sizeof buf, "%s\t%%0, %%1, %%2", mn);
  return buf;
}

const char *
fructus_output_iseq (rtx *operands, bool ne)
{
  if (CONST_INT_P (operands[2]))
    return ne ? "iseq\t%0, %1, #%2\n\txor\t%0, %0, #1" : "iseq\t%0, %1, #%2";
  return ne ? "iseq\t%0, %1, %2\n\txor\t%0, %0, #1" : "iseq\t%0, %1, %2";
}

/* ==========================================================================
   Expanders
   ========================================================================== */

/* Every store takes a register: there is no store-immediate and no
   memory-to-memory move.  */

bool
fructus_expand_move (rtx *operands, machine_mode mode)
{
  if (can_create_pseudo_p ()
      && MEM_P (operands[0])
      && !register_operand (operands[1], mode))
    operands[1] = force_reg (mode, operands[1]);
  return false;
}

/* Split a 32-bit move into two 16-bit ones, in whichever order does not
   overwrite a source half, or the address, before it is read.  */

void
fructus_split_double_move (rtx *operands, machine_mode mode)
{
  rtx dst = operands[0], src = operands[1];
  rtx dlo = simplify_gen_subreg (HImode, dst, mode, 0);
  rtx dhi = simplify_gen_subreg (HImode, dst, mode, 2);
  rtx slo = simplify_gen_subreg (HImode, src, mode, 0);
  rtx shi = simplify_gen_subreg (HImode, src, mode, 2);

  if (reg_overlap_mentioned_p (dlo, shi))
    {
      emit_move_insn (dhi, shi);
      emit_move_insn (dlo, slo);
    }
  else
    {
      emit_move_insn (dlo, slo);
      emit_move_insn (dhi, shi);
    }
}

/* a - b.  A constant b becomes an add of -b, and a constant a is rsb's
   immediate.  */

bool
fructus_expand_sub (rtx *operands)
{
  rtx a = operands[1], b = operands[2];

  if (CONST_INT_P (b))
    {
      rtx nb = gen_int_mode (-INTVAL (b), HImode);
      if (!fructus_alu_imm_p (PLUS, INTVAL (nb), true))
	nb = force_reg (HImode, nb);
      emit_insn (gen_addhi3 (operands[0], force_reg (HImode, a), nb));
      return true;
    }
  if (!fructus_imm10_operand (a, HImode))
    operands[1] = force_reg (HImode, a);
  if (!register_operand (b, HImode))
    operands[2] = force_reg (HImode, b);
  return false;
}

void
fructus_expand_cbranch (rtx *operands, machine_mode mode)
{
  enum rtx_code code = GET_CODE (operands[0]);

  if (!register_operand (operands[1], mode))
    operands[1] = force_reg (mode, operands[1]);
  if (CONST_INT_P (operands[2])
      ? !fructus_cbranch_imm_p (code, mode, INTVAL (operands[2]))
      : !register_operand (operands[2], mode))
    operands[2] = force_reg (mode, operands[2]);
}

/* OPERANDS[N] is the (mem:QI address) of a call.  */

void
fructus_expand_call (rtx *operands, int n)
{
  rtx addr = XEXP (operands[n], 0);
  if (!fructus_call_operand (addr, Pmode))
    operands[n] = gen_rtx_MEM (QImode, force_reg (Pmode, addr));
}

/* ==========================================================================
   The frame

   Callee-saved registers go in one push of up to three, lr first:

       push lr, r4, r3        push r2         (at most four)

   A register is saved when this function's own ABI promises to preserve it
   and something here writes it - including a call whose ABI clobbers it,
   which under the sliding convention happens: a one-argument function that
   calls a four-argument one must preserve r2 and r3 for its caller while
   its callee destroys them.
   ========================================================================== */

struct GTY(()) machine_function
{
  /* Registers to save, in push order, as GCC regnos.  */
  int saved[4];
  int nsaved;
  HOST_WIDE_INT frame_size;
};

static struct machine_function *
fructus_init_machine_status (void)
{
  return ggc_cleared_alloc<machine_function> ();
}

static bool
fructus_call_clobbers_p (unsigned regno)
{
  for (rtx_insn *insn = get_insns (); insn; insn = NEXT_INSN (insn))
    if (CALL_P (insn) && insn_callee_abi (insn).clobbers_full_reg_p (regno))
      return true;
  return false;
}

/* THIS IS WHERE THE SLIDING CONVENTION IS KEPT.  crtl->abi is this function's
   own ABI, which fructus_fntype_abi derived from its type; the static
   call_used_regs array describes only the widest of the three and would
   leave a two-argument function clobbering r2 and r3 behind its caller's
   back.  */

static bool
fructus_save_reg_p (unsigned regno)
{
  if (regno == FRUCTUS_LR)
    return !crtl->is_leaf || df_regs_ever_live_p (FRUCTUS_LR);
  if (regno == HARD_FRAME_POINTER_REGNUM && frame_pointer_needed)
    return true;
  if (fixed_regs[regno] || crtl->abi->clobbers_full_reg_p (regno))
    return false;
  return df_regs_ever_live_p (regno) || fructus_call_clobbers_p (regno);
}

static void
fructus_compute_frame (void)
{
  static const int order[] = { FRUCTUS_LR, FRUCTUS_R4, FRUCTUS_R3, FRUCTUS_R2 };
  machine_function *m = cfun->machine;

  m->nsaved = 0;
  for (unsigned i = 0; i < ARRAY_SIZE (order); i++)
    if (fructus_save_reg_p (order[i]))
      m->saved[m->nsaved++] = order[i];
  m->frame_size = get_frame_size ();
}

HOST_WIDE_INT
fructus_initial_elimination_offset (int from, int to ATTRIBUTE_UNUSED)
{
  fructus_compute_frame ();
  if (from == FRAME_POINTER_REGNUM)
    return 0;
  gcc_assert (from == ARG_POINTER_REGNUM);
  return 2 * cfun->machine->nsaved + cfun->machine->frame_size;
}

static void
fructus_emit_push (const int *regs, int n)
{
  rtx r[3];
  for (int i = 0; i < n; i++)
    r[i] = gen_rtx_REG (HImode, regs[i]);
  switch (n)
    {
    case 1: emit_insn (gen_pushhi1 (r[0])); break;
    case 2: emit_insn (gen_fructus_push2 (r[0], r[1])); break;
    case 3: emit_insn (gen_fructus_push3 (r[0], r[1], r[2])); break;
    default: gcc_unreachable ();
    }
}

static void
fructus_emit_pop (const int *regs, int n)
{
  rtx r[3];
  for (int i = 0; i < n; i++)
    r[i] = gen_rtx_REG (HImode, regs[i]);
  switch (n)
    {
    case 1: emit_insn (gen_fructus_pop1 (r[0])); break;
    case 2: emit_insn (gen_fructus_pop2 (r[0], r[1])); break;
    case 3: emit_insn (gen_fructus_pop3 (r[0], r[1], r[2])); break;
    default: gcc_unreachable ();
    }
}

/* sp += N.  Up to +-512 is one add; beyond that the constant goes through
   r5, which no argument or return value ever occupies.  */

static void
fructus_emit_sp_add (HOST_WIDE_INT n)
{
  if (n == 0)
    return;
  rtx v = gen_int_mode (n, HImode);
  if (!fructus_alu_imm_p (PLUS, INTVAL (v), true))
    {
      if (cfun->static_chain_decl)
	sorry ("a nested function with a frame over 512 bytes");
      rtx r5 = gen_rtx_REG (HImode, FRUCTUS_R5);
      emit_move_insn (r5, v);
      v = r5;
    }
  emit_insn (gen_addhi3 (stack_pointer_rtx, stack_pointer_rtx, v));
}

void
fructus_expand_prologue (void)
{
  machine_function *m = cfun->machine;

  fructus_compute_frame ();
  if (flag_stack_usage_info)
    current_function_static_stack_size = 2 * m->nsaved + m->frame_size;

  for (int i = 0; i < m->nsaved; i += 3)
    fructus_emit_push (m->saved + i, MIN (3, m->nsaved - i));
  fructus_emit_sp_add (-m->frame_size);
  if (frame_pointer_needed)
    emit_move_insn (hard_frame_pointer_rtx, stack_pointer_rtx);
}

void
fructus_expand_epilogue (bool sibcall)
{
  machine_function *m = cfun->machine;

  fructus_compute_frame ();
  emit_insn (gen_blockage ());
  if (frame_pointer_needed)
    emit_move_insn (stack_pointer_rtx, hard_frame_pointer_rtx);
  fructus_emit_sp_add (m->frame_size);

  /* The pushes in reverse, each one's register list reversed too: a push
     names its first register highest and a pop takes them lowest first.  */
  for (int first = m->nsaved > 0 ? (m->nsaved - 1) / 3 * 3 : -1;
       first >= 0; first -= 3)
    {
      int n = MIN (3, m->nsaved - first), regs[3];
      for (int i = 0; i < n; i++)
	regs[i] = m->saved[first + n - 1 - i];
      fructus_emit_pop (regs, n);
    }

  /* A tail call is its own return: the jump that follows this reaches the
     callee, whose `ret' goes to our caller through the lr just restored.  */
  if (!sibcall)
    emit_jump_insn (gen_fructus_return ());
}

static bool
fructus_can_eliminate (const int from ATTRIBUTE_UNUSED, const int to)
{
  return to == HARD_FRAME_POINTER_REGNUM || !frame_pointer_needed;
}

static bool
fructus_frame_pointer_required (void)
{
  return cfun->calls_alloca;
}

/* ==========================================================================
   Arguments and return values

   r0, r1, r2, r3 in order, then the stack.  A value takes (size + 1) / 2
   registers, high half first; no pair alignment, no splitting, no
   backfilling, unnamed arguments on the stack.

   Aggregates go through memory for now - passed on the stack, returned
   through the hidden pointer - where isa/abi.s decomposes them into their
   fields.  That is the one place this port departs from the ABI.
   ========================================================================== */

/* Registers a value of MODE occupies.  */

static int
fructus_value_regs (machine_mode mode)
{
  if (mode == VOIDmode || mode == BLKmode)
    return 0;
  return (GET_MODE_SIZE (mode) + 1) / 2;
}

/* The GCC regno of the LOW word of a value of N registers starting at machine
   register rP.  The high word is in rP, the low one in rP+N-1, and GCC's
   numbering runs backwards so that the pair is consecutive the right way.  */

static unsigned
fructus_arg_regno (int p, int n)
{
  return FRUCTUS_REGNO (p + n - 1);
}

static bool fructus_return_in_memory (const_tree, const_tree);
static int fructus_aggregate_regs (const_tree);

/* Whether FNTYPE returns through the hidden pointer.  Not aggregate_value_p,
   which asks TARGET_FNTYPE_ABI whether the return register is clobbered - and
   the ABI is what this is being asked in order to work out.  */

static bool
fructus_hidden_return_p (const_tree fntype)
{
  tree ret = TREE_TYPE (fntype);
  return !VOID_TYPE_P (ret) && fructus_return_in_memory (ret, fntype);
}

void
fructus_init_cumulative_args (CUMULATIVE_ARGS *cum, tree fntype,
			      machine_mode libcall_mode)
{
  cum->nregs = 0;
  cum->stack = false;
  cum->full = false;
  cum->ret_regs = 0;

  if (fntype)
    {
      tree ret = TREE_TYPE (fntype);
      /* Without a prototype neither end can count the arguments, so
	 assume the widest convention.  */
      if (!prototype_p (fntype))
	cum->full = true;
      if (fructus_hidden_return_p (fntype))
	cum->ret_regs = 1;	/* the hidden pointer comes back in r0 */
      else if (AGGREGATE_TYPE_P (ret))
	cum->ret_regs = fructus_aggregate_regs (ret);
      else
	cum->ret_regs = fructus_value_regs (TYPE_MODE (ret));
    }
  else
    cum->ret_regs = fructus_value_regs (libcall_mode);
}

/* ==========================================================================
   Aggregates are decomposed into their fields

   isa/abi.s: "Structs are decomposed into their fields and each field is
   assigned independently - there is no such thing as passing a struct."  A
   field of any size up to 16 bits takes a WHOLE register, which is why the
   limit is four REGISTERS rather than eight bytes: `struct { char a, b, c,
   d, e; }' is five registers and goes through memory even though it is five
   bytes.

   What is not decomposed, because the ABI's rule does not reach it: a union
   or a bitfield, whose fields share storage and so have no independent
   assignment, and anything needing more than the four argument registers.
   Those go through memory, which the ABI already provides for.
   ========================================================================== */

#define FRUCTUS_MAX_FIELDS 4

struct fructus_fields
{
  int n;					/* fields found */
  int regs;					/* registers they need */
  HOST_WIDE_INT offset[FRUCTUS_MAX_FIELDS];	/* byte offset in the value */
  machine_mode mode[FRUCTUS_MAX_FIELDS];
};

static bool
fructus_collect_fields (const_tree type, HOST_WIDE_INT offset,
			struct fructus_fields *f)
{
  switch (TREE_CODE (type))
    {
    case RECORD_TYPE:
      for (tree field = TYPE_FIELDS (type); field; field = DECL_CHAIN (field))
	{
	  if (TREE_CODE (field) != FIELD_DECL)
	    continue;
	  if (DECL_BIT_FIELD (field) || !tree_fits_shwi_p (bit_position (field)))
	    return false;
	  HOST_WIDE_INT bits = int_bit_position (field);
	  if (bits % BITS_PER_UNIT)
	    return false;
	  if (!fructus_collect_fields (TREE_TYPE (field),
				       offset + bits / BITS_PER_UNIT, f))
	    return false;
	}
      return true;

    case ARRAY_TYPE:
      {
	HOST_WIDE_INT size = int_size_in_bytes (type);
	HOST_WIDE_INT esize = int_size_in_bytes (TREE_TYPE (type));
	if (size <= 0 || esize <= 0)
	  return false;
	for (HOST_WIDE_INT at = 0; at < size; at += esize)
	  if (!fructus_collect_fields (TREE_TYPE (type), offset + at, f))
	    return false;
	return true;
      }

    case UNION_TYPE:
    case QUAL_UNION_TYPE:
      return false;

    default:
      {
	HOST_WIDE_INT size = int_size_in_bytes (type);
	machine_mode mode = TYPE_MODE (type);
	if (size <= 0 || mode == BLKmode || f->n >= FRUCTUS_MAX_FIELDS)
	  return false;
	f->offset[f->n] = offset;
	f->mode[f->n] = mode;
	f->n++;
	f->regs += fructus_value_regs (mode);
	return f->regs <= 4;
      }
    }
}

/* TYPE's fields, or false if it goes through memory instead.  */

static bool
fructus_decompose (const_tree type, struct fructus_fields *f)
{
  f->n = 0;
  f->regs = 0;
  if (!type || !AGGREGATE_TYPE_P (type)
      || !TYPE_SIZE (type) || !tree_fits_uhwi_p (TYPE_SIZE (type)))
    return false;
  return fructus_collect_fields (type, 0, f) && f->n > 0;
}

/* Registers an aggregate return value occupies: its fields' registers, as
   fructus_function_value places them.  NOT its mode's size - a six-byte
   struct has no integer mode and would count as none, and four chars are an
   SImode that counts as two, while their fields reach r2 and r3.  Counting
   either way short gives a function the convention that preserves r2 or r3
   while its return value is in them, and its epilogue then restores the
   caller's register over a field.  */

static int
fructus_aggregate_regs (const_tree type)
{
  struct fructus_fields f;
  return fructus_decompose (type, &f) ? f.regs : 0;
}

/* ARG's fields, when they all reach registers.  Returns how many there are,
   or zero if the aggregate goes on the stack instead.

   ALL OR NOTHING, which is the no-splitting rule read at the struct rather
   than the field: an aggregate whose fields do not all fit is placed whole
   on the stack, wasting at most the registers that were left.  Letting it
   straddle is what GCC's pretend_args_size is for, and it would put the
   rebuilding of the struct in every callee's prologue - byte by byte, since
   a char field takes a whole register and one byte of stack.  */

static int
fructus_arg_fields (const CUMULATIVE_ARGS *cum, const function_arg_info &arg,
		    struct fructus_fields *f)
{
  if (!arg.named || cum->stack || !fructus_decompose (arg.type, f))
    return 0;
  return cum->nregs + f->regs <= 4 ? f->n : 0;
}

/* Registers ARG takes, or 0 if it goes on the stack.  */

static int
fructus_arg_regs (const CUMULATIVE_ARGS *cum, const function_arg_info &arg)
{
  if (!arg.named || cum->stack)
    return 0;

  if (arg.aggregate_type_p () || arg.mode == BLKmode)
    {
      struct fructus_fields f;
      return fructus_arg_fields (cum, arg, &f) ? f.regs : 0;
    }

  int n = fructus_value_regs (arg.mode);
  if (n == 0 || cum->nregs + n > 4)
    return 0;
  return n;
}

/* ==========================================================================
   The sliding convention

   r2 and r3 are caller saved exactly when the signature uses them, for an
   argument or for the return value, and callee saved otherwise.  Since the
   registers a signature uses always form a prefix of r0-r3, that is three
   ABIs:

     id 0   r0-r3 used or unknown   r0 r1 r2 r3 r5 lr clobbered (the default)
     id 1   r0-r2 used              r3 preserved
     id 2   at most r0-r1 used      r2 and r3 preserved

   A function learns its own through TARGET_FNTYPE_ABI, from its type.  A
   call site learns its callee's from the cookie function_arg returns for the
   end marker, which the call patterns carry as UNSPEC_CALLEE_CC, because by
   the time anything asks what a call clobbers the callee's type is gone -
   and for an indirect call it was only ever the pointer's type.  Both ends
   run the same register assignment over the same types, so they agree.
   ========================================================================== */

static unsigned
fructus_abi_id (const CUMULATIVE_ARGS *cum)
{
  int used = MAX (cum->nregs, cum->ret_regs);
  if (cum->full || used > 3)
    return 0;
  return used == 3 ? 1 : 2;
}

static const predefined_function_abi &
fructus_abi (unsigned id)
{
  predefined_function_abi &abi = function_abis[id];
  if (!abi.initialized_p ())
    {
      HARD_REG_SET clobbers = default_function_abi.full_reg_clobbers ();
      if (id >= 1)
	CLEAR_HARD_REG_BIT (clobbers, FRUCTUS_R3);
      if (id >= 2)
	CLEAR_HARD_REG_BIT (clobbers, FRUCTUS_R2);
      abi.initialize (id, clobbers);
    }
  return abi;
}

/* The registers ARG's fields occupy, as a PARALLEL of register and byte
   offset - which is how GCC is told that a value arrives in pieces.  */

static rtx
fructus_fields_rtx (const struct fructus_fields *f, int k, int first,
		    machine_mode mode)
{
  rtx slot[FRUCTUS_MAX_FIELDS];
  int p = first;

  for (int i = 0; i < k; i++)
    {
      int need = fructus_value_regs (f->mode[i]);
      slot[i] = gen_rtx_EXPR_LIST (VOIDmode,
				   gen_rtx_REG (f->mode[i],
						fructus_arg_regno (p, need)),
				   GEN_INT (f->offset[i]));
      p += need;
    }
  return gen_rtx_PARALLEL (mode, gen_rtvec_v (k, slot));
}

static rtx
fructus_function_arg (cumulative_args_t cum_v, const function_arg_info &arg)
{
  CUMULATIVE_ARGS *cum = get_cumulative_args (cum_v);

  if (arg.end_marker_p ())
    return GEN_INT (fructus_abi_id (cum));

  if (arg.named && !cum->stack
      && (arg.aggregate_type_p () || arg.mode == BLKmode))
    {
      struct fructus_fields f;
      int k = fructus_arg_fields (cum, arg, &f);
      if (k == 0)
	return NULL_RTX;
      return fructus_fields_rtx (&f, k, cum->nregs, arg.mode);
    }

  int n = fructus_arg_regs (cum, arg);
  if (n == 0)
    return NULL_RTX;
  return gen_rtx_REG (arg.mode, fructus_arg_regno (cum->nregs, n));
}

static void
fructus_function_arg_advance (cumulative_args_t cum_v,
			      const function_arg_info &arg)
{
  CUMULATIVE_ARGS *cum = get_cumulative_args (cum_v);
  int n = fructus_arg_regs (cum, arg);

  if (n)
    cum->nregs += n;
  else
    cum->stack = true;
}

static const predefined_function_abi &
fructus_fntype_abi (const_tree fntype)
{
  CUMULATIVE_ARGS cum;
  cumulative_args_t cum_v = pack_cumulative_args (&cum);

  fructus_init_cumulative_args (&cum, const_cast<tree> (fntype), VOIDmode);
  if (!cum.full)
    {
      if (fructus_hidden_return_p (fntype))
	fructus_function_arg_advance (cum_v,
				      function_arg_info (ptr_type_node, true));
      for (tree a = TYPE_ARG_TYPES (fntype); a && a != void_list_node;
	   a = TREE_CHAIN (a))
	{
	  tree type = TREE_VALUE (a);
	  if (type == error_mark_node || !COMPLETE_TYPE_P (type))
	    return fructus_abi (0);
	  fructus_function_arg_advance (cum_v, function_arg_info (type, true));
	}
    }
  return fructus_abi (fructus_abi_id (&cum));
}

static const predefined_function_abi &
fructus_insn_callee_abi (const rtx_insn *insn)
{
  rtx pat = PATTERN (insn);

  if (GET_CODE (pat) == PARALLEL)
    for (int i = 0; i < XVECLEN (pat, 0); i++)
      {
	rtx x = XVECEXP (pat, 0, i);
	if (GET_CODE (x) == USE
	    && GET_CODE (XEXP (x, 0)) == UNSPEC
	    && XINT (XEXP (x, 0), 1) == UNSPEC_CALLEE_CC)
	  return fructus_abi (INTVAL (XVECEXP (XEXP (x, 0), 0, 0)));
      }
  return default_function_abi;
}

/* Whether a call to DECL may become a tail call.

   THE SLIDING CONVENTION DECIDES THIS, and not the frame.  A tail call
   leaves the callee running with our caller's return address, so the
   callee's clobbers become ours: everything it destroys we must already
   have been entitled to destroy, or our caller finds a register gone.  The
   three ABIs nest - id 2 clobbers less than id 1, which clobbers less than
   id 0 - so the question is a subset test.

   It refuses exactly the case libc/memset.s documents by hand: bzero takes
   two arguments, so r2 is callee saved in it, and memset takes three, so r2
   is an argument there and destroyed.  `mov r2, r1; mov r1, #0; jmpr
   memset' would return to bzero's caller with r2 in pieces.

   AN INDIRECT CALL IS ASKED THE SAME QUESTION, of the pointer's type - which
   is all a call site ever knew about its callee anyway, and the same type the
   UNSPEC_CALLEE_CC cookie is computed from, so the two agree by construction.
   A call through a pointer whose type says four arguments may not be tail
   called from a function that promised to preserve r2 and r3, exactly as if
   it had been named.

   A call from a function with pretend arguments is refused: a varargs
   function has its register arguments pushed below its frame, and the
   epilogue above does not pop them.  */

static bool
fructus_function_ok_for_sibcall (tree decl, tree exp)
{
  if (crtl->args.pretend_args_size != 0)
    return false;

  tree fntype = NULL_TREE;
  if (decl != NULL_TREE)
    fntype = TREE_TYPE (decl);
  else if (exp != NULL_TREE)
    {
      tree fn = CALL_EXPR_FN (exp);
      tree type = fn != NULL_TREE ? TREE_TYPE (fn) : NULL_TREE;
      if (type != NULL_TREE && POINTER_TYPE_P (type))
	fntype = TREE_TYPE (type);
    }
  if (fntype == NULL_TREE || !FUNC_OR_METHOD_TYPE_P (fntype))
    return false;

  return hard_reg_set_subset_p (fructus_fntype_abi (fntype).full_reg_clobbers (),
				crtl->abi->full_reg_clobbers ());
}

/* The return value: r0, r0:r1 or r0:r1:r2:r3, high to low - and for an
   aggregate, its fields from r0 upward, exactly as they would be passed.  */

static rtx
fructus_function_value (const_tree valtype, const_tree, bool)
{
  machine_mode mode = TYPE_MODE (valtype);

  if (AGGREGATE_TYPE_P (valtype))
    {
      struct fructus_fields f;
      if (fructus_decompose (valtype, &f))
	return fructus_fields_rtx (&f, f.n, 0, mode);
    }

  return gen_rtx_REG (mode, fructus_arg_regno (0, fructus_value_regs (mode)));
}

static rtx
fructus_libcall_value (machine_mode mode, const_rtx)
{
  return gen_rtx_REG (mode, fructus_arg_regno (0, fructus_value_regs (mode)));
}

static bool
fructus_function_value_regno_p (const unsigned int regno)
{
  return regno >= FRUCTUS_R3 && regno <= FRUCTUS_R0;
}

/* An aggregate comes back in registers when its fields fit in four, and
   through the caller's hidden pointer when they do not.  */

static bool
fructus_return_in_memory (const_tree type, const_tree)
{
  if (AGGREGATE_TYPE_P (type))
    {
      struct fructus_fields f;
      return !fructus_decompose (type, &f);
    }

  HOST_WIDE_INT size = int_size_in_bytes (type);
  return size < 0 || size > 8 || TYPE_MODE (type) == BLKmode;
}

/* ==========================================================================
   Addresses: a register plus a signed 10-bit byte displacement, or a
   register alone.  There is no absolute and no indexed mode.
   ========================================================================== */

static bool
fructus_base_reg_ok_p (rtx x, bool strict)
{
  if (!REG_P (x))
    return false;
  unsigned regno = REGNO (x);
  if (strict)
    return (FRUCTUS_BASE_REGNO_P (regno)
	    || (regno >= FIRST_PSEUDO_REGISTER
		&& FRUCTUS_BASE_REGNO_P (reg_renumber[regno])));
  return regno >= FIRST_PSEUDO_REGISTER || FRUCTUS_BASE_REGNO_P (regno);
}

static bool
fructus_legitimate_address_p (machine_mode mode, rtx x, bool strict,
			      code_helper = ERROR_MARK)
{
  if (fructus_base_reg_ok_p (x, strict))
    return true;

  if (GET_CODE (x) == PLUS
      && fructus_base_reg_ok_p (XEXP (x, 0), strict)
      && CONST_INT_P (XEXP (x, 1)))
    {
      /* A 32-bit access is split into two 16-bit ones, and the second must
	 reach too.  */
      HOST_WIDE_INT off = INTVAL (XEXP (x, 1));
      HOST_WIDE_INT size = GET_MODE_SIZE (mode);
      HOST_WIDE_INT last = size > 2 ? off + size - 2 : off;
      return fructus_imm10_p (off) && fructus_imm10_p (last);
    }

  return false;
}

/* ==========================================================================
   Printing
   ========================================================================== */

static void
fructus_print_operand_address (FILE *file, machine_mode, rtx addr)
{
  rtx base = addr;
  HOST_WIDE_INT off = 0;

  if (GET_CODE (addr) == PLUS && CONST_INT_P (XEXP (addr, 1)))
    base = XEXP (addr, 0), off = INTVAL (XEXP (addr, 1));
  if (!REG_P (base))
    {
      output_operand_lossage ("invalid address");
      return;
    }
  fprintf (file, "[%s, #" HOST_WIDE_INT_PRINT_DEC "]",
	   reg_names[REGNO (base)], off);
}

/* %H and %L name the high and low words of a 32-bit operand.  */

static void
fructus_print_operand (FILE *file, rtx x, int code)
{
  if (code == 'H' || code == 'L')
    {
      int w = code == 'H';
      if (REG_P (x))
	fputs (reg_names[REGNO (x) + w], file);
      else if (MEM_P (x))
	output_address (HImode, XEXP (adjust_address (x, HImode, 2 * w), 0));
      else if (CONST_INT_P (x))
	fprintf (file, HOST_WIDE_INT_PRINT_DEC,
		 sext_hwi ((INTVAL (x) >> (16 * w)) & 0xffff, 16));
      else
	output_operand_lossage ("invalid operand for %%%c", code);
      return;
    }
  if (code != 0)
    {
      output_operand_lossage ("invalid operand code %%%c", code);
      return;
    }

  switch (GET_CODE (x))
    {
    case REG:
      fputs (reg_names[REGNO (x)], file);
      return;
    case MEM:
      output_address (GET_MODE (x), XEXP (x, 0));
      return;
    case CONST_INT:
      fprintf (file, HOST_WIDE_INT_PRINT_DEC, INTVAL (x));
      return;
    default:
      if (CONSTANT_P (x))
	output_addr_const (file, x);
      else
	output_operand_lossage ("invalid operand");
      return;
    }
}

/* ==========================================================================
   Registers
   ========================================================================== */

static unsigned int
fructus_hard_regno_nregs (unsigned int regno, machine_mode mode)
{
  if (regno >= FRUCTUS_SFP)
    return 1;
  return (GET_MODE_SIZE (mode) + 1) / 2;
}

/* A multi-register value may not include sp or lr, and must end at r0.  */

static bool
fructus_hard_regno_mode_ok (unsigned int regno, machine_mode mode)
{
  int n = (GET_MODE_SIZE (mode) + 1) / 2;

  if (regno >= FRUCTUS_SFP)
    return mode == Pmode;
  if (n <= 1)
    return true;
  return regno >= FRUCTUS_R5 && regno + n - 1 <= FRUCTUS_R0;
}

static bool
fructus_modes_tieable_p (machine_mode m1, machine_mode m2)
{
  return m1 == m2
	 || (GET_MODE_SIZE (m1) <= 2
	     && GET_MODE_SIZE (m2) <= 2);
}

static unsigned HOST_WIDE_INT
fructus_shift_truncation_mask (machine_mode mode)
{
  return mode == HImode ? 15 : 0;
}

/* ==========================================================================
   Costs.  Rough, and in bytes more than in cycles, which on this bus are
   nearly the same thing.
   ========================================================================== */

static bool
fructus_rtx_costs (rtx x, machine_mode mode, int, int, int *total, bool)
{
  switch (GET_CODE (x))
    {
    case CONST_INT:
      *total = (fructus_imm10_p (INTVAL (x)) || fructus_mask5_p (INTVAL (x))
		? 0 : COSTS_N_INSNS (1));
      return true;
    case CONST:
    case LABEL_REF:
    case SYMBOL_REF:
      *total = COSTS_N_INSNS (1);
      return true;
    case CONST_DOUBLE:
      *total = COSTS_N_INSNS (2);
      return true;
    case MULT:
      *total = COSTS_N_INSNS (GET_MODE_SIZE (mode) > 2 ? 40 : 15);
      return true;
    case DIV: case UDIV: case MOD: case UMOD:
      *total = COSTS_N_INSNS (GET_MODE_SIZE (mode) > 2 ? 80 : 40);
      return true;
    default:
      return false;
    }
}

/* ==========================================================================
   Multiword shifts by a whole number of words

   Those are register moves and a fill, and GCC does not find them by itself
   for a four-word value: it expands a TWO-word shift inline and falls back to
   a libcall for anything wider.  That is how `(unsigned long long) q << 32'
   became a call to __ashldi3 - a variable 64-bit shift, with the constant 32
   pushed on the stack - at 76 cycles where two moves would do.

   Anything that is not a constant multiple of the word size still goes to the
   libcall, which the expander does by failing.
   ========================================================================== */

bool
fructus_expand_word_shift (rtx *operands, enum rtx_code code)
{
  machine_mode mode = GET_MODE (operands[0]);
  int words = GET_MODE_SIZE (mode) / UNITS_PER_WORD;

  if (!CONST_INT_P (operands[2]))
    return false;

  HOST_WIDE_INT n = INTVAL (operands[2]);
  if (n <= 0 || n % BITS_PER_WORD != 0)
    return false;

  int k = n / BITS_PER_WORD;
  if (k >= words)
    return false;		/* nothing of the value survives */

  rtx dst = operands[0], src = operands[1], fill = const0_rtx;

  if (code == ASHIFTRT)
    {
      /* The vacated words are all copies of the sign bit.  */
      rtx top = simplify_gen_subreg (HImode, src, mode,
				     (words - 1) * UNITS_PER_WORD);
      fill = gen_reg_rtx (HImode);
      emit_insn (gen_ashrhi3 (fill, force_reg (HImode, top), GEN_INT (15)));
    }

  /* Left shifts move words up, so they copy from the top down; right shifts
     move them down, so they copy from the bottom up.  Either way a word is
     read before anything overwrites it, which is what makes this safe when
     the destination is also the source.  */
  for (int i = 0; i < words; i++)
    {
      int to = code == ASHIFT ? words - 1 - i : i;
      int from = code == ASHIFT ? to - k : to + k;
      rtx d = simplify_gen_subreg (HImode, dst, mode, to * UNITS_PER_WORD);
      rtx s = (from >= 0 && from < words)
	      ? simplify_gen_subreg (HImode, src, mode,
				     from * UNITS_PER_WORD)
	      : fill;
      if (!d || !s)
	return false;
      emit_move_insn (d, s);
    }
  return true;
}

/* ==========================================================================
   Division

   libgcc's __udivmodhi4 returns BOTH results - the quotient in r0 and the
   remainder in r1, which is one 32-bit value under isa/abi.s.  Registering it
   as the udivmod libfunc lets the divmod pass turn `x / 10' and `x % 10',
   which is every digit of every number printed, into one call instead of two
   divisions.

   Only the unsigned one: there is no hand-written signed __divmodhi4, and
   without a libfunc the pass leaves signed division alone.
   ========================================================================== */

static void
fructus_init_libfuncs (void)
{
  set_optab_libfunc (udivmod_optab, HImode, "__udivmodhi4");
}

static void
fructus_expand_divmod_libfunc (rtx libfunc, machine_mode mode, rtx op0,
			       rtx op1, rtx *quot, rtx *rem)
{
  gcc_assert (mode == HImode);

  rtx pair = emit_library_call_value (libfunc, NULL_RTX, LCT_CONST, SImode,
				      op0, mode, op1, mode);

  /* THE QUOTIENT IS THE HIGH WORD, which is where this differs from every
     other port doing this: r0 holds the high half of a register pair, and the
     quotient is in r0.  */
  *quot = simplify_gen_subreg (mode, pair, SImode, GET_MODE_SIZE (mode));
  *rem = simplify_gen_subreg (mode, pair, SImode, 0);

  gcc_assert (*quot && *rem);
}

/* ==========================================================================
   Options
   ========================================================================== */

static void
fructus_option_override (void)
{
  init_machine_status = fructus_init_machine_status;

  /* sp-relative addressing is how every frame is reached; r4 as a frame
     pointer would only cost a register.  */
  if (!OPTION_SET_P (flag_omit_frame_pointer))
    flag_omit_frame_pointer = 1;
}

/* ==========================================================================
   The target structure
   ========================================================================== */

#undef  TARGET_OPTION_OVERRIDE
#define TARGET_OPTION_OVERRIDE fructus_option_override

#undef  TARGET_ASM_ALIGNED_HI_OP
#define TARGET_ASM_ALIGNED_HI_OP "\t.short\t"
#undef  TARGET_ASM_ALIGNED_SI_OP
#define TARGET_ASM_ALIGNED_SI_OP "\t.long\t"
#undef  TARGET_ASM_UNALIGNED_HI_OP
#define TARGET_ASM_UNALIGNED_HI_OP "\t.short\t"
#undef  TARGET_ASM_UNALIGNED_SI_OP
#define TARGET_ASM_UNALIGNED_SI_OP "\t.long\t"
#undef  TARGET_ASM_UNALIGNED_DI_OP
#define TARGET_ASM_UNALIGNED_DI_OP "\t.quad\t"

#undef  TARGET_PRINT_OPERAND
#define TARGET_PRINT_OPERAND fructus_print_operand
#undef  TARGET_PRINT_OPERAND_ADDRESS
#define TARGET_PRINT_OPERAND_ADDRESS fructus_print_operand_address

#undef  TARGET_LEGITIMATE_ADDRESS_P
#define TARGET_LEGITIMATE_ADDRESS_P fructus_legitimate_address_p

#undef  TARGET_HARD_REGNO_NREGS
#define TARGET_HARD_REGNO_NREGS fructus_hard_regno_nregs
#undef  TARGET_HARD_REGNO_MODE_OK
#define TARGET_HARD_REGNO_MODE_OK fructus_hard_regno_mode_ok
#undef  TARGET_MODES_TIEABLE_P
#define TARGET_MODES_TIEABLE_P fructus_modes_tieable_p

#undef  TARGET_FUNCTION_ARG
#define TARGET_FUNCTION_ARG fructus_function_arg
#undef  TARGET_FUNCTION_ARG_ADVANCE
#define TARGET_FUNCTION_ARG_ADVANCE fructus_function_arg_advance
#undef  TARGET_FUNCTION_VALUE
#define TARGET_FUNCTION_VALUE fructus_function_value
#undef  TARGET_LIBCALL_VALUE
#define TARGET_LIBCALL_VALUE fructus_libcall_value
#undef  TARGET_FUNCTION_VALUE_REGNO_P
#define TARGET_FUNCTION_VALUE_REGNO_P fructus_function_value_regno_p
#undef  TARGET_RETURN_IN_MEMORY
#define TARGET_RETURN_IN_MEMORY fructus_return_in_memory
#undef  TARGET_MUST_PASS_IN_STACK
#define TARGET_MUST_PASS_IN_STACK must_pass_in_stack_var_size_or_pad
#undef  TARGET_STRICT_ARGUMENT_NAMING
#define TARGET_STRICT_ARGUMENT_NAMING hook_bool_CUMULATIVE_ARGS_true

#undef  TARGET_FNTYPE_ABI
#define TARGET_FNTYPE_ABI fructus_fntype_abi
#undef  TARGET_INSN_CALLEE_ABI
#define TARGET_INSN_CALLEE_ABI fructus_insn_callee_abi
#undef  TARGET_FUNCTION_OK_FOR_SIBCALL
#define TARGET_FUNCTION_OK_FOR_SIBCALL fructus_function_ok_for_sibcall

#undef  TARGET_CAN_ELIMINATE
#define TARGET_CAN_ELIMINATE fructus_can_eliminate
#undef  TARGET_FRAME_POINTER_REQUIRED
#define TARGET_FRAME_POINTER_REQUIRED fructus_frame_pointer_required

#undef  TARGET_RTX_COSTS
#define TARGET_RTX_COSTS fructus_rtx_costs
#undef  TARGET_SHIFT_TRUNCATION_MASK
#define TARGET_SHIFT_TRUNCATION_MASK fructus_shift_truncation_mask

#undef  TARGET_INIT_LIBFUNCS
#define TARGET_INIT_LIBFUNCS fructus_init_libfuncs
#undef  TARGET_EXPAND_DIVMOD_LIBFUNC
#define TARGET_EXPAND_DIVMOD_LIBFUNC fructus_expand_divmod_libfunc

struct gcc_target targetm = TARGET_INITIALIZER;

#include "gt-fructus.h"
