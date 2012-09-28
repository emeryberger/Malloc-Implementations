/*
 * Streamflow memory allocator.
 *
 * Copyright (C) 2007  Scott Schneider, Christos Antonopoulos
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "streamflow.h"

#ifdef PROFILE
unsigned long long malloc_cycles;
unsigned long long malloc_compute_cycles;
unsigned long long garbage_cycles;
unsigned long long get_pgblk_cycles;
unsigned long long free_cycles;
unsigned long long free_compute_cycles;
unsigned long long extract_cycles;
unsigned long long local_free_cycles;
unsigned long long adopt_pgblk_cycles;
unsigned long long remote_free_cycles;
unsigned long long supermap_cycles;
unsigned long long superunmap_cycles;

unsigned long long malloc_calls;
unsigned long long free_calls;
#endif

/* Used for gathering memory stats. */
static volatile unsigned int num_total_small;
static volatile unsigned int num_total_medium;
static volatile unsigned int num_total_large;
static volatile unsigned int num_frees;
static volatile unsigned int num_remote_frees;
static volatile unsigned int num_adoptions;
static volatile unsigned long long size_total_small;
static volatile unsigned long long size_total_medium;
static volatile unsigned long long size_total_large;

#ifdef MEMORY
static int init_flag;
static lock_t init_lock;
#endif

__thread counting_queue_t remote_cache;
__thread unsigned int remote_cache_total;

/* Global thread_id counter. */
unsigned int global_id_counter = 0;

/* Thread ID that is our index in the global object table. */
__thread unsigned int thread_id = 0;

#ifdef NUMA
/* Maps a cpu to the node its on. */
int cpu_to_node[NUM_NUMA_NODES];
#endif

static radix_interior_t* radix_root;

/* All superpage related structures. The lock protects 
 * the other two structures. */
#ifdef NUMA
static __thread lock_t super_lock;
static __thread double_list_t superpage_list;
static __thread quickieblock_t sph_pageblocks;
#else
static lock_t super_lock;
static double_list_t superpage_list;
static quickieblock_t sph_pageblocks;
#endif

#ifdef BIBOP
/* Each page in virtual memory has an entry in the page vector that records two 
 * pieces of information: is this a "large" or small object, and the offset to 
 * the start of the pageblock/object from this page. */
static page_record_t bibop[PAGES_IN_ADDR_SPACE];
#endif

/* The following four arrays are for pageblocks in their various states of use:
 *
 * 	local_heap:
 * 		thread local active pageblocks, indexed by object class; the address 
 * 		doubles as the thread id
 * 	local_inactive_pageblocks:
 * 		thread local cached inactive pageblocks, indexed by pageblock size
 * 	global_partial_pageblocks:
 * 		global orphaned pageblocks whose owning thread has terminated, 
 * 		indexed by object class 
 * 	global_free_pageblocks:
 * 		global cached completely free pageblocks, indexed by pageblock size 
 */
static __thread heap_t local_heap[OBJECT_SIZE_CLASSES];
static __thread counting_queue_t local_inactive_pageblocks[PAGEBLOCK_SIZE_CLASSES];
static counting_lf_lifo_queue_t global_partial_pageblocks[OBJECT_SIZE_CLASSES];
static counting_lf_lifo_queue_t global_free_pageblocks[PAGEBLOCK_SIZE_CLASSES];

static inline unsigned int quick_log2(unsigned int x);
static inline int max(int a, int b);
static inline int compute_size_class(size_t size);
static inline int malloc_compute_size_class(size_t size);
static inline int free_compute_size_class(size_t size);
static inline int reverse_size_class(size_t size_class);

/* Radix tree operations. */
static void radix_register(void* start, int num_pages, void* ptr, size_t size, short object_type);
static inline void radix_extract(void* object, void** ptr, size_t* size, short* is_large);
static inline radix_interior_t* radix_interior_alloc(void);
static inline radix_leaf_t* radix_leaf_alloc(void);
static inline void radix_interior_free(radix_interior_t* node);
static inline void radix_leaf_free(radix_leaf_t* node);

/* Operations on quickie pageblocks. */
static void* quickie_alloc(quickieblock_t* quickie, size_t object_size);
static inline void quickie_free(quickieblock_t* quickie, void* object);

/* Buddy operations on superpages. */
static inline int find_index(superpage_t* super, page_chunk_t* chunk, int order);
static inline void* find_buddy(superpage_t* super, page_chunk_t* chunk, int order);
static inline int find_bit_index(superpage_t* super, page_chunk_t* chunk, int order);
static void* buddy_alloc(superpage_t* super, size_t size);
static void buddy_free(superpage_t* super, void* start, size_t length);

/* All other superpage operations. */
static void get_free_superpage(superpage_t** sp, size_t size);
static void* supermap(size_t size);
static void superunmap(void* start, size_t length);

/* All virtual page operations. */
static inline void register_pages(void* start, int num_pages, void* ptr, size_t size, short object_type);
static inline void* page_alloc(size_t size);
static inline void page_free(void* start, size_t length);
static void* medium_or_large_alloc(size_t size);

/* All operations on the global free list. */
static void insert_global_free_pageblocks(pageblock_t* pageblock);
static void insert_global_partial_pageblocks(pageblock_t* pageblock, int class_index);
static pageblock_t* remove_global_pageblocks(int class_index, int pageblock_size);

/* All double list operations. */
static void double_list_insert_front(void* new_node, double_list_t* list);
static void double_list_rotate_back(double_list_t* list);
static void double_list_remove(void* node, double_list_t* list);

/* Helper functions for malloc */
static inline void headerize_object(void** object, void* ptr, size_t size, short object_type);
static inline int compute_pageblock_size(int index);
static pageblock_t* get_free_pageblock(heap_t* heap, int index);

/* Helper functions for free. */
static inline void local_free(void* object, pageblock_t* pageblock, heap_t* my_heap);
static void remote_free(void* object, pageblock_t* pageblock, heap_t* my_heap);
static void adopt_pageblock(void* object, pageblock_t* pageblock, heap_t* my_heap);

static const int base[] = {	0, 16, 24, 28, 30, 31, 31, 32, 32, 32, 
				32, 33, 33, 33, 33, 34, 34, 34, 34, 35, 
				35, 35, 35, 36, 36, 36, 36, 37, 37, 37, 
				37, 38, 38, 38, 38, 39, 39, 39, 39, 40, 
				40, 40, 40, 41, 41, 41, 41, 42, 42, 42, 
				42, 43, 43, 43, 43, 44, 44, 44, 44, 45, 
				45, 45, 45, 46, 46, 46, 46, 47, 47, 47, 
				47, 48, 48, 48, 48, 49, 49, 49, 49, 50, 
				50, 50, 50, 51, 51, 51, 51, 52, 52, 52, 
				52, 53, 53, 53, 53, 54, 54, 54, 54, 55, 
				55, 55, 55, 56, 56, 56, 56, 57, 57, 57, 
				57, 58, 58, 58, 58, 59, 59, 59, 59, 60, 
				60, 60, 60, 61, 61, 61, 61, 62, 62, 62, 
				62, 63, 63, 63, 63, 64, 64, 64, 64, 65, 
				65, 65, 65, 66, 66, 66, 66, 67, 67, 67, 
				67, 68, 68, 68, 68, 69, 69, 69, 69, 70, 
				70, 70, 70, 71, 71, 71, 71, 72, 72, 72, 
				72, 73, 73, 73, 73, 74, 74, 74, 74, 75, 
				75, 75, 75, 76, 76, 76, 76, 77, 77, 77, 
				77, 78, 78, 78, 78, 79, 79, 79, 79, 80, 
				80, 80, 80, 81, 81, 81, 81, 82, 82, 82, 
				82, 83, 83, 83, 83, 84, 84, 84, 84, 85, 
				85, 85, 85, 86, 86, 86, 86, 87, 87, 87, 
				87, 88, 88, 88, 88, 89, 89, 89, 89, 90, 
				90, 90, 90, 91, 91, 91, 91, 92, 92, 92, 
				92, 93, 93, 93, 93, 94, 94, 94, 94 };

