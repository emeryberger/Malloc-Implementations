/******************************************************************************
 * Cache-Aware Memory Allocator (CAMA) with Area Blocks
 *
 *                                   CAMAREA
 *
 * Version 1.0
 *
 * Authors:
 *   Joerg Herter (jherter@cs.uni-saarland.de)
 *   Christoph Mallon (mallon@cs.uni-saarland.de)
 *
 ******************************************************************************/

#include "bitops.h"
#include "camarea.h"

#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * CONFIGURATION A
 *
 * Program/allocator specifics:
 * ***  cache sets available for descriptors
 * ***  number of size classes/levels
 * ***  improved splitting
 */
/*
 * Let [l,u] be the interval of cache sets to which the memory locations of
 * descriptor blocks may be mapped.
 * Then DESC_START_SET denotes l.
 */
#define DESC_START_SET   0
/*
 * Let [l,u] be the interval of cache sets to which the memory locations of
 * descriptor blocks may be mapped.
 * Then DESC_SETS denotes (u-l)+1, i.e. the size of the interval or in other
 * words the number of cache sets designated to hold descriptor blocks.
 */
#define DESC_SETS        11
/*
 * SIZE CLASSES ARE SPECIFIED HERE:
 */
#define LD_GRANULARITY	 2
#define LD_LINEAR_STEPS  2 // Number of "third level" size classes;
#define ALLOC_SIZE      13 // Number of "second level" size classes; max allocation = 2^ALLOC_SIZE - 1
/*
 * IMPROVED SPLITTING IS SPECIFIED HERE:
 * If FIXED_MAX_FREE is not defined, the allocator will keep track of the largest free blocks and try to use 
 * them to satisfy allocation requests before resorting to requesting additional memory from the underlying OS.
 * If FIXED_MAX_FREE is defined, the allocator keeps track of blocks equal or larger than the specified level
 * and tries to use them to satisfy allocation requests before resorting to requesting additional memory from 
 * the underlying OS. 
 * When you specify FIXED_MAX_FREE the level of the largest possible request a program regularily poses is 
 * a good choice.
 * 
 */
#ifndef FIXED_MAX_FREE
#define FIXED_MAX_FREE  25// level of largest "common" allocation (of the program using the allocator)
#endif
/*
 * CONFIGURATION B 
 * 
 * Hardware specifics:
 * ***  number of cache sets and size of a cache line
 * **** use ld of the respective value to specify
 */
#define CACHE_SET_BITS 7 // 2^7 cache sets
#define LINE_SIZE_BITS 5 // line size of 2^5 bytes
/*
 * CONFIGURATION C
 * 
 * Automatic precomputations/definitions.
 */
#define CACHE_SETS     (1U << CACHE_SET_BITS)
#define LINE_SIZE      (1U << LINE_SIZE_BITS)
#define ROUND_LINE_SIZE(x) (((x) + (LINE_SIZE - 1)) & ~(LINE_SIZE - 1))
#define GRANULARITY (1U << LD_GRANULARITY)
#define LINEAR_STEPS (1U << LD_LINEAR_STEPS)
#define WAY_SIZE        (CACHE_SETS * LINE_SIZE)
#define MULTIBLOCK_SIZE ROUND_LINE_SIZE((CACHE_SETS - DESC_SETS) * LINE_SIZE / (DESC_SETS * LINE_SIZE / sizeof(descriptor)))
#define SIZE_THRESHOLD  ((MULTIBLOCK_SIZE - sizeof(multi_head)) / 2)
#define lengthof(x) (sizeof(x) / sizeof(*(x)))
#define MAX_ALLOC ((1U << (ALLOC_SIZE - 1)) * (2 * LINEAR_STEPS - 1) / LINEAR_STEPS)
/*
 *   END OF CONFIGURATION
 */


/* Calculate ld(x) + 1 rounded down.
 *
 * Note:
 *   This is a slightly modified version of an implementation of an
 *   nlz-function (number of leading zeros) given in:
 *
 *   Henry S. Warren: 
 *          "Hacker's Delight"
 *   Addison-Wesley Longman, 2002.
 */
static unsigned ld1(size_t x)
{
	int y, m, n;

	y = -(x >> 16);
	m = (y >> 16) & 16;
	n = 16 - m;
	x = x >> m;

	y = x - 0x100;
	m = (y >> 16) & 8;
	n += m;
	x = x << m;

	y = x - 0x1000;
	m = (y >> 16) & 4;
	n += m;
	x = x << m;

	y = x - 0x4000;
	m = (y >> 16) & 2;
	n += m;
	x = x << m;

	y = x >> 14;
	m = y & ~(y >> 1);
	return 30 - n + m;
}


typedef struct block_head  block_head;
typedef struct descriptor  descriptor;
typedef struct multi_head  multi_head;
typedef struct common_head common_head;


struct block_head
{
	common_head* back;
};


struct multi_head
{
	void*    back;
	size_t   size;
	unsigned free;
};


struct descriptor
{
	// Start of managed block, either a block_head or a multi_head
	void*        start;
	// Size of managed block, < 0 iff free
	ssize_t      size;
	size_t       extra;
	// Physically adjacent blocks
	descriptor*  left;
	descriptor*  right;
	// Previous/Next block in free list
	descriptor** prev_next;
	descriptor*  next;
};


struct common_head
{
	void*    back;
	size_t   size;
};


union heads
{
	common_head common;
	multi_head  multi;
	descriptor  desc;
	block_head  block;
};


