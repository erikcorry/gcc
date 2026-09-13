;; Predicate definitions for Fructus.
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

;; A register, or a constant some form of add, and, or or xor can carry:
;; imm10 in any form, and the two 5-bit mask tables in the tied forms.
(define_predicate "fructus_alu_operand"
  (ior (match_operand 0 "register_operand")
       (and (match_code "const_int")
	    (match_test "fructus_alu_imm_p (PLUS, INTVAL (op), true)"))))

;; A register or an imm10 - rsb and iseq, which have no mask forms.
(define_predicate "fructus_imm10_operand"
  (ior (match_operand 0 "register_operand")
       (and (match_code "const_int")
	    (match_test "fructus_imm10_p (INTVAL (op))"))))

(define_predicate "fructus_shift_operand"
  (ior (match_operand 0 "register_operand")
       (and (match_code "const_int")
	    (match_test "IN_RANGE (INTVAL (op), 0, 15)"))))

;; Whether a constant suits a particular comparison is decided by the insn
;; condition and the per-code constraints; this admits any.
(define_predicate "fructus_cmp_operand"
  (ior (match_operand 0 "register_operand")
       (match_code "const_int")))

(define_predicate "fructus_mask_operand"
  (and (match_code "const_int")
       (match_test "fructus_mask5_p (INTVAL (op))")))

(define_predicate "fructus_bitpos_operand"
  (and (match_code "const_int")
       (match_test "IN_RANGE (INTVAL (op), 0, 15)")))

;; What a push may push: any register but sp.  GCC's (set (mem (pre_dec sp))
;; (reg sp)) stores the value sp had BEFORE the decrement, and the ISA's push
;; stores the value after it - the ambiguity isa/fructus.toml leaves open.
;; Refusing sp here makes GCC copy it to another register first, so the
;; question never reaches the hardware.
(define_predicate "fructus_push_operand"
  (and (match_operand 0 "register_operand")
       (not (match_test "reg_mentioned_p (stack_pointer_rtx, op)"))))

(define_predicate "fructus_call_operand"
  (ior (match_code "symbol_ref,label_ref,const,const_int")
       (match_operand 0 "register_operand")))

;; What a tail call can reach with a bare `jmpr', which is an address and not
;; a register.  A target that is not one of these is staged in r5 and reached
;; with `jmp r5' instead; see the sibcall patterns.
(define_predicate "fructus_sibcall_operand"
  (match_code "symbol_ref,label_ref,const"))
