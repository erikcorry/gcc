;; Machine description for Fructus, a 16-bit retrocomputer instruction set.
;; Copyright (C) 2026 Free Software Foundation, Inc.
;;
;; This file is part of GCC.
;;
;; GCC is free software; you can redistribute it and/or modify it
;; under the terms of the GNU General Public License as published
;; by the Free Software Foundation; either version 3, or (at your
;; option) any later version.
;;
;; GCC is distributed in the hope that it will be useful, but WITHOUT
;; ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
;; or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
;; License for more details.
;;
;; You should have received a copy of the GNU General Public License
;; along with GCC; see the file COPYING3.  If not see
;; <http://www.gnu.org/licenses/>.

;; The instruction set is isa/fructus.toml in the fructus repository.  Two of
;; its properties shape everything below:
;;
;;   GAS PICKS THE FORM AND NEVER SYNTHESISES ONE.  `add r1, r2, #5' may come
;;   out as any of several encodings, and the templates name the instruction
;;   and let gas choose.  But no immediate is ever built through a scratch
;;   register, so a constant no form carries has to be in a register before
;;   the insn is matched - which is what the predicates and constraints are
;;   for.
;;
;;   COMPARE AND BRANCH ARE ONE INSTRUCTION, with no flags anywhere.  So there
;;   is no cc register and no separate compare; a cbranch is a single insn,
;;   and its displacement is eight bits that gas does not relax.

(include "constraints.md")
(include "predicates.md")

(define_constants
  [(R0_REGNUM  0)
   (R4_REGNUM  4)
   (R5_REGNUM  5)
   (SP_REGNUM  6)
   (LR_REGNUM  7)])

(define_c_enum "unspec" [UNSPEC_CALLEE_CC UNSPEC_CLMUL UNSPEC_ZP UNSPEC_SUBV])
(define_c_enum "unspecv" [UNSPECV_BLOCKAGE])

(define_attr "length" "" (const_int 2))

(define_mode_iterator QHI [QI HI])
(define_mode_iterator SISF [SI SF])
(define_mode_attr bw [(QI "8") (HI "")])

;; -------------------------------------------------------------------------
;; Moves
;; -------------------------------------------------------------------------

(define_expand "mov<mode>"
  [(set (match_operand:QHI 0 "nonimmediate_operand")
	(match_operand:QHI 1 "general_operand"))]
  ""
{
  if (fructus_expand_move (operands, <MODE>mode))
    DONE;
})

(define_insn "*mov<mode>"
  [(set (match_operand:QHI 0 "nonimmediate_operand" "=r,r,r,m")
	(match_operand:QHI 1 "general_operand"       "r,i,m,r"))]
  "register_operand (operands[0], <MODE>mode)
   || register_operand (operands[1], <MODE>mode)"
  "@
   mov\t%0, %1
   mov\t%0, #%1
   ld<bw>\t%0, %1
   st<bw>\t%1, %0"
  [(set (attr "length") (symbol_ref "fructus_move_length (operands)"))])

;; 32-bit values are two registers, and split into two moves once the
;; registers are known.
(define_expand "mov<mode>"
  [(set (match_operand:SISF 0 "nonimmediate_operand")
	(match_operand:SISF 1 "general_operand"))]
  ""
{
  if (fructus_expand_move (operands, <MODE>mode))
    DONE;
})

(define_insn_and_split "*mov<mode>"
  [(set (match_operand:SISF 0 "nonimmediate_operand" "=r,r,r,m")
	(match_operand:SISF 1 "general_operand"       "r,iF,m,r"))]
  "register_operand (operands[0], <MODE>mode)
   || register_operand (operands[1], <MODE>mode)"
  "#"
  "reload_completed"
  [(const_int 0)]
{
  fructus_split_double_move (operands, <MODE>mode);
  DONE;
}
  [(set (attr "length") (symbol_ref "fructus_double_move_length (operands)"))])

;; -------------------------------------------------------------------------
;; Push and pop.  sp points at the last thing pushed, so a push is a
;; pre-decrement and a pop a post-increment.
;; -------------------------------------------------------------------------

;; There is no byte push, and none is needed: a byte argument takes a word
;; slot with the value in its low byte and the high byte unspecified, as in a
;; register, so a word push of the register holding it is the byte push.
;; Little endian puts the low byte at sp.  The RTL says sp moves by two and
;; only the byte is stored, which is all a caller or callee may rely on;
;; PUSH_ROUNDING says the same to the argument layout.
(define_insn "pushqi1"
  [(set (reg:HI SP_REGNUM) (plus:HI (reg:HI SP_REGNUM) (const_int -2)))
   (set (mem:QI (plus:HI (reg:HI SP_REGNUM) (const_int -2)))
	(match_operand:QI 0 "fructus_push_operand" "r"))]
  ""
  "push\t%0")

(define_insn "pushhi1"
  [(set (mem:HI (pre_dec:HI (reg:HI SP_REGNUM)))
	(match_operand:HI 0 "fructus_push_operand" "r"))]
  ""
  "push\t%0")

;; High word first, so that it lands higher: little endian in memory.
(define_insn "push<mode>1"
  [(set (mem:SISF (pre_dec:HI (reg:HI SP_REGNUM)))
	(match_operand:SISF 0 "fructus_push_operand" "r"))]
  ""
  "push\t%H0, %L0")

(define_insn "fructus_push2"
  [(set (mem:HI (plus:HI (reg:HI SP_REGNUM) (const_int -2)))
	(match_operand:HI 0 "register_operand" "r"))
   (set (mem:HI (plus:HI (reg:HI SP_REGNUM) (const_int -4)))
	(match_operand:HI 1 "register_operand" "r"))
   (set (reg:HI SP_REGNUM)
	(plus:HI (reg:HI SP_REGNUM) (const_int -4)))]
  ""
  "push\t%0, %1")

(define_insn "fructus_push3"
  [(set (mem:HI (plus:HI (reg:HI SP_REGNUM) (const_int -2)))
	(match_operand:HI 0 "register_operand" "r"))
   (set (mem:HI (plus:HI (reg:HI SP_REGNUM) (const_int -4)))
	(match_operand:HI 1 "register_operand" "r"))
   (set (mem:HI (plus:HI (reg:HI SP_REGNUM) (const_int -6)))
	(match_operand:HI 2 "register_operand" "r"))
   (set (reg:HI SP_REGNUM)
	(plus:HI (reg:HI SP_REGNUM) (const_int -6)))]
  ""
  "push\t%0, %1, %2")

(define_insn "fructus_pop1"
  [(set (match_operand:HI 0 "register_operand" "=r")
	(mem:HI (post_inc:HI (reg:HI SP_REGNUM))))]
  ""
  "pop\t%0")

(define_insn "fructus_pop2"
  [(set (match_operand:HI 0 "register_operand" "=r")
	(mem:HI (reg:HI SP_REGNUM)))
   (set (match_operand:HI 1 "register_operand" "=r")
	(mem:HI (plus:HI (reg:HI SP_REGNUM) (const_int 2))))
   (set (reg:HI SP_REGNUM)
	(plus:HI (reg:HI SP_REGNUM) (const_int 4)))]
  ""
  "pop\t%0, %1")

(define_insn "fructus_pop3"
  [(set (match_operand:HI 0 "register_operand" "=r")
	(mem:HI (reg:HI SP_REGNUM)))
   (set (match_operand:HI 1 "register_operand" "=r")
	(mem:HI (plus:HI (reg:HI SP_REGNUM) (const_int 2))))
   (set (match_operand:HI 2 "register_operand" "=r")
	(mem:HI (plus:HI (reg:HI SP_REGNUM) (const_int 4))))
   (set (reg:HI SP_REGNUM)
	(plus:HI (reg:HI SP_REGNUM) (const_int 6)))]
  ""
  "pop\t%0, %1, %2")

;; -------------------------------------------------------------------------
;; Extension.  ld8 zero extends on the way in; there is no signed byte load.
;; -------------------------------------------------------------------------

(define_insn "zero_extendqihi2"
  [(set (match_operand:HI 0 "register_operand" "=r,r")
	(zero_extend:HI (match_operand:QI 1 "nonimmediate_operand" "r,m")))]
  ""
  "@
   and\t%0, %1, #255
   ld8\t%0, %1"
  [(set (attr "length")
	(if_then_else (eq_attr "alternative" "0")
		      (symbol_ref "(fructus_same_reg_p (operands[0], operands[1]) ? 2 : 3)")
		      (symbol_ref "fructus_move_length (operands)")))])

(define_insn "extendqihi2"
  [(set (match_operand:HI 0 "register_operand" "=r")
	(sign_extend:HI (match_operand:QI 1 "register_operand" "r")))]
  ""
  "sxt8\t%0, %1")

;; -------------------------------------------------------------------------
;; Arithmetic and logic.  One template per operation; gas picks the form.
;; The tied alternative is the one that can carry the mask tables.
;; -------------------------------------------------------------------------

(define_insn "addhi3"
  [(set (match_operand:HI 0 "register_operand" "=r,r,r")
	(plus:HI (match_operand:HI 1 "register_operand" "%0,r,r")
		 (match_operand:HI 2 "fructus_add_operand" "IK,IZ,r")))]
  ""
{
  return fructus_output_alu (PLUS, operands);
}
  [(set (attr "length") (symbol_ref "fructus_alu_length (PLUS, operands)"))])

(define_code_iterator logic [and ior xor])

(define_insn "<code>hi3"
  [(set (match_operand:HI 0 "register_operand" "=r,r,r")
	(logic:HI (match_operand:HI 1 "register_operand" "%0,r,r")
		  (match_operand:HI 2 "fructus_alu_operand" "IK,I,r")))]
  ""
{
  return fructus_output_alu (<CODE>, operands);
}
  [(set (attr "length") (symbol_ref "fructus_alu_length (<CODE>, operands)"))])

;; There is no subtract: rsb d, a, b is b - a.
(define_expand "subhi3"
  [(set (match_operand:HI 0 "register_operand")
	(minus:HI (match_operand:HI 1 "nonmemory_operand")
		  (match_operand:HI 2 "nonmemory_operand")))]
  ""
{
  if (fructus_expand_sub (operands))
    DONE;
})

(define_insn "*subhi3"
  [(set (match_operand:HI 0 "register_operand" "=r,r")
	(minus:HI (match_operand:HI 1 "fructus_imm10_operand" "r,I")
		  (match_operand:HI 2 "register_operand" "r,r")))]
  ""
  "@
   rsb\t%0, %2, %1
   rsb\t%0, %2, #%1"
  [(set (attr "length") (symbol_ref "fructus_rsb_length (operands)"))])

(define_insn "neghi2"
  [(set (match_operand:HI 0 "register_operand" "=r")
	(neg:HI (match_operand:HI 1 "register_operand" "r")))]
  ""
  "rsb\t%0, %1, #0")

(define_insn "one_cmplhi2"
  [(set (match_operand:HI 0 "register_operand" "=r")
	(not:HI (match_operand:HI 1 "register_operand" "r")))]
  ""
  "xor\t%0, %1, #-1")

(define_code_iterator shift [ashift ashiftrt lshiftrt])
(define_code_attr shift_insn [(ashift "shl") (ashiftrt "asr") (lshiftrt "lsr")])
(define_code_attr shift_name [(ashift "ashl") (ashiftrt "ashr") (lshiftrt "lshr")])

;; The tied form carries any count 0-15; the untied one only shift3's eight.
(define_insn "<shift_name>hi3"
  [(set (match_operand:HI 0 "register_operand" "=r,r,r")
	(shift:HI (match_operand:HI 1 "register_operand" "0,r,r")
		  (match_operand:HI 2 "fructus_shift_operand" "J,S,r")))]
  ""
  "@
   <shift_insn>\t%0, %1, #%2
   <shift_insn>\t%0, %1, #%2
   <shift_insn>\t%0, %1, %2")

;; A 64-bit shift by 16, 32 or 48 is register moves, which GCC does not find
;; on its own: it expands a two-word shift inline and calls a libcall for a
;; four-word one.  Any other count still goes to the libcall, by failing.
(define_expand "<shift_name>di3"
  [(set (match_operand:DI 0 "register_operand")
	(shift:DI (match_operand:DI 1 "register_operand")
		  (match_operand:HI 2 "nonmemory_operand")))]
  ""
{
  if (!fructus_expand_word_shift (operands, <CODE>))
    FAIL;
  DONE;
})

;; mul is the low half of the product, from the DSP, in two cycles; only
;; with -mmul, since a part without a multiplier has no mul.  The tied form
;; carries imm5 in two bytes, the untied one imm10 in three.
;;
;; WITHOUT mul THE EXPANDER CALLS __mulhi3 ITSELF, and it has to.  Were
;; there no mulhi3 at all, expand_binop would try a wider mode's insn before
;; the libcall - and umulhisi3 is one - so every int multiply became a
;; 32-bit __umulhisi3, 70 cycles slower.
(define_expand "mulhi3"
  [(set (match_operand:HI 0 "register_operand")
	(mult:HI (match_operand:HI 1 "register_operand")
		 (match_operand:HI 2 "fructus_imm10_operand")))]
  ""
{
  if (!TARGET_MUL)
    {
      fructus_expand_mulhi3_libcall (operands);
      DONE;
    }
})

(define_insn "*mulhi3"
  [(set (match_operand:HI 0 "register_operand" "=r,r,r")
	(mult:HI (match_operand:HI 1 "register_operand" "%0,r,r")
		 (match_operand:HI 2 "fructus_imm10_operand" "I,I,r")))]
  "TARGET_MUL"
  "@
   mul\t%0, %1, #%2
   mul\t%0, %1, #%2
   mul\t%0, %1, %2"
  [(set (attr "length")
	(if_then_else (match_test "CONST_INT_P (operands[2])
				   && !(which_alternative == 0
					&& fructus_imm5_p (INTVAL (operands[2])))")
		      (const_int 3)
		      (const_int 2)))])

;; Products wider than mul's sixteen bits: byte products inline with -mmul
;; for speed, and libgcc's helpers otherwise - see fructus.cc.  Declaring
;; the widening ones is what lets a 16 x 16 -> 32 product reach
;; __umulhisi3 and __mulhisi3 at all; without them GCC widened both
;; operands and called __mulsi3.
(define_expand "umulhisi3"
  [(set (match_operand:SI 0 "register_operand")
	(mult:SI (zero_extend:SI (match_operand:HI 1 "register_operand"))
		 (zero_extend:SI (match_operand:HI 2 "register_operand"))))]
  ""
{
  fructus_expand_widen_mult (operands, false);
  DONE;
})

(define_expand "mulhisi3"
  [(set (match_operand:SI 0 "register_operand")
	(mult:SI (sign_extend:SI (match_operand:HI 1 "register_operand"))
		 (sign_extend:SI (match_operand:HI 2 "register_operand"))))]
  ""
{
  fructus_expand_widen_mult (operands, true);
  DONE;
})

(define_expand "umulsidi3"
  [(set (match_operand:DI 0 "register_operand")
	(mult:DI (zero_extend:DI (match_operand:SI 1 "register_operand"))
		 (zero_extend:DI (match_operand:SI 2 "register_operand"))))]
  ""
{
  fructus_expand_umulsidi3 (operands);
  DONE;
})

;; A 32 x 32 -> 32 product is a call, as it was before umulsidi3 existed -
;; but it must be SAID to be one.  With only a libfunc for SImode,
;; expand_binop tries a wider mode's insn before the libcall, found
;; umulsidi3, and made every long multiply a 64-bit product.
(define_expand "mulsi3"
  [(set (match_operand:SI 0 "register_operand")
	(mult:SI (match_operand:SI 1 "register_operand")
		 (match_operand:SI 2 "register_operand")))]
  ""
{
  fructus_expand_mulsi3 (operands);
  DONE;
})

;; The high half alone, which is how GCC divides by a constant: by its
;; reciprocal.  Only with mul, which makes it four byte products; the
;; shift-and-add equivalent would be wider than a word, which expmed.cc
;; does not attempt.
(define_expand "umulhi3_highpart"
  [(set (match_operand:HI 0 "register_operand")
	(truncate:HI
	  (lshiftrt:SI
	    (mult:SI (zero_extend:SI (match_operand:HI 1 "register_operand"))
		     (zero_extend:SI (match_operand:HI 2 "nonmemory_operand")))
	    (const_int 16))))]
  "TARGET_MUL"
{
  fructus_expand_mult_highpart (operands, false);
  DONE;
})

(define_expand "smulhi3_highpart"
  [(set (match_operand:HI 0 "register_operand")
	(truncate:HI
	  (lshiftrt:SI
	    (mult:SI (sign_extend:SI (match_operand:HI 1 "register_operand"))
		     (sign_extend:SI (match_operand:HI 2 "nonmemory_operand")))
	    (const_int 16))))]
  "TARGET_MUL"
{
  fructus_expand_mult_highpart (operands, true);
  DONE;
})

(define_insn "clzhi2"
  [(set (match_operand:HI 0 "register_operand" "=r")
	(clz:HI (match_operand:HI 1 "register_operand" "r")))]
  ""
  "clz\t%0, %1")

(define_insn "popcounthi2"
  [(set (match_operand:HI 0 "register_operand" "=r")
	(popcount:HI (match_operand:HI 1 "register_operand" "r")))]
  ""
  "popcount\t%0, %1")
;; The number of bits after the sign that repeat it: clz of the value, or
;; of its complement if negative, less the sign itself.
(define_expand "clrsbhi2"
  [(match_operand:HI 0 "register_operand")
   (match_operand:HI 1 "register_operand")]
  ""
{
  rtx t = gen_reg_rtx (HImode);
  rtx_code_label *pos = gen_label_rtx ();
  emit_move_insn (t, operands[1]);
  emit_cmp_and_jump_insns (operands[1], const0_rtx, GE, NULL_RTX, HImode, 0,
			   pos);
  emit_insn (gen_one_cmplhi2 (t, operands[1]));
  emit_label (pos);
  emit_insn (gen_clzhi2 (t, t));
  emit_insn (gen_addhi3 (operands[0], t, constm1_rtx));
  DONE;
})
;; clmul multiplies the two bytes of one register without carries.  There
;; is no RTL code for a carry-less product, so it is an unspec, reached
;; through __builtin_fructus_clmul; the operands are packed by movhi below.
(define_insn "fructus_clmul"
  [(set (match_operand:HI 0 "register_operand" "=r")
	(unspec:HI [(match_operand:HI 1 "register_operand" "r")] UNSPEC_CLMUL))]
  ""
  "clmul\t%0, %1")

;; -------------------------------------------------------------------------
;; movhi: ra with its high byte replaced, (x << 8) | (ra & 0xff), where x is
;; rb or an immediate.  No named pattern asks for it; combine finds it.
;;
;; THE TWO BYTES DO NOT OVERLAP, so an ior, a plus and an xor of them are the
;; same value, and the source may have written any of the three.  The low
;; byte reaches combine as a zero_extend of a QImode value, or as an and
;; with 255 when the value was HImode all along; the high byte is an ashift
;; by 8, or for the immediate a constant whose low byte is zero.
;; -------------------------------------------------------------------------

(define_code_iterator bytes [ior plus xor])

(define_insn "*movhi_<code>_zext"
  [(set (match_operand:HI 0 "register_operand" "=r")
	(bytes:HI (ashift:HI (match_operand:HI 2 "register_operand" "r")
			     (const_int 8))
		  (zero_extend:HI (match_operand:QI 1 "register_operand" "r"))))]
  ""
  "movhi\t%0, %1, %2")

(define_insn "*movhi_<code>_and"
  [(set (match_operand:HI 0 "register_operand" "=r")
	(bytes:HI (ashift:HI (match_operand:HI 2 "register_operand" "r")
			     (const_int 8))
		  (and:HI (match_operand:HI 1 "register_operand" "r")
			  (const_int 255))))]
  ""
  "movhi\t%0, %1, %2")

(define_insn "*movhi_<code>_imm_zext"
  [(set (match_operand:HI 0 "register_operand" "=r")
	(bytes:HI (zero_extend:HI (match_operand:QI 1 "register_operand" "r"))
		  (match_operand:HI 2 "fructus_high_byte_operand" "i")))]
  ""
{
  return fructus_output_movhi_imm (operands);
}
  [(set_attr "length" "3")])

(define_insn "*movhi_<code>_imm_and"
  [(set (match_operand:HI 0 "register_operand" "=r")
	(bytes:HI (and:HI (match_operand:HI 1 "register_operand" "r")
			  (const_int 255))
		  (match_operand:HI 2 "fructus_high_byte_operand" "i")))]
  ""
{
  return fructus_output_movhi_imm (operands);
}
  [(set_attr "length" "3")])

;; A rotate by 8 is a byte swap: the high byte shifted down, then movhi puts
;; the source's low byte over it.  Two instructions and four bytes, where the
;; shifts and an or are three and six.
;;
;; THE lsr WRITES BEFORE THE movhi READS THE SOURCE, so the shifted byte needs
;; a register that is not the source.  A different destination is one, and
;; costs nothing; when the destination IS the source, a scratch is.  Without
;; the second alternative the tied case paid a `mov' to make them differ.
(define_code_iterator rot8 [rotate rotatert])
(define_code_attr rot_name [(rotate "rotl") (rotatert "rotr")])

