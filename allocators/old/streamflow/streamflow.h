/*
 * Streamflow memory allocator.
 *
 * Main code contributors: Christos Antonopoulos, Scott Schneider
 */

#ifndef __FREEFLOW_H__
#define __FREEFLOW_H__

/* HACK! There's some nastiness going on when I compile 
 * with xlc. The system files assume this definition will 
 * be there, but it's not. It's noted as a bug online. */
/*
typedef struct {
	unsigned int u[4];
} __attribute((aligned(16))) __uint128_t;
*/

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <math.h>
#include <assert.h>
#include <errno.h>
#include <string.h>

#include <stddef.h>
#include <dirent.h>

#include "queue.h"
#include "lock.h"
#include "bitops.h"

extern unsigned int global_id_counter;
extern __thread unsigned int thread_id;

/* Architecture-dependent parameters. */
#ifdef x86

#define CACHE_LINE_SIZE		128
#define SUPERPAGE_SIZE		(4 * 1024 * 1024)
#define BUDDY_ORDER_MAX		11
#define BUDDY_BITMAP_SIZE	148
#define PAGES_IN_ADDR_SPACE	1048676	/* (4 gig address space) / (page size) = # of pages in system */
#define BPL			32
#define GOLDEN_RATIO		2654435769UL

#elif ppc64

#define CACHE_LINE_SIZE		128
#define SUPERPAGE_SIZE		(16 * 1024 * 1024)
#define BUDDY_ORDER_MAX		13
#define BUDDY_BITMAP_SIZE	560
#define BPL			64
#define GOLDEN_RATIO		1140071482UL

#else

#error "Must define an architecture (x86, ppc64)."

#endif

#if !defined(HEADERS) && !defined(BIBOP) && !defined(RADIX_TREE)
#error "Must define a meta-information method (HEADERS, BIBOP or RADIX_TREE)."
#endif

/* System parameters */
#define PAGE_SIZE		4096
#define PAGES_PER_SUPERPAGE	(SUPERPAGE_SIZE / PAGE_SIZE)
#define SUPERPAGE_LOCATION	"/mnt/huge/superpage_file_XXXXXX"

#define SUPERPAGE_DIRECTORY	"/mnt/huge/"
#define SUPERPAGE_TEMP		"/tmp/supermap/"

#define PAGE_BITS		12
#define PAGEBLOCK_BITS		((sizeof(void*) * 8) - PAGE_BITS)
#define HEADER_SIZE		sizeof(void*)

/* Policy parameters */
#define MAX_PRIVATE_INACTIVE	1
#define MAX_GLOBAL_INACTIVE	0
#define MIN_PAGEBLOCK_SIZE	(4 *  PAGE_SIZE)
#define MAX_PAGEBLOCK_SIZE	(16 * PAGE_SIZE)
#define PAGEBLOCK_SIZE_CLASSES	5	/* log(MAX_PAGEBLOCK_SIZE/PAGE_SIZE) - log(MIN_PAGEBLOCK_SIZE/PAGE_SIZE) + 1 */
#define OBJECTS_PER_PAGEBLOCK	1024
#define OBJECT_GRANULARITY	HEADER_SIZE
#define COLOR_MAX		16
#define	COLOR_THRESHOLD		0
#define HASH_TABLE_SIZE		1024
#define LOG2_HASH_TABLE_SIZE	10
#define ORPHAN			UINT_MAX

/* The radix tree is RADIX_DEPTH levels deep. Hence, we need to split 
 * a page pointer into RADIX_DEPTH prefixes. If the page pointer is not 
 * evenly divisible by RADIX_DEPTH, then the intertior nodes have the 
 * greater number of bits, and the leaves less. */
#define RADIX_BITS		PAGEBLOCK_BITS
#define RADIX_DEPTH		3
#define RADIX_INTERIOR_BITS	(((RADIX_BITS + (RADIX_DEPTH - 1)) / RADIX_DEPTH))
#define RADIX_LEAF_BITS		(RADIX_BITS - 2 * (RADIX_INTERIOR_BITS))
#define RADIX_INTERIOR_SIZE	(1UL << RADIX_INTERIOR_BITS)
#define RADIX_LEAF_SIZE		(1UL << RADIX_LEAF_BITS)

union header {
	struct {
		unsigned long size:31,large_blk:1;
	};
	struct {
		unsigned long pageblock:PAGEBLOCK_BITS,index:11,large:1;
	};
};

struct queue_node {
	unsigned short next;
	unsigned short count;
};

struct page_record {
	unsigned char offset:7, large:1;
};

struct radix_interior {
	struct radix_interior* prefixes[RADIX_INTERIOR_SIZE];	
};

struct radix_leaf {
	struct page_record values[RADIX_LEAF_SIZE];
};