static const int factor[] = {	4, 8, 16, 32, 64, 128, 256, 256, 256, 256,
				256, 256, 256, 256, 256, 256, 256, 256, 256, 256,
				256, 256, 256, 256, 256, 256, 256, 256, 256, 256,
				256, 256, 256, 256, 256, 256, 256, 256, 256, 256,
				256, 256, 256, 256, 256, 256, 256, 256, 256, 256,
				256, 256, 256, 256, 256, 256, 256, 256, 256, 256,
				256, 256, 256, 256, 256, 256, 256, 256, 256, 256,
				256, 256, 256, 256, 256, 256, 256, 256, 256, 256,
				256, 256, 256, 256, 256, 256, 256, 256, 256, 256,
				256, 256, 256, 256, 256, 256, 256, 256, 256, 256,
				256, 256, 256, 256, 256, 256, 256, 256, 256, 256,
				256, 256, 256, 256, 256, 256, 256, 256, 256, 256,
				256, 256, 256, 256, 256, 256, 256, 256, 256, 256,
				256, 256, 256, 256, 256, 256, 256, 256, 256, 256,
				256, 256, 256, 256, 256, 256, 256, 256, 256, 256,
				256, 256, 256, 256, 256, 256, 256, 256, 256, 256,
				256, 256, 256, 256, 256, 256, 256, 256, 256, 256,
				256, 256, 256, 256, 256, 256, 256, 256, 256, 256,
				256, 256, 256, 256, 256, 256, 256, 256, 256, 256,
				256, 256, 256, 256, 256, 256, 256, 256, 256, 256,
				256, 256, 256, 256, 256, 256, 256, 256, 256, 256,
				256, 256, 256, 256, 256, 256, 256, 256, 256, 256,
				256, 256, 256, 256, 256, 256, 256, 256, 256, 256,
				256, 256, 256, 256, 256, 256, 256, 256, 256, 256,
				256, 256, 256, 256, 256, 256, 256, 256, 256, 256,
				256, 256, 256, 256, 256, 256, 256, 256, 256 };

static const int reverse[] = {	4, 8, 12, 16, 20, 24, 28, 32, 36, 40,
				44, 48, 52, 56, 60, 64, 72, 80, 88, 96,
				104, 112, 120, 128, 144, 160, 176, 192, 224, 256,
				320, 448, 704, 960, 1216, 1472, 1728, 1984, 2240, 2496,
				2752, 3008, 3264, 3520, 3776, 4032, 4288, 4544, 4800, 5056,
				5312, 5568, 5824, 6080, 6336, 6592, 6848, 7104, 7360, 7616,
				7872, 8128, 8384, 8640, 8896, 9152, 9408, 9664, 9920, 10176,
				10432, 10688, 10944, 11200, 11456, 11712, 11968, 12224, 12480, 12736,
				12992, 13248, 13504, 13760, 14016, 14272, 14528, 14784, 15040, 15296,
				15552, 15808, 16064, 16320, 16576 }; 

static inline unsigned long get_cycles(void);

#if __INTEL_COMPILER
#include <ia64intrin.h>
inline static unsigned long get_cycles(void)
{
	unsigned long long t;
	t = __getReg(_IA64_REG_AR_ITC);
	return t;
}
#else
inline static unsigned long get_cycles(void)
{
	unsigned long tmp;
	__asm__ __volatile__("mov %0=ar.itc":"=r"(tmp)::"memory");
	return tmp;
}
#endif
     
#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)

/* If we're collecting memory stats, does an atomic add. If not, does nothing. */
static inline void memory_add32(volatile unsigned int* address, int value)
{
#ifdef MEMORY
	atmc_add32(address, value);
#endif
}

static inline void memory_add64(volatile unsigned long long* address, unsigned long long value)
{
#ifdef MEMORY
	atmc_add64(address, value);
#endif
}

/* So, this is embarassingly naive and brittle, but it also noticabley 
 * outperforms log2(). Returns the base 2 logarithm of x, assuming x is a 
 * power-of-2. */
static inline unsigned int quick_log2(unsigned int x)
{
	switch (x) {
		case 1:		return 0;
		case 2:		return 1;
		case 4:		return 2;
		case 8:		return 3;
		case 16:	return 4;
		case 32:	return 5;
		case 64:	return 6;
		case 128:	return 7;
		case 256:	return 8;
		case 512:	return 9;
		case 1024:	return 10;
		case 2048:	return 11;
		case 4096:	return 12;
		case 8192:	return 12;
		case 16384:	return 13;
	}

	fprintf(stderr, "quick_log2() unhandled number: %u\n", x);
	assert(0);

	return -1;
}

static inline int max(int a, int b)
{
	return (a > b) ? a: b;
}

static inline int compute_size_class(size_t size)
{
	if (size < OBJECT_GRANULARITY) {
		size = OBJECT_GRANULARITY;
	}

	unsigned int bin = size / (CACHE_LINE_SIZE >> 1);
	unsigned int position = (size - 1) % (CACHE_LINE_SIZE >> 1);

	if (size % (CACHE_LINE_SIZE >> 1) == 0) {
		bin = (size - 1) / (CACHE_LINE_SIZE >> 1);
		position = (size - 2) % (CACHE_LINE_SIZE >> 1);
	}

	return base[bin] + (position / factor[bin]);
}

static inline int malloc_compute_size_class(size_t size)
{
#ifdef PROFILE
	unsigned long long local_malloc_compute_cycles = get_cycles();
#endif 
	if (size < OBJECT_GRANULARITY) {
		size = OBJECT_GRANULARITY;
	}

	unsigned int bin = size / (CACHE_LINE_SIZE >> 1);
	unsigned int position = (size - 1) % (CACHE_LINE_SIZE >> 1);

	if (size % (CACHE_LINE_SIZE >> 1) == 0) {
		bin = (size - 1) / (CACHE_LINE_SIZE >> 1);
		position = (size - 2) % (CACHE_LINE_SIZE >> 1);
	}

	int ret = base[bin] + (position / factor[bin]);
#ifdef PROFILE
	malloc_compute_cycles += get_cycles() - local_malloc_compute_cycles;
#endif
	return ret;
}

static inline int free_compute_size_class(size_t size)
{
#ifdef PROFILE
	unsigned long long local_free_compute_cycles = get_cycles();
#endif
	if (size < OBJECT_GRANULARITY) {
		size = OBJECT_GRANULARITY;
	}

	unsigned int bin = size / (CACHE_LINE_SIZE >> 1);
	unsigned int position = (size - 1) % (CACHE_LINE_SIZE >> 1);

	if (size % (CACHE_LINE_SIZE >> 1) == 0) {
		bin = (size - 1) / (CACHE_LINE_SIZE >> 1);
		position = (size - 2) % (CACHE_LINE_SIZE >> 1);
	}

	int ret = base[bin] + (position / factor[bin]);
#ifdef PROFILE
	free_compute_cycles += get_cycles() - local_free_compute_cycles;
#endif
	return ret;
}

static inline int reverse_size_class(size_t size_class)
{
	return reverse[size_class];
}