(define_insn "*<code>8"
  [(set (match_operand:HI 0 "register_operand" "=&r,r")
	(rot8:HI (match_operand:HI 1 "register_operand" "r,0")
		 (const_int 8)))
   (clobber (match_scratch:HI 2 "=X,&r"))]
  ""
  "@
   lsr\t%0, %1, #8\n\tmovhi\t%0, %0, %1
   lsr\t%2, %1, #8\n\tmovhi\t%0, %2, %1"
  [(set_attr "length" "4")])

(define_insn "bswaphi2"
  [(set (match_operand:HI 0 "register_operand" "=&r,r")
	(bswap:HI (match_operand:HI 1 "register_operand" "r,0")))
   (clobber (match_scratch:HI 2 "=X,&r"))]
  ""
  "@
   lsr\t%0, %1, #8\n\tmovhi\t%0, %0, %1
   lsr\t%2, %1, #8\n\tmovhi\t%0, %2, %1"
  [(set_attr "length" "4")])

;; Only a rotate by 8 is one; any other count FAILs, and GCC builds it from
;; shifts as it did before.
(define_expand "<rot_name>hi3"
  [(parallel [(set (match_operand:HI 0 "register_operand")
		   (rot8:HI (match_operand:HI 1 "register_operand")
			    (match_operand:HI 2 "const_int_operand")))
	      (clobber (scratch:HI))])]
  ""
{
  if (INTVAL (operands[2]) != 8)
    FAIL;
})