// Pointer to current break.
static char*         curbrk;
// Pointer to highest-address memory block currently managed by the allocator.
static descriptor*   tail;
// Pointer to the descriptor area currently used (i.e., with free descriptor)
static multi_head*   desc_free_list;
// Table of free lists.
static descriptor*   free_lists[CACHE_SETS][ALLOC_SIZE * LINEAR_STEPS];
// Bit vector. ith bit set iff ith free list contains free blocks (i.e., their descriptors).
static unsigned long non_empty[(sizeof(free_lists) / sizeof(free_lists[0][0]) + LONG_BIT - 1) / LONG_BIT];
// Bit vector. ith bit set iff cache set i contains free blocks of at least max_free_level (see below)
static unsigned long max_free[(CACHE_SETS + LONG_BIT - 1) / LONG_BIT];
// Number of cache sets for which such blocks are (definitively) available.
static size_t        n_max_free;
#ifdef FIXED_MAX_FREE
static size_t const  max_free_level = FIXED_MAX_FREE;
#else
static size_t        max_free_level;
#endif


// Determines to which cache set a given pointer/address is mapped.
static inline unsigned set_from_addr(void const* const ptr)
{
	return (uintptr_t)ptr / LINE_SIZE % CACHE_SETS;
}

// Determines the level (within the free table structure) to which a given size belongs.
static inline unsigned level_from_size(size_t const size)
{
	unsigned const x = ld1(size) - 1;
	unsigned const l =
		/* Exponential size class */
		(x - LD_GRANULARITY) * LINEAR_STEPS +
		/* Linear sub-class, might overflow and then correctly increase size class by 1 */
		((size + (1 << (x - 2)) - 1) >> (x - 2)) - LINEAR_STEPS;
	assert(l < lengthof(free_lists[0]));
	return l;
}

// Determines the level "rounded down" (within the free table structure) to which a given size belongs.
static inline unsigned level_from_size_down(size_t const size)
{
	unsigned const x = ld1(size) - 1;
	unsigned const l =
		/* Exponential size class */
		(x - LD_GRANULARITY) * LINEAR_STEPS +
		/* Linear sub-class, might overflow and then correctly increase size class by 1 */
		(size >> (x - 2)) - LINEAR_STEPS;

	return l < lengthof(free_lists[0]) ? l : lengthof(free_lists[0]) - 1;
}


static inline unsigned bit_index(unsigned const set, unsigned const level)
{
	return lengthof(free_lists[0]) * set + level;
}



// START DEBUG FUNCTIONS
static int in_lr_list(descriptor* const desc)
{
	for (descriptor* i = tail; i; i = i->left) {
		if (i == desc)
			return 1;
	}
	return 0;
}
static void check_heap(void)
{
	assert(tail);
	assert((char*)tail->start + (tail->size < 0 ? -tail->size + tail->extra : tail->size + tail->extra) == curbrk);

	for (descriptor* i = tail; i; i = i->left) {
		assert(!i->left || i->left->right == i);
		assert(!i->left || (char*)i->left->start + i->left->extra + (i->left->size < 0 ? -i->left->size : i->left->size) == i->start);
		assert(i->start);
		assert(i->size > 0 || i->extra == 0);
		assert(i->size < 0 || ((common_head*)i->start)->back == i);
		if (i->size < 0) {
			assert(*i->prev_next = i);
			assert(!i->next || i->next->prev_next == &i->next);
		}
	}

	descriptor* const free_head = (descriptor*)(desc_free_list + 1);
	assert(free_head->next != free_head);

	for (multi_head* i = desc_free_list; i;) {
		assert(!(i->free & 1));
		for (size_t k = 0; k != (DESC_SETS * LINE_SIZE - sizeof(multi_head)) / sizeof(descriptor); ++k) {
			if (i->free & 1U << k)
				continue;

			descriptor* const desc = &((descriptor*)(i + 1))[k];
			assert(in_lr_list(desc));
		}

		descriptor* const self = &((descriptor*)(i + 1))[0];
		assert((descriptor*)(desc_free_list + 1) == self ? !self->prev_next : *self->prev_next == self);
		descriptor* const next = self->next;
		if (!next)
			break;
		i = (multi_head*)next - 1;
	}

#ifndef FIXED_MAX_FREE
	for (size_t i = 0; i != CACHE_SETS; ++i) {
		assert(!test_bit(CACHE_SETS - 1 - i, max_free) || free_lists[i][max_free_level]);
	}
#endif
}
void print_table(void)
{
        fprintf(stdout, "Freelist table:\n");
        for(int i = 0; i < CACHE_SETS; i++)
        {
                for(int j = 0; j < ALLOC_SIZE * LINEAR_STEPS; j++)
                {
                        if(free_lists[i][j])
                                fprintf(stdout, "[%d, %d]->", i, j);
                        for(descriptor* descr = free_lists[i][j]; descr; descr = descr->next)
                                fprintf(stdout, "%p (%Zd)->", descr->start, -descr->size);
                        if(free_lists[i][j])
                                fprintf(stdout, "\n");
                }
        }

        fprintf(stdout, "Managed blocks:\n");
        for (descriptor* i = tail; i; i = i->left) {
                fprintf(stdout, "@%p (size: %Zd; desc@%p)\n", i->start, i->size, i);
        }

        fprintf(stdout, "Descr. Freelist @%p\n", desc_free_list);
}
void monitor_desc_free_list()
{
        fprintf(stdout, "Size of desc_free_list is %Zd.\n", ((descriptor *)desc_free_list->back)->size);
}
// END DEBUG FUNCTIONS