static inline radix_interior_t* radix_interior_alloc(void)
{
	void* node = page_alloc(sizeof(radix_interior_t));
	return (radix_interior_t*)node;
}

static inline radix_leaf_t* radix_leaf_alloc(void)
{
	void* node = page_alloc(sizeof(radix_leaf_t));
	return (radix_leaf_t*)node;
}

static inline void radix_interior_free(radix_interior_t* node)
{
	page_free(node, sizeof(radix_interior_t));
}

static inline void radix_leaf_free(radix_leaf_t* node)
{
	page_free(node, sizeof(radix_leaf_t));
}

static void radix_register(void* start, int num_pages, void* ptr, size_t size, short object_type)
{
	/* Ensure in a lock-free manner that we have a root node. */
	if (radix_root == NULL) {
		radix_interior_t* temp_root = radix_interior_alloc();
		if (!compare_and_swap_ptr(&radix_root, NULL, temp_root)) {
			radix_interior_free(temp_root);
		}
	}

	int i;

	unsigned int log_size = 0;
	if (object_type == OBJECT_MEDIUM) {
		log_size = quick_log2(size / PAGE_SIZE);
	}

	unsigned long page = (unsigned long)start >> PAGE_BITS;
	for (i = 0; i < num_pages; ++i) {
		unsigned long level1 = page >> (RADIX_INTERIOR_BITS + RADIX_LEAF_BITS);
		unsigned long level2 = (page >> RADIX_LEAF_BITS) & (RADIX_INTERIOR_SIZE - 1);
		unsigned long level3 = page & (RADIX_LEAF_SIZE - 1);
		page_record_t record;

		if (radix_root->prefixes[level1] == NULL) {
			radix_interior_t* temp_interior = radix_interior_alloc();
			if (!compare_and_swap_ptr(&radix_root->prefixes[level1], NULL, temp_interior)) {
				radix_interior_free(temp_interior);
			}
		}

		if (radix_root->prefixes[level1]->prefixes[level2] == NULL) {
			radix_leaf_t* temp_leaf = radix_leaf_alloc();
			if (!compare_and_swap_ptr(&radix_root->prefixes[level1]->prefixes[level2], NULL, temp_leaf)) {
				radix_leaf_free(temp_leaf);
			}
		}

		/* Accessing the third level does not need any synchronization. Since there is a 
		 * one-to-one correspondence between pages in the system and third level values, 
		 * and we assume the OS will not return the same page multiple times, we know 
		 * that we are the only one accessing this location. */
		record.object_type = object_type;
		switch (object_type) {
			case OBJECT_SMALL:	record.pageblock = (unsigned long)ptr >> PAGE_BITS;
						break;

			case OBJECT_MEDIUM: 	record.sph = (unsigned long)ptr >> SUPERPAGE_BITS; 
						record.log_size = log_size;
						break;

			case OBJECT_LARGE:	record.size = (unsigned long)size;
						break;
		}

		((radix_leaf_t*)radix_root->prefixes[level1]->prefixes[level2])->values[level3] = record;

		page += 1;	
	}
}

/* We assume that radix_register has already been called for this object's page. This 
 * allows us to assume that the nodes are already allocated. */
static inline void radix_extract(void* object, void** ptr, size_t* size, short* object_type)
{
	unsigned long page = (unsigned long)object >> PAGE_BITS;
	unsigned long level1 = page >> (RADIX_INTERIOR_BITS + RADIX_LEAF_BITS);
	unsigned long level2 = (page >> RADIX_LEAF_BITS) & (RADIX_INTERIOR_SIZE - 1);
	unsigned long level3 = page & (RADIX_LEAF_SIZE - 1);
	page_record_t record;
	record = ((radix_leaf_t*)radix_root->prefixes[level1]->prefixes[level2])->values[level3];

	*object_type = record.object_type;
	switch (*object_type) {
		case OBJECT_SMALL:	*ptr = (void*)(record.pageblock << PAGE_BITS);
					break;
		case OBJECT_MEDIUM:	*ptr = (void*)(record.sph << SUPERPAGE_BITS);
					*size = (size_t)(1 << record.log_size) * PAGE_SIZE;
					break;
		case OBJECT_LARGE:	*size = (size_t)record.size;
					break;
	}
}

static inline int find_index(superpage_t* super, page_chunk_t* chunk, int order)
{
	return ((unsigned long)chunk - (unsigned long)super->page_pool) / (PAGE_SIZE * (1 << order));
}

static inline void* find_buddy(superpage_t* super, page_chunk_t* chunk, int order)
{
	void* buddy;
	int i = find_index(super, chunk, order);

	/* If i is even, buddy is on the right; if odd, buddy 
	 * is on the left. */
	if (i % 2 == 0) {
		buddy = (superpage_t*)((unsigned long)chunk + ((1 << order) * PAGE_SIZE));	
	}
	else {
		buddy = (superpage_t*)((unsigned long)chunk - ((1 << order) * PAGE_SIZE));
	}

	return buddy;
}

/* When we index the bitmap, each buddy in a pair needs to map to the same 
 * location. find_bit_index() takes care of this. */
static inline int find_bit_index(superpage_t* super, page_chunk_t* chunk, int order)
{
	int i = find_index(super, chunk, order);

	/* We'll decide that the even buddy (the one on the right) has the 
	 * correct location, so we need to adjust the odd buddy. */
	if (i % 2 != 0) {
		--i;
	}

	return i / 2;
}

/* Allocates size pages from the buddy allocation scheme.
 * size is guarenteed to be a multiple of PAGE_SIZE. */
static void* buddy_alloc(superpage_t* super, size_t size)
{
	page_chunk_t* chunk = NULL;
	unsigned int order = quick_log2(size / PAGE_SIZE);
	unsigned int curr_order;

	/* Starting at the closest fit, try to find a page chunk to satisfy 
	 * the request. */
	for (curr_order = order; curr_order < BUDDY_ORDER_MAX; ++curr_order) {

		if (super->buddy[curr_order].free_list.head != NULL) {
			chunk = super->buddy[curr_order].free_list.head;
			double_list_remove(chunk, &super->buddy[curr_order].free_list);
			__change_bit(find_bit_index(super, chunk, curr_order), (unsigned long *)super->buddy[curr_order].bitmap);
			break;
		}
	}

	/* If our page chunk is from a higher order, we need to split it 
	 * up. */
	size = 1 << curr_order;	
	page_chunk_t* buddy;
	while (curr_order > order) {

		--curr_order;
		size >>= 1;

		/* We don't need to call find_buddy() because we know that chunk is 
		 * on the left. */
		buddy = (page_chunk_t*)((unsigned long)chunk + (size * PAGE_SIZE));

		double_list_insert_front(chunk, &super->buddy[curr_order].free_list);
		__change_bit(find_bit_index(super, chunk, curr_order), (unsigned long *)super->buddy[curr_order].bitmap);
		chunk = buddy;
	}

	/* Figure out what the highest free order is. */
	if (super->buddy[super->largest_free_order].free_list.head == NULL) {
		int sorder;

		for (sorder = super->largest_free_order - 1; sorder >= 0; --sorder) {
			if (super->buddy[sorder].free_list.head != NULL) {
				super->largest_free_order = sorder;
				break;
			}
		}
		if (sorder < 0) {
			super->largest_free_order = BUDDY_ORDER_MAX + 1;
		}
	}

	return (void*)chunk;
}

