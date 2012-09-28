/*
 * PowerPC atomic bit operations.
 *
 * Merged version by David Gibson <david@gibson.dropbear.id.au>.
 * Based on ppc64 versions by: Dave Engebretsen, Todd Inglett, Don
 * Reed, Pat McCarthy, Peter Bergner, Anton Blanchard.  They
 * originally took it from the ppc32 code.
 *
 * Within a word, bits are numbered LSB first.  Lot's of places make
 * this assumption by directly testing bits with (val & (1<<nr)).
 * This can cause confusion for large (> 1 word) bitmaps on a
 * big-endian system because, unlike little endian, the number of each
 * bit depends on the word size.
 *
 * The bitop functions are defined to work on unsigned longs, so for a
 * ppc64 system the bits end up numbered:
 *   |63..............0|127............64|191...........128|255...........196|
 * and on ppc32:
 *   |31.....0|63....31|95....64|127...96|159..128|191..160|223..192|255..224|
 *
 * There are a few little-endian macros used mostly for filesystem
 * bitmaps, these work on similar bit arrays layouts, but
 * byte-oriented:
 *   |7...0|15...8|23...16|31...24|39...32|47...40|55...48|63...56|
 *
 * The main difference is that bit 3-5 (64b) or 3-4 (32b) in the bit
 * number field needs to be reversed compared to the big-endian bit
 * fields. This can be achieved by XOR with 0x38 (64b) or 0x18 (32b).
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#ifndef _ASM_POWERPC_BITOPS_H
#define _ASM_POWERPC_BITOPS_H

#define BITS_PER_LONG		64
#define PPC_LLARX		"lwarx "
#define PPC_STLCX		"stwcx. "
#define PPC405_ERR77(ra, rb)	"dcbt " #ra #rb
#define PPC_CNTLZL		"cntlzw "

#define likely(x)	__builtin_expect((x),1)
#define unlikely(x)	__builtin_expect((x),0)

/*
 * clear_bit doesn't imply a memory barrier
 */
#define smp_mb__before_clear_bit()	smp_mb()
#define smp_mb__after_clear_bit()	smp_mb()

#define BITOP_MASK(nr)		(1UL << ((nr) % BITS_PER_LONG))
#define BITOP_WORD(nr)		((nr) / BITS_PER_LONG)
#define BITOP_LE_SWIZZLE	((BITS_PER_LONG-1) & ~0x7)

static __inline__ void set_bit(int nr, volatile unsigned long *addr)
{
	unsigned long old;
	unsigned long mask = BITOP_MASK(nr);
	unsigned long *p = ((unsigned long *)addr) + BITOP_WORD(nr);

	__asm__ __volatile__(
"1:"	PPC_LLARX "%0,0,%3	# set_bit\n"
	"or	%0,%0,%2\n"
	PPC405_ERR77(0,%3)
	PPC_STLCX "%0,0,%3\n"
	"bne-	1b"
	: "=&r"(old), "=m"(*p)
	: "r"(mask), "r"(p), "m"(*p)
	: "cc" );
}

static __inline__ void clear_bit(int nr, volatile unsigned long *addr)
{
	unsigned long old;
	unsigned long mask = BITOP_MASK(nr);
	unsigned long *p = ((unsigned long *)addr) + BITOP_WORD(nr);

	__asm__ __volatile__(
"1:"	PPC_LLARX "%0,0,%3	# clear_bit\n"
	"andc	%0,%0,%2\n"
	PPC405_ERR77(0,%3)
	PPC_STLCX "%0,0,%3\n"
	"bne-	1b"
	: "=&r"(old), "=m"(*p)
	: "r"(mask), "r"(p), "m"(*p)
	: "cc" );
}

static __inline__ void change_bit(int nr, volatile unsigned long *addr)
{
	unsigned long old;
	unsigned long mask = BITOP_MASK(nr);
	unsigned long *p = ((unsigned long *)addr) + BITOP_WORD(nr);

	__asm__ __volatile__(
"1:"	PPC_LLARX "%0,0,%3	# change_bit\n"
	"xor	%0,%0,%2\n"
	PPC405_ERR77(0,%3)
	PPC_STLCX "%0,0,%3\n"
	"bne-	1b"
	: "=&r"(old), "=m"(*p)
	: "r"(mask), "r"(p), "m"(*p)
	: "cc" );
}

static __inline__ int test_and_set_bit(unsigned long nr, volatile unsigned long *addr)
{
	unsigned long old, t;
	unsigned long mask = BITOP_MASK(nr);
	unsigned long *p = ((unsigned long *)addr) + BITOP_WORD(nr);

	__asm__ __volatile__(
	EIEIO_ON_SMP
"1:"	PPC_LLARX "%0,0,%3		# test_and_set_bit\n"
	"or	%1,%0,%2 \n"
	PPC405_ERR77(0,%3)
	PPC_STLCX "%1,0,%3 \n"
	"bne-	1b"
	ISYNC_ON_SMP
	: "=&r" (old), "=&r" (t)
	: "r" (mask), "r" (p)
	: "cc", "memory");

	return (old & mask) != 0;
}