// Inserts a given descriptor into the appropriate free block list.
static unsigned insert_descriptor(descriptor* const desc, size_t const size)
{
	assert(size < SIZE_THRESHOLD || size == (desc->size < 0 ? -desc->size : desc->size));
	assert(size > 0);
	assert(desc->start);
	unsigned     const set    = set_from_addr(desc->start);
	unsigned     const level  = level_from_size_down(size);
	descriptor** const anchor = &free_lists[set][level];
	descriptor*  const head   = *anchor;

	desc->prev_next = anchor;
	desc->next      = head;
	assert(desc->next != desc);

	if (head) {
		assert(head != desc);
		head->prev_next = &desc->next;
	} else {
		assert(!test_bit(bit_index(set, level), non_empty));
		set_bit(bit_index(set, level), non_empty);
	}

#ifdef FIXED_MAX_FREE
	if (level >= max_free_level)
#else
	if (level == max_free_level)
#endif
	{
		size_t const set_idx = CACHE_SETS - 1 - set;
		if (!test_bit(set_idx, max_free)) {
			#ifdef VERBOSE_DEBUG 
			fprintf(stderr, ".max_free insert set %u level %u %p\n", set, level, desc);
			#endif
			
			set_bit(set_idx, max_free);
			++n_max_free;
		}
#ifndef FIXED_MAX_FREE
	} else if (level > max_free_level) {
		#ifdef VERBOSE_DEBUG 		
		fprintf(stderr, "max_free replace set %u level %u (was %u) %p\n", set, level, max_free_level, desc);
		#endif
		
		size_t const set_idx = CACHE_SETS - 1 - set;
		memset(max_free, 0, sizeof(max_free));
		set_bit(set_idx, max_free);
		max_free_level = level;
		n_max_free     = 1;
#endif
	}

	*anchor = desc;
	return level;
}


static descriptor* unlink_descriptor(unsigned const set, unsigned const idx, unsigned const set_start)
{
	size_t       const level  = idx - set_start;
	descriptor** const anchor = &free_lists[set][level];
	descriptor*  const desc   = *anchor;
	assert(desc->prev_next = anchor);
	descriptor* const next = desc->next;
	*anchor = next;
	if (!next) {
		// Removing the last entry on the freelist. Mark it as empty.
		assert(test_bit(idx, non_empty));
		clear_bit(idx, non_empty);
	} else {
		next->prev_next = anchor;
	}
	desc->prev_next = 0;
	desc->next      = 0;
	assert(desc->size < 0);

#ifdef FIXED_MAX_FREE
	if (!*anchor && level >= max_free_level)
#else
	if (!*anchor && level == max_free_level)
#endif
	{
#ifdef FIXED_MAX_FREE
		unsigned const nbits   = lengthof(free_lists[0]) * (set + 1);
		unsigned const bit_pos = bit_index(set, max_free_level);
		unsigned const idx     = find_next_bit(non_empty, nbits, bit_pos);
		if (idx == nbits)
#endif
		{
			size_t const set_idx = CACHE_SETS - 1 - set;
			if (test_bit(set_idx, max_free)) {
				#ifdef VERBOSE_DEBUG
				fprintf(stderr, ".max_free unlink set %u level %u %p\n", set, level, desc);
				#endif

				clear_bit(set_idx, max_free);
				--n_max_free;
#ifndef FIXED_MAX_FREE
				if (n_max_free == 0)
					max_free_level = 0;
#endif
			}
		}
	}

	#ifdef VERBOSE_DEBUG
	fprintf(stderr, "unlink %p, next %p\n", desc, next);
	#endif
	return desc;
}


// Remove descriptor from its free block list
static void remove_descriptor(descriptor* const desc, size_t const size)
{
	descriptor** const prev_next = desc->prev_next;
	descriptor*  const next      = desc->next;

	#ifdef VERBOSE_DEBUG
	fprintf(stderr, "remove %p, prev_next %p, next %p\n", desc, prev_next, next);
	#endif
	
	if (next) {
		assert(&next->next != prev_next);
		next->prev_next = prev_next;
	}

	assert(*prev_next == desc);
	*prev_next = next;

	unsigned const set   = set_from_addr(desc->start);
	unsigned const level = level_from_size_down(size);
	if (!free_lists[set][level]) {
		assert(test_bit(bit_index(set, level), non_empty));
		clear_bit(bit_index(set, level), non_empty);

		size_t const set_idx = CACHE_SETS - 1 - set;
#ifdef FIXED_MAX_FREE
		if (level >= max_free_level && test_bit(set_idx, max_free))
#else
		if (level == max_free_level && test_bit(set_idx, max_free))
#endif
		{
#ifdef FIXED_MAX_FREE
			unsigned const nbits   = lengthof(free_lists[0]) * (set + 1);
			unsigned const bit_pos = bit_index(set, max_free_level);
			unsigned const idx     = find_next_bit(non_empty, nbits, bit_pos);
			if (idx == nbits)
#endif
			{
				#ifdef VERBOSE_DEBUG
				fprintf(stderr, "max_free remove set %u level %u %p\n", set, level, desc);
				#endif

				clear_bit(set_idx, max_free);
				--n_max_free;
#ifndef FIXED_MAX_FREE
				if (n_max_free == 0)
					max_free_level = 0;
#endif
			}
		}
	}

	desc->prev_next = 0;
	desc->next      = 0;
}


