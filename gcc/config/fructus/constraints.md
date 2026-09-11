;; Constraint definitions for Fructus.
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

;; Every constant constraint is a question about whether SOME encoding can
;; carry the value; gas then chooses which.  The tables live in fructus-isa.h,
;; generated from isa/fructus.toml.

(define_constraint "I"
  "A signed 10-bit constant: every two-register immediate form reaches it."
  (and (match_code "const_int")
       (match_test "fructus_imm10_p (ival)")))

(define_constraint "K"
  "An immbit5 or immask5 constant: a single bit, a field, a stripe, or the
   complement of one.  Tied forms, and the mask branches, only."
  (and (match_code "const_int")
       (match_test "fructus_mask5_p (ival)")))

(define_constraint "J"
  "A shift count the tied imm5 form carries: 0 to 15."
  (and (match_code "const_int")
       (match_test "IN_RANGE (ival, 0, 15)")))

(define_constraint "S"
  "A shift count in the shift3 table, for the untied shift form."
  (and (match_code "const_int")
       (match_test "fructus_shift3_p (ival)")))

;; The compare-and-branch constants.  condimm5 pairs a condition with its
;; constant, so whether `br ra, #k' can test k depends on the comparison as
;; well as on k.  A constraint cannot see the operator, so there is one per
;; comparison code and width - "C" + cond3 name + h (16-bit br) or q (br8).
;; An insn condition alone would not do: LRA can substitute a constant for a
;; pseudo it knows is equivalent, checking only the constraint.

(define_constraint "Ceqh" "br eq constant."
  (and (match_code "const_int")
       (match_test "fructus_cbranch_imm_p (EQ, HImode, ival)")))
(define_constraint "Cneh" "br ne constant."
  (and (match_code "const_int")
       (match_test "fructus_cbranch_imm_p (NE, HImode, ival)")))
(define_constraint "Clth" "br lt constant."
  (and (match_code "const_int")
       (match_test "fructus_cbranch_imm_p (LT, HImode, ival)")))
(define_constraint "Cleh" "br le constant."
  (and (match_code "const_int")
       (match_test "fructus_cbranch_imm_p (LE, HImode, ival)")))
(define_constraint "Cgth" "br gt constant."
  (and (match_code "const_int")
       (match_test "fructus_cbranch_imm_p (GT, HImode, ival)")))
(define_constraint "Cgeh" "br ge constant."
  (and (match_code "const_int")
       (match_test "fructus_cbranch_imm_p (GE, HImode, ival)")))
(define_constraint "Cloh" "br lo constant."
  (and (match_code "const_int")
       (match_test "fructus_cbranch_imm_p (LTU, HImode, ival)")))
(define_constraint "Clsh" "br ls constant."
  (and (match_code "const_int")
       (match_test "fructus_cbranch_imm_p (LEU, HImode, ival)")))
(define_constraint "Chih" "br hi constant."
  (and (match_code "const_int")
       (match_test "fructus_cbranch_imm_p (GTU, HImode, ival)")))
(define_constraint "Chsh" "br hs constant."
  (and (match_code "const_int")
       (match_test "fructus_cbranch_imm_p (GEU, HImode, ival)")))

(define_constraint "Ceqq" "br8 eq constant."
  (and (match_code "const_int")
       (match_test "fructus_cbranch_imm_p (EQ, QImode, ival)")))
(define_constraint "Cneq" "br8 ne constant."
  (and (match_code "const_int")
       (match_test "fructus_cbranch_imm_p (NE, QImode, ival)")))
(define_constraint "Cltq" "br8 lt constant."
  (and (match_code "const_int")
       (match_test "fructus_cbranch_imm_p (LT, QImode, ival)")))
(define_constraint "Cleq" "br8 le constant."
  (and (match_code "const_int")
       (match_test "fructus_cbranch_imm_p (LE, QImode, ival)")))
(define_constraint "Cgtq" "br8 gt constant."
  (and (match_code "const_int")
       (match_test "fructus_cbranch_imm_p (GT, QImode, ival)")))
(define_constraint "Cgeq" "br8 ge constant."
  (and (match_code "const_int")
       (match_test "fructus_cbranch_imm_p (GE, QImode, ival)")))
(define_constraint "Cloq" "br8 lo constant."
  (and (match_code "const_int")
       (match_test "fructus_cbranch_imm_p (LTU, QImode, ival)")))
(define_constraint "Clsq" "br8 ls constant."
  (and (match_code "const_int")
       (match_test "fructus_cbranch_imm_p (LEU, QImode, ival)")))
(define_constraint "Chiq" "br8 hi constant."
  (and (match_code "const_int")
       (match_test "fructus_cbranch_imm_p (GTU, QImode, ival)")))
(define_constraint "Chsq" "br8 hs constant."
  (and (match_code "const_int")
       (match_test "fructus_cbranch_imm_p (GEU, QImode, ival)")))
