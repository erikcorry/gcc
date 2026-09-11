/* 32-bit shifts by a variable count, for Fructus.
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

/* A 32-bit value is two 16-bit words, and every shift the machine has is a
   16-bit one taking any count, so each of these is at most three shifts and
   an or - no loop.  The only 32-bit shifts written below are by exactly 16,
   which GCC does with word moves; a variable one would call back in here.  */

typedef unsigned int uint32_type __attribute__ ((mode (SI)));
typedef          int sint32_type __attribute__ ((mode (SI)));
typedef unsigned int uint16_type __attribute__ ((mode (HI)));
typedef          int sint16_type __attribute__ ((mode (HI)));
typedef int word_type __attribute__ ((mode (__word__)));

uint32_type __ashlsi3 (uint32_type, word_type);
uint32_type __lshrsi3 (uint32_type, word_type);
sint32_type __ashrsi3 (sint32_type, word_type);

static inline uint32_type
join (uint16_type hi, uint16_type lo)
{
  return ((uint32_type) hi << 16) | lo;
}

uint32_type
__ashlsi3 (uint32_type a, word_type n)
{
  uint16_type lo = a, hi = a >> 16;

  n &= 31;
  if (n >= 16)
    return join (lo << (n - 16), 0);
  if (n == 0)
    return a;
  return join ((hi << n) | (lo >> (16 - n)), lo << n);
}

uint32_type
__lshrsi3 (uint32_type a, word_type n)
{
  uint16_type lo = a, hi = a >> 16;

  n &= 31;
  if (n >= 16)
    return join (0, hi >> (n - 16));
  if (n == 0)
    return a;
  return join (hi >> n, (lo >> n) | (hi << (16 - n)));
}

sint32_type
__ashrsi3 (sint32_type a, word_type n)
{
  uint16_type lo = a;
  sint16_type hi = (uint32_type) a >> 16;

  n &= 31;
  if (n >= 16)
    return join (hi >> 15, hi >> (n - 16));
  if (n == 0)
    return a;
  return join (hi >> n, (lo >> n) | ((uint16_type) hi << (16 - n)));
}