static descriptor* copy_desc(descriptor* const dst, descriptor* const src, descriptor* const track)
{
	#ifdef VERBOSE_DEBUG
	fprintf(stderr, "%s(%p, %p)\n", __func__, dst, src);
	#endif
	
	*dst = *src;

	if (dst != src) {
		assert(src->start);

		((common_head*)dst->start)->back = dst;

		if (dst->left) {
			assert(dst->left->right == src);
			dst->left->right = dst;
		}
		if (dst->right) {
			assert(dst->right->left == src);
			dst->right->left = dst;
		}

		if (dst->prev_next) {
			assert(!src->prev_next || *src->prev_next == src);
			*dst->prev_next = dst;
		}
		if (dst->next) {
			assert(!src->next->prev_next || src->next->prev_next == &src->next);
			dst->next->prev_next = &dst->next;
		}

		if (tail == src) tail = dst;
	}

	// TODO
	memset(src, 0, sizeof(*src));

	return track == src ? dst : track;
}


static void mark_desc_free(size_t const src_idx)
{
	assert(!(desc_free_list->free & 1U << src_idx));
	desc_free_list->free |= 1U << src_idx;
}


// Put descriptor on free descriptor list
static descriptor* free_descriptor(descriptor* const desc, descriptor* track)
{
	#ifdef VERBOSE_DEBUG
	fprintf(stderr, "%s(%p, %p)\n", __func__, desc, track);
	#endif

	descriptor* const left  = desc->left;
	descriptor* const right = desc->right;
	if (left)  left->right = right;
	if (right) right->left = left;

	if (tail == desc) tail = left;

	memset(desc, 0, sizeof(*desc)); // <----TODO

	// Calcutate descriptor block multi_head address.
	multi_head* const head = (multi_head*)(((size_t)desc & ~(WAY_SIZE - 1)) + DESC_START_SET * LINE_SIZE);
	if (head == desc_free_list) {
		#ifdef VERBOSE_DEBUG
		fprintf(stderr, "head == desc_free_list\n");
		#endif
		descriptor* const descs = (descriptor*)(head + 1);

		assert(descs[0].size >= DESC_SETS * LINE_SIZE);
		assert(descs[0].start == head);

		size_t const idx = desc - descs;
		#ifdef VERBOSE_DEBUG
		fprintf(stderr, "idx = %zu\nhead->free = %x\n(1U << idx) = %x\n", idx, head->free, (1U << idx));
		#endif
		assert(!(head->free & (1U << idx)));
		head->free |= 1U << idx;
	} else {
		#ifdef VERBOSE_DEBUG
		fprintf(stderr, "head != desc_free_list\n");
		#endif
		
		descriptor* const descs     = (descriptor*)(desc_free_list + 1);
		unsigned    const free_mask = (1U << (DESC_SETS * LINE_SIZE - sizeof(multi_head)) / sizeof(descriptor)) - 1 - 1;
		unsigned          used_idx  = ffs(desc_free_list->free ^ free_mask);
		if (used_idx == 0) {
			#ifdef VERBOSE_DEBUG
			fprintf(stderr, "used_idx == 0\n");
			#endif
			descriptor* const self        = &descs[0];
			descriptor* const used_blocks = self->next;
			self->next               = 0;
			used_blocks[0].prev_next = 0;
			desc_free_list           = (multi_head*)used_blocks - 1;

			descriptor* left = self->left;
			if (left && left->size < 0) {
				#ifdef VERBOSE_DEBUG
				fprintf(stderr, "left free\n");
				#endif
				// Left neighbour is free, merge it.
				remove_descriptor(left, -left->size);
				left->size -= self->size;
				left->right = self->right;
				if (left->right) {
					self->right->left = left;
				} else {
					assert(tail == self);
					tail = left;
				}

				descriptor* const right = self->right;
				if (right && right->size < 0) {
					#ifdef VERBOSE_DEBUG
					fprintf(stderr, "right free\n");
					#endif
					remove_descriptor(right, -right->size);
					left->size += right->size;

					left->right = right->right;
					if (left->right) {
						left->right->left = left;
					} else {
						assert(tail == right);
						tail = left;
					}

					size_t const src_idx = &used_blocks[2] != desc ? 2 : 3;
					if (left == &used_blocks[src_idx])
						left = right;

					// Copy new free head index 2.
					mark_desc_free(src_idx);
					track = copy_desc(right, &used_blocks[src_idx], track);
				}

				insert_descriptor(left, -left->size);

				// Copy new free head index 1.
				mark_desc_free(1);
				track = copy_desc(desc, &used_blocks[1], track);
			} else {
				self->size  = -self->size - self->extra;
				self->extra = 0;
				track = copy_desc(desc, self, track);

				descriptor* const right = desc->right;
				if (right && right->size < 0) {
					#ifdef VERBOSE_DEBUG
					fprintf(stderr, "right free\n");
					#endif
					// Right neighbour is free, merge it
					remove_descriptor(right, -right->size);
					desc->size += right->size;
					insert_descriptor(desc, -desc->size);
					desc->right = right->right;
					if (desc->right) {
						desc->right->left = desc;
					} else {
						assert(tail == right);
						tail = desc;
					}

					// Copy new free head index 1.
					mark_desc_free(1);
					track = copy_desc(right, &used_blocks[1], track);
				} else {
					#ifdef VERBOSE_DEBUG
					fprintf(stderr, "none free\n");
					#endif
					// No mergable free neighbours.
					insert_descriptor(desc, -desc->size);
				}
			}
		} else {
			#ifdef VERBOSE_DEBUG
			fprintf(stderr, "used_idx != 0\n");
			#endif
			--used_idx;
			mark_desc_free(used_idx);
			track = copy_desc(desc, &descs[used_idx], track);
		}
	}

	return track;
}