/* Frees pages back to the buddy scheme. */
static void buddy_free(superpage_t* super, void* start, size_t length)
{
	page_chunk_t* chunk = (page_chunk_t*)start;
	page_chunk_t* buddy;
	unsigned int order = quick_log2(length / PAGE_SIZE);
	unsigned int curr_order;

	length = 1 << order;

	for (curr_order = order; curr_order < BUDDY_ORDER_MAX - 1; ++curr_order) {

		length <<= 1;

		/* If the buddy is still allocated, then no merging can take place. */
		if (!__test_and_change_bit(find_bit_index(super, chunk, curr_order), (unsigned long *)super->buddy[curr_order].bitmap)) {
			break;
		}
		buddy = find_buddy(super, chunk, curr_order);
		double_list_remove(buddy, &super->buddy[curr_order].free_list);

		/* If I am the odd buddy, then I need to change where I am
		 * for the next pass. */
		if (find_index(super, chunk, curr_order) % 2 != 0) {
			chunk = buddy;
		}
	}

	/* If there are still used page chunks, add it to the appropriate 
	 * free list. Otherwise, we merged page chunks all the way back up 
	 * to an entire superpage, which means we can return it to the OS. */
	if (curr_order < BUDDY_ORDER_MAX - 1) {
		double_list_insert_front(chunk, &super->buddy[curr_order].free_list);
		if (curr_order > super->largest_free_order || super->largest_free_order > BUDDY_ORDER_MAX) {
			super->largest_free_order = curr_order;
		}
	}
	else {
		page_free(chunk, SUPERPAGE_SIZE);
		double_list_remove(super, super->list);
		quickie_free(super->quickie, super);
	}
}

static void* quickie_alloc(quickieblock_t* quickie, size_t object_size)
{
	void* object;

	/* We need to allocate space for a new pageblock in two cases: the first time 
	 * this function is called (in which case unallocated will not point to 
	 * anything), and when there is no more space in the last pageblock we allocated. */
	if (quickie->unallocated == NULL || quickie->num_free_objects == 0) {
		quickie->unallocated = page_alloc(PAGE_SIZE);
		quickie->num_free_objects = PAGE_SIZE / object_size;
	}

	if (quickie->freed != NULL) {
		object = quickie->freed;
		quickie->freed = *((void**)(quickie->freed));
	}
	else {
		object = (superpage_t*)quickie->unallocated;
		quickie->unallocated += object_size;
	}
	--(quickie->num_free_objects);

	return object;
}

static inline void quickie_free(quickieblock_t* quickie, void* object)
{
	*((void**)object) = quickie->freed;
	quickie->freed = (void*)object;
}

static void get_free_superpage(superpage_t** super, size_t size)
{
	double_list_elem_t* curr;
	double_list_elem_t* first;
	unsigned int order;

	/* Find a superpage with enough space for this allocation. */
	*super = NULL;
	curr = superpage_list.head;
	if (curr != NULL) {
		first = curr;
		do {
			/* First check to see if the superpage has any free pages; a value larger than 
			 * BUDDY_ORDER_MAX in the largest free order indicates this. Then check to see if 
			 * there's enough room for an allocation of size. */
			if (((superpage_t*)curr)->largest_free_order < BUDDY_ORDER_MAX && 
					(1 << ((superpage_t*)curr)->largest_free_order) >= (size / PAGE_SIZE))
			{
				*super = (superpage_t*)curr;
				break;
			}
			curr = curr->next;
			/* 
			double_list_rotate_back(&superpage_list);
			*/
		} while (curr != NULL && curr != first);
	}

	/* If we couldn't find an existing superpage, get a new one from OS. */
	if (*super == NULL) {
		*super = (superpage_t*)quickie_alloc(&sph_pageblocks, sizeof(superpage_t));
		(*super)->page_pool = page_alloc(SUPERPAGE_SIZE);
		
		/* Initialize bitmaps for buddy allocation */
		int byte = 0;
		(*super)->buddy[0].bitmap = &(*super)->bitmaps[0];
		for (order = 0; order < BUDDY_ORDER_MAX - 1; ++order) {
			byte += max(sizeof(unsigned long), (int)ceil(((double)PAGES_PER_SUPERPAGE / ((1 << order) * 8 * 2))));
			(*super)->buddy[order + 1].bitmap = &(*super)->bitmaps[byte];
		}
		memset((*super)->bitmaps, 0, BUDDY_BITMAP_SIZE); 

		/* The thread local super lock governs all superpages owned by 
		 * this thread. It doubles as a thread ID. */
		(*super)->lock = &super_lock;
		(*super)->list = &superpage_list;
		(*super)->quickie = &sph_pageblocks;

		/* Stick the entire superpage into the buddy allocation scheme. */
		double_list_insert_front((*super)->page_pool, &((*super)->buddy[BUDDY_ORDER_MAX - 1].free_list));
		(*super)->largest_free_order = BUDDY_ORDER_MAX - 1;
		double_list_insert_front(*super, &superpage_list);
	}
}

static void* supermap(size_t size)
{
	superpage_t* super;
	void* pages;

#ifdef PROFILE
	unsigned long long local_supermap_cycles = get_cycles();
#endif

	spin_lock(&super_lock);

	get_free_superpage(&super, size);

	/* Allocate pages from the superpage. */
	pages = buddy_alloc(super, size);
	/* UGLY: Write the superpage header pointer into the pageblock header. We 
	 * shouldn't "know" about pageblock at this point, but this is the easiest 
	 * way for superunmap() to know where the superpage header is for the 
	 * freed superpage. */
	((pageblock_t*)pages)->sph = super;

	spin_unlock(&super_lock);

#ifdef PROFILE
	supermap_cycles += get_cycles() - local_supermap_cycles;
#endif

	return pages;
}

static void superunmap(void* start, size_t length)
{
	superpage_t* super = ((pageblock_t*)start)->sph;
	lock_t* lock;

#ifdef PROFILE
	unsigned long long local_superunmap_cycles = get_cycles();
#endif

#ifdef NUMA
	lock = super->lock;
#else
	lock = &super_lock;
#endif
	spin_lock(lock);
	buddy_free(super, start, length);
	spin_unlock(lock);

#ifdef PROFILE
	local_superunmap_cycles += get_cycles() - local_superunmap_cycles;
#endif
}

/* If not using headers, registers pages in appropriate data structure. 
 * We assume that num_pages is a multiple of PAGE_SIZE. */
static inline void register_pages(void* start, int num_pages, void* ptr, size_t size, short object_type)
{
#ifdef RADIX_TREE
	radix_register(start, num_pages, ptr, size, object_type);
#elif BIBOP
	int i;
	unsigned long page = (unsigned long)start;
	for (i = 0; i < num_pages; ++i) {

		bibop[page / PAGE_SIZE].object_type = object_type;
		switch (object_type) {
			case OBJECT_SMALL:	bibop[page / PAGE_SIZE].pageblock = (unsigned long)ptr >> PAGE_BITS;
						break;

			case OBJECT_MEDIUM: 	bibop[page / PAGE_SIZE].sph = (unsigned long)ptr >> SUPERPAGE_BITS; 
						bibop[page / PAGE_SIZE].log_size = quick_log2(size / PAGE_SIZE);
						break;

			case OBJECT_LARGE:	bibop[page / PAGE_SIZE].size = size;
						break;
		}
		page += PAGE_SIZE;
	}
#endif
}

/* Makes a request to the system for size bytes. */
static inline void* page_alloc(size_t size)
{
	void* addr;

	addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (addr == MAP_FAILED) {
		perror("page_alloc");
		fprintf(stderr, "page_alloc() mmap of size %zd failed\n", size);
		fflush(stderr);
		exit(1);
	}

	return addr;
}

/* Frees length bytes to the system. */
static inline void page_free(void* start, size_t length)
{
	munmap(start, length);
}