;; -------------------------------------------------------------------------
;; Predicates as values: iseq and isset.  Only == and != exist, so the
;; other comparisons FAIL and are done with branches.
;; -------------------------------------------------------------------------

(define_expand "cstorehi4"
  [(set (match_operand:HI 0 "register_operand")
	(match_operator:HI 1 "ordered_comparison_operator"
	  [(match_operand:HI 2 "register_operand")
	   (match_operand:HI 3 "nonmemory_operand")]))]
  ""
{
  if (GET_CODE (operands[1]) != EQ && GET_CODE (operands[1]) != NE)
    FAIL;
  if (!fructus_imm10_operand (operands[3], HImode))
    operands[3] = force_reg (HImode, operands[3]);
})

(define_insn "*seq"
  [(set (match_operand:HI 0 "register_operand" "=r")
	(eq:HI (match_operand:HI 1 "register_operand" "r")
	       (match_operand:HI 2 "fructus_imm10_operand" "rI")))]
  ""
{
  return fructus_output_iseq (operands, false);
}
  [(set (attr "length") (symbol_ref "fructus_iseq_length (operands, false)"))])

(define_insn "*sne"
  [(set (match_operand:HI 0 "register_operand" "=r")
	(ne:HI (match_operand:HI 1 "register_operand" "r")
	       (match_operand:HI 2 "fructus_imm10_operand" "rI")))]
  ""
{
  return fructus_output_iseq (operands, true);
}
  [(set (attr "length") (symbol_ref "fructus_iseq_length (operands, true)"))])

