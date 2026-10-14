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
#include "langhooks.h"
#include "function-abi.h"
#include "insn-config.h"
#include "recog.h"
#include "insn-attr.h"
#include "optabs.h"
#include "optabs-libfuncs.h"
#include "explow.h"
#include "predict.h"
#include "opts.h"
#include "tm-constrs.h"
#include "gimple.h"
#include "gimple-iterator.h"
#include "gimple-walk.h"
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
  int width = 16;
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

/* The truth set of `(x & M) == 0', or of `!= 0' if SET, when it is an
   interval.  The x that fit inside ~M form one only when ~M is a run of low
   ones: [0, 2^n) for the clear test and the rest of the circle for the set.  */

static bool
fructus_mask_interval (HOST_WIDE_INT m, bool set, HOST_WIDE_INT *start,
		       HOST_WIDE_INT *len)
{
  const HOST_WIDE_INT u = ~m & 0xffff, run = u + 1;

  if (run & u)
    return false;
  if (!set)
    *start = 0, *len = run;
  else
    *start = run & 0xffff, *len = 0x10000 - run;
  if (*len == 0)
    *start = 0;
  return true;
}

/* brclr and brset ask whether %0 & mask is zero, and when the mask is a run
   of high ones that is a comparison: `x <u 256' is `brclr #0xff00', `x < 0'
   is `brset #0x8000'.  condimm5 holds no such twin - check.js refuses one -
   so a comparison it lacks may still be a single mask branch, and a mask
   neither table holds may still be a condimm5 entry.  Both are found by
   comparing truth sets, the way gas finds them (tools/brequiv.js).  */

/* The encodable mask whose test, clear or SET, is CODE against K; 0 if none.
   0 is never a mask anyone branches on.  */

static HOST_WIDE_INT
fructus_cond_mask (enum rtx_code code, HOST_WIDE_INT k, bool *set)
{
  HOST_WIDE_INT s0, l0, s1, l1;

  if (!fructus_truth_set (code, 16, k, &s0, &l0))
    return 0;
  for (int pass = 0; pass < 2; pass++)
    {
      const int *tab = pass ? fructus_immask5 : fructus_immbit5;
      for (unsigned i = 0; i < 32; i++)
	for (int sense = 0; sense < 2; sense++)
	  if (fructus_mask_interval (tab[i], sense, &s1, &l1)
	      && s0 == s1 && l0 == l1)
	    {
	      *set = sense;
	      return tab[i] & 0xffff;
	    }
    }
  return 0;
}

/* The condimm5 entry whose test is `(x & M) != 0' if SET, `== 0' if not;
   -1 if none.  */

static int
fructus_mask_condimm (HOST_WIDE_INT m, bool set)
{
  HOST_WIDE_INT s0, l0, s1, l1;

  if (!fructus_mask_interval (m, set, &s0, &l0))
    return -1;
  for (unsigned i = 0; i < ARRAY_SIZE (fructus_condimm5); i++)
    if (fructus_truth_set (fructus_cond3_code (fructus_condimm5[i].cond),
			   16, fructus_condimm5[i].imm, &s1, &l1)
	&& s0 == s1 && l0 == l1)
      return i;
  return -1;
}

bool
fructus_cbranch_imm_p (enum rtx_code code, machine_mode mode, HOST_WIDE_INT k)
{
  bool set;
  return (fructus_condimm_index (code, mode, k) >= 0
	  || fructus_cond_mask (code, k, &set) != 0);
}

/* Whether a brclr or brset can test M, in both senses - the inverted branch
   a long jump needs is the other one.  Either from a mask table or as the
   condimm5 entry that asks the same.  */