/* Gets a large amount of memory from the OS and tags it appropriately. */
static void* medium_or_large_alloc(size_t size)
{
	void* mem;
	
	if (size <= SUPERPAGE_SIZE) {
		/* need to round size up to the nearest power-of-2 pages */
		size = ceil((double)size / PAGE_SIZE) * PAGE_SIZE;
		unsigned int pow = (size_t)ceil(log2(size));
		size = 1 << pow;
		mem = supermap(size);

		/* Optimization: since we really only care about the first
		 * page with large objects (that's the only page that free() 
		 * ever gets), we only need to register the first page. */
		register_pages(mem, 1, ((pageblock_t*)mem)->sph, size, OBJECT_MEDIUM);
		headerize_object(&mem, ((pageblock_t*)mem)->sph, size, OBJECT_MEDIUM);

		memory_add32(&num_total_medium, 1);
		memory_add64(&size_total_medium, size);
	}
	else {
		mem = page_alloc(size);
		register_pages(mem, 1, NULL, size, OBJECT_LARGE);
		headerize_object(&mem, NULL, size, OBJECT_LARGE);

		memory_add32(&num_total_large, 1);
		memory_add64(&size_total_large, size);
	}

	return mem;
}

/* Adds a pageblock to one of the global lists, or frees it to the OS/page manager. */
static void insert_global_free_pageblocks(pageblock_t* pageblock)
{
	int size_index = quick_log2((pageblock->mem_pool_size + (unsigned long)pageblock->mem_pool - (unsigned long)pageblock) / PAGE_SIZE)
			- quick_log2(MIN_PAGEBLOCK_SIZE / PAGE_SIZE);

	if (global_free_pageblocks[size_index].count >= MAX_GLOBAL_INACTIVE) {
		superunmap(pageblock, pageblock->mem_pool_size + CACHE_LINE_SIZE);
	}
	else {
		atmc_add32(&global_free_pageblocks[size_index].count, 1);
		lf_lifo_enqueue(&global_free_pageblocks[size_index].queue, pageblock);
	}
}

static void insert_global_partial_pageblocks(pageblock_t* pageblock, int class_index)
{
	atmc_add32(&global_partial_pageblocks[class_index].count, 1);
	lf_lifo_enqueue(&global_partial_pageblocks[class_index].queue, pageblock);
}

/* Attempts to get a global pageblock. */
static pageblock_t* remove_global_pageblocks(int class_index, int pageblock_size)
{
	pageblock_t* pageblock = (pageblock_t*)lf_lifo_dequeue(&global_partial_pageblocks[class_index].queue);
	if (pageblock) {
		atmc_add32(&global_partial_pageblocks[class_index].count, -1);
	}
	else {
		int size_index = quick_log2(pageblock_size / PAGE_SIZE) - quick_log2(MIN_PAGEBLOCK_SIZE / PAGE_SIZE);

		pageblock = (pageblock_t*)lf_lifo_dequeue(&global_free_pageblocks[size_index].queue);
		if (pageblock) {
			atmc_add32(&global_free_pageblocks[size_index].count, -1);
		}
	}

	return pageblock;
}

/* Places new_node at the front of the list. */
static void double_list_insert_front(void* new_node, double_list_t* list)
{
	double_list_elem_t* elem_new = (double_list_elem_t*)new_node;
	double_list_elem_t* old_head = list->head;

	if (old_head == NULL) {
		list->tail = elem_new;
	}
	else {
		old_head->prev = elem_new;
	}

	elem_new->next = old_head;
	elem_new->prev = NULL;
	list->head = elem_new;
}

/* Moves head to the back. */
static void double_list_rotate_back(double_list_t* list)
{
	double_list_elem_t* old_head = list->head;
	double_list_elem_t* old_tail = list->tail;
	double_list_elem_t* new_head = NULL;

	if (old_head == old_tail) {
		return;
	}

	new_head = old_head->next;

	new_head->prev = NULL;
	old_tail->next = old_head;
	old_head->prev = old_tail;
	old_head->next = NULL;
	
	list->head = new_head;
	list->tail = old_head;
}

/* Removes node from the list. */
static void double_list_remove(void* node, double_list_t* list)
{
	double_list_elem_t* elem_node = (double_list_elem_t*)node;

	if (elem_node->prev != NULL) {
		elem_node->prev->next = elem_node->next;
	}
	else {
		list->head = elem_node->next;
	}

	if (elem_node->next != NULL) {
		elem_node->next->prev = elem_node->prev;
	}
	else {
		list->tail = elem_node->prev;
	}

	if (list->head != NULL && list->head->next == NULL) {
		list->tail = list->head;
	}
	else if (list->tail != NULL && list->tail->prev == NULL) {
		list->head = list->tail;
	}
}

/* Garbage collect a single pageblock. */
static void garbage_collect(pageblock_t* collectee)
{
	unsigned int chain;
	queue_node_t header;
	unsigned short index;
#ifdef PROFILE
	unsigned long long local_garbage_cycles = get_cycles();
#endif

	chain = lf_lifo_chain_dequeue_nABA32((unsigned int *)&(collectee->garbage_head));
	header = *((queue_node_t*)&chain);
	index = header.next;
	collectee->freed = index;
	collectee->num_free_objects += ((queue_node_t *)&header)->count;

#ifdef PROFILE
	garbage_cycles += get_cycles() - local_garbage_cycles;
#endif
}

void streamflow_thread_finalize(void)
{
	int i;
	pageblock_t *pageblock, *next_pageblock;

	for (i = 0; i < OBJECT_SIZE_CLASSES; ++i) {
		heap_t* heap = &local_heap[i];

		/* TODO: Optimization. Create a linked list of all pageblocks 
		 * that need to go on the global list. (Both active and 
		 * inactive.) Then atomically link that list into the 
		 * global list. */

		pageblock = (pageblock_t*)heap->active_pageblocks.head;
		
		/* If active head is NULL the specific size has never been used */
		if (pageblock) {
			do {
				next_pageblock = pageblock->next;
				if (pageblock->num_free_objects == pageblock->mem_pool_size / pageblock->object_size) {
					insert_global_free_pageblocks(pageblock);
				}
				else {
					if (pageblock->num_free_objects > 0 || pageblock->garbage_head.next != 0) {
						insert_global_partial_pageblocks(pageblock, i);
					}
					else {
						unsigned long long with_id;
						unsigned long long no_id;

						with_id = pageblock->together;
						((unsigned int*)&no_id)[0] = ORPHAN;
						((unsigned int*)&no_id)[1] = 0;

						if (!compare_and_swap64(&(pageblock->together), with_id, no_id)) {
							insert_global_partial_pageblocks(pageblock, i);
						}
					}
				}			
				pageblock = next_pageblock;
			} while (pageblock != NULL);
		}
	}

	for (i = 0; i < PAGEBLOCK_SIZE_CLASSES; ++i) {
		while ((pageblock = (pageblock_t*)seq_lifo_dequeue(&local_inactive_pageblocks[i].queue)) != NULL) {
			insert_global_free_pageblocks(pageblock);
		}
	}
}

/* Returns a pageblock size that is a power-of-2. */
static inline int compute_pageblock_size(int index)
{
	/* Make sure that the suggestion is a page multiple. */
	unsigned int suggestion = ceil((double)(reverse_size_class(index) * OBJECTS_PER_PAGEBLOCK) / PAGE_SIZE) * PAGE_SIZE;

	/* 2^pow is the closest power-of-2 to suggestion. */
	unsigned int pow = (unsigned int)ceil(((log2(suggestion)) + 0.5));

	/* Make sure that suggestion is a power-of-2. */
	suggestion = 1 << pow;

	if (suggestion < MIN_PAGEBLOCK_SIZE) {
		return MIN_PAGEBLOCK_SIZE; 
	}
	else if (suggestion > MAX_PAGEBLOCK_SIZE) {
		return MAX_PAGEBLOCK_SIZE;
	}

	return suggestion;
}