(define_insn "*isset"
  [(set (match_operand:HI 0 "register_operand" "=r")
	(ne:HI (and:HI (match_operand:HI 1 "register_operand" "0")
		       (match_operand:HI 2 "fructus_mask_operand" "K"))
	       (const_int 0)))]
  ""
  "isset\t%0, %0, #%2")

(define_insn "*isset_bit"
  [(set (match_operand:HI 0 "register_operand" "=r")
	(zero_extract:HI (match_operand:HI 1 "register_operand" "0")
			 (const_int 1)
			 (match_operand 2 "fructus_bitpos_operand" "")))]
  ""
{
  operands[2] = gen_int_mode (HOST_WIDE_INT_1 << INTVAL (operands[2]), HImode);
  return "isset\t%0, %0, #%2";
})

;; -------------------------------------------------------------------------
;; Compare and branch.
;;
;; A branch reaches -128..127 from the next instruction.  The length
;; attribute tests a conservative -120..120 from the branch's own address,
;; which also covers GCC measuring forward branches from the far end, and
;; beyond that fructus_output_cbranch jumps over an absolute jmp.
;;
;; One insn per comparison code, so that the constant alternative can have
;; a constraint that knows the code - see constraints.md.
;; -------------------------------------------------------------------------

(define_expand "cbranchhi4"
  [(set (pc)
	(if_then_else (match_operator 0 "ordered_comparison_operator"
			[(match_operand:HI 1 "register_operand")
			 (match_operand:HI 2 "nonmemory_operand")])
		      (label_ref (match_operand 3 ""))
		      (pc)))]
  ""
{
  fructus_expand_cbranch (operands, HImode);
})

