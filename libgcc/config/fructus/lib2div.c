/* 16- and 32-bit division and remainder for Fructus.
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

/* The machine has no divide, so these are shift-and-subtract.  The
   algorithm is msp430's, which is generic C; this file only instantiates it
   twice without msp430's ABI aliases.  */

typedef          int sint32_type __attribute__ ((mode (SI)));
typedef unsigned int uint32_type __attribute__ ((mode (SI)));
typedef          int sint16_type __attribute__ ((mode (HI)));
typedef unsigned int uint16_type __attribute__ ((mode (HI)));
typedef int word_type __attribute__ ((mode (__word__)));

#define C3B(a,b,c) a##b##c
#define C3(a,b,c) C3B(a,b,c)

/* The 16-bit half is not here: __udivmodhi4 and its four wrappers are hand
   written, in lib1funcs.S.  They cost 46 cycles where this loop costs 326,
   which matters because dividing by ten is how every number is printed.  */

#define UINT_TYPE	uint32_type
#define SINT_TYPE	sint32_type
#define BITS_MINUS_1	31
#define NAME_MODE	si
#include "../msp430/msp430-divmod.h"