bool
fructus_brmask_p (HOST_WIDE_INT m)
{
  return (fructus_mask5_p (m)
	  || (fructus_mask_condimm (m & 0xffff, false) >= 0
	      && fructus_mask_condimm (m & 0xffff, true) >= 0));
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

/* A compare-and-branch testing CODE, as assembler text with %0 and %1 left
   for output_asm_insn and the target to follow.  False when no single
   instruction tests it.  The text names the instruction, which is a mask
   branch when the comparison is its twin.  */

static bool
fructus_cond_text (char *buf, size_t size, enum rtx_code code,
		   machine_mode mode, rtx op)
{
  if (!CONST_INT_P (op))
    {
      snprintf (buf, size, "br\t%s, %%0, %%1", fructus_cond_name (code));
      return true;
    }
  int i = fructus_condimm_index (code, mode, INTVAL (op));
  if (i >= 0)
    {
      snprintf (buf, size, "br\t%s, %%0, #%d", fructus_condimm5[i].cond,
		fructus_condimm5[i].imm);
      return true;
    }
  bool set;
  HOST_WIDE_INT m = fructus_cond_mask (code, INTVAL (op), &set);
  if (m == 0)
    return false;
  snprintf (buf, size, "%s\t%%0, #0x%04x", set ? "brset" : "brclr",
	    (unsigned) m);
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
  enum rtx_code cc = invert ? reverse_condition (code) : code;
  bool c_ok = fructus_cond_text (c, sizeof c, cc, mode, operands[1]);
  bool nc_ok = fructus_cond_text (nc, sizeof nc, reverse_condition (cc), mode,
				  operands[1]);

  if (c_ok && length == 3)
    snprintf (buf, sizeof buf, "%s, %%l2", c);
  else if (nc_ok)
    snprintf (buf, sizeof buf, "%s, .Lb%%=\n\tjmp\t%%l2\n.Lb%%=:", nc);
  else
    {
      gcc_assert (c_ok);
      snprintf (buf, sizeof buf,
		"%s, .Lt%%=\n\tjmpr\t.Lf%%=\n.Lt%%=:\n\tjmp\t%%l2\n.Lf%%=:",
		c);
    }
  return buf;
}

/* brset / brclr: taken when %0 & %1 is non-zero if SET, zero otherwise.
   A mask neither table holds is the condimm5 entry that asks the same -
   fructus_brmask_p let it in only if one does, in both senses.  */

const char *
fructus_output_bitbranch (rtx *operands, bool set, int length)
{
  static char buf[96];
  HOST_WIDE_INT m = INTVAL (operands[1]) & 0xffff;
  bool s = length == 3 ? set : !set;
  char test[40];

  if (fructus_mask5_p (m))
    snprintf (test, sizeof test, "%s\t%%0, #%%1", s ? "brset" : "brclr");
  else
    {
      int i = fructus_mask_condimm (m, s);
      gcc_assert (i >= 0);
      snprintf (test, sizeof test, "br\t%s, %%0, #%d",
		fructus_condimm5[i].cond, fructus_condimm5[i].imm);
    }
  if (length == 3)
    snprintf (buf, sizeof buf, "%s, %%l2", test);
  else
    snprintf (buf, sizeof buf, "%s, .Lb%%=\n\tjmp\t%%l2\n.Lb%%=:", test);
  return buf;
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

  /* Indexed: two registers, two bytes, and no displacement to weigh.  */
  if (GET_CODE (addr) == PLUS
      && REG_P (XEXP (addr, 0)) && REG_P (XEXP (addr, 1)))
    return 2;

  /* The zero page: always the imm10 form, the only one with a relocation.  */
  if (GET_CODE (addr) == PLUS && fructus_zp_offset_p (XEXP (addr, 1)))
    return 3;

  if (GET_CODE (addr) == PLUS && CONST_INT_P (XEXP (addr, 1)))
    base = XEXP (addr, 0), off = INTVAL (XEXP (addr, 1));

  if (load && off == 0 && fructus_reg_is (reg, 0) && fructus_reg_is (base, 0))
    return 1;
  /* ld r0, [sp, #0] and st r0, [sp, #0]: the first argument or the return
     value spilled to the top of the stack and back.  */
  if (off == 0 && fructus_reg_is (reg, 0) && fructus_reg_is (base, FRUCTUS_SP))
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
  /* mov r0, r1, mov r1, r0 and mov r2, r0 have one-byte encodings.  */
  if (REG_P (s))
    return ((fructus_reg_is (d, 0) && fructus_reg_is (s, 1))
	    || (fructus_reg_is (d, 1) && fructus_reg_is (s, 0))
	    || (fructus_reg_is (d, 2) && fructus_reg_is (s, 0))) ? 1 : 2;
  if (CONST_INT_P (s))
    {
      HOST_WIDE_INT v = INTVAL (s);
      /* So do mov rN, #0 for r0 to r3, and mov r0, #1.  */
      if (v == 0 && REG_P (d) && REGNO (d) <= (unsigned) FRUCTUS_REGNO (3))
	return 1;
      if (v == 1 && fructus_reg_is (d, 0))
	return 1;
      if (fructus_imm5_p (v) || fructus_mask5_p (v))
	return 2;
    }
  return 3;
}

/* push r0 and push lr, pop r2 and pop lr have one-byte encodings.  */

int
fructus_push_length (rtx reg)
{
  return fructus_reg_is (reg, 0) || fructus_reg_is (reg, FRUCTUS_LR) ? 1 : 2;
}

int
fructus_pop_length (rtx reg)
{
  return fructus_reg_is (reg, 2) || fructus_reg_is (reg, FRUCTUS_LR) ? 1 : 2;
}

/* jmp r5 and call r5 are one byte; any other register is two.  */

int
fructus_indirect_length (rtx reg)
{
  return fructus_reg_is (reg, FRUCTUS_R5) ? 1 : 2;
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

  if (fructus_zp_offset_p (b))
    return 3;

  if (!CONST_INT_P (b))
    {
      /* add r0, r0, r1 and add r1, r1, r0 have one-byte encodings, and
	 fructus_output_alu puts a commutative pair in whichever order
	 reaches them.  It prints add r0, r0, r0 as shl r0, r0, #1, which
	 is one byte too.  */
      if (code == PLUS && fructus_reg_is (d, 0)
	  && fructus_reg_is (a, 0) && fructus_reg_is (b, 0))
	return 1;
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
    case ASHIFT:
      /* shl r0, r0, #1 is one byte.  */
      return tied && fructus_reg_is (d, 0) && v == 1 ? 1 : 2;
    case ASHIFTRT: case LSHIFTRT:
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

/* ishi is two bytes; the inclusive comparisons add the xor that inverts it,
   one byte into r0 and two otherwise, as fructus_iseq_length's != does.  */

int
fructus_ishi_length (rtx *operands, bool inv)
{
  return 2 + (!inv ? 0 : fructus_reg_is (operands[0], 0) ? 1 : 2);
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

  if (CONST_INT_P (operands[2]) || fructus_zp_offset_p (operands[2]))
    {
      snprintf (buf, sizeof buf, "%s\t%%0, %%1, #%%2", mn);
      return buf;
    }

  /* A doubling is a shift by one, which costs the same and has a one-byte
     encoding for r0 that add r0, r0, r0 does not.  */
  if (code == PLUS && fructus_same_reg_p (operands[0], operands[1])
      && fructus_same_reg_p (operands[0], operands[2]))
    return "shl\t%0, %0, #1";

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

/* Branch to LABEL when A - B overflows: br vs.  */

static void
fructus_emit_branch_vs (rtx a, rtx b, rtx label)
{
  rtx test = gen_rtx_NE (VOIDmode,
			 gen_rtx_UNSPEC (HImode, gen_rtvec (2, a, b),
					 UNSPEC_SUBV),
			 const0_rtx);
  rtx_insn *jump
    = emit_jump_insn (gen_rtx_SET (pc_rtx,
				   gen_rtx_IF_THEN_ELSE (VOIDmode, test,
							 gen_rtx_LABEL_REF
							   (VOIDmode, label),
							 pc_rtx)));
  JUMP_LABEL (jump) = label;
  LABEL_NUSES (label)++;
}

/* addv, subv and negv: %0 = A CODE B, branching to LABEL if that overflows.
   NEG takes no B.

   br vs is taken when its first operand minus its second overflows, and it
   is asked of the result and one operand, never of the two operands:

     a + b    s - b is a when the sum fitted and a -/+ 65536 when it
	      wrapped, so br vs, s, b
     a - b    a - s is b when the difference fitted and b +/- 65536 when
	      it wrapped, so br vs, a, s

   That way the other operand is dead once s is made, and only two values
   are live across the branch.  Testing a - b on a and b would need s as
   well, since gcc's if-conversion moves the arithmetic above the branch
   whatever order it is emitted in; in a function that keeps its arguments
   for a slow path, that third value takes lr, which is then saved and
   restored for nothing.

   Against a constant k the test is a plain comparison of the operand,
   a + k overflowing exactly when a > 32767 - k (or a < -32768 - k for a
   negative k), and condimm5 holds the four that x++, x += 2, x-- and x -= 2
   produce.  -a overflows only for -32768, which is lt #-32767.

   The branch is emitted last, so that the expander's caller finds it and
   marks it unlikely, and on copies of any operand the store of the result
   would overwrite.  */

void
fructus_expand_addsubv (rtx *operands, enum rtx_code code)
{
  rtx dst = operands[0], a = operands[1];
  rtx b = code == NEG ? NULL_RTX : operands[2];
  rtx label = operands[code == NEG ? 2 : 3];
  rtx t = gen_reg_rtx (HImode);

  if (!register_operand (a, HImode))
    a = force_reg (HImode, a);

  if (code == NEG || CONST_INT_P (b))
    {
      enum rtx_code cmp;
      HOST_WIDE_INT lim;

      if (code == NEG)
	{
	  emit_insn (gen_neghi2 (t, a));
	  cmp = LT, lim = -32767;
	}
      else
	{
	  HOST_WIDE_INT k = INTVAL (b);
	  rtx s = expand_simple_binop (HImode, code, a, b, t, 0,
				       OPTAB_DIRECT);
	  if (s != t)
	    emit_move_insn (t, s);
	  if (code == MINUS && k == -32768)
	    cmp = GE, lim = 0;
	  else
	    {
	      k = code == MINUS ? -k : k;
	      if (k == 0)
		{
		  emit_move_insn (dst, t);
		  return;
		}
	      cmp = k > 0 ? GT : LT;
	      lim = k > 0 ? 32767 - k : -32768 - k;
	    }
	}
      if (reg_overlap_mentioned_p (dst, a))
	a = copy_to_reg (a);
      emit_move_insn (dst, t);
      emit_cmp_and_jump_insns (a, gen_int_mode (lim, HImode), cmp, NULL_RTX,
			       HImode, 0, as_a <rtx_code_label *> (label));
      return;
    }

  if (!register_operand (b, HImode))
    b = force_reg (HImode, b);
  rtx x, y;
  if (code == PLUS)
    {
      emit_insn (gen_addhi3 (t, a, b));
      x = t, y = b;
    }
  else
    {
      emit_insn (gen_subhi3 (t, a, b));
      x = a, y = t;
    }
  if (reg_overlap_mentioned_p (dst, x))
    x = copy_to_reg (x);
  if (reg_overlap_mentioned_p (dst, y))
    y = copy_to_reg (y);
  emit_move_insn (dst, t);
  fructus_emit_branch_vs (x, y, label);
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

  /* The zero page: whether this function reaches it through a base
     register, decided before expansion, and that register once made.  */
  bool zp_base_p;
  rtx zp_base;
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
  /* Rounded to a word, so that sp stays even: STACK_BOUNDARY.  The saved
     registers are words already.  */
  m->frame_size = ROUND_UP (get_frame_size (), STACK_BOUNDARY / BITS_PER_UNIT);
}

/* PUSH_ROUNDING: a push is a whole number of words.  See fructus.h.  */

poly_int64
fructus_push_rounding (poly_int64 bytes)
{
  return (bytes + 1) & ~1;
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
   r5, which no argument or return value ever occupies.

   UNLESS KEEP_R5, which the epilogue of an indirect tail call sets: its
   target is already waiting in r5 for the `jmp r5' after the epilogue.  Then
   a large N is a run of adds that each fit.  Only a frame over 511 bytes
   pays for it, and only in that one epilogue.  */

static void
fructus_emit_sp_add (HOST_WIDE_INT n, bool keep_r5 = false)
{
  if (n == 0)
    return;
  rtx v = gen_int_mode (n, HImode);
  if (!fructus_alu_imm_p (PLUS, INTVAL (v), true) && keep_r5)
    {
      gcc_assert (n > 0);
      for (; n > 511; n -= 511)
	emit_insn (gen_addhi3 (stack_pointer_rtx, stack_pointer_rtx,
			       GEN_INT (511)));
      v = GEN_INT (n);
    }
  else if (!fructus_alu_imm_p (PLUS, INTVAL (v), true))
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
  fructus_emit_sp_add (m->frame_size, sibcall);

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
   registers, low half first; no pair alignment, no splitting, no
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

/* The GCC regno of a value of N registers starting at machine register rP.
   The low word is in rP and the high one in rP+N-1, which is the order GCC
   wants a multi-register value in, so N does not matter.  */

static unsigned
fructus_arg_regno (int p, int n ATTRIBUTE_UNUSED)
{
  return FRUCTUS_REGNO (p);
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

  /* An indirect tail call stages its target in r5, which is also the static
     chain register, and one register cannot carry both.  */
  if (decl == NULL_TREE && exp != NULL_TREE && CALL_EXPR_STATIC_CHAIN (exp))
    return false;

  return hard_reg_set_subset_p (fructus_fntype_abi (fntype).full_reg_clobbers (),
				crtl->abi->full_reg_clobbers ());
}

/* The return value: r0, r0:r1 or r0:r1:r2:r3, low to high - and for an
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
  return regno <= FRUCTUS_R3;
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

/* ==========================================================================
   The zero page

   0x0000-0x03ff, reached as [rb, #imm10] with rb holding 0x200: imm10 is
   signed, so the base in the middle reaches the whole kilobyte.  A variable
   goes there by attribute,

       int count __attribute__ ((zp));          zp_lo, 0x0000-0x01ff
       int flags __attribute__ ((zp ("hi")));   zp_hi, 0x0200-0x03ff

   in sections .zplo or .zphi, or .zplo.bss and .zphi.bss when it starts at
   zero, which the linker scripts place.  Its address is still an ordinary
   one, and `mov rd, #x' then `ld rd, [rd, #0]' still reaches it.  What the
   attribute adds is the other way:

       mov  r4, #0x200                 2 bytes, once
       ld   r0, [r4, #zp(count)]       3 bytes, R_FRUCTUS_ZP10
       st   r1, [r4, #zp(flags)]

   The offset is the linker's, so the form is always the three-byte imm10
   one - nothing shorter can carry the relocation.

   WHICH IS SMALLER DEPENDS ON THE FUNCTION.  A direct access to d distinct
   variables making r references costs 3d + 2r bytes, if each address is
   kept in a register; through the base it is 2 + 3r, in one register.  So
   the base wins with several variables and loses with one variable used
   often.  fructus_expand_to_rtl_hook counts both in the GIMPLE and decides
   for the whole function - -mzp-base=auto, the default; =always and =never
   override it.  Within the function the base is one pseudo set once at the
   top, so the register allocator makes the remaining choice: keep it in a
   register, or rematerialize the two-byte mov where it is needed.
   ========================================================================== */

#define FRUCTUS_ZP_BASE 0x200
#define SYMBOL_FLAG_ZP SYMBOL_FLAG_MACH_DEP

/* Which half of the zero page DECL's attribute asks for, or NULL.  */

static const char *
fructus_zp_half (const_tree decl)
{
  if (!VAR_P (decl))
    return NULL;
  tree a = lookup_attribute ("zp", DECL_ATTRIBUTES (decl));
  if (!a)
    return NULL;
  tree args = TREE_VALUE (a);
  if (args && TREE_CODE (TREE_VALUE (args)) == STRING_CST
      && strcmp (TREE_STRING_POINTER (TREE_VALUE (args)), "hi") == 0)
    return "hi";
  return "lo";
}

/* Whether DECL lives in the zero page: by attribute, or put in one of its
   sections by hand.  */

static bool
fructus_zp_decl_p (const_tree decl)
{
  if (!VAR_P (decl) || (!TREE_STATIC (decl) && !DECL_EXTERNAL (decl)))
    return false;
  if (fructus_zp_half (decl))
    return true;
  const char *sec = DECL_SECTION_NAME (decl);
  return sec && (startswith (sec, ".zplo") || startswith (sec, ".zphi"));
}

static tree
fructus_handle_zp_attribute (tree *node, tree name, tree args, int,
			     bool *no_add_attrs)
{
  tree decl = *node;

  if (!VAR_P (decl) || (!TREE_STATIC (decl) && !DECL_EXTERNAL (decl)))
    {
      warning (OPT_Wattributes,
	       "%qE attribute applies only to variables with static storage",
	       name);
      *no_add_attrs = true;
      return NULL_TREE;
    }
  if (args)
    {
      tree a = TREE_VALUE (args);
      if (TREE_CODE (a) != STRING_CST
	  || (strcmp (TREE_STRING_POINTER (a), "lo") != 0
	      && strcmp (TREE_STRING_POINTER (a), "hi") != 0))
	{
	  error ("argument of %qE attribute must be %qs or %qs",
		 name, "lo", "hi");
	  *no_add_attrs = true;
	  return NULL_TREE;
	}
    }
  if (DECL_SECTION_NAME (decl))
    {
      error ("%qE attribute conflicts with a section attribute", name);
      *no_add_attrs = true;
      return NULL_TREE;
    }
  /* A common symbol is placed by the linker in .bss, out of reach.  */
  DECL_COMMON (decl) = 0;
  return NULL_TREE;
}

TARGET_GNU_ATTRIBUTES (fructus_attribute_table,
  {
    /* { name, min_num_args, max_num_args, decl_req, type_req, fn_type_req,
	 affects_type_identity, handler, exclude } */
    { "zp", 0, 1, true, false, false, false, fructus_handle_zp_attribute,
      NULL }
  });

/* The section is chosen here and not by select_section, because varasm
   sends a zero-initialised static to .lcomm and -fdata-sections to a section
   of its own before select_section is asked.  A named section bypasses both.
   By now the initializer is known, so .bss or not can be decided.  */

/* A LIBCALL'S ABI IS ITS COOKIE'S, and not its decl's.  optabs-libfuncs.cc
   gives every libfunc the decl of `int f ()', with no prototype, and under
   -fipa-ra insn_callee_abi asks a call's decl before it asks
   fructus_insn_callee_abi.  No prototype is the widest convention, so every
   `a * b' and `a / b' made the function around it save r2 and r3 - ten
   cycles of push and pop in a two-argument function, for registers that
   __mulhi3 and __udivhi3 never touch.  Without the decl, the call is asked
   by its UNSPEC_CALLEE_CC, which the libcall's own argument modes set.  */

static bool
fructus_libfunc_decl_p (tree decl)
{
  return (TREE_CODE (decl) == FUNCTION_DECL && DECL_ARTIFICIAL (decl)
	  && DECL_EXTERNAL (decl)
	  && DECL_SOURCE_LOCATION (decl) == UNKNOWN_LOCATION
	  && !prototype_p (TREE_TYPE (decl)));
}

static void
fructus_encode_section_info (tree decl, rtx rtl, int first)
{
  default_encode_section_info (decl, rtl, first);

  if (first && fructus_libfunc_decl_p (decl)
      && MEM_P (rtl) && GET_CODE (XEXP (rtl, 0)) == SYMBOL_REF)
    SET_SYMBOL_REF_DECL (XEXP (rtl, 0), NULL_TREE);

  if (!VAR_P (decl) || !MEM_P (rtl) || GET_CODE (XEXP (rtl, 0)) != SYMBOL_REF)
    return;

  const char *half = fructus_zp_half (decl);
  if (half && first && !DECL_EXTERNAL (decl) && !DECL_SECTION_NAME (decl))
    set_decl_section_name (decl, (bss_initializer_p (decl)
				  ? (*half == 'h' ? ".zphi.bss" : ".zplo.bss")
				  : (*half == 'h' ? ".zphi" : ".zplo")));
  if (fructus_zp_decl_p (decl))
    SYMBOL_REF_FLAGS (XEXP (rtl, 0)) |= SYMBOL_FLAG_ZP;
}

static unsigned int
fructus_section_type_flags (tree decl, const char *name, int reloc)
{
  unsigned int flags = default_section_type_flags (decl, name, reloc);
  /* NOTYPE is the default's for a name it does not know, and would leave
     the section @progbits.  */
  if (startswith (name, ".zplo.bss") || startswith (name, ".zphi.bss"))
    flags = (flags & ~SECTION_NOTYPE) | SECTION_BSS;
  return flags;
}

/* Whether X is the address of zero page data - a flagged SYMBOL_REF, or one
   plus a constant - and if so, which and how far into it.  */

static bool
fructus_zp_symbolic_p (rtx x, rtx *sym, HOST_WIDE_INT *off)
{
  *off = 0;
  if (GET_CODE (x) == CONST)
    {
      x = XEXP (x, 0);
      if (GET_CODE (x) != PLUS || !CONST_INT_P (XEXP (x, 1)))
	return false;
      *off = INTVAL (XEXP (x, 1));
      x = XEXP (x, 0);
    }
  if (GET_CODE (x) != SYMBOL_REF || !(SYMBOL_REF_FLAGS (x) & SYMBOL_FLAG_ZP))
    return false;
  *sym = x;
  return true;
}

bool
fructus_zp_address_p (rtx x)
{
  rtx sym;
  HOST_WIDE_INT off;
  return fructus_zp_symbolic_p (x, &sym, &off);
}

/* The displacement of a zero page address from the base, as an operand:
   (const (unspec [sym] UNSPEC_ZP)), with (plus ... (const_int OFF)) inside
   the const when OFF is not zero.  It prints as `zp(sym+OFF)'.  */

static rtx
fructus_zp_offset (rtx sym, HOST_WIDE_INT off)
{
  rtx u = gen_rtx_UNSPEC (Pmode, gen_rtvec (1, sym), UNSPEC_ZP);
  if (off)
    u = gen_rtx_PLUS (Pmode, u, GEN_INT (off));
  return gen_rtx_CONST (Pmode, u);
}

static bool
fructus_zp_offset_split (rtx x, rtx *sym, HOST_WIDE_INT *off)
{
  *off = 0;
  if (GET_CODE (x) != CONST)
    return false;
  x = XEXP (x, 0);
  if (GET_CODE (x) == PLUS && CONST_INT_P (XEXP (x, 1)))
    {
      *off = INTVAL (XEXP (x, 1));
      x = XEXP (x, 0);
    }
  if (GET_CODE (x) != UNSPEC || XINT (x, 1) != UNSPEC_ZP)
    return false;
  *sym = XVECEXP (x, 0, 0);
  return true;
}

bool
fructus_zp_offset_p (rtx x)
{
  rtx sym;
  HOST_WIDE_INT off;
  return fructus_zp_offset_split (x, &sym, &off);
}

/* Whether zp(SYM+K) is sure to fit its field: S + K - 0x200 must be in
   -512..511, and only an address inside the object is sure to be in the
   zero page.  Without this, combine folds the base's value into the
   offset - [ri, #zp(a+512)] for a[i] - which is the right address and fits
   only if `a' is below 0x200.  */

static bool
fructus_zp_offset_in_object (rtx sym, HOST_WIDE_INT k)
{
  tree decl = SYMBOL_REF_DECL (sym);
  if (k == 0)
    return true;
  if (!decl || !DECL_SIZE_UNIT (decl) || !tree_fits_shwi_p (DECL_SIZE_UNIT (decl)))
    return false;
  return k > 0 && k < tree_to_shwi (DECL_SIZE_UNIT (decl));
}

static void
fructus_print_zp_offset (FILE *file, rtx x)
{
  rtx sym;
  HOST_WIDE_INT off;
  if (!fructus_zp_offset_split (x, &sym, &off))
    gcc_unreachable ();
  fputs ("zp(", file);
  output_addr_const (file, sym);
  if (off)
    fprintf (file, "%+" HOST_WIDE_INT_PRINT "d", off);
  fputc (')', file);
}

/* The base register, made the first time a function needs it, with its one
   set at the top of the function - after parm_birth_insn, in the topmost
   sequence whatever sequence the caller is in.  */

static rtx
fructus_zp_base_reg (void)
{
  machine_function *m = cfun->machine;
  if (!m->zp_base)
    {
      m->zp_base = gen_reg_rtx (Pmode);
      start_sequence ();
      emit_move_insn (m->zp_base, GEN_INT (FRUCTUS_ZP_BASE));
      rtx_insn *seq = end_sequence ();
      push_topmost_sequence ();
      emit_insn_after (seq, parm_birth_insn);
      pop_topmost_sequence ();
    }
  return m->zp_base;
}

/* Flatten a sum into its zero page part - a symbol, which still needs the
   base added, or an offset from the base, whose base is among the other
   terms - its constant part, and everything else.  At most one zero page
   part, and at most four other terms, or false.  */

struct fructus_zp_sum
{
  rtx sym;
  bool need_base;
  HOST_WIDE_INT off;
  rtx terms[4];
  int nterms;
};

static bool
fructus_zp_flatten (rtx x, fructus_zp_sum *z)
{
  rtx sym;
  HOST_WIDE_INT off;

  if (GET_CODE (x) == PLUS)
    return (fructus_zp_flatten (XEXP (x, 0), z)
	    && fructus_zp_flatten (XEXP (x, 1), z));
  if (CONST_INT_P (x))
    {
      z->off += INTVAL (x);
      return true;
    }
  bool symbolic = fructus_zp_symbolic_p (x, &sym, &off);
  if (symbolic || fructus_zp_offset_split (x, &sym, &off))
    {
      if (z->sym)
	return false;
      z->sym = sym;
      z->need_base = symbolic;
      z->off += off;
      return true;
    }
  if (z->nterms == 4)
    return false;
  z->terms[z->nterms++] = x;
  return true;
}

/* A zero page address is not a CONSTANT_ADDRESS_P (see fructus.h), so
   memory_address brings it here rather than straight to force_reg.  Outside
   expansion, or in a function that decided against the base, it goes to a
   register as any other constant address would.

   With the base, a zero page address with a constant offset becomes
   [rb, #zp(x+k)].  One with an index does not: `mov rt, #a; ld rd, [rt,
   ri]' is five bytes and needs no base, where `add rt, ri, rb; ld rd, [rt,
   #zp(a)]' is five as well and does.  So an index turns the address back
   into a direct one - including one that was [rb, #zp(a)] before the index
   was added to it, whose base is then among the terms and is dropped.  */

static rtx
fructus_legitimize_address (rtx x, rtx, machine_mode)
{
  rtx sym;
  HOST_WIDE_INT off;
  bool base = (currently_expanding_to_rtl && cfun && cfun->machine->zp_base_p
	       && can_create_pseudo_p ());

  if (!base)
    return fructus_zp_symbolic_p (x, &sym, &off) ? force_reg (Pmode, x) : x;

  fructus_zp_sum z = {};
  if (!fructus_zp_flatten (x, &z) || !z.sym)
    return x;

  /* An offset from the base has the base among its terms; it is accounted
     for by the offset's bias, so it goes.  If it cannot be found the sum is
     left alone.  */
  if (!z.need_base)
    {
      int i;
      for (i = 0; i < z.nterms && z.terms[i] != cfun->machine->zp_base; i++)
	;
      if (i == z.nterms)
	return x;
      z.terms[i] = z.terms[--z.nterms];
    }

  rtx direct = plus_constant (Pmode, z.sym, z.off);

  /* A constant offset: [rb, #zp(x+k)], or, with k outside the object, the
     address in a register.  */
  if (z.nterms == 0)
    return (fructus_zp_offset_in_object (z.sym, z.off)
	    ? gen_rtx_PLUS (Pmode, fructus_zp_base_reg (),
			    fructus_zp_offset (z.sym, z.off))
	    : force_reg (Pmode, direct));

  /* An index: [rt, ri] with rt the address, the other terms added to ri.  */
  rtx r = force_operand (z.terms[0], NULL_RTX);
  for (int i = 1; i < z.nterms; i++)
    r = expand_simple_binop (Pmode, PLUS, r,
			     force_operand (z.terms[i], NULL_RTX),
			     NULL_RTX, 0, OPTAB_LIB_WIDEN);
  return gen_rtx_PLUS (Pmode, force_reg (Pmode, direct), force_reg (Pmode, r));
}

/* The zero page decision.  Every memory reference to zero page data counts
   - a VAR_DECL, or a MEM_REF through its address - and an address taken
   as a value does not, since the base cannot help with that.  */

struct fructus_zp_count
{
  hash_set<tree> *seen;
  int refs;
};

static void
fructus_zp_note (fructus_zp_count *c, tree t)
{
  if (t && fructus_zp_decl_p (t))
    {
      c->seen->add (t);
      c->refs++;
    }
}

static tree
fructus_zp_walk (tree *tp, int *walk_subtrees, void *data)
{
  fructus_zp_count *c
    = (fructus_zp_count *) ((struct walk_stmt_info *) data)->info;
  tree t = *tp;

  if (TYPE_P (t))
    *walk_subtrees = 0;
  else if (TREE_CODE (t) == ADDR_EXPR)
    *walk_subtrees = 0;
  else if ((TREE_CODE (t) == ARRAY_REF || TREE_CODE (t) == ARRAY_RANGE_REF)
	   && TREE_CODE (TREE_OPERAND (t, 1)) != INTEGER_CST)
    {
      /* An element at a variable index is reached [rt, ri] either way, so
	 it counts for neither; its index might itself be zero page data.  */
      walk_tree (&TREE_OPERAND (t, 1), fructus_zp_walk, data, NULL);
      tree base = TREE_OPERAND (t, 0);
      while (handled_component_p (base))
	base = TREE_OPERAND (base, 0);
      if (!VAR_P (base))
	walk_tree (&TREE_OPERAND (t, 0), fructus_zp_walk, data, NULL);
      *walk_subtrees = 0;
    }
  else if (TREE_CODE (t) == TARGET_MEM_REF && TMR_INDEX (t))
    *walk_subtrees = 0;
  else if ((TREE_CODE (t) == MEM_REF || TREE_CODE (t) == TARGET_MEM_REF)
	   && TREE_CODE (TREE_OPERAND (t, 0)) == ADDR_EXPR)
    {
      fructus_zp_note (c, get_base_address (TREE_OPERAND
					    (TREE_OPERAND (t, 0), 0)));
      *walk_subtrees = 0;
    }
  else if (VAR_P (t))
    fructus_zp_note (c, t);
  return NULL_TREE;
}

static void
fructus_expand_to_rtl_hook (void)
{
  machine_function *m = cfun->machine;
  m->zp_base = NULL_RTX;
  m->zp_base_p = fructus_zp_base == 2;
  if (fructus_zp_base != 1)
    return;

  hash_set<tree> seen;
  fructus_zp_count c = { &seen, 0 };
  basic_block bb;
  FOR_EACH_BB_FN (bb, cfun)
    for (gimple_stmt_iterator gsi = gsi_start_bb (bb); !gsi_end_p (gsi);
	 gsi_next (&gsi))
      {
	if (is_gimple_debug (gsi_stmt (gsi)))
	  continue;
	struct walk_stmt_info wi;
	memset (&wi, 0, sizeof wi);
	wi.info = &c;
	walk_gimple_op (gsi_stmt (gsi), fructus_zp_walk, &wi);
      }

  /* Through the base: 2 + 3r.  Direct: 3d + 2r.  A tie goes to the base,
     which holds one register where direct access holds d.  */
  int d = seen.elements ();
  m->zp_base_p = d >= 2 && 2 + 3 * c.refs <= 3 * d + 2 * c.refs;
}

/* For debug information: [rb, #zp(x+n)] is x + n.  */

static rtx
fructus_delegitimize_address (rtx x)
{
  rtx orig = delegitimize_mem_from_attrs (x);
  rtx a = MEM_P (orig) ? XEXP (orig, 0) : orig, sym;
  HOST_WIDE_INT off;

  if (GET_CODE (a) == PLUS && REG_P (XEXP (a, 0))
      && fructus_zp_offset_split (XEXP (a, 1), &sym, &off))
    a = plus_constant (Pmode, sym, off);
  else if (fructus_zp_offset_split (a, &sym, &off))
    a = plus_constant (Pmode, sym, off - FRUCTUS_ZP_BASE);
  else
    return orig;
  return MEM_P (orig) ? replace_equiv_address_nv (orig, a) : a;
}

static bool
fructus_output_addr_const_extra (FILE *file, rtx x)
{
  if (GET_CODE (x) == UNSPEC && XINT (x, 1) == UNSPEC_ZP)
    {
      fputs ("zp(", file);
      output_addr_const (file, XVECEXP (x, 0, 0));
      fputc (')', file);
      return true;
    }
  return false;
}

/* A zero page offset is an operand of an address or of an add, and never a
   value on its own: no mov can carry it.  */

static bool
fructus_legitimate_constant_p (machine_mode, rtx x)
{
  return !fructus_zp_offset_p (x);
}

static bool
fructus_cannot_force_const_mem (machine_mode, rtx x)
{
  return fructus_zp_offset_p (x);
}

static bool
fructus_legitimate_address_p (machine_mode mode, rtx x, bool strict,
			      code_helper = ERROR_MARK)
{
  if (fructus_base_reg_ok_p (x, strict))
    return true;

  /* The zero page: [rb, #zp(x)], whatever the mode.  The linker checks the
     range, including a 32-bit access's second word at +2.  */
  rtx sym;
  HOST_WIDE_INT k;
  if (GET_CODE (x) == PLUS
      && fructus_base_reg_ok_p (XEXP (x, 0), strict)
      && fructus_zp_offset_split (XEXP (x, 1), &sym, &k)
      && fructus_zp_offset_in_object (sym, k))
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

  /* Register + register: `ld rd, [ra, rb]', two bytes, the two added unscaled.
     THERE IS NO DISPLACEMENT FIELD IN THIS FORM, and a 32-bit access is split
     into two 16-bit ones at +0 and +2 - so it is offered only for the modes
     that fit in a single access.  Widening it would mean materialising the
     sum into a register anyway, which is what the middle end does for us.  */
  if (GET_CODE (x) == PLUS
      && (mode == QImode || mode == HImode)
      && fructus_base_reg_ok_p (XEXP (x, 0), strict)
      && fructus_base_reg_ok_p (XEXP (x, 1), strict))
    return true;

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

  /* The indexed form is two registers and no displacement at all.  */
  if (GET_CODE (addr) == PLUS
      && REG_P (XEXP (addr, 0)) && REG_P (XEXP (addr, 1)))
    {
      fprintf (file, "[%s, %s]", reg_names[REGNO (XEXP (addr, 0))],
	       reg_names[REGNO (XEXP (addr, 1))]);
      return;
    }

  if (GET_CODE (addr) == PLUS && REG_P (XEXP (addr, 0))
      && fructus_zp_offset_p (XEXP (addr, 1)))
    {
      fprintf (file, "[%s, #", reg_names[REGNO (XEXP (addr, 0))]);
      fructus_print_zp_offset (file, XEXP (addr, 1));
      fputc (']', file);
      return;
    }

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
      if (fructus_zp_offset_p (x))
	fructus_print_zp_offset (file, x);
      else if (CONSTANT_P (x))
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

/* A multi-register value may not include sp or lr, so must end by r5.  */

static bool
fructus_hard_regno_mode_ok (unsigned int regno, machine_mode mode)
{
  int n = (GET_MODE_SIZE (mode) + 1) / 2;

  if (regno >= FRUCTUS_SFP)
    return mode == Pmode;
  if (n <= 1)
    return true;
  return regno + n - 1 <= FRUCTUS_R5;
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
   Costs.  For size, rough and in bytes: most instructions are two, and a
   constant no form carries is a separate `mov'.  For speed, COSTS_N_INSNS
   (1) is a cycle on the pipeline in rtl/cpu.sv - see the cost model under
   [cpu] in isa/fructus.toml - where an ALU instruction is one cycle whatever
   its length, a load of N accesses N + 3, and a taken branch four.  An access
   is a byte, or a word at an even address; a word at an odd one is two, so
   ld8 and an aligned ld are 4 and an odd ld 5.

   ONLY `mult' TELLS THE TWO APART, and that is measured rather than
   overlooked.  Pricing a MEM at its cycles for speed - ld 5, ld8 4, when
   every word went a byte at a time - left every -O2 program in the fructus
   repository's C tests and tests/bench byte for byte the same; so did a
   TARGET_MEMORY_MOVE_COST of 8 or 12 against the default 4, to within six
   bytes and 21 cycles in 4.3 million, the wrong way.  And BRANCH_COST above 1
   made tests/div32.c 2.1% slower and 28 bytes larger - see fructus.h.  The
   costs gcc falls back on already describe this machine well enough; what
   did not was the multiply, which -mmul makes one instruction.

   ALIGNMENT IS SETTLED BY THE LAYOUT, NOT BY A PRICE: every type holding
   words is aligned to two - see fructus.h.  An unaligned word is still
   cheaper than two bytes, so the port says nothing of slow unaligned access and lets block
   moves go by words whatever the pointers are.  The one price that had to
   change is a byte load widened to a word - fructus_widened_byte_cost.
   ========================================================================== */

/* Whether X is one of the shapes movhi matches: the low byte of a register
   combined with a register shifted left by 8, or with a constant whose low
   byte is zero, by an ior, a plus or an xor - see fructus.md.  */

static bool
fructus_movhi_shape_p (rtx x)
{
  if (GET_MODE (x) != HImode
      || (GET_CODE (x) != IOR && GET_CODE (x) != PLUS && GET_CODE (x) != XOR))
    return false;

  auto low_byte_p = [] (rtx op)
    {
      if (GET_CODE (op) == ZERO_EXTEND)
	return GET_MODE (XEXP (op, 0)) == QImode && REG_P (SUBREG_P (XEXP (op, 0))
						       ? SUBREG_REG (XEXP (op, 0))
						       : XEXP (op, 0));
      return (GET_CODE (op) == AND && REG_P (XEXP (op, 0))
	      && CONST_INT_P (XEXP (op, 1)) && INTVAL (XEXP (op, 1)) == 255);
    };

  rtx a = XEXP (x, 0), b = XEXP (x, 1);
  if (GET_CODE (a) == ASHIFT)
    return (CONST_INT_P (XEXP (a, 1)) && INTVAL (XEXP (a, 1)) == 8
	    && register_operand (XEXP (a, 0), HImode) && low_byte_p (b));
  return (low_byte_p (a) && CONST_INT_P (b) && (INTVAL (b) & 0xff) == 0);
}

/* movhi rd, ra, #imm with the immediate a constant whose low byte is zero:
   the instruction takes the byte, not the constant.  */

const char *
fructus_output_movhi_imm (rtx *operands)
{
  operands[2] = GEN_INT ((INTVAL (operands[2]) >> 8) & 0xff);
  return "movhi\t%0, %1, #%2";
}

/* Whether an object of TYPE holds words, and so is worth an even address:
   two bytes or more, and not an array of bytes, which is read a byte at a
   time wherever it is - a string gains nothing from padding.  */

static bool
fructus_holds_words_p (const_tree type)
{
  if (!type || !TYPE_SIZE_UNIT (type)
      || !tree_fits_uhwi_p (TYPE_SIZE_UNIT (type))
      || tree_to_uhwi (TYPE_SIZE_UNIT (type)) < 2)
    return false;
  const_tree elt = strip_array_types (const_cast<tree> (type));
  return (TYPE_SIZE_UNIT (elt) && tree_fits_uhwi_p (TYPE_SIZE_UNIT (elt))
	  && tree_to_uhwi (TYPE_SIZE_UNIT (elt)) >= 2);
}

/* DATA_ALIGNMENT: a global or static that holds words goes at an even
   address.  See fructus.h.  */

unsigned int
fructus_data_alignment (tree type, unsigned int align)
{
  return fructus_holds_words_p (type) && align < 16 ? 16 : align;
}

/* TARGET_CONSTANT_ALIGNMENT: the same for a constant, but never a string,
   which is bytes.  */

static HOST_WIDE_INT
fructus_constant_alignment (const_tree exp, HOST_WIDE_INT align)
{
  if (TREE_CODE (exp) == STRING_CST)
    return align;
  return fructus_holds_words_p (TREE_TYPE (exp)) && align < 16 ? 16 : align;
}

/* A BYTE IN MEMORY READ AS A WORD is what a paradoxical subreg of it
   becomes.  `(u16) *p << 8' combines into a shift of (subreg:HI (mem:QI p)),
   which reload makes an ld whose high byte the shift throws away; priced as
   ld8, combine took it over ld8 and its free extension, and half of those
   words were odd - tests/bench's crc, 256 words a byte at a time.  rtx_cost
   prices a subreg's MEM in the MEM's own mode and never asks the target
   about a subreg, so the operation holding it is where it can be seen.  It
   is priced a cycle more for speed; for size the two are the same two bytes,
   and a quarter of an instruction breaks the tie the fast way.  Returns
   whether X has such an operand, with its cost in *TOTAL.  */

static bool
fructus_widened_byte_cost (rtx x, machine_mode mode, bool speed, int *total)
{
  const char *fmt = GET_RTX_FORMAT (GET_CODE (x));
  bool wide = false;
  int sum = COSTS_N_INSNS (1);
  for (int i = GET_RTX_LENGTH (GET_CODE (x)) - 1; i >= 0; i--)
    if (fmt[i] == 'e')
      {
	rtx op = XEXP (x, i);
	if (SUBREG_P (op) && MEM_P (SUBREG_REG (op))
	    && paradoxical_subreg_p (op)
	    && MEM_ALIGN (SUBREG_REG (op)) < 16)
	  {
	    wide = true;
	    sum += speed ? COSTS_N_INSNS (2) : COSTS_N_INSNS (1) + 1;
	  }
	else
	  sum += rtx_cost (op, mode, GET_CODE (x), i, speed);
      }
  if (wide)
    *total = sum;
  return wide;
}

/* A product, as what it becomes.  For speed, cycles: mul, mulhu and
   mulhs are two each, and the rest are libgcc's helpers as tools/sim.js
   counts them, call and return included, or what fructus_expand_mulsi3
   puts inline.  For size, the instructions at the call site - a call and
   the moves into its argument registers - or the inline ones.

   SIZE IS NOT SPEED HERE, and pricing them alike cost bytes: at 40 for
   both, -Os multiplied a long by 1000 with 33 instructions of shifts and
   adds where a call is four.  */

static int
fructus_mult_cost (rtx x, machine_mode mode, bool speed)
{
  enum rtx_code c0 = GET_CODE (XEXP (x, 0)), c1 = GET_CODE (XEXP (x, 1));
  bool widening = ((c0 == ZERO_EXTEND || c0 == SIGN_EXTEND)
		   && (c1 == c0 || CONST_INT_P (XEXP (x, 1))));

  if (mode == HImode)
    {
      if (TARGET_MUL)
	return COSTS_N_INSNS (speed ? 2 : 1);
      /* For size, five: a call clobbers r0, r1 and r5 and can cost the
	 caller its lr, so x * 13 in four shifts and adds stays smaller -
	 at three, tests/bench's matmul called __mulhi3 for it.  */
      return COSTS_N_INSNS (speed ? 15 : 5);
    }
  if (mode == SImode && widening)
    {
      if (TARGET_MUL)
	return COSTS_N_INSNS (speed ? 4 : 2);
      return COSTS_N_INSNS (speed ? 150 : 3);
    }
  if (mode == SImode)
    {
      if (!speed)
	return COSTS_N_INSNS (4);
      return COSTS_N_INSNS (TARGET_MUL ? 10 : 320);
    }
  if (!speed)
    return COSTS_N_INSNS (5);
  if (TARGET_MUL)
    return COSTS_N_INSNS (widening ? 50 : 110);
  return COSTS_N_INSNS (1200);
}

static bool
fructus_rtx_costs (rtx x, machine_mode mode, int, int, int *total,
		   bool speed)
{
  /* movhi is one instruction for what would otherwise be an and, a shift
     or a constant load, and an or.  Priced as the one instruction it is,
     combine takes it; left to the defaults it prices the same as the three
     it replaces, and combine is then free not to.  */
  if (fructus_movhi_shape_p (x))
    {
      *total = COSTS_N_INSNS (1);
      return true;
    }

  if (fructus_widened_byte_cost (x, mode, speed, total))
    return true;

  switch (GET_CODE (x))
    {
    case ZERO_EXTEND:
      /* ld8 zero extends on the way in, for nothing - which is also what
	 fructus_widened_byte_cost's widened word is weighed against.  */
      if (MEM_P (XEXP (x, 0)))
	{
	  *total = COSTS_N_INSNS (1);
	  return true;
	}
      return false;
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
      *total = fructus_mult_cost (x, mode, speed);
      return true;
    case TRUNCATE:
      /* The high half of a 16 x 16 product: mulhu or mulhs, two cycles.
	 Without mul there is no umulhi3_highpart, and nothing asks.  */
      if (mode == HImode && GET_CODE (XEXP (x, 0)) == LSHIFTRT
	  && GET_CODE (XEXP (XEXP (x, 0), 0)) == MULT)
	{
	  *total = COSTS_N_INSNS (speed ? 2 : 1);
	  return true;
	}
      return false;
    case DIV: case UDIV: case MOD: case UMOD:
      /* Calls.  For size a call and its divisor's mov - which is what
	 keeps a reciprocal's seventeen instructions out of -Os.  */
      if (!speed)
	*total = COSTS_N_INSNS (GET_MODE_SIZE (mode) > 2 ? 5 : 4);
      else
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
   Products wider than mul

   With -mmul a 16 x 16 -> 32 product is two instructions, mul for the low
   half and mulhu or mulhs for the high, and every wider product is built
   from those: no carry is tested short of 64 bits.  Without it, libgcc's
   shift-and-add routines.
   ========================================================================== */

/* A library routine of two operands, as a const call.  */

static rtx
fructus_libcall2 (const char *name, rtx target, machine_mode mode,
		  rtx a, rtx b, machine_mode op_mode)
{
  return emit_library_call_value (init_one_libfunc (name), target,
				  LCT_CONST, mode, a, op_mode, b, op_mode);
}

/* The two halves of a 16 x 16 product into LO and HI, either of which may
   be null.  */

static void
fructus_emit_halves (rtx lo, rtx hi, rtx a, rtx b, bool sign)
{
  a = force_reg (HImode, a);
  b = force_reg (HImode, b);
  if (lo)
    emit_insn (gen_mulhi3 (lo, a, b));
  if (hi)
    emit_insn (sign ? gen_smulhi3_highpart (hi, a, b)
		    : gen_umulhi3_highpart (hi, a, b));
}

/* umulhisi3 and mulhisi3: operands[0] = (SI) operands[1] * operands[2].
   With mul, its two halves, at -Os as well: four bytes and four cycles,
   where a call alone is three bytes.  Without mul, shift and add in
   __umulhisi3 - which before these expanders existed was reached only
   through __mulsi3, after widening both operands to 32 bits, and for a
   signed product never at all.  */

void
fructus_expand_widen_mult (rtx *operands, bool sign)
{
  if (TARGET_MUL)
    {
      rtx lo = gen_reg_rtx (HImode), hi = gen_reg_rtx (HImode);
      fructus_emit_halves (lo, hi, operands[1], operands[2], sign);
      emit_move_insn (simplify_gen_subreg (HImode, operands[0], SImode, 0), lo);
      emit_move_insn (simplify_gen_subreg (HImode, operands[0], SImode, 2), hi);
      return;
    }
  rtx v = fructus_libcall2 (sign ? "__mulhisi3" : "__umulhisi3",
			    operands[0], SImode, operands[1], operands[2],
			    HImode);
  if (v != operands[0])
    emit_move_insn (operands[0], v);
}

/* mulhi3 without mul: a call to __mulhi3.  */

void
fructus_expand_mulhi3_libcall (rtx *operands)
{
  rtx v = fructus_libcall2 ("__mulhi3", operands[0], HImode, operands[1],
			    force_reg (HImode, operands[2]), HImode);
  if (v != operands[0])
    emit_move_insn (operands[0], v);
}

/* mulsi3: with mul and for speed, the product inline -

	(ah:al) * (bh:bl) = al*bl + ((al*bh + ah*bl) << 16)

   which is al*bl's two halves and the cross terms' low halves, four
   products and two adds.  For size, or without mul, a call to
   __mulsi3_mul or __mulsi3: the call and its moves are smaller.  */

void
fructus_expand_mulsi3 (rtx *operands)
{
  if (TARGET_MUL && optimize_insn_for_speed_p ())
    {
      auto half = [] (rtx x, int byte)
	{
	  return force_reg (HImode, simplify_gen_subreg (HImode, x, SImode,
							 byte));
	};
      rtx al = half (operands[1], 0), ah = half (operands[1], 2);
      rtx bl = half (operands[2], 0), bh = half (operands[2], 2);
      rtx lo = gen_reg_rtx (HImode), c = gen_reg_rtx (HImode);
      rtx x = gen_reg_rtx (HImode), y = gen_reg_rtx (HImode);
      fructus_emit_halves (lo, c, al, bl, false);
      emit_insn (gen_mulhi3 (x, al, bh));
      emit_insn (gen_mulhi3 (y, ah, bl));
      rtx hi = expand_simple_binop (HImode, PLUS, x, y, NULL_RTX, 1,
				    OPTAB_DIRECT);
      hi = expand_simple_binop (HImode, PLUS, hi, c, NULL_RTX, 1,
				OPTAB_DIRECT);
      emit_move_insn (simplify_gen_subreg (HImode, operands[0], SImode, 0), lo);
      emit_move_insn (simplify_gen_subreg (HImode, operands[0], SImode, 2), hi);
      return;
    }
  rtx v = fructus_libcall2 (TARGET_MUL ? "__mulsi3_mul" : "__mulsi3",
			    operands[0], SImode, operands[1], operands[2],
			    SImode);
  if (v != operands[0])
    emit_move_insn (operands[0], v);
}

/* umulsidi3: 32 x 32 -> 64, unsigned, which libgcc has by hand.  Without
   this a 64-bit product of two 32-bit values was a full __muldi3.  */

void
fructus_expand_umulsidi3 (rtx *operands)
{
  rtx v = fructus_libcall2 (TARGET_MUL ? "__umulsidi3_mul" : "__umulsidi3",
			    operands[0], DImode, operands[1], operands[2],
			    SImode);
  if (v != operands[0])
    emit_move_insn (operands[0], v);
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

   With -mmul every division has a routine of its own, built on recip: the
   `_mul' names below.  As for the products, they are separate routines and
   not the same names built twice, because libgcc is built once, without
   -mmul, and has to run on a part with no multiplier.  32 bits gains a
   udivmod libfunc too, __udivmodsi4_mul, which returns the quotient in r0:r1
   and the remainder in r2:r3 - one 64-bit value under isa/abi.s, exactly as
   __udivmodhi4's pair is one 32-bit value.  Without -mmul the 32-bit
   routines are libgcc's C, which returns the remainder through a pointer,
   and has no such libfunc.
   ========================================================================== */

static void
fructus_init_libfuncs (void)
{
  if (TARGET_MUL)
    {
      set_optab_libfunc (udivmod_optab, HImode, "__udivmodhi4_mul");
      set_optab_libfunc (udiv_optab, HImode, "__udivhi3_mul");
      set_optab_libfunc (umod_optab, HImode, "__umodhi3_mul");
      set_optab_libfunc (sdiv_optab, HImode, "__divhi3_mul");
      set_optab_libfunc (smod_optab, HImode, "__modhi3_mul");

      set_optab_libfunc (udivmod_optab, SImode, "__udivmodsi4_mul");
      set_optab_libfunc (udiv_optab, SImode, "__udivsi3_mul");
      set_optab_libfunc (umod_optab, SImode, "__umodsi3_mul");
      set_optab_libfunc (sdiv_optab, SImode, "__divsi3_mul");
      set_optab_libfunc (smod_optab, SImode, "__modsi3_mul");

      set_optab_libfunc (udiv_optab, DImode, "__udivdi3_mul");
      set_optab_libfunc (umod_optab, DImode, "__umoddi3_mul");
      set_optab_libfunc (sdiv_optab, DImode, "__divdi3_mul");
      set_optab_libfunc (smod_optab, DImode, "__moddi3_mul");
    }
  else
    {
      set_optab_libfunc (udivmod_optab, HImode, "__udivmodhi4");
    }

  /* With mul, the wide products have helpers of their own, built from it.
     libgcc is built without -mmul and runs on either kind of part, so these
     are separate routines rather than the same names built twice - and
     --gc-sections leaves out whichever a program does not call.  */
  if (TARGET_MUL)
    {
      set_optab_libfunc (smul_optab, DImode, "__muldi3_mul");
    }
}

/* The pair comes back as one value twice the width: the quotient in its low
   half and the remainder in its high.  */

static void
fructus_expand_divmod_libfunc (rtx libfunc, machine_mode mode, rtx op0,
			       rtx op1, rtx *quot, rtx *rem)
{
  gcc_assert (mode == HImode || mode == SImode);
  machine_mode pair_mode = mode == HImode ? SImode : DImode;

  rtx pair = emit_library_call_value (libfunc, NULL_RTX, LCT_CONST, pair_mode,
				      op0, mode, op1, mode);

  *quot = simplify_gen_subreg (mode, pair, pair_mode, 0);
  *rem = simplify_gen_subreg (mode, pair, pair_mode, GET_MODE_SIZE (mode));

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

#undef  TARGET_ATTRIBUTE_TABLE
#define TARGET_ATTRIBUTE_TABLE fructus_attribute_table
#undef  TARGET_ENCODE_SECTION_INFO
#define TARGET_ENCODE_SECTION_INFO fructus_encode_section_info
#undef  TARGET_SECTION_TYPE_FLAGS
#define TARGET_SECTION_TYPE_FLAGS fructus_section_type_flags
#undef  TARGET_LEGITIMIZE_ADDRESS
#define TARGET_LEGITIMIZE_ADDRESS fructus_legitimize_address
#undef  TARGET_EXPAND_TO_RTL_HOOK
#define TARGET_EXPAND_TO_RTL_HOOK fructus_expand_to_rtl_hook
#undef  TARGET_DELEGITIMIZE_ADDRESS
#define TARGET_DELEGITIMIZE_ADDRESS fructus_delegitimize_address
#undef  TARGET_ASM_OUTPUT_ADDR_CONST_EXTRA
#define TARGET_ASM_OUTPUT_ADDR_CONST_EXTRA fructus_output_addr_const_extra
#undef  TARGET_LEGITIMATE_CONSTANT_P
#define TARGET_LEGITIMATE_CONSTANT_P fructus_legitimate_constant_p
#undef  TARGET_CANNOT_FORCE_CONST_MEM
#define TARGET_CANNOT_FORCE_CONST_MEM fructus_cannot_force_const_mem

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
#undef  TARGET_CONSTANT_ALIGNMENT
#define TARGET_CONSTANT_ALIGNMENT fructus_constant_alignment
#undef  TARGET_SHIFT_TRUNCATION_MASK
#define TARGET_SHIFT_TRUNCATION_MASK fructus_shift_truncation_mask

#undef  TARGET_INIT_LIBFUNCS
#define TARGET_INIT_LIBFUNCS fructus_init_libfuncs
#undef  TARGET_EXPAND_DIVMOD_LIBFUNC
#define TARGET_EXPAND_DIVMOD_LIBFUNC fructus_expand_divmod_libfunc

/* ==========================================================================
   Builtins

   One, for clmul: it has no RTL code, so nothing reaches the instruction
   except by name.  __builtin_fructus_clmul (x) is the carry-less product of
   x's two bytes, exactly as the instruction defines it; the operands are
   packed first, `(a & 0xff) | (b << 8)', which combine turns into movhi.

   It is const: two calls with the same operand are one, and a call whose
   result is unused goes.  With a constant operand it folds.
   ========================================================================== */

enum fructus_builtin
{
  FRUCTUS_BUILTIN_CLMUL,
  FRUCTUS_BUILTIN_MAX
};

static GTY(()) tree fructus_builtin_decls[FRUCTUS_BUILTIN_MAX];

static void
fructus_init_builtins (void)
{
  tree u16 = unsigned_intHI_type_node;
  tree fn = build_function_type_list (u16, u16, NULL_TREE);
  tree decl = add_builtin_function ("__builtin_fructus_clmul", fn,
				    FRUCTUS_BUILTIN_CLMUL, BUILT_IN_MD,
				    NULL, NULL_TREE);
  TREE_READONLY (decl) = 1;
  TREE_NOTHROW (decl) = 1;
  fructus_builtin_decls[FRUCTUS_BUILTIN_CLMUL] = decl;
}

static tree
fructus_builtin_decl (unsigned code, bool)
{
  return code < FRUCTUS_BUILTIN_MAX ? fructus_builtin_decls[code]
				     : error_mark_node;
}

/* The product of the two bytes, as tools/sim.js computes it.  */

static unsigned
fructus_clmul_value (unsigned x)
{
  unsigned r = 0;
  for (int i = 0; i < 8; i++)
    if ((x >> i) & 1)
      r ^= ((x >> 8) & 0xff) << i;
  return r;
}

static tree
fructus_fold_builtin (tree fndecl, int, tree *args, bool)
{
  if (DECL_MD_FUNCTION_CODE (fndecl) == FRUCTUS_BUILTIN_CLMUL
      && TREE_CODE (args[0]) == INTEGER_CST)
    return build_int_cst (TREE_TYPE (TREE_TYPE (fndecl)),
			  fructus_clmul_value (TREE_INT_CST_LOW (args[0])
					       & 0xffff));
  return NULL_TREE;
}

static rtx
fructus_expand_builtin (tree exp, rtx target, rtx, machine_mode, int)
{
  tree fndecl = TREE_OPERAND (CALL_EXPR_FN (exp), 0);
  gcc_assert (DECL_MD_FUNCTION_CODE (fndecl) == FRUCTUS_BUILTIN_CLMUL);

  rtx op = force_reg (HImode, expand_normal (CALL_EXPR_ARG (exp, 0)));
  if (!target || !register_operand (target, HImode))
    target = gen_reg_rtx (HImode);
  emit_insn (gen_fructus_clmul (target, op));
  return target;
}

#undef  TARGET_INIT_BUILTINS
#define TARGET_INIT_BUILTINS fructus_init_builtins
#undef  TARGET_BUILTIN_DECL
#define TARGET_BUILTIN_DECL fructus_builtin_decl
#undef  TARGET_FOLD_BUILTIN
#define TARGET_FOLD_BUILTIN fructus_fold_builtin
#undef  TARGET_EXPAND_BUILTIN
#define TARGET_EXPAND_BUILTIN fructus_expand_builtin

struct gcc_target targetm = TARGET_INITIALIZER;

#include "gt-fructus.h"
