/* 16-bit bit counts for Fructus.
   Copyright (C) 2026 Free Software Foundation, Inc.

   This file is part of GCC.

   GCC is free software; you can redistribute it and/or modify it under
   the terms of the GNU General Public License as published by the Free
   Software Foundation; either version 3, or (at your option) any later
   version.

   GCC is distributed in the hope that it will be useful, but WITHOUT ANY
   WARRANTY; without even the implied warranty of MERCHANTABILITY or
   FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
   for more details.

   Under Section 7 of GPL version 3, you are granted additional
   permissions described in the GCC Runtime Library Exception, version
   3.1, as published by the Free Software Foundation.

   You should have received a copy of the GNU General Public License and
   a copy of the GCC Runtime Library Exception along with this program;
   see the files COPYING3 and COPYING.RUNTIME respectively.  If not, see
   <http://www.gnu.org/licenses/>.  */

/* msp430's file, which builds libgcc2.c's clz, ctz, ffs, parity and
   popcount with 16-bit words, giving __clzhi2 and the rest, plus clrsb,
   which msp430 has no use for: gcc expands the others inline, but at -Os
   __builtin_clrsb on an int calls __clrsbhi2.  */

#define L_clrsbsi2
#include "../msp430/lib2bitcountHI.c"
