/* 64-bit multiplication for Fructus parts with a multiplier.
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

/* __muldi3_mul is what a long long product calls with -mmul, as __muldi3
   from libgcc2.c is without it.  The same algorithm:

	(uh:ul) * (vh:vl) = ul*vl + ((ul*vh + uh*vl) << 32)

   with the 64-bit product of the low words and the two 32-bit cross terms
   from lib1funcs.S's routines that use mul.  THEY ARE CALLED BY NAME, and
   not by writing the products in C, because this library is built without
   -mmul - it has to run on a part without a multiplier - so C would reach
   the shift-and-add routines instead.  Nothing calls this unless it was
   compiled with -mmul, so nothing reaches mul on a part without one.  */

typedef unsigned int uint32_type __attribute__ ((mode (SI)));
typedef unsigned int uint64_type __attribute__ ((mode (DI)));
typedef          int sint64_type __attribute__ ((mode (DI)));

extern uint64_type __umulsidi3_mul (uint32_type, uint32_type);
extern uint32_type __mulsi3_mul (uint32_type, uint32_type);
extern sint64_type __muldi3_mul (sint64_type, sint64_type);

sint64_type
__muldi3_mul (sint64_type u, sint64_type v)
{
  uint32_type ul = u, uh = (uint64_type) u >> 32;
  uint32_type vl = v, vh = (uint64_type) v >> 32;
  uint64_type w = __umulsidi3_mul (ul, vl);
  uint32_type hi = (uint32_type) (w >> 32)
		   + __mulsi3_mul (ul, vh) + __mulsi3_mul (uh, vl);
  return (sint64_type) ((uint64_type) hi << 32 | (uint32_type) w);
}
