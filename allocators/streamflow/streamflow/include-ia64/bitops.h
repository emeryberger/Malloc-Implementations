#ifndef _ASM_IA64_BITOPS_H
#define _ASM_IA64_BITOPS_H

/*
 * Copyright (C) 1998-2003 Hewlett-Packard Co
 *	David Mosberger-Tang <davidm@hpl.hp.com>
 *
 * 02/06/02 find_next_bit() and find_first_bit() added from Erich Focht's ia64
 * O(1) scheduler patch
 */

/**
 * __change_bit - Toggle a bit in memory
 * @nr: the bit to set
 * @addr: the address to start counting from
 *
 * Unlike change_bit(), this function is non-atomic and may be reordered.
 * If it's called on the same region of memory simultaneously, the effect
 * may be that only one operation succeeds.
 */
static __inline__ void
__change_bit (int nr, volatile void *addr)
{
	*((unsigned int *) addr + (nr >> 5)) ^= (1 << (nr & 31));
}

/*
 * WARNING: non atomic version.
 */
static __inline__ int
__test_and_change_bit (int nr, void *addr)
{
	unsigned int old, bit = (1 << (nr & 31));
	unsigned int *m = (unsigned int *) addr + (nr >> 5);

	old = *m;
	*m = old ^ bit;
	return (old & bit) != 0;
}

#endif

