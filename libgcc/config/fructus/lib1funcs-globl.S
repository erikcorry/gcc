/* The entry points of lib1funcs.S, made global.

   lib1funcs.S is a byte-for-byte copy of libgcc/lib1funcs.s in the fructus
   repository, which is written in the customasm dialect: one flat namespace,
   no .globl.  Declaring the exports here keeps the copy identical to the
   file its suite tests.  */

	.text
	.globl	__mulhi3
	.globl	__umulhisi3
	.globl	__mulhisi3
	.globl	__mulsi3
	.globl	__umulsidi3
	.globl	__udivmodhi4
	.globl	__udivhi3
	.globl	__umodhi3
	.globl	__divhi3
	.globl	__modhi3

#include "lib1funcs.S"