(define_code_iterator any_cond [eq ne lt le gt ge ltu leu gtu geu])
(define_code_attr cc [(eq "eq") (ne "ne") (lt "lt") (le "le") (gt "gt")
		      (ge "ge") (ltu "lo") (leu "ls") (gtu "hi") (geu "hs")])

(define_insn "*cbranchhi_<code>"
  [(set (pc)
	(if_then_else (any_cond (match_operand:HI 0 "register_operand" "r,r")
				(match_operand:HI 1 "fructus_cmp_operand"
						   "r,C<cc>h"))
		      (label_ref (match_operand 2 ""))
		      (pc)))]
  "fructus_cbranch_ok_p (operands[1], <CODE>, HImode)"
{
  return fructus_output_cbranch (operands, <CODE>, HImode, false,
				 get_attr_length (insn));
}
  [(set (attr "length")
	(if_then_else
	  (and (ge (minus (match_dup 2) (pc)) (const_int -120))
	       (le (minus (match_dup 2) (pc)) (const_int 120)))
	  (if_then_else
	    (match_test "fructus_cbranch_ok_p (operands[1], <CODE>, HImode)")
	    (const_int 3)
	    (if_then_else
	      (match_test "fructus_cbranch_ok_p (operands[1],
				 reverse_condition (<CODE>), HImode)")
	      (const_int 6) (const_int 8)))
	  (if_then_else
	    (match_test "fructus_cbranch_ok_p (operands[1],
			       reverse_condition (<CODE>), HImode)")
	    (const_int 6) (const_int 8))))])

