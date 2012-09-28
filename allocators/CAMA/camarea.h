/******************************************************************************
 * Cache-Aware Memory Allocator (CAMA) with Area Blocks
 * Version 1.0
 *
 * Authors:
 *   Christoph Mallon (mallon@cs.uni-saarland.de)
 *   Joerg Herter (jherter@cs.uni-saarland.de)
 *
 ******************************************************************************/

#ifndef _CAMAREA_H
#define _CAMAREA_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/*
 * Initializes the allocator.
 * A program using CAMAREA needs to call this method once before using
 * camalloc(...) and cafree(...).
 */
void  cainit(void);
/*
 * Returns a pointer to a memory block of at least size bytes where
 * the first byte is mapped to cache set set; or null if the allocator
 * was unable to satisfy the request.
 */
void* camalloc(size_t size, unsigned set);
/*
 * Deallocates, i.e. marks as free, the memory block pointed to by ptr. 
 */
void  cafree(void* ptr);

/* EDB: Added. */
size_t camsize(void* ptr);


/*
 * Cache-set relational allocations add-on:
 */
enum alloc_relation_t {
	ALLOC_DIFFERENT_SET,
	ALLOC_SAME_SET
};
/* Allocate size bytes of memory, which has set relation rel to the null-pointer
 * terminated list of pointers.
 * For ALLOC_DIFFERENT_SET at least one set must not be excluded.
 * For ALLOC_SAME_SET at least one pointer must be given and all pointers must
 * point to the same set. */
void* carelmalloc(size_t size, enum alloc_relation_t rel, ...);

#ifdef __cplusplus
}
#endif

#endif