struct double_list_elem {
	void*				__padding;
	struct double_list_elem*	next;
	struct double_list_elem*	prev;
};

struct double_list {
	struct double_list_elem*	head;
	struct double_list_elem*	tail;
};

struct hash_node {
	void*           	key;
	struct hash_node*	next;
	struct hash_node*	prev;
	unsigned long   	used_colors[COLOR_MAX];
};

struct counting_queue {
	struct queue_elem_t*	queue;
	unsigned int		count;
};

struct counting_lf_lifo_queue {
	lf_lifo_queue_t		queue;
	unsigned int		count;
};

struct heap {
	struct double_list active_pageblocks;	/* active pageblocks that don't need synchronization */
};

/* An array of these gives us the main data structure necessary for 
 * the buddy allocation algorithm. Each buddy_order_t represents 
 * free page chunks of order i, where the size of the page chunk 
 * (in pages) is 2^i. The free list has the list of free page chunks, 
 * of size 2^i, and bitmap points to the bitmap used to tell which 
 * page chunks are allocated. */
struct buddy_order {
	struct double_list	free_list;
	char*			bitmap;
};

/* Represents a superpage.*/
struct superpage {
	void*			page_pool;			/* points to the superpage itself */
	struct superpage*	next;
	struct superpage*	prev;

	/* Data structures and values used for buddy 
	 * allocation.*/
	struct buddy_order	buddy[BUDDY_ORDER_MAX];
	char			bitmaps[BUDDY_BITMAP_SIZE];
	unsigned short		largest_free_order;

	unsigned long		file_offset;			/* this superpage's offset into SUPERPAGE_LOCATION */
};

struct pageblock {
	struct superpage*	sph;			/* points to the superpage header; needs to 
							 * be the first 4 bytes of a pageblock_t */	
	struct pageblock*	next;			/* points to next pageblock in pageblock list */
							/* pageblock_t *next must be the second pointer in the struct *
							 * for compatibility with queue_elem_t in queue.h	 */
	struct pageblock*	prev;			/* points to previous pageblock in pageblock list */
	unsigned short		freed;			/* points to first free, recycled object */
	                                                /* ... or to the first block with a recycled object if we *
	                                                 * are in 'SPATIAL_LOCALITY' mode			  */
	unsigned short		unallocated;		/* points to first free, never used object */
	struct heap*		owning_heap;		/* pointer to thread-local object table */
	int 			object_size;		/* size in bytes of all objects in pageblock; our "size class" */
	int			num_free_objects;	/* total number of free objects in pageblock */
	int			mem_pool_size;		/* size in bytes of the object space */

	/* These two values have be in a union together because we 
	 * do 8-byte compared-and-swap operations on them. */
 	union {
		struct {
			volatile unsigned int		owning_thread;	/* indicates which thread owns the pageblock */
			volatile struct queue_node	garbage_head;	/* objects that need to be garbage collected in this pageblock */
		};
		volatile unsigned long long together;
	};

	char*			mem_pool;
};

/* A quickieblock is used to keep track of headers for internal data structures. 
 * It doesn't need the same level of organization that a normal pageblock does, but 
 * the same-named fields do the same thing. */
struct quickieblock {
	void*	freed;
	void*	unallocated;
	int	num_free_objects;
};

/* A page_chunk_t is used to represent chunks of pages in the buddy allocation 
 * algorithm. We just use the empty space of the chunk itself to contain this 
 * information, which conveniently means that &page_chunk_variable is the address 
 * of the start of the page chunk itself. This also implies that used chunk pages 
 * are not represented in the data structures. */
typedef struct double_list_elem page_chunk_t;

typedef union header			header_t;
typedef struct queue_node		queue_node_t;
typedef struct page_record		page_record_t;
typedef struct radix_interior		radix_interior_t;
typedef struct radix_leaf		radix_leaf_t;
typedef struct double_list_elem		double_list_elem_t;
typedef struct double_list		double_list_t;
typedef struct hash_node		hash_node_t;
typedef struct counting_lf_lifo_queue	counting_lf_lifo_queue_t;
typedef struct counting_queue		counting_queue_t;
typedef struct heap			heap_t;
typedef struct superpage		superpage_t;
typedef struct pageblock		pageblock_t;
typedef struct quickieblock		quickieblock_t;
typedef struct buddy_order		buddy_order_t;

/* public streamflow operations */
void streamflow_thread_finalize(void);
void* malloc(size_t requested_size);
void free(void* object);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void *valloc(size_t size);
void *memalign(size_t boundary, size_t size);
int posix_memalign(void **memptr, size_t alignment, size_t size);

#ifdef MEMORY
void timer_handler(int);
#endif

#endif	// __FREEFLOW_H__

