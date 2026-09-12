/* 32-bit division and remainder for Fructus.
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

/* The 16-bit routines are hand written, in lib1funcs.S.  These are C, and
   the point of writing them out rather than instantiating a generic loop is
   what the generic loop cannot say:

     A 16-BIT DIVIDEND IS THE COMMON CASE for a `long' on a 16-bit machine,
     and it is not a 32-bit division at all.  Either the divisor is larger,
     which is the whole answer, or both values fit in a word and the
     hand-written 16-bit routine does it in 46 cycles.

     clz ALIGNS THE DIVISOR IN ONE INSTRUCTION.  A generic C divmod walks the
     divisor up a bit at a time - up to 31 iterations before any division
     happens - because it cannot assume the machine can count leading zeros.
     This one can: GCC turns __builtin_clzl into two clz instructions and a
     branch, because the port has clz for a word.

     ONE CALL GIVES BOTH RESULTS.  The generic version takes a flag saying
     which result is wanted and throws the other away, so a program that
     wants both divides twice.

   THE REMAINDER COMES BACK THROUGH A POINTER, and that is measured rather
   than assumed.  The tidier interface is one 64-bit value with the quotient
   on top - which is what isa/abi.s returns in r0:r1 and r2:r3, and what
   TARGET_EXPAND_DIVMOD_LIBFUNC would want - and it was tried twice:

     64-bit pair, composed with __ashldi3     1579 cycles a call
     64-bit pair, once a shift by a whole
       word became register moves in the port 1556
     remainder through a pointer              1373

   Free shifts took most of the cost out of the pair, and the rest is the
   marshalling either side of it: four words in and out of registers against
   one store and one load.  */

typedef          int sint32_type __attribute__ ((mode (SI)));
typedef unsigned int uint32_type __attribute__ ((mode (SI)));
typedef unsigned int uint16_type __attribute__ ((mode (HI)));

/* lib1funcs.S: the quotient in r0 and the remainder in r1, which is one
   32-bit value with the quotient on top.  */
extern uint32_type __udivmodhi4 (uint16_type, uint16_type);

uint32_type __udivmodsi4 (uint32_type, uint32_type, uint32_type *);
uint32_type __udivsi3 (uint32_type, uint32_type);
uint32_type __umodsi3 (uint32_type, uint32_type);
sint32_type __divsi3 (sint32_type, sint32_type);
sint32_type __modsi3 (sint32_type, sint32_type);

/* Returns the quotient, and stores the remainder through REM.  */

uint32_type
__udivmodsi4 (uint32_type num, uint32_type den, uint32_type *rem)
{
  /* Nothing to divide.  Also every case where the divisor is the larger, so
     what follows may assume den <= num.  */
  if (den > num)
    {
      *rem = num;
      return 0;
    }

  /* Both in one word: the hand-written routine does it in 46 cycles.  */
  if (num <= 0xffffU)
    {
      uint32_type qr = __udivmodhi4 ((uint16_type) num, (uint16_type) den);
      *rem = qr & 0xffffU;
      return qr >> 16;
    }

  /* Restoring division, one bit of quotient per iteration.  den is zero here
     only if num is too, which the first test caught, so clz is defined -
     and it is what makes the alignment a few instructions instead of a
     loop.  */
  int s = __builtin_clzl (den) - __builtin_clzl (num);
  uint32_type d = den << s;
  uint32_type q = 0;

  do
    {
      q <<= 1;
      if (num >= d)
	{
	  num -= d;
	  q |= 1;
	}
      d >>= 1;
    }
  while (s--);

  *rem = num;
  return q;
}

uint32_type
__udivsi3 (uint32_type a, uint32_type b)
{
  uint32_type r;
  return __udivmodsi4 (a, b, &r);
}

uint32_type
__umodsi3 (uint32_type a, uint32_type b)
{
  uint32_type r;
  __udivmodsi4 (a, b, &r);
  return r;
}

/* C truncates a quotient towards zero and gives the remainder the dividend's
   sign, so the magnitudes divide and the signs go back on afterwards.  */

sint32_type
__divsi3 (sint32_type a, sint32_type b)
{
  int neg = (a < 0) ^ (b < 0);
  uint32_type r;
  uint32_type q = __udivmodsi4 (a < 0 ? -a : a, b < 0 ? -b : b, &r);
  return neg ? -(sint32_type) q : (sint32_type) q;
}

sint32_type
__modsi3 (sint32_type a, sint32_type b)
{
  uint32_type r;
  __udivmodsi4 (a < 0 ? -a : a, b < 0 ? -b : b, &r);
  return a < 0 ? -(sint32_type) r : (sint32_type) r;
}
