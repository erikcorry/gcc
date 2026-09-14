/* Prototypes for Fructus functions used in the md file and fructus.h.
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

#ifndef GCC_FRUCTUS_PROTOS_H
#define GCC_FRUCTUS_PROTOS_H

extern bool fructus_imm5_p (HOST_WIDE_INT);
extern bool fructus_imm10_p (HOST_WIDE_INT);
extern bool fructus_imm3_p (HOST_WIDE_INT);
extern bool fructus_shift3_p (HOST_WIDE_INT);
extern bool fructus_mask5_p (HOST_WIDE_INT);

extern HOST_WIDE_INT fructus_initial_elimination_offset (int, int);
extern void fructus_expand_prologue (void);
extern void fructus_expand_epilogue (bool);

#ifdef TREE_CODE
extern void fructus_init_cumulative_args (CUMULATIVE_ARGS *, tree,
					  machine_mode);
#endif

#ifdef RTX_CODE
extern bool fructus_alu_imm_p (enum rtx_code, HOST_WIDE_INT, bool);
extern bool fructus_cbranch_imm_p (enum rtx_code, machine_mode,
				   HOST_WIDE_INT);
extern bool fructus_cbranch_ok_p (rtx, enum rtx_code, machine_mode);

extern int fructus_move_length (rtx *);
extern bool fructus_same_reg_p (rtx, rtx);
extern int fructus_double_move_length (rtx *);
extern int fructus_alu_length (enum rtx_code, rtx *);
extern int fructus_rsb_length (rtx *);
extern int fructus_iseq_length (rtx *, bool);

extern bool fructus_expand_move (rtx *, machine_mode);
extern bool fructus_expand_word_shift (rtx *, enum rtx_code);
extern void fructus_split_double_move (rtx *, machine_mode);
extern bool fructus_expand_sub (rtx *);
extern void fructus_expand_cbranch (rtx *, machine_mode);
extern void fructus_expand_call (rtx *, int);

extern const char *fructus_output_alu (enum rtx_code, rtx *);
extern const char *fructus_output_cbranch (rtx *, enum rtx_code,
					   machine_mode, bool, int);
extern const char *fructus_output_bitbranch (rtx *, bool, int);
extern const char *fructus_output_iseq (rtx *, bool);
#endif

#endif /* GCC_FRUCTUS_PROTOS_H */