/* At the end of this function, pageblock is guarenteed to point to a pageblock 
 * with a free object. */
static pageblock_t* get_free_pageblock(heap_t* heap, int index)
{
	pageblock_t* pageblock;
	int pageblock_size;
	int size_index;

#ifdef PROFILE
	unsigned long long local_get_pgblk_cycles = get_cycles();
#endif

	pageblock_size = compute_pageblock_size(index);
	size_index = quick_log2(pageblock_size / PAGE_SIZE) - quick_log2(MIN_PAGEBLOCK_SIZE / PAGE_SIZE);

	/* Check our inactive pageblocks. */
	pageblock = (pageblock_t*)seq_lifo_dequeue(&local_inactive_pageblocks[size_index].queue);
	
	/* If none are on inactive, check the global list. */
	if (pageblock == NULL) {		
		pageblock = remove_global_pageblocks(index, pageblock_size);

		if (pageblock && pageblock->num_free_objects == 0) {
			garbage_collect(pageblock);
		}
	}
	else {
		--local_inactive_pageblocks[size_index].count;
	}

	/* If there were no pre-allocated pageblocks, we need to grab one from the OS. */
	if (pageblock == NULL) {
		pageblock = (pageblock_t*)supermap(pageblock_size);
		register_pages(pageblock, pageblock_size / PAGE_SIZE, pageblock, 0, OBJECT_SMALL);

		lf_lifo_queue_init_nABA32((unsigned int *)&(pageblock->garbage_head));
		pageblock->freed = 0;
		pageblock->unallocated = 1;
		pageblock->object_size = reverse_size_class(index); 
		pageblock->mem_pool = (char*)((unsigned long)pageblock + (unsigned long)ceil((double)sizeof(pageblock_t) / CACHE_LINE_SIZE) * CACHE_LINE_SIZE);
		pageblock->mem_pool_size = pageblock_size - (unsigned long)pageblock->mem_pool + (unsigned long)pageblock;
		pageblock->num_free_objects = pageblock->mem_pool_size / pageblock->object_size;
	}
	else if (pageblock->object_size != reverse_size_class(index)) {
		pageblock->freed = 0;
		pageblock->unallocated = 1;
		pageblock->object_size = reverse_size_class(index);
		pageblock->num_free_objects = pageblock->mem_pool_size / pageblock->object_size;
	}

	/* Claim ownership of the pageblock. */
	pageblock->owning_heap = local_heap;
	pageblock->owning_thread = thread_id;

	/* New pageblock goes to front of active list. */
	double_list_insert_front(pageblock, &heap->active_pageblocks);

#ifdef PROFILE
	get_pgblk_cycles += get_cycles() - local_get_pgblk_cycles;
#endif

	return pageblock;
}

void timer_handler(int sig)
{
	fprintf(stderr, "totsmall %u totmedium %u totlarge %u szsmall %llu szmedium %llu szlarge %llu frees %u remote %u adopt %u\n",
			num_total_small, num_total_medium, num_total_large, 
			size_total_small, size_total_medium, size_total_large, 
			num_frees, num_remote_frees, num_adoptions);
	fflush(stderr);
}

void cycle_counts(int sig)
{
#ifdef PROFILE
	printf("malloc:         %llu\t%llu\n", malloc_cycles, malloc_calls);
	printf("  compute:      %llu\n", malloc_compute_cycles);
	printf("  garbage:      %llu\n", garbage_cycles);
	printf("  get_pgblk:    %llu\n", get_pgblk_cycles);
	printf("    supermap:   %llu\n", supermap_cycles);
	printf("\n");
	printf("free:           %llu\t%llu\n", free_cycles, free_calls);
	printf("  extract:      %llu\n", extract_cycles);
	printf("  compute:      %llu\n", free_compute_cycles);
	printf("  local_free:   %llu\n", local_free_cycles);
	printf("    superunmap: %llu\n", superunmap_cycles);
	printf("  remote_free:  %llu\n", remote_free_cycles);
	printf("  adopt_pgblk:  %llu\n", adopt_pgblk_cycles);
#endif
}

/* Checks to see if memory stuff has been initialized. If we're
 * not collecting memory stats, does nothing. */
static inline void memory_init_check(void)
{
#ifdef MEMORY
	if (!init_flag) {
		spin_lock(&init_lock);

		if (!init_flag) {
			struct sigaction act;

			memset(&act, 0, sizeof(act));
			act.sa_handler = &timer_handler;
			sigaction(SIGUSR1, &act, NULL);
			atexit(timer_handler);
		}

		init_flag = 1;
		spin_unlock(&init_lock);
	}
#endif
}

/* Adds an object header to an object if we're using headers. */
static inline void headerize_object(void** object, void* ptr, size_t size, short object_type)
{
#ifdef HEADERS
	((header_t *)*object)->object_type = object_type;

	switch (object_type) {
		case OBJECT_SMALL:	((header_t *)*object)->pageblock = (unsigned long)ptr >> PAGE_BITS;
					break;

		case OBJECT_MEDIUM: 	((header_t *)*object)->sph = (unsigned long)ptr >> SUPERPAGE_BITS; 
					((header_t *)*object)->log_size = quick_log2(size / PAGE_SIZE);
					break;

		case OBJECT_LARGE:	((header_t *)*object)->size = size;
					break;
	}
	*object += sizeof(header_t);
#endif
}

void* malloc(size_t requested_size)
{
	int index;
	void* pointer = NULL;
	pageblock_t *pageblock;
	heap_t *heap;

#ifdef PROFILE
	unsigned long long local_malloc_cycles = get_cycles();
	++malloc_calls;
#endif

	memory_init_check();
	
	if (requested_size == 0) {
		return NULL;
	}

#ifdef HEADERS
	requested_size += sizeof(header_t);
#endif

	/* We forward "large" objects directly to the OS. We define
	 * "large" as anything larger than half a page. */
	if (requested_size > MAX_OBJECT_SIZE) {
		return medium_or_large_alloc(requested_size);
	}

	memory_add32(&num_total_small, 1);
	memory_add64(&size_total_small, requested_size);

	index = malloc_compute_size_class(requested_size);

	heap = &local_heap[index];
	pageblock = (pageblock_t*)heap->active_pageblocks.head;

	/* Do we have a pageblock that needs garbage collection? */
	if (pageblock != NULL && pageblock->num_free_objects == 0) {
		garbage_collect(pageblock);
		if (pageblock->num_free_objects == 0) {
			double_list_rotate_back(&heap->active_pageblocks);
		}
	}

	/* If the head of the active list doesn't have a free object, we need to 
	 * get it elsewhere. */
	if (pageblock == NULL || pageblock->num_free_objects == 0) {
		pageblock = get_free_pageblock(heap, index);
	}

	/* Reserve object from pageblock. It can can be either an already 
	 * used object, or a never used object. After this if-else, pointer 
	 * points to the newly allocated object. */
	if (pageblock->freed != 0) {
		pointer = pageblock->mem_pool + (pageblock->freed - 1) * pageblock->object_size;
		pageblock->freed = ((queue_node_t *)pointer)->next;
	}
	else {
		pointer = pageblock->mem_pool + (pageblock->unallocated - 1) * pageblock->object_size;
		pageblock->unallocated++;
		
		if (pageblock->unallocated > pageblock->mem_pool_size / pageblock->object_size) {
			pageblock->unallocated = 0;
		}
	}

	--(pageblock->num_free_objects);

	if (pageblock->num_free_objects == 0) {
		double_list_rotate_back(&heap->active_pageblocks);
	}

	headerize_object(&pointer, pageblock, requested_size, OBJECT_SMALL);

#ifdef PROFILE
	malloc_cycles += get_cycles() - local_malloc_cycles;
#endif

	return pointer;
}