static __inline__ int test_and_clear_bit(unsigned long nr, volatile unsigned long *addr)
{
	unsigned long old, t;
	unsigned long mask = BITOP_MASK(nr);
	unsigned long *p = ((unsigned long *)addr) + BITOP_WORD(nr);

	__asm__ __volatile__(
	EIEIO_ON_SMP
"1:"	PPC_LLARX "%0,0,%3		# test_and_clear_bit\n"
	"andc	%1,%0,%2 \n"
	PPC405_ERR77(0,%3)
	PPC_STLCX "%1,0,%3 \n"
	"bne-	1b"
	ISYNC_ON_SMP
	: "=&r" (old), "=&r" (t)
	: "r" (mask), "r" (p)
	: "cc", "memory");

	return (old & mask) != 0;
}

static __inline__ int test_and_change_bit(unsigned long nr, volatile unsigned long *addr)
{
	unsigned long old, t;
	unsigned long mask = BITOP_MASK(nr);
	unsigned long *p = ((unsigned long *)addr) + BITOP_WORD(nr);

	__asm__ __volatile__(
	EIEIO_ON_SMP
"1:"	PPC_LLARX "%0,0,%3		# test_and_change_bit\n"
	"xor	%1,%0,%2 \n"
	PPC405_ERR77(0,%3)
	PPC_STLCX "%1,0,%3 \n"
	"bne-	1b"
	ISYNC_ON_SMP
	: "=&r" (old), "=&r" (t)
	: "r" (mask), "r" (p)
	: "cc", "memory");

	return (old & mask) != 0;
}

static __inline__ void set_bits(unsigned long mask, unsigned long *addr)
{
        unsigned long old;

	__asm__ __volatile__(
"1:"	PPC_LLARX "%0,0,%3         # set_bits\n"
	"or	%0,%0,%2\n"
	PPC_STLCX "%0,0,%3\n"
	"bne-	1b"
	: "=&r" (old), "=m" (*addr)
	: "r" (mask), "r" (addr), "m" (*addr)
	: "cc");
}

/* Non-atomic versions */
static __inline__ int test_bit(unsigned long nr, __const__ volatile unsigned long *addr)
{
	return 1UL & (addr[BITOP_WORD(nr)] >> (nr & (BITS_PER_LONG-1)));
}

static __inline__ void __set_bit(unsigned long nr, volatile unsigned long *addr)
{
	unsigned long mask = BITOP_MASK(nr);
	unsigned long *p = ((unsigned long *)addr) + BITOP_WORD(nr);

	*p  |= mask;
}

static __inline__ void __clear_bit(unsigned long nr, volatile unsigned long *addr)
{
	unsigned long mask = BITOP_MASK(nr);
	unsigned long *p = ((unsigned long *)addr) + BITOP_WORD(nr);

	*p &= ~mask;
}

static __inline__ void __change_bit(unsigned long nr, volatile unsigned long *addr)
{
	unsigned long mask = BITOP_MASK(nr);
	unsigned long *p = ((unsigned long *)addr) + BITOP_WORD(nr);

	*p ^= mask;
}

static __inline__ int __test_and_set_bit(unsigned long nr, volatile unsigned long *addr)
{
	unsigned long mask = BITOP_MASK(nr);
	unsigned long *p = ((unsigned long *)addr) + BITOP_WORD(nr);
	unsigned long old = *p;

	*p = old | mask;
	return (old & mask) != 0;
}

static __inline__ int __test_and_clear_bit(unsigned long nr, volatile unsigned long *addr)
{
	unsigned long mask = BITOP_MASK(nr);
	unsigned long *p = ((unsigned long *)addr) + BITOP_WORD(nr);
	unsigned long old = *p;

	*p = old & ~mask;
	return (old & mask) != 0;
}

static __inline__ int __test_and_change_bit(unsigned long nr, volatile unsigned long *addr)
{
	unsigned long mask = BITOP_MASK(nr);
	unsigned long *p = ((unsigned long *)addr) + BITOP_WORD(nr);
	unsigned long old = *p;

	*p = old ^ mask;
	return (old & mask) != 0;
}

/*
 * Return the zero-based bit position (LE, not IBM bit numbering) of
 * the most significant 1-bit in a double word.
 */
static __inline__ int __ilog2(unsigned long x)
{
	int lz;

	asm (PPC_CNTLZL "%0,%1" : "=r" (lz) : "r" (x));
	return BITS_PER_LONG - 1 - lz;
}

#endif /* _ASM_POWERPC_BITOPS_H */