static inline void setbrk(void* const addr)
{
	assert(((uintptr_t)addr & (LINE_SIZE - 1)) == 0);

#ifdef __APPLE__
	void* res;
#else
	int res;
#endif
	res = brk(addr);

#ifdef __APPLE__
	assert(res != (void*)-1);
#else
	assert(res != -1);
#endif

	(void)res;
}


static descriptor* get_descriptor(void)
{
	descriptor* res;

	int free_idx = ffs(desc_free_list->free);
	if (free_idx == 0) {
		unsigned const set     = DESC_START_SET;
		size_t   const size    = DESC_SETS * LINE_SIZE;
		unsigned const level   = level_from_size(size);
		unsigned const nbits   = lengthof(free_lists[0]) * (set + 1);
		unsigned const bit_pos = bit_index(set, level);
		unsigned       idx     = find_next_bit(non_empty, nbits, bit_pos);
		if (idx == nbits) {
			// No free suitable blocks
			char*       const oldbrk     = curbrk;
			unsigned    const oldbrk_set = (uintptr_t)oldbrk / LINE_SIZE % CACHE_SETS;
			char*             start      = oldbrk - (uintptr_t)oldbrk % WAY_SIZE + DESC_START_SET * LINE_SIZE;
			if (DESC_START_SET <= oldbrk_set) start += WAY_SIZE;
			size_t      const gap        = start - oldbrk;
			multi_head* const head       = (multi_head*)start;
			descriptor* const descs      = (descriptor*)(head + 1);

			curbrk = start + DESC_SETS * LINE_SIZE;
			setbrk(curbrk);

			head->free = (1U << (DESC_SETS * LINE_SIZE - sizeof(multi_head)) / sizeof(descriptor)) - 1;

			descriptor* const t    = tail;
			size_t            i    = 0;
			descriptor* const self = &descs[i];
			head->free &= ~(1U << i++);
			#ifdef VERBOSE_DEBUG
			fprintf(stderr, "using %d: %p\n", __LINE__, self);
			#endif
			self->left  = t;
			self->right = 0;
			self->start = start;
			self->size  = DESC_SETS * LINE_SIZE;
			head->back  = self;
			t->right    = self;
			tail = self;

			descriptor* const old_self_head = (descriptor*)(desc_free_list + 1);
			old_self_head->prev_next = &self->next;
			self->prev_next          = 0;
			self->next               = old_self_head;
			desc_free_list           = head;

			if (t->size < 0) {
				// The last block is free, add the gap to it.
				remove_descriptor(t, -t->size);
				t->size -= gap;
				insert_descriptor(t, -t->size);
			} else if (gap < SIZE_THRESHOLD) {
				// The last block is used and the gap is too small for an independent block.
				t->extra += gap;
			} else {
				descriptor* const desc = &descs[i];
				head->free &= ~(1U << i++);
				#ifdef VERBOSE_DEBUG
				fprintf(stderr, "using %d: %p\n", __LINE__, desc);
				#endif
				desc->left  = t;
				desc->right = self;
				desc->start = oldbrk;
				desc->size  = -(ssize_t)gap;

				t->right = desc;

				self->left = desc;

				insert_descriptor(desc, gap);
			}

			res = &descs[i];
			head->free &= ~(1U << i++);
			#ifdef VERBOSE_DEBUG
			fprintf(stderr, "using %d: %p\n", __LINE__, res);
			#endif
		} else {
			unsigned    const set_start = bit_index(set, 0);
			descriptor* const desc      = unlink_descriptor(set, idx, set_start);
			assert((size_t)-desc->size >= size);

			multi_head* const head  = (multi_head*)desc->start;
			descriptor* const descs = (descriptor*)(head + 1);
			descriptor* const self  = &descs[0];
			*self = *desc;
			self->left->right = self;
			if (self->right)
				self->right->left = self;

			head->back = self;
			head->free = (1U << (DESC_SETS * LINE_SIZE - sizeof(multi_head)) / sizeof(descriptor)) - 1 - 1;

			descriptor* const old_self_head = (descriptor*)(desc_free_list + 1);
			old_self_head->prev_next = &self->next;
			self->prev_next          = 0;
			self->next               = old_self_head;
			desc_free_list           = head;

			size_t const rest_size = -self->size - size;
			if (rest_size >= SIZE_THRESHOLD) {
				// Rest of block is large enough to split it.
				self->size  = size;

				descriptor* const rest = &descs[1];
				head->free &= ~(1U << 1);

				rest->start = (char*)self->start + size;
				rest->size  = -(ssize_t)rest_size;
				descriptor* const right = self->right;
				if (right) {
					right->left = rest;
				} else {
					assert(tail == desc);
					tail = rest;
				}
				rest->right = right;
				rest->left  = self;
				self->right = rest;
				insert_descriptor(rest, rest_size);
			} else {
				self->size  = size;
				self->extra = rest_size;
			}

			res = desc;
		}
	} else {
		--free_idx;
		desc_free_list->free &= ~(1U << free_idx);
		res = &((descriptor*)(desc_free_list + 1))[free_idx];
		#ifdef VERBOSE_DEBUG
		fprintf(stderr, "using %d: %p\n", __LINE__, res);
		#endif
	}

	memset(res, 0, sizeof(*res)); // <---- TODO

	return res;
}