static inline void local_free(void* object, pageblock_t* pageblock, heap_t* my_heap)
{
#ifdef PROFILE
	//unsigned long long local_local_free_cycles = get_cycles();
#endif

	((queue_node_t *)object)->next = pageblock->freed;
	pageblock->freed = ((unsigned long)object - (unsigned long)pageblock->mem_pool) / pageblock->object_size + 1;

	++(pageblock->num_free_objects);

	/* If the pageblock is now completely empty, remove it from the active 
	 * list and add it to the inactive list. */
	if (pageblock->num_free_objects == (pageblock->mem_pool_size / pageblock->object_size)) {
		int size_index = quick_log2((pageblock->mem_pool_size + (unsigned long)pageblock->mem_pool - (unsigned long)pageblock) / PAGE_SIZE) - quick_log2(MIN_PAGEBLOCK_SIZE / PAGE_SIZE);

		double_list_remove(pageblock, &my_heap->active_pageblocks);
		if (local_inactive_pageblocks[size_index].count < MAX_PRIVATE_INACTIVE) {
			seq_lifo_enqueue(&local_inactive_pageblocks[size_index].queue, pageblock);
			local_inactive_pageblocks[size_index].count++;
		}
		else {
			insert_global_free_pageblocks(pageblock);
		}
	}
	/* Otherwise, we want to move it to the front of the active list. */
	else if (pageblock != (pageblock_t*)my_heap->active_pageblocks.head && pageblock->num_free_objects > 1) {
		double_list_remove(pageblock, &my_heap->active_pageblocks);
		double_list_insert_front(pageblock, &my_heap->active_pageblocks);
	}
	
#ifdef PROFILE
	//local_free_cycles += get_cycles() - local_local_free_cycles;
#endif
}

static void adopt_pageblock(void* object, pageblock_t* pageblock, heap_t* my_heap)
{
#ifdef PROFILE
	unsigned long long local_adopt_pgblk_cycles = get_cycles();
#endif

	/* So we try to adopt it. If we succeed, treat it like our own. If we fail, 
	 * let the new parent deal with it. */
	if (compare_and_swap32(&pageblock->owning_thread, ORPHAN, thread_id)) {
		double_list_insert_front(pageblock, &my_heap->active_pageblocks);
		local_free(object, pageblock, my_heap);

		memory_add32(&num_adoptions, 1);
	}
	else {
		remote_free(object, pageblock, my_heap);
	}

#ifdef PROFILE
	adopt_pgblk_cycles += get_cycles() - local_adopt_pgblk_cycles;
#endif
}

static void remote_free(void* object, pageblock_t* pageblock, heap_t* my_heap)
{
	queue_node_t temp_head, index;
	unsigned int temp_id;
	unsigned long long old_value;
	unsigned long long new_value;

#ifdef PROFILE
	unsigned long long local_remote_free_cycles = get_cycles();
#endif

	memory_add32(&num_remote_frees, 1);

	index.next = ((unsigned long)object - (unsigned long)pageblock->mem_pool) / pageblock->object_size + 1;
	do {
		temp_id = pageblock->owning_thread;

		if (temp_id == ORPHAN) {
			adopt_pageblock(object, pageblock, my_heap);
			break;
		}
		
		temp_head = pageblock->garbage_head;
		((queue_node_t *)object)->next = temp_head.next;
		index.count = temp_head.count + 1;

		((unsigned int*)&old_value)[0] = temp_id;
		((unsigned int*)&old_value)[1] = *((unsigned int*)&temp_head);
		((unsigned int*)&new_value)[0] = temp_id;
		((unsigned int*)&new_value)[1] = *((unsigned int*)&index);
	} while(!compare_and_swap64(&pageblock->together, old_value, new_value));

#ifdef PROFILE
	remote_free_cycles += get_cycles() - local_remote_free_cycles;
#endif
}

/*
static void chain_remote_free(void* object, pageblock_t* pageblock, heap_t* my_heap)
{
	queue_node_t temp_head, index;
	unsigned int temp_id;
	unsigned long long old_value;
	unsigned long long new_value;
	void* tail;

	memory_add32(&num_remote_frees, 1);

	index.next = ((unsigned long)object - (unsigned long)pageblock->mem_pool) / pageblock->object_size + 1;
	tail = pageblock->mem_pool + (((queue_node_t *)object)->count - 1) * pageblock->object_size;
	do {
		temp_id = pageblock->owning_thread;

		if (temp_id == ORPHAN) {
			adopt_pageblock(object, pageblock, my_heap);
			break;
		}
		
		temp_head = pageblock->garbage_head;
		((queue_node_t *)tail)->next = temp_head.next;
		index.count = temp_head.count + 1;

		((unsigned int*)&old_value)[0] = temp_id;
		((unsigned int*)&old_value)[1] = *((unsigned int*)&temp_head);
		((unsigned int*)&new_value)[0] = temp_id;
		((unsigned int*)&new_value)[1] = *((unsigned int*)&index);
	} while(!compare_and_swap64(&pageblock->together, old_value, new_value));
}
*/

/* Extracts the meta information for an object for free(). */
static inline void object_extract(void** object, void** ptr, size_t* size, short* object_type)
{
#ifdef PROFILE
	//unsigned long long local_extract_cycles = get_cycles();
#endif
#ifdef HEADERS
	*object -= sizeof(header_t);
	*object_type = ((header_t *)*object)->object_type;
	switch (*object_type) {
		case OBJECT_SMALL:	*ptr = (void*)(((header_t *)*object)->pageblock << PAGE_BITS);
					break;
		case OBJECT_MEDIUM:	*ptr = (void*)(((header_t *)*object)->sph << SUPERPAGE_BITS);
					*size = (size_t)(1 << ((header_t *)*object)->log_size) * PAGE_SIZE;
					break;
		case OBJECT_LARGE:	*size = ((header_t *)*object)->size;
					break;
	}
#elif RADIX_TREE
	radix_extract(*object, ptr, size, object_type);
#elif BIBOP
	unsigned long page = (unsigned long)*object & ~(PAGE_SIZE - 1);
	*object_type = bibop[page / PAGE_SIZE].object_type;

	switch (*object_type) {
		case OBJECT_SMALL:	*ptr = (void*)(bibop[page / PAGE_SIZE].pageblock << PAGE_BITS);
					break;
		case OBJECT_MEDIUM:	*ptr = (void*)(bibop[page / PAGE_SIZE].sph << SUPERPAGE_BITS);
					*size = (size_t)(1 << bibop[page / PAGE_SIZE].log_size) * PAGE_SIZE;
					break;
		case OBJECT_LARGE:	*size = (size_t)(bibop[page / PAGE_SIZE].size);
					break;
	}
#endif
#ifdef PROFILE
	//extract_cycles += get_cycles() - local_extract_cycles;
#endif
}