;; The same with the arms exchanged, which is what inverting a jump produces
;; when the reversed comparison has no encoding of its own.
(define_insn "*cbranchhi_<code>_rev"
  [(set (pc)
	(if_then_else (any_cond (match_operand:HI 0 "register_operand" "r,r")
				(match_operand:HI 1 "fructus_cmp_operand"
						   "r,C<cc>h"))
		      (pc)
		      (label_ref (match_operand 2 ""))))]
  "fructus_cbranch_ok_p (operands[1], <CODE>, HImode)"
{
  return fructus_output_cbranch (operands, <CODE>, HImode, true,
				 get_attr_length (insn));
}
  [(set (attr "length")
	(if_then_else
	  (and (ge (minus (match_dup 2) (pc)) (const_int -120))
	       (le (minus (match_dup 2) (pc)) (const_int 120)))
	  (if_then_else
	    (match_test "fructus_cbranch_ok_p (operands[1],
			       reverse_condition (<CODE>), HImode)")
	    (const_int 3)
	    (const_int 6))
	  (const_int 6)))])

(define_code_iterator eqne [eq ne])

;; Signed overflow: addv and subv, through fructus_expand_addsubv.
(define_expand "addvhi4"
  [(match_operand:HI 0 "register_operand")
   (match_operand:HI 1 "register_operand")
   (match_operand:HI 2 "nonmemory_operand")
   (match_operand 3 "")]
  ""
{
  fructus_expand_addsubv (operands, PLUS);
  DONE;
})

(define_expand "subvhi4"
  [(match_operand:HI 0 "register_operand")
   (match_operand:HI 1 "nonmemory_operand")
   (match_operand:HI 2 "nonmemory_operand")
   (match_operand 3 "")]
  ""
{
  fructus_expand_addsubv (operands, MINUS);
  DONE;
})

;; -x overflows only for -32768, which is lt #-32767.
(define_expand "negvhi3"
  [(match_operand:HI 0 "register_operand")
   (match_operand:HI 1 "register_operand")
   (match_operand 2 "")]
  ""
{
  rtx a = operands[1];
  rtx t = gen_reg_rtx (HImode);
  emit_insn (gen_neghi2 (t, a));
  if (reg_overlap_mentioned_p (operands[0], a))
    a = copy_to_reg (a);
  emit_move_insn (operands[0], t);
  emit_cmp_and_jump_insns (a, gen_int_mode (-32767, HImode), LT, NULL_RTX,
			   HImode, 0, as_a <rtx_code_label *> (operands[2]));
  DONE;
})

;; br vs and br vc: taken when %0 - %1 overflows, or does not.  The unspec
;; is that overflow as a value, there being no RTL code for it.
(define_expand "fructus_branch_vs"
  [(set (pc)
	(if_then_else (ne (unspec:HI [(match_operand:HI 0 "register_operand")
				      (match_operand:HI 1 "register_operand")]
				     UNSPEC_SUBV)
			  (const_int 0))
		      (label_ref (match_operand 2 ""))
		      (pc)))]
  "")

(define_insn "*branch_v_<code>"
  [(set (pc)
	(if_then_else (eqne (unspec:HI [(match_operand:HI 0 "register_operand" "r")
					(match_operand:HI 1 "register_operand" "r")]
				       UNSPEC_SUBV)
			    (const_int 0))
		      (label_ref (match_operand 2 ""))
		      (pc)))]
  ""
{
  bool vs = <CODE> == NE;
  if (get_attr_length (insn) == 3)
    return vs ? "br\tvs, %0, %1, %l2" : "br\tvc, %0, %1, %l2";
  return (vs ? "br\tvc, %0, %1, .Lb%=\n\tjmp\t%l2\n.Lb%=:"
	  : "br\tvs, %0, %1, .Lb%=\n\tjmp\t%l2\n.Lb%=:");
}
  [(set (attr "length")
	(if_then_else (and (ge (minus (match_dup 2) (pc)) (const_int -120))
			   (le (minus (match_dup 2) (pc)) (const_int 120)))
		      (const_int 3) (const_int 6)))])

;; brset and brclr: a masked test against zero.
(define_insn "*brmask_<code>"
  [(set (pc)
	(if_then_else (eqne (and:HI (match_operand:HI 0 "register_operand" "r")
				    (match_operand:HI 1 "fructus_mask_operand" "K"))
			    (const_int 0))
		      (label_ref (match_operand 2 ""))
		      (pc)))]
  ""
{
  return fructus_output_bitbranch (operands, <CODE> == NE,
				   get_attr_length (insn));
}
  [(set (attr "length")
	(if_then_else (and (ge (minus (match_dup 2) (pc)) (const_int -120))
			   (le (minus (match_dup 2) (pc)) (const_int 120)))
		      (const_int 3) (const_int 6)))])