static descriptor* allocate_memory(unsigned const set, size_t const size)
{
	descriptor* const res = get_descriptor();

	char*    const oldbrk     = curbrk;
	unsigned const oldbrk_set = (uintptr_t)oldbrk / LINE_SIZE % CACHE_SETS;
	char*          start      = oldbrk - (uintptr_t)oldbrk % WAY_SIZE + set * LINE_SIZE;
	if (set < oldbrk_set || (set == oldbrk_set && (uintptr_t)oldbrk % LINE_SIZE != 0)) start += WAY_SIZE;
	size_t   const gap        = start - oldbrk;

	curbrk = start + size;
	setbrk(curbrk);

	descriptor* const t = tail;
	res->start = start;
	res->left  = t;
	res->right = t->right;
	res->size  = size;
	tail = res;

	if (t->right) {
		t->right->left = res;
	}
	t->right = res;

	if (t->size < 0) {
		// The last block is free, add the gap to it.
		assert((char*)t->start - t->size == oldbrk);
		remove_descriptor(t, -t->size);
		t->size -= gap;
		insert_descriptor(t, -t->size);
	} else if (gap < SIZE_THRESHOLD) {
		// The last block is used and the gap is too small for an independent block.
		t->extra += gap;
	} else {
		descriptor* const desc = get_descriptor();
		desc->start = oldbrk;
		desc->left  = t;
		desc->right = res;
		desc->size  = -(ssize_t)gap;
		insert_descriptor(desc, gap);

		res->left = desc;
		t->right  = desc;
	}

	return res;
}


static descriptor* allocate_block(size_t const size, unsigned const set)
{
	unsigned const level   = level_from_size(size);
	unsigned const nbits   = lengthof(free_lists[0]) * (set + 1);
	unsigned const bit_pos = bit_index(set, level);
	unsigned const idx     = find_next_bit(non_empty, nbits, bit_pos);
	if (idx != nbits) {
		unsigned    const set_start = bit_index(set, 0);
		descriptor* const desc      = unlink_descriptor(set, idx, set_start);
		assert((size_t)-desc->size >= size);
		size_t const rest_size = -desc->size - size;
		if (rest_size >= SIZE_THRESHOLD) {
			// Rest of block is large enough to split it.
			desc->size  = size;
			descriptor* const rest = get_descriptor();
			rest->start = (char*)desc->start + size;
			rest->size  = -(ssize_t)rest_size;
			descriptor* const right = desc->right;
			if (right) {
				right->left = rest;
			} else {
				assert(tail == desc);
				tail = rest;
			}
			rest->right = right;
			rest->left  = desc;
			desc->right = rest;
			insert_descriptor(rest, rest_size);
		} else {
			desc->size  = size;
			desc->extra = rest_size;
		}

		assert(desc->size > 0);
		return desc;
	}

	if (level <= max_free_level) {
		unsigned const max_nbits   = sizeof(max_free) * CHAR_BIT;
		unsigned const max_bit_pos = CACHE_SETS - 1 - set;
		unsigned       max_idx     = find_next_bit(max_free, max_nbits, max_bit_pos);
		size_t         lgap;
		if (max_idx == max_nbits) {
			#ifdef VERBOSE_DEBUG
			fprintf(stderr, "scan 2\n");
			#endif

			max_idx = find_next_bit(max_free, max_bit_pos, 0);
			if (max_idx == max_bit_pos)
				goto allocate;
			lgap = (CACHE_SETS + max_idx - max_bit_pos) * LINE_SIZE;
		} else {
			#ifdef VERBOSE_DEBUG
			fprintf(stderr, "scan 1\n");
			#endif
			lgap = (max_idx - max_bit_pos) * LINE_SIZE;
		}

		unsigned const oset = CACHE_SETS - 1 - max_idx;
#ifdef FIXED_MAX_FREE
		unsigned const nbits   = lengthof(free_lists[0]) * (oset + 1);
		unsigned const bit_pos = bit_index(oset, max_free_level);
		unsigned const idx     = find_next_bit(non_empty, nbits, bit_pos);
		assert(idx != nbits);
		unsigned const olevel  = idx - oset * lengthof(free_lists[0]);
#else
		unsigned const olevel = max_free_level;
#endif
		if (lgap + size <= -free_lists[oset][olevel]->size) {
			#ifdef VERBOSE_DEBUG
			fprintf(stderr, "lgap %zu\n", lgap);
			#endif

			unsigned    const oset_start = oset * (ALLOC_SIZE * LINEAR_STEPS);
			descriptor* const desc       = unlink_descriptor(oset, oset_start + olevel, oset_start);
			assert(lgap + size <= -desc->size);

			#ifdef VERBOSE_DEBUG
			fprintf(stderr, "desc->size %zd size %zu\n", desc->size, size);
			#endif

			void* const start = desc->start;
			desc->start = start + lgap;
			desc->size  = -desc->size - lgap;

			descriptor* const left = desc->left;
			if (lgap > SIZE_THRESHOLD) {
				#ifdef VERBOSE_DEBUG
				fprintf(stderr, "new lgap %zu\n", lgap);
				#endif

				descriptor* const lgap_desc = get_descriptor();
				lgap_desc->start = start;
				lgap_desc->left  = left;
				lgap_desc->right = desc;
				lgap_desc->size  = -(ssize_t)lgap;
				insert_descriptor(lgap_desc, lgap);
				assert(*lgap_desc->prev_next == lgap_desc);

				left->right = lgap_desc;
				desc->left  = lgap_desc;

				desc->size += desc->extra;
				desc->extra = 0;
			} else {
				#ifdef VERBOSE_DEBUG
				fprintf(stderr, "merge lgap\n");
				#endif	
				// The last block is used and the gap is too small for an independent block.
				left->extra += lgap;
			}

			#ifdef VERBOSE_DEBUG
			fprintf(stderr, "desc->size %zd size %zu\n", desc->size, size);
			#endif

			size_t const rgap = desc->size - size;

			#ifdef VERBOSE_DEBUG
			fprintf(stderr, "rgap %zu desc->size %zd size %zu\n", rgap, desc->size, size);
			#endif

			if (rgap > SIZE_THRESHOLD) {
				descriptor* const rgap_desc = get_descriptor();

				#ifdef VERBOSE_DEBUG
				fprintf(stderr, "new rgap %zu desc %p rgap_desc %p\n", rgap, desc, rgap_desc);
				#endif

				rgap_desc->start = desc->start + size;
				rgap_desc->left  = desc;
				rgap_desc->right = desc->right;
				rgap_desc->size  = -(ssize_t)rgap - desc->extra;
				insert_descriptor(rgap_desc, -rgap_desc->size);
				assert(*rgap_desc->prev_next == rgap_desc);

				if (desc->right) {
					desc->right->left = rgap_desc;
				} else {
					assert(tail == desc);
					tail = rgap_desc;
				}

				desc->right = rgap_desc;
				desc->size  = size;
				desc->extra = 0;
			} else {
				desc->size  = size;
				desc->extra = rgap;
				#ifdef VERBOSE_DEBUG
				fprintf(stderr, "merge rgap\n");
				#endif
			}

			return desc;
		}
	}

allocate:
	// No free suitable blocks
	return allocate_memory(set, size);
}


