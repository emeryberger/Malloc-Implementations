/******************************************************************************
 * Cache-Aware Memory Allocator (CAMA) with Area Blocks
 * Version 1.0
 *
 * Authors:
 *   Peter Backes (rtc@cs.uni-saarland.de)
 *   Joerg Herter (jherter@cs.uni-saarland.de)
 *   Christoph Mallon (mallon@cs.uni-saarland.de)
 *
 ******************************************************************************/

#ifndef BITOPS_H
#define BITOPS_H

#ifdef __linux__
#	ifndef _GNU_SOURCE
#		define _GNU_SOURCE 1
#	endif
#	include <string.h>
#else
#	include <strings.h>
#endif

#include <limits.h>

#define BIT_MASK(nr) (1UL << ((nr) % LONG_BIT))
#define BIT_WORD(nr) ((nr) / LONG_BIT)

#ifndef LONG_BIT
#define LONG_BIT (CHAR_BIT * sizeof (long))
#endif

static inline unsigned long find_next_bit(const unsigned long *addr,
		unsigned long const size, unsigned long offset)
{
	const unsigned long *p = addr + BIT_WORD(offset);
	unsigned long result = offset & ~(LONG_BIT-1), tmp;

	unsigned long count = (size - result) / LONG_BIT;
	unsigned long rest  = (size - result) % LONG_BIT;

	offset %= LONG_BIT;
	if (offset) {
		tmp = *p++;
		tmp &= ~0UL << offset;
		if (count == 0)
			goto tailing_bits;
		if (tmp)
			goto found;
		count  -= 1;
		result += LONG_BIT;
	}
	while (count != 0) {
		if ((tmp = *p++))
			goto found;

		count  -= 1;
		result += LONG_BIT;
	}
	if (rest == 0)
		return result;

	tmp = *p;

tailing_bits:
	tmp &= ~0UL >> (LONG_BIT - rest);
	if (tmp == 0)
		return size;
found:
	return result + ffsl(tmp) - 1;
}


static inline unsigned long set_bit(int nr, unsigned long *addr)
{
	return addr[BIT_WORD(nr)] |= BIT_MASK(nr);
}

static inline unsigned long clear_bit(int nr, unsigned long *addr)
{
	return addr[BIT_WORD(nr)] &= ~BIT_MASK(nr);
}

static inline int test_bit(int nr, const unsigned long *addr)
{
	return 1UL & addr[BIT_WORD(nr)] >> (nr & (LONG_BIT-1));
}

#endif