(define_insn "*brmask_<code>_rev"
  [(set (pc)
	(if_then_else (eqne (and:HI (match_operand:HI 0 "register_operand" "r")
				    (match_operand:HI 1 "fructus_mask_operand" "K"))
			    (const_int 0))
		      (pc)
		      (label_ref (match_operand 2 ""))))]
  ""
{
  return fructus_output_bitbranch (operands, <CODE> == EQ,
				   get_attr_length (insn));
}
  [(set (attr "length")
	(if_then_else (and (ge (minus (match_dup 2) (pc)) (const_int -120))
			   (le (minus (match_dup 2) (pc)) (const_int 120)))
		      (const_int 3) (const_int 6)))])

(define_insn "*brbit_<code>"
  [(set (pc)
	(if_then_else (eqne (zero_extract:HI
			      (match_operand:HI 0 "register_operand" "r")
			      (const_int 1)
			      (match_operand 1 "fructus_bitpos_operand" ""))
			    (const_int 0))
		      (label_ref (match_operand 2 ""))
		      (pc)))]
  ""
{
  operands[1] = gen_int_mode (HOST_WIDE_INT_1 << INTVAL (operands[1]), HImode);
  return fructus_output_bitbranch (operands, <CODE> == NE,
				   get_attr_length (insn));
}
  [(set (attr "length")
	(if_then_else (and (ge (minus (match_dup 2) (pc)) (const_int -120))
			   (le (minus (match_dup 2) (pc)) (const_int 120)))
		      (const_int 3) (const_int 6)))])

;; -------------------------------------------------------------------------
;; Jumps
;; -------------------------------------------------------------------------

;; There is no wide relative jump: `jmpr' reaches a byte's displacement and
;; `jmp' reaches everywhere absolutely, in the same three bytes and a cycle
;; less.  So this picks by distance itself rather than leaving it to gas.
(define_insn "jump"
  [(set (pc) (label_ref (match_operand 0 "" "")))]
  ""
{
  return get_attr_length (insn) == 2 ? "jmpr\t%l0" : "jmp\t%l0";
}
  [(set (attr "length")
	(if_then_else (and (ge (minus (match_dup 0) (pc)) (const_int -120))
			   (le (minus (match_dup 0) (pc)) (const_int 120)))
		      (const_int 2) (const_int 3)))])

;; A jump through a register is `jmp ra' in two bytes, so the allocator picks
;; the register and nothing has to be moved first.  It used to be `jmp r5' in
;; one byte, and the byte it saved was charged back in allocation: every
;; indirect jump had to get its address into the one register the assembler
;; may also clobber.  Both spellings cost three cycles - what the instruction
;; pays for is reading a register, not fetching a byte.
;;
;; Before either existed the sequence was `mov lr, ra' and `ret', which cost
;; the same three bytes but DESTROYED THE RETURN ADDRESS, so a function with a
;; switch table had to save lr as though it made calls.

(define_insn "indirect_jump"
  [(set (pc) (match_operand:HI 0 "register_operand" "r"))]
  ""
  "jmp\t%0"
  [(set_attr "length" "2")])

(define_insn "tablejump"
  [(set (pc) (match_operand:HI 0 "register_operand" "r"))
   (use (label_ref (match_operand 1 "")))]
  ""
  "jmp\t%0"
  [(set_attr "length" "2")])

;; -------------------------------------------------------------------------
;; Calls.  Each carries its callee's ABI id, from the end-marker cookie,
;; for fructus_insn_callee_abi to read back.
;; -------------------------------------------------------------------------

(define_expand "call"
  [(parallel [(call (match_operand:QI 0 "memory_operand")
		    (match_operand 1 ""))
	      (use (unspec:HI [(match_operand 2 "")] UNSPEC_CALLEE_CC))])]
  ""
{
  fructus_expand_call (operands, 0);
  if (operands[2] == NULL_RTX)
    operands[2] = const0_rtx;
})

(define_insn "*call"
  [(call (mem:QI (match_operand:HI 0 "fructus_call_operand" "i,r"))
	 (match_operand 1 "" ""))
   (use (unspec:HI [(match_operand 2 "const_int_operand" "")]
		   UNSPEC_CALLEE_CC))]
  "!SIBLING_CALL_P (insn)"
  "call\t%0"
  [(set_attr "length" "3,2")])

(define_expand "call_value"
  [(parallel [(set (match_operand 0 "")
		   (call (match_operand:QI 1 "memory_operand")
			 (match_operand 2 "")))
	      (use (unspec:HI [(match_operand 3 "")] UNSPEC_CALLEE_CC))])]
  ""
{
  fructus_expand_call (operands, 1);
  if (operands[3] == NULL_RTX)
    operands[3] = const0_rtx;
})

(define_insn "*call_value"
  [(set (match_operand 0 "" "")
	(call (mem:QI (match_operand:HI 1 "fructus_call_operand" "i,r"))
	      (match_operand 2 "" "")))
   (use (unspec:HI [(match_operand 3 "const_int_operand" "")]
		   UNSPEC_CALLEE_CC))]
  "!SIBLING_CALL_P (insn)"
  "call\t%1"
  [(set_attr "length" "3,2")])