static inline unsigned subblock_count(size_t const size)
{
	unsigned const n = (MULTIBLOCK_SIZE - sizeof(multi_head)) / size;
	assert(1 < n);
	assert(n < sizeof(unsigned) * CHAR_BIT);
	return n;
}


static size_t here_malloc = 0;
void* camalloc(size_t size, unsigned set)
{
	#ifdef VERBOSE_DEBUG
	fprintf(stderr, "enter camalloc: %zu\n", ++here_malloc);
	#endif
	#ifdef CHECK_HEAP
	check_heap();
	#endif
	
	// Add size of header to allocation and round up to a multiple of GRANULARITY.
	size = (size + sizeof(block_head) + GRANULARITY - 1) & ~(GRANULARITY - 1);

	void* res;
	if (size > SIZE_THRESHOLD) {
		// A big allocation.
		descriptor* const desc  = allocate_block(ROUND_LINE_SIZE(size), set);
		block_head* const block = desc->start;
		block->back = (common_head*)desc;
		res = block + 1;
	} else {
		// A small allocation.
		unsigned const n_subblocks = subblock_count(size);
		unsigned const set_range   = (sizeof(multi_head) + (n_subblocks - 1) * size) / LINE_SIZE + 1;
		set = set / set_range * set_range;
		unsigned    const level = level_from_size(size);
		descriptor* const desc  = free_lists[set][level];

		if (!desc) {
			// No free suitable blocks
			size_t      const multi_size  = ROUND_LINE_SIZE(sizeof(multi_head) + n_subblocks * size);
			descriptor* const desc        = allocate_block(multi_size, set);
			multi_head* const multi       = (multi_head*)desc->start;
			multi->back = desc;
			multi->free = (1U << n_subblocks) - 1 - 1; // FIXME fails if n_subblocks equals the number of bits in unsigned.
			multi->size = size;
			block_head* const block       = (block_head*)(multi + 1);
			block->back = (common_head*)multi;
			insert_descriptor(desc, size);
			assert(desc->size >  0);
			res = block + 1;
		} else {
			multi_head* const multi = (multi_head*)desc->start;
			assert(multi->free != 0);
			unsigned    const slot  = ld1(multi->free) - 1;
			block_head* const block = (block_head*)((char*)(multi + 1) + slot * multi->size);
			block->back = (common_head*)multi;
			multi->free &= ~(1U << slot);
			if (multi->free == 0) {
				// This was the last free subblock, remove descriptor from free list
				remove_descriptor(desc, multi->size); // TODO optimize? We know this is the head
			}
			assert(desc->size > 0);
			res = block + 1;
		}
	}

	#ifdef VERBOSE_DEBUG
	fprintf(stderr, "%s(%zu, %u) = %p\n", __func__, size, set, res);
	#endif
	#ifdef CHECK_HEAP
	check_heap();
	#endif
	return res;
}


static size_t here = 0;

/* EDB: Added (adapted from cafree, below): returns object size */

size_t camsize(void* const ptr)
{
	#ifdef VERBOSE_DEBUG
	fprintf(stderr, "enter %s(%p)\n", __func__, ptr);
	#endif
	#ifdef CHECK_HEAP	
	check_heap();
	#endif

	if (!ptr) return 0;

	block_head*  const block = (block_head*)ptr - 1;
	common_head* const head  = block->back;
	assert(head->size >= 0);
	return head->size;
}