void free(void* object)
{
	size_t size = 0;
	pageblock_t *pageblock = NULL;
	void* ptr = NULL;
	short object_type = OBJECT_SMALL - 1;

#ifdef PROFILE
	unsigned long long local_free_cycles = get_cycles();
	++free_calls;
#endif

	if (unlikely(!object)) {
		return;
	}

	memory_add32(&num_frees, 1);

	object_extract(&object, &ptr, &size, &object_type);

	/* Large, medium or small? We handle each differently. */
	if (unlikely(object_type == OBJECT_LARGE)) {
		page_free(object, size);
		return;
	}
	else if (unlikely(object_type == OBJECT_MEDIUM)) {
		((pageblock_t*)object)->sph = (superpage_t*)ptr;
		superunmap(object, size);
		return;
	}

	pageblock = (pageblock_t*)ptr;
	heap_t* my_heap = &local_heap[free_compute_size_class(pageblock->object_size)];

	/* If we own the pageblock, then we can handle the object free right away. */
	if (likely(pageblock->owning_thread == thread_id)) {
		local_free(object, pageblock, my_heap);
	}

	/* No one owns the pageblock. */
	else if (pageblock->owning_thread == ORPHAN) {
		adopt_pageblock(object, pageblock, my_heap);
	}
		
	/* Someone else owns the pageblock. */
	else {
		/*
		if (remote_cache_total < 1000) {
			void* head_object = seq_lifo_dequeue(&remote_cache.queue);

			if (head_object != NULL) {
				--remote_cache.count;
				object_extract(&head_object, &ptr, &size, &object_type);
				pageblock_t* head_pageblock = (pageblock_t*)ptr;

				if (head_pageblock == pageblock) {
					((queue_node_t *)object)->next = ((unsigned long)head_object - (unsigned long)pageblock->mem_pool)
									/ pageblock->object_size + 1;
					((queue_node_t *)object)->count = ((queue_node_t *)head_object)->count;

				}
				else {
					seq_lifo_enqueue(&remote_cache.queue, head_object);
					++remote_cache.count;
				}
			}
			else {
				((queue_node_t *)object)->count = ((unsigned long)object - (unsigned long)pageblock->mem_pool)
							/ pageblock->object_size + 1;
			}

			seq_lifo_enqueue(&remote_cache.queue, object);
			++remote_cache.count;
			++remote_cache_total;
		}
		else {
			remote_free(object, pageblock, my_heap);
			while (remote_cache.count > 0) {
				object = seq_lifo_dequeue(&remote_cache.queue);
				--remote_cache.count;

				object_extract(&object, &ptr, &size, &object_type);
				pageblock = (pageblock_t*)ptr;
				heap_t* my_heap = &local_heap[compute_size_class(pageblock->object_size)];

				chain_remote_free(object, pageblock, my_heap);
			}
			remote_cache_total = 0;
		}
		*/

		remote_free(object, pageblock, my_heap);
	}

#ifdef PROFILE
	free_cycles += get_cycles() - local_free_cycles;
#endif
}

void* calloc(size_t nmemb, size_t size)
{
	void* ptr;
	
	ptr = malloc(nmemb * size);
	if (!ptr) {
		return NULL;
	}

	return memset(ptr, 0, nmemb * size);
}

void* valloc(size_t size)
{
	fprintf(stderr, "valloc() called. Not implemented! Exiting.\n");
	exit(1);
}

void* memalign(size_t boundary, size_t size)
{
	void *p;

	p = malloc((size + boundary - 1) & ~(boundary - 1));
	if (!p) {
		return NULL;
	}

	return(void*)(((unsigned long)p + boundary - 1) & ~(boundary - 1)); 
}

int posix_memalign(void **memptr, size_t alignment, size_t size)
{
	*memptr = memalign(alignment, size);
	if (*memptr) {
		return 0;
	}
	else {
		/* We have to "personalize" the return value according to the error */
		return -1;
	}
}

void *realloc(void *object, size_t size)
{
	size_t old_size = 0;
	void* ptr = NULL;
	short object_type = OBJECT_SMALL - 1;
	void* original_object = object;
	void* new_object = NULL;


	if (object == NULL) {
		return malloc(size);
	}
	else if (size == 0) {
		free(object);
		return NULL;
	}

	object_extract(&object, &ptr, &old_size, &object_type);

	if (size == old_size) {
		return original_object;
	}

	/* Object was previously large */
	if (object_type == OBJECT_LARGE) {
	
		/* Used to be large object, will become medium or small object */
		if (size <= SUPERPAGE_SIZE) {
			new_object = malloc(size);
			if (new_object == NULL) {
				return ((void *)MAP_FAILED);
			}

			memcpy(new_object, object, size);
			page_free(object, old_size);

			return new_object;
		}
	
		/* Remains large, but has to shrink */
		else if (size < old_size) {
			/* Do not remap objects until the new size is less than half the old size */ 
			if (size > old_size / 2) {
				return object;
			}
			
			/* Was and remains large object */
			/* Remap without allowing virt. address change */
			new_object = mremap(object, old_size, size, 0);
			if (new_object == MAP_FAILED) {
				fprintf(stderr, "realloc(): first mremap failed at address %p for %lu -> %lu\n", object, old_size, size);
				exit(1);
			}
		}

		/* Object has to expand */
		else {
			new_object = mremap(object, old_size, size, MREMAP_MAYMOVE);
			if (new_object == MAP_FAILED) {
				fprintf(stderr, "realloc(): second mremap failed at address %p for %lu -> %lu\n", object, old_size, size);
				exit(1);
			}
		}

		/* Update the page map to declare the pages as headered ones.
		 * Possible optimization here... If the address has not changed 
		 * we can set only new pages */
		register_pages(new_object, 1, NULL, size, OBJECT_LARGE);
		headerize_object(&new_object, NULL, size, OBJECT_LARGE); 

		return new_object;
	}

	/* Object was previously medium */
	else if (object_type == OBJECT_MEDIUM) {

		/* Object will become large */
		if (size > SUPERPAGE_SIZE) {
			new_object = page_alloc(size);
			register_pages(new_object, 1, NULL, size, OBJECT_LARGE);
			headerize_object(&new_object, NULL, size, OBJECT_LARGE);
		}

		/* Object will remain medium */
		else if (size > MAX_OBJECT_SIZE && size <= SUPERPAGE_SIZE) {

			size_t super_size = ceil((double)size / PAGE_SIZE) * PAGE_SIZE;
			unsigned int pow = (size_t)ceil(log2(super_size));
			super_size = 1 << pow;

			/* Don't bother reallocating if the new size falls in same class as old size */
			if (super_size == old_size) {
				return original_object;
			}

			new_object = supermap(super_size);
			register_pages(new_object, 1, ((pageblock_t*)new_object)->sph, super_size, OBJECT_MEDIUM);
			headerize_object(&new_object, ((pageblock_t*)new_object)->sph, super_size, OBJECT_MEDIUM);
		}

		/* Object will become small */
		else {
			new_object = malloc(size);
			if (new_object == NULL) {
				return (void *)MAP_FAILED;
			}
		}

		memcpy(new_object, object, old_size);

		((pageblock_t*)object)->sph = (superpage_t*)ptr;
		superunmap(object, old_size);

		return new_object;
	}

	/* Object was previously small */
	pageblock_t* pageblock = (pageblock_t*)ptr;
	old_size = pageblock->object_size;

	/* Don't bother reallocating if the new size falls in the same class as the old size */
	if (size < MAX_OBJECT_SIZE && compute_size_class(old_size) == compute_size_class(size)) {
		return original_object;
	}

	/* Don't bother reallocating unless the object shrinks by at least half */
	if (size < old_size && size > (old_size / 2)) {
		return original_object;
	}
	
	/* Reallocate object; handles all cases of shrinking and growing */
	new_object = malloc(size);
	if (new_object == NULL) {
		return (void *)MAP_FAILED;
	}
	memcpy(new_object, object, old_size);
	free(original_object);

	return new_object;
}