;; -------------------------------------------------------------------------
;; Tail calls.  A tail call is a jump, so the callee's `ret' returns to OUR
;; caller: the epilogue runs first, leaving that return address in lr, and
;; two bytes and a return are saved over `call' followed by `ret'.
;;
;; A SYMBOL GOES IN THE JUMP, ANYTHING ELSE GOES THROUGH r5.  An indirect tail
;; call was impossible until the machine had `jmp ra': through lr it would
;; have destroyed the very return address it exists to pass along.  r5 is safe
;; to stage it in because no epilogue touches r5 - it is caller saved in all
;; three ABIs - so the move below survives the unwinding that the sibcall
;; epilogue does between here and the jump.
;;
;; IT HAS TO BE r5 AND NOT WHATEVER THE ALLOCATOR PICKS, unlike indirect_jump.
;; The epilogue is emitted between the move and the jump, and it pops every
;; callee-saved register the function used - so a target the allocator put in
;; r2, r3 or r4 is overwritten with the caller's value before the jump reads
;; it.  pr34456 at -Os did exactly that: `pop r2, r3, lr' then `jmp r2'.
;; Nothing in the jump's operand tells the allocator about the epilogue, since
;; the epilogue does not exist yet when registers are allocated.
;;
;; Whether a tail call is allowed AT ALL is a question about the sliding
;; convention, not about the frame: see fructus_function_ok_for_sibcall.
;; -------------------------------------------------------------------------

(define_expand "sibcall"
  [(parallel [(call (match_operand:QI 0 "memory_operand")
		    (match_operand 1 ""))
	      (use (unspec:HI [(match_operand 2 "")] UNSPEC_CALLEE_CC))])]
  ""
{
  rtx addr = XEXP (operands[0], 0);
  if (!fructus_sibcall_operand (addr, Pmode))
    {
      emit_move_insn (gen_rtx_REG (Pmode, R5_REGNUM), addr);
      operands[0] = gen_rtx_MEM (QImode, gen_rtx_REG (Pmode, R5_REGNUM));
    }
  if (operands[2] == NULL_RTX)
    operands[2] = const0_rtx;
})

(define_insn "*sibcall_reg"
  [(call (mem:QI (reg:HI R5_REGNUM))
	 (match_operand 0 "" ""))
   (use (unspec:HI [(match_operand 1 "const_int_operand" "")]
		   UNSPEC_CALLEE_CC))]
  "SIBLING_CALL_P (insn)"
  "jmp\tr5"
  [(set_attr "length" "2")])

(define_insn "*sibcall"
  [(call (mem:QI (match_operand:HI 0 "fructus_sibcall_operand" "i"))
	 (match_operand 1 "" ""))
   (use (unspec:HI [(match_operand 2 "const_int_operand" "")]
		   UNSPEC_CALLEE_CC))]
  "SIBLING_CALL_P (insn)"
  "jmp\t%0"
  [(set_attr "length" "3")])

(define_expand "sibcall_value"
  [(parallel [(set (match_operand 0 "")
		   (call (match_operand:QI 1 "memory_operand")
			 (match_operand 2 "")))
	      (use (unspec:HI [(match_operand 3 "")] UNSPEC_CALLEE_CC))])]
  ""
{
  rtx addr = XEXP (operands[1], 0);
  if (!fructus_sibcall_operand (addr, Pmode))
    {
      emit_move_insn (gen_rtx_REG (Pmode, R5_REGNUM), addr);
      operands[1] = gen_rtx_MEM (QImode, gen_rtx_REG (Pmode, R5_REGNUM));
    }
  if (operands[3] == NULL_RTX)
    operands[3] = const0_rtx;
})

(define_insn "*sibcall_value_reg"
  [(set (match_operand 0 "" "")
	(call (mem:QI (reg:HI R5_REGNUM))
	      (match_operand 1 "" "")))
   (use (unspec:HI [(match_operand 2 "const_int_operand" "")]
		   UNSPEC_CALLEE_CC))]
  "SIBLING_CALL_P (insn)"
  "jmp\tr5"
  [(set_attr "length" "2")])

(define_insn "*sibcall_value"
  [(set (match_operand 0 "" "")
	(call (mem:QI (match_operand:HI 1 "fructus_sibcall_operand" "i"))
	      (match_operand 2 "" "")))
   (use (unspec:HI [(match_operand 3 "const_int_operand" "")]
		   UNSPEC_CALLEE_CC))]
  "SIBLING_CALL_P (insn)"
  "jmp\t%1"
  [(set_attr "length" "3")])

;; -------------------------------------------------------------------------
;; Prologue and epilogue
;; -------------------------------------------------------------------------

(define_expand "prologue"
  [(const_int 0)]
  ""
{
  fructus_expand_prologue ();
  DONE;
})

(define_expand "epilogue"
  [(return)]
  ""
{
  fructus_expand_epilogue (false);
  DONE;
})

;; The same unwinding, but the jump that follows it is the tail call, so
;; there is no `ret' of our own.  lr is restored here like any other saved
;; register: it is what the callee will return through.
(define_expand "sibcall_epilogue"
  [(const_int 0)]
  ""
{
  fructus_expand_epilogue (true);
  DONE;
})

(define_insn "fructus_return"
  [(return)
   (use (reg:HI LR_REGNUM))]
  "reload_completed"
  "ret"
  [(set_attr "length" "1")])

;; -------------------------------------------------------------------------
;; Miscellany
;; -------------------------------------------------------------------------

(define_insn "nop"
  [(const_int 0)]
  ""
  "nop"
  [(set_attr "length" "1")])

(define_insn "blockage"
  [(unspec_volatile [(const_int 0)] UNSPECV_BLOCKAGE)]
  ""
  ""
  [(set_attr "length" "0")])