void cafree(void* const ptr)
{
	#ifdef VERBOSE_DEBUG
	fprintf(stderr, "enter %s(%p)\n", __func__, ptr);
	#endif
	#ifdef CHECK_HEAP	
	check_heap();
	#endif

	if (!ptr) return;

	block_head*  const block = (block_head*)ptr - 1;
	common_head* const head  = block->back;
	assert(head->size >= 0);

	descriptor* desc;
	if (head->size > SIZE_THRESHOLD) {
		#ifdef VERBOSE_DEBUG
		fprintf(stderr, "cafree: big\n");
		#endif
		desc = (descriptor*)head;
	} else {
		#ifdef VERBOSE_DEBUG
		fprintf(stderr, "cafree: small\n");
		#endif
		// A small block
		desc = head->back;

		assert((void*)head == (void*)desc->start);
		multi_head* const multi = (multi_head*)head;
		unsigned    const slot  = ((char*)block - (char*)&multi[1]) / multi->size;
		if (multi->free == 0) {
			// Multi-block was completely used, now one slot is free.
			multi->free = 1U << slot;
			insert_descriptor(desc, multi->size);
			#ifdef CHECK_HEAP	
			check_heap();
			#endif
			return;
		} else {
			assert(!(multi->free & 1U << slot));
			multi->free |= 1U << slot;
			unsigned const n_subblocks = subblock_count(multi->size);
			if (multi->free != (1U << n_subblocks) - 1) {
				#ifdef CHECK_HEAP
				check_heap();
				#endif
				return; // FIXME fails if n_subblocks equals the number of bits in unsigned.
			}
			// All subblocks are free
			remove_descriptor(desc, multi->size);
		}
	}

	for (descriptor* i = tail;; i = i->left) {
		assert(i);
		if (i == desc)
			break;
	}

	// A big block or a completely free multi-block
	#ifdef VERBOSE_DEBUG
	fprintf(stderr, "%s: desc %p left %p right %p\n", __func__, desc, desc->left, desc->right);
	#endif
	#ifdef CHECK_HEAP
	check_heap();
	#endif
	#ifdef VERBOSE_DEBUG
	fprintf(stderr, "cafree: %zu\n", ++here);
	#endif
	
	ssize_t size = -desc->size - desc->extra;
	desc->extra = 0;

	descriptor* right = desc->right;
	if (right && right->size < 0) {
		#ifdef VERBOSE_DEBUG
		fprintf(stderr, "%s: right\n", __func__);
		#endif
		// Right neighbour is free, merge it
		size += right->size;
		remove_descriptor(right, -right->size);
		#ifdef VERBOSE_DEBUG
		fprintf(stderr, "%s: before desc %p left %p right %p\n", __func__, desc, desc->left, desc->right);
		#endif
		desc = free_descriptor(right, desc);
		#ifdef VERBOSE_DEBUG
		fprintf(stderr, "%s: after desc %p left %p right %p\n", __func__, desc, desc->left, desc->right);
		#endif
		
		desc->size = size;
		insert_descriptor(desc, -desc->size);
		#ifdef CHECK_HEAP
		check_heap();
		#endif		
		remove_descriptor(desc, -desc->size);
	}

	descriptor* const left = desc->left;
	if (left->size < 0) {
		#ifdef VERBOSE_DEBUG
		fprintf(stderr, "%s: left\n", __func__);
		#endif
		remove_descriptor(left, -left->size);
		left->size += size;
		insert_descriptor(left, -left->size);
		free_descriptor(desc, 0);
	} else {
		size_t const extra = left->extra;
		left->extra  = 0;
		desc->start -= extra;
		desc->size   = size - extra;
		assert(desc->size < 0);
		insert_descriptor(desc, -desc->size);
	}
	// TODO lower brk if this free block ends at the brk
	#ifdef CHECK_HEAP	
	check_heap();
	#endif
}


void cainit(void)
{
	char* const old_brk    = sbrk(0);
	char* const desc_start = (char*)((((uintptr_t)old_brk + (WAY_SIZE - 1) - DESC_START_SET * LINE_SIZE) & ~(WAY_SIZE - 1)) + DESC_START_SET * LINE_SIZE);
	curbrk = desc_start + DESC_SETS * LINE_SIZE;
	setbrk(curbrk);

	multi_head* const head = (multi_head*)desc_start;
	head->free = (1U << (DESC_SETS * LINE_SIZE - sizeof(multi_head)) / sizeof(descriptor)) - 1 - 1;
	head->size = sizeof(descriptor);

	#ifdef VERBOSE_DEBUG
	fprintf(stderr, "initial descriptor block: %p\n", head);
	#endif

	descriptor* const self = (descriptor*)(head + 1);
	self->left      = 0;
	self->right     = 0;
	self->prev_next = 0;
	self->next      = 0;
	self->start     = head;
	self->size      = DESC_SETS * LINE_SIZE;
	head->back      = self;

	desc_free_list = head;
	tail           = self;
}

static inline unsigned ptr2set(void const* const ptr)
{
	return (uintptr_t)ptr >> LINE_SIZE_BITS & ((1U << CACHE_SET_BITS) - 1);
}

void* carelmalloc(size_t const size, enum alloc_relation_t const rel, ...)
{
	switch (rel) {
	case ALLOC_DIFFERENT_SET: {
		unsigned long sets[(CACHE_SETS + LONG_BIT - 1) / LONG_BIT];
		for (size_t i = 0; i != 2; ++i) {
			if (i == 0) {
				memcpy(sets, max_free, sizeof(sets));
			} else {
				memset(sets, 0xFF, sizeof(sets));
			}
			va_list ap;
			va_start(ap, rel);
			for (;;) {
				void const* ptr = va_arg(ap, void const*);
				if (!ptr)
					break;
				clear_bit(ptr2set(ptr), sets);
			}
			va_end(ap);

			unsigned const set = find_next_bit(sets, CACHE_SETS, 0);
			if (set >= CACHE_SETS)
				continue;
			return camalloc(size, set);
		}
		return 0;
	}

	case ALLOC_SAME_SET: {
		void* res = 0;

		va_list ap;
		va_start(ap, rel);
		void const* const first = va_arg(ap, void const*);
		if (first) {
			unsigned const set = ptr2set(first);
			for (;;) {
				void const* ptr = va_arg(ap, void const*);
				if (!ptr) {
					res = camalloc(size, set);
					break;
				} else if (ptr2set(ptr) != set) {
					break;
				}
			}
		}

		va_end(ap);
		return res;
	}

	default:
		abort();
	}
}
