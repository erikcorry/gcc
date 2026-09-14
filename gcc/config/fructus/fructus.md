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
  [(LR_REGNUM  0)
   (SP_REGNUM  1)
   (R5_REGNUM  2)
   (R4_REGNUM  3)
   (R0_REGNUM  7)])

(define_c_enum "unspec" [UNSPEC_CALLEE_CC])
(define_c_enum "unspecv" [UNSPECV_BLOCKAGE])

(define_attr "length" "" (const_int 2))

(define_mode_iterator QHI [QI HI])
(define_mode_iterator SISF [SI SF])
(define_mode_attr bw [(QI "8") (HI "")])
(define_mode_attr mw [(QI "q") (HI "h")])

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

(define_insn "pushqi1"
  [(set (mem:QI (pre_dec:HI (reg:HI SP_REGNUM)))
	(match_operand:QI 0 "fructus_push_operand" "r"))]
  ""
  "push8\t%0")

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
		 (match_operand:HI 2 "fructus_alu_operand" "IK,I,r")))]
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

(define_expand "cbranch<mode>4"
  [(set (pc)
	(if_then_else (match_operator 0 "ordered_comparison_operator"
			[(match_operand:QHI 1 "register_operand")
			 (match_operand:QHI 2 "nonmemory_operand")])
		      (label_ref (match_operand 3 ""))
		      (pc)))]
  ""
{
  fructus_expand_cbranch (operands, <MODE>mode);
})

(define_code_iterator any_cond [eq ne lt le gt ge ltu leu gtu geu])
(define_code_attr cc [(eq "eq") (ne "ne") (lt "lt") (le "le") (gt "gt")
		      (ge "ge") (ltu "lo") (leu "ls") (gtu "hi") (geu "hs")])

(define_insn "*cbranch<mode>_<code>"
  [(set (pc)
	(if_then_else (any_cond (match_operand:QHI 0 "register_operand" "r,r")
				(match_operand:QHI 1 "fructus_cmp_operand"
						   "r,C<cc><mw>"))
		      (label_ref (match_operand 2 ""))
		      (pc)))]
  "fructus_cbranch_ok_p (operands[1], <CODE>, <MODE>mode)"
{
  return fructus_output_cbranch (operands, <CODE>, <MODE>mode, false,
				 get_attr_length (insn));
}
  [(set (attr "length")
	(if_then_else
	  (and (ge (minus (match_dup 2) (pc)) (const_int -120))
	       (le (minus (match_dup 2) (pc)) (const_int 120)))
	  (if_then_else
	    (match_test "fructus_cbranch_ok_p (operands[1], <CODE>, <MODE>mode)")
	    (const_int 3)
	    (if_then_else
	      (match_test "fructus_cbranch_ok_p (operands[1],
				 reverse_condition (<CODE>), <MODE>mode)")
	      (const_int 6) (const_int 8)))
	  (if_then_else
	    (match_test "fructus_cbranch_ok_p (operands[1],
			       reverse_condition (<CODE>), <MODE>mode)")
	    (const_int 6) (const_int 8))))])

;; The same with the arms exchanged, which is what inverting a jump produces
;; when the reversed comparison has no encoding of its own.
(define_insn "*cbranch<mode>_<code>_rev"
  [(set (pc)
	(if_then_else (any_cond (match_operand:QHI 0 "register_operand" "r,r")
				(match_operand:QHI 1 "fructus_cmp_operand"
						   "r,C<cc><mw>"))
		      (pc)
		      (label_ref (match_operand 2 ""))))]
  "fructus_cbranch_ok_p (operands[1], <CODE>, <MODE>mode)"
{
  return fructus_output_cbranch (operands, <CODE>, <MODE>mode, true,
				 get_attr_length (insn));
}
  [(set (attr "length")
	(if_then_else
	  (and (ge (minus (match_dup 2) (pc)) (const_int -120))
	       (le (minus (match_dup 2) (pc)) (const_int 120)))
	  (if_then_else
	    (match_test "fructus_cbranch_ok_p (operands[1],
			       reverse_condition (<CODE>), <MODE>mode)")
	    (const_int 3)
	    (const_int 6))
	  (const_int 6)))])

;; brset and brclear: a masked test against zero.
(define_code_iterator eqne [eq ne])

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

;; gas relaxes jmpr between two and three bytes on its own.
(define_insn "jump"
  [(set (pc) (label_ref (match_operand 0 "" "")))]
  ""
  "jmpr\t%l0"
  [(set (attr "length")
	(if_then_else (and (ge (minus (match_dup 0) (pc)) (const_int -120))
			   (le (minus (match_dup 0) (pc)) (const_int 120)))
		      (const_int 2) (const_int 3)))])

;; A jump through a register is `jmp r5', so the address goes to r5 first.
;; It used to go through lr - `mov lr, ra' and `ret' - which cost the same
;; three bytes but DESTROYED THE RETURN ADDRESS, so a function with a switch
;; table had to save lr as though it made calls.  Now it does not.
;;
;; r5 rather than a register the allocator picks, because there is no class
;; that names one register, and because r5 is the one that is expendable
;; everywhere: caller saved in all three ABIs, never an argument or a return
;; value, and untouched by any epilogue - which is what lets a tail call
;; through a pointer put its target here and still unwind afterwards.
(define_expand "indirect_jump"
  [(set (pc) (match_operand:HI 0 "register_operand"))]
  ""
{
  emit_move_insn (gen_rtx_REG (HImode, R5_REGNUM), operands[0]);
  operands[0] = gen_rtx_REG (HImode, R5_REGNUM);
})

(define_insn "*indirect_jump"
  [(set (pc) (reg:HI R5_REGNUM))]
  ""
  "jmp\tr5"
  [(set_attr "length" "1")])

(define_expand "tablejump"
  [(parallel [(set (pc) (match_operand:HI 0 "register_operand"))
	      (use (label_ref (match_operand 1 "")))])]
  ""
{
  emit_move_insn (gen_rtx_REG (HImode, R5_REGNUM), operands[0]);
  operands[0] = gen_rtx_REG (HImode, R5_REGNUM);
})

(define_insn "*tablejump"
  [(set (pc) (reg:HI R5_REGNUM))
   (use (label_ref (match_operand 0 "")))]
  ""
  "jmp\tr5"
  [(set_attr "length" "1")])

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
;; call was impossible until the machine had `jmp r5': through lr it would
;; have destroyed the very return address it exists to pass along.  r5 is safe
;; to stage it in because no epilogue touches r5 - it is caller saved in all
;; three ABIs - so the move below survives the unwinding that the sibcall
;; epilogue does between here and the jump.
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
  [(set_attr "length" "1")])

(define_insn "*sibcall"
  [(call (mem:QI (match_operand:HI 0 "fructus_sibcall_operand" "i"))
	 (match_operand 1 "" ""))
   (use (unspec:HI [(match_operand 2 "const_int_operand" "")]
		   UNSPEC_CALLEE_CC))]
  "SIBLING_CALL_P (insn)"
  "jmpr\t%0"
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
  [(set_attr "length" "1")])

(define_insn "*sibcall_value"
  [(set (match_operand 0 "" "")
	(call (mem:QI (match_operand:HI 1 "fructus_sibcall_operand" "i"))
	      (match_operand 2 "" "")))
   (use (unspec:HI [(match_operand 3 "const_int_operand" "")]
		   UNSPEC_CALLEE_CC))]
  "SIBLING_CALL_P (insn)"
  "jmpr\t%1"
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
