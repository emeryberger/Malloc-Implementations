/*
 * Streamflow memory allocator.
 *
 * Main code contributors: Christos Antonopoulos, Scott Schneider
 */

#include "streamflow.h"

/* Used for gathering memory stats. */
static volatile unsigned int num_total_small;
static volatile unsigned int num_total_large;
static volatile unsigned int num_active_pageblocks;
static volatile unsigned int num_active_small;
static volatile unsigned int num_active_large;
static volatile unsigned int num_frees;
static volatile unsigned int num_remote_frees;
static volatile double avg_small;
static volatile double avg_large;

#ifdef MEMORY
static int init_flag;
static lock_t init_lock;
#endif

static __thread quickieblock_t hn_pageblocks;
static __thread double_list_t hash_table[HASH_TABLE_SIZE];

/* Global thread_id counter. */
unsigned int global_id_counter = 0;

/* Thread ID that is our index in the global object table. */
__thread unsigned int thread_id = 0;

static radix_interior_t* radix_root;

/* Dangerous. We need a more robust way of letting multiple processes
 * use superpages. */
static char superpage_location[256];

/* Protects all superpage operations. */
static lock_t super_lock;

/* File descriptor for the superpage file; protected by super_lock. */
static int superpage_fd;

/* Used when mmaping a new superpage; need to know the proper offset 
 * into the file; protected by super_lock. */
static unsigned long super_file_offset;

/* SuperPage Header pageblocks; protected by super_lock. */
static quickieblock_t sph_pageblocks;

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
static __thread heap_t local_heap[(PAGE_SIZE >> 1) / OBJECT_GRANULARITY];
static __thread counting_queue_t local_inactive_pageblocks[PAGEBLOCK_SIZE_CLASSES];
static counting_lf_lifo_queue_t global_partial_pageblocks[(PAGE_SIZE >> 1) / OBJECT_GRANULARITY];
static counting_lf_lifo_queue_t global_free_pageblocks[PAGEBLOCK_SIZE_CLASSES];

/* global list of superpages */
static double_list_t superpage_list;

static inline unsigned int quick_log2(unsigned int x);

/* Radix tree operations. */
static inline void radix_register(void* start, int num_pages, pageblock_t* pageblock, short large_bit);
static inline void radix_extract(void* object, pageblock_t** pageblock, short* is_large);

/* Operations on quickie pageblocks. */
static inline void* quickie_alloc(quickieblock_t* quickie, size_t object_size);
static inline void quickie_free(quickieblock_t* quickie, void* object);

/* Hash table operations. */
static inline unsigned int hashit(void* key);
static inline unsigned long* hash_table_find(double_list_t* hash_table, void* key);

/* Page coloring operations. */
static inline unsigned long compute_starting_color(superpage_t* super, page_chunk_t* chunk);
static inline void mark_colors(superpage_t* super, page_chunk_t* chunk, size_t size);
static inline void unmark_colors(superpage_t* super, page_chunk_t* chunk, size_t size);
static inline unsigned long conflict_estimate(superpage_t* super, page_chunk_t* chunk, size_t size);

/* Buddy operations on superpages. */
static inline int find_index(superpage_t* super, page_chunk_t* chunk, int order);
static inline void* find_buddy(superpage_t* super, page_chunk_t* chunk, int order);
static inline int find_bit_index(superpage_t* super, page_chunk_t* chunk, int order);
static inline void* buddy_alloc_pages(superpage_t* super, size_t size);
static inline void buddy_free_pages(superpage_t* super, void* start, size_t length);

/* All other superpage operations. */
static inline void get_free_superpage(superpage_t** sp, size_t size);
static inline void* supermap(size_t size);
static inline void superunmap(void* start, size_t length);

/* All virtual page operations. */
static inline void register_pages(void* start, int num_pages, pageblock_t* pageblock, short large_bit);
static inline void* request_pages(size_t object_size, size_t alignment, size_t size);
static inline void free_pages(void* start, size_t length);
static inline void* large_memory_request(size_t size);

/* All operations on the global free list. */
static inline void insert_global_free_pageblocks(pageblock_t* pageblock);
static inline void insert_global_partial_pageblocks(pageblock_t* pageblock, int class_index);
static inline pageblock_t* remove_global_pageblocks(int class_index, int pageblock_size);

/* All double list operations. */
static inline void double_list_insert_front(void* new_node, double_list_t* list);
static inline void double_list_rotate_back(double_list_t* list);
static inline void double_list_remove(void* node, double_list_t* list);

/* Helper functions for malloc */
static inline int compute_pageblock_size(int index);
static pageblock_t* get_free_pageblock(heap_t* heap, int index);

/* Helper functions for free. */
static inline void local_free(void* object, pageblock_t* pageblock, heap_t* my_heap);
static inline void remote_free(void* object, pageblock_t* pageblock, heap_t* my_heap);
static inline void adopt_pageblock(void* object, pageblock_t* pageblock, heap_t* my_heap);

/* If we're collecting memory stats, does an atomic add. If not, does nothing. */
static inline void memory_add(volatile unsigned int* address, int value)
{
#ifdef MEMORY
	atmc_add(address, value);
#endif
}

/* Keeps a running average, atomically, of a value if we're collecting memory 
 * stats. If not, does nothing. */
static inline void memory_average(volatile double* avg, volatile unsigned int* total, volatile unsigned int* active, double delta)
{
#ifdef MEMORY
	double newval;
	atmc_add(total, 1);
	atmc_add(active, delta);
	do {
		newval = *avg + delta / (double)*total;
	}while (!compare_and_swap64((unsigned long long *)avg, avg, newval));
#endif
}

/* Often we need to take the base 2 logarithm of a power-of-2. Since we have a 
 * known range of numbers, this function is sufficient and significantly faster 
 * than calling log(x)/log(2.0). */
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
	}

	fprintf(stderr, "quick_log2() unhandled number: %u", x);
	fflush(stderr);
	exit(1);

	return -1;
}

static inline int max(int a, int b)
{
	return (a > b) ? a: b;
}

static inline radix_interior_t* radix_interior_alloc()
{
	void* node = mmap(NULL, sizeof(radix_interior_t), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (node == MAP_FAILED) {
		fprintf(stderr, "radix_interior_alloc() mmap of size %zd failed\n", sizeof(radix_interior_t));
		fflush(stderr);
		exit(1);
	}

	return (radix_interior_t*)node;
}

static inline radix_leaf_t* radix_leaf_alloc()
{
	void* node = mmap(NULL, sizeof(radix_leaf_t), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (node == MAP_FAILED) {
		fprintf(stderr, "radix_leaf_alloc() mmap of size %zd failed\n", sizeof(radix_leaf_t));
		fflush(stderr);
		exit(1);
	}

	return (radix_leaf_t*)node;
}

static inline void radix_interior_free(radix_interior_t* node)
{
	munmap(node, sizeof(radix_interior_t));
}

static inline void radix_leaf_free(radix_leaf_t* node)
{
	munmap(node, sizeof(radix_leaf_t));
}

static inline void radix_register(void* start, int num_pages, pageblock_t* pageblock, short large_bit)
{
	/* Ensure in a lock-free manner that we have a root node. */
	if (radix_root == NULL) {
		radix_interior_t* temp_root = radix_interior_alloc();
		if (!compare_and_swap_ptr(&radix_root, NULL, temp_root)) {
			radix_interior_free(temp_root);
		}
	}

	int i;
	unsigned long page = (unsigned long)start >> PAGE_BITS;
	for (i = 0; i < num_pages; ++i) {
		unsigned long level1 = page >> (RADIX_INTERIOR_BITS + RADIX_LEAF_BITS);
		unsigned long level2 = (page >> RADIX_LEAF_BITS) & (RADIX_INTERIOR_SIZE - 1);
		unsigned long level3 = page & (RADIX_LEAF_SIZE - 1);
		page_record_t record = {0, 0};

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
		record.offset = ((page << PAGE_BITS) - (unsigned long)start) / PAGE_SIZE;
		record.large = large_bit;
		((radix_leaf_t*)radix_root->prefixes[level1]->prefixes[level2])->values[level3] = record;

		page += 1;	
	}
}

/* We assume that radix_register has already been called for this object's page. This 
 * allows us to assume that the nodes are already allocated. */
static inline void radix_extract(void* object, pageblock_t** pageblock, short* is_large)
{
	unsigned long page = (unsigned long)object >> PAGE_BITS;
	unsigned long level1 = page >> (RADIX_INTERIOR_BITS + RADIX_LEAF_BITS);
	unsigned long level2 = (page >> RADIX_LEAF_BITS) & (RADIX_INTERIOR_SIZE - 1);
	unsigned long level3 = page & (RADIX_LEAF_SIZE - 1);
	page_record_t record = {0, 0};

	record = ((radix_leaf_t*)radix_root->prefixes[level1]->prefixes[level2])->values[level3];
	*pageblock = (pageblock_t*)((page << PAGE_BITS) - (record.offset * PAGE_SIZE));
	*is_large = record.large;
}

static inline unsigned int hashit(void* key)
{
	return ((unsigned long)key * GOLDEN_RATIO) >> (BPL - LOG2_HASH_TABLE_SIZE);
}

static inline unsigned long* hash_table_find(double_list_t* hash_table, void* key)
{
	unsigned int i;
	hash_node_t* node;

	i = hashit(key);
	for (node = (hash_node_t*)hash_table[i].head; node != NULL && node->key != key; node = node->next) ;

	/* We've never seen this key before, so we must make an entry 
	 * in the table. */
	if (node == NULL) {
		node = (hash_node_t*)quickie_alloc(&hn_pageblocks, sizeof(hash_node_t));
		node->key = key;
		double_list_insert_front(node, &hash_table[i]);
	}

	return node->used_colors;
}

static inline unsigned long compute_starting_color(superpage_t* super, page_chunk_t* chunk)
{
	unsigned long bias = ((unsigned long)super->page_pool / PAGE_SIZE) % COLOR_MAX;
	unsigned long virtual_start = ((unsigned long)chunk - (unsigned long)super->page_pool) / PAGE_SIZE;

	return (COLOR_MAX + virtual_start - bias) % COLOR_MAX;
}

static inline void mark_colors(superpage_t* super, page_chunk_t* chunk, size_t size)
{
#ifdef COLOR
	unsigned long start = compute_starting_color(super, chunk);
	unsigned long remainder = (size / PAGE_SIZE) % COLOR_MAX;
	unsigned long* color_vector = hash_table_find(hash_table, super);
	unsigned long color;

	for (color = start; color < start + remainder; ++color) {
		++color_vector[color];
	}
#endif
}

static inline void unmark_colors(superpage_t* super, page_chunk_t* chunk, size_t size)
{
#ifdef COLOR
	unsigned long start = compute_starting_color(super, chunk);
	unsigned long remainder = (size / PAGE_SIZE) % COLOR_MAX;
	unsigned long* color_vector = hash_table_find(hash_table, super);
	unsigned long color;

	for (color = start; color < start + remainder; ++color) {
		--color_vector[color];
	}
#endif
}

static inline unsigned long conflict_estimate(superpage_t* super, page_chunk_t* chunk, size_t size)
{
	unsigned long start = compute_starting_color(super, chunk);
	unsigned long remainder = size % COLOR_MAX;
	unsigned long conflicts = 0;
	unsigned long* color_vector = hash_table_find(hash_table, super);
	unsigned long color;

	for (color = start; color < start + remainder; ++color) {
		conflicts += color_vector[color];
	}

	return conflicts;
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

/* size is guarenteed to be a multiple of PAGE_SIZE. */
static inline void* buddy_alloc_pages(superpage_t* super, size_t size)
{
	page_chunk_t* chunk;
	unsigned int order = quick_log2(size / PAGE_SIZE);
	unsigned int curr_order;

	/* Starting at the closest fit, try to find a page chunk to satisfy 
	 * the request. */
	for (curr_order = order; curr_order < BUDDY_ORDER_MAX; ++curr_order) {

		if (super->buddy[curr_order].free_list.head != NULL) {
#ifdef COLOR
			/* We only bother looking at colors when we're looking for less than 
			 * 16 pages. */
			if ((1 << curr_order) < COLOR_THRESHOLD) {
				page_chunk_t* curr_chunk;
				unsigned int lowest_conflict = ULONG_MAX;

				/* Find the page chunk that conflicts the least with the colors we're
				 * currently using. */
				for (curr_chunk = super->buddy[curr_order].free_list.head; curr_chunk != NULL; curr_chunk = curr_chunk->next) {
					unsigned int e = conflict_estimate(super, curr_chunk, 1 << curr_order);
					if (e <= lowest_conflict) {
						lowest_conflict = e;
						chunk = curr_chunk;
					}
				}
			}
			else {
				chunk = super->buddy[curr_order].free_list.head;
			}
#else
			chunk = super->buddy[curr_order].free_list.head;
#endif

			double_list_remove(chunk, &super->buddy[curr_order].free_list);
			__change_bit(find_bit_index(super, chunk, curr_order), (unsigned long *)super->buddy[curr_order].bitmap);

			break;
		}
	}

	/* If our page chunk is from a higher order, we need to split it 
	 * up. */
	size = 1 << curr_order;	
	while (curr_order > order) {
		page_chunk_t* buddy;

		--curr_order;
		size >>= 1;

		/* We don't need to call find_buddy() because we know that chunk is 
		 * on the left. */
		buddy = (page_chunk_t*)((unsigned long)chunk + (size * PAGE_SIZE));

#ifdef COLOR
		/* We only bother looking at colors when we're at less than 16 
		 * pages. Estimating the conflict can get expensive, so we 
		 * want to avoid it when it's unnecessary. */
		if (size < COLOR_THRESHOLD) {
			unsigned long chunk_conflicts = conflict_estimate(super, chunk, size);
			unsigned long buddy_conflicts = conflict_estimate(super, buddy, size);

			/* The left side has less conflicts, so choose it. */
			if (chunk_conflicts < buddy_conflicts) {
				double_list_insert_front(buddy, &super->buddy[curr_order].free_list);
				__change_bit(find_bit_index(super, buddy, curr_order), (unsigned long*)super->buddy[curr_order].bitmap);
			}
			else {
				double_list_insert_front(chunk, &super->buddy[curr_order].free_list);
				__change_bit(find_bit_index(super, chunk, curr_order), (unsigned long*)super->buddy[curr_order].bitmap);
				chunk = buddy;
			}
		}
		else {
			double_list_insert_front(chunk, &super->buddy[curr_order].free_list);
			__change_bit(find_bit_index(super, chunk, curr_order), (unsigned long*)super->buddy[curr_order].bitmap);
			chunk = buddy;
		}
#else
		double_list_insert_front(chunk, &super->buddy[curr_order].free_list);
		__change_bit(find_bit_index(super, chunk, curr_order), (unsigned long *)super->buddy[curr_order].bitmap);
		chunk = buddy;
#endif
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

	mark_colors(super, chunk, size);

	return (void*)chunk;
}

static inline void buddy_free_pages(superpage_t* super, void* start, size_t length)
{
	page_chunk_t* chunk = (page_chunk_t*)start;
	page_chunk_t* buddy;
	unsigned int order = quick_log2(length / PAGE_SIZE);
	unsigned int curr_order;

	length = 1 << order;
	unmark_colors(super, chunk, length);

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
		munmap(chunk, SUPERPAGE_SIZE);
		double_list_remove(super, &superpage_list);
		quickie_free(&sph_pageblocks, super);
	}
}

static inline void* quickie_alloc(quickieblock_t* quickie, size_t object_size)
{
	void* object;

	/* We need to allocate space for a new pageblock in two cases: the first time 
	 * this function is called (in which case unallocated will not point to 
	 * anything), and when there is no more space in the last pageblock we allocated. */
	if (quickie->unallocated == NULL || quickie->num_free_objects == 0) {
		quickie->unallocated = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		if (quickie->unallocated == MAP_FAILED) {
			fprintf(stderr, "quickie_alloc() mmap failed\n");
			fflush(stderr);
			exit(1);
		}

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

static inline void set_superpage_location()
{
	DIR* slash_tmp = opendir(SUPERPAGE_TEMP);
	char tmp_location[256];

	if (!slash_tmp) {
		fprintf(stderr, "set_superpage_location(): ");
		switch (errno) {
			case EACCES:	fprintf(stderr, "EACCESS"); break;
			case EMFILE:	fprintf(stderr, "EMFILE"); break;
			case ENFILE:	fprintf(stderr, "ENFILE"); break;
			case ENOENT:	fprintf(stderr, "ENOENT"); break;
			case ENOMEM:	fprintf(stderr, "ENOMEM"); break;
			case ENOTDIR:	fprintf(stderr, "ENOTDIR"); break;
		}
		fprintf(stderr, "\n");
		fflush(stderr);
		exit(1);
	}

	union {
		struct dirent d;
		char b[offsetof (struct dirent, d_name) + NAME_MAX + 1];
	} entry;
	struct dirent* result;

	while(1) {
		if (readdir_r(slash_tmp, &entry, &result)) {
			fprintf(stderr, "set_superpage_location(): readdir_r\n");
			fflush(stderr);
			exit(1);
		}

		if (!strcmp(result->d_name, ".") || !strcmp(result->d_name, "..")) {
			continue;
		}

		strcpy(tmp_location, SUPERPAGE_TEMP);
		strcat(tmp_location, result->d_name);

		if (!unlink(tmp_location)) {
			break;
		}
	}
		
	strcpy(superpage_location, SUPERPAGE_DIRECTORY);
	strcat(superpage_location, result->d_name);
}

static inline void get_free_superpage(superpage_t** super, size_t size)
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

		if (!superpage_fd) {

			set_superpage_location();

			superpage_fd = open(superpage_location, O_RDWR | O_CREAT /*| O_TRUNC*/, 0777);
			if (superpage_fd < 0) {
				fprintf(stderr, "get_free_superpage() open failed\n");
				fflush(stderr);
				exit(1);
			}
		}

		*super = (superpage_t*)quickie_alloc(&sph_pageblocks, sizeof(superpage_t));

		/* If the file_offset is zero, then this is a never-before used header and we 
		 * need to get a file offset from the global counter. If not, we can 
		 * just recycle the old one. */
		if ((*super)->file_offset == 0) {
			(*super)->file_offset = super_file_offset;
			super_file_offset += SUPERPAGE_SIZE;
		}

		(*super)->page_pool = (superpage_t*)mmap(	NULL, SUPERPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, 
								superpage_fd, (*super)->file_offset);

		if ((*super)->page_pool == MAP_FAILED) {
			fprintf(stderr, "get_free_superpage() mmap failed\n");
			switch (errno) {
				case EACCES:	fprintf(stderr, "EACCES\n"); break;
				case EAGAIN:	fprintf(stderr, "EAGAIN\n"); break;
				case EBADF:	fprintf(stderr, "EBADF\n"); break;
				case EINVAL:	fprintf(stderr, "EINVAL\n"); break;
				case ENFILE:	fprintf(stderr, "ENFILE\n"); break;
				case ENODEV:	fprintf(stderr, "ENODEV\n"); break;
				case ENOMEM:	fprintf(stderr, "ENOMEM\n"); break;
				case EPERM:	fprintf(stderr, "EPERM\n"); break;
				case ETXTBSY:	fprintf(stderr, "ETXTBSY\n"); break;
				default:	fprintf(stderr, "other\n");
			}
			fflush(stderr);
			exit(1);
		}
		
		/* Initialize bitmaps for buddy allocation */
		int byte = 0;
		(*super)->buddy[0].bitmap = &(*super)->bitmaps[0];
		for (order = 0; order < BUDDY_ORDER_MAX - 1; ++order) {
			byte += max(sizeof(unsigned long), (int)ceil(((double)PAGES_PER_SUPERPAGE / ((1 << order) * 8 * 2))));
			(*super)->buddy[order + 1].bitmap = &(*super)->bitmaps[byte];
		}
		memset((*super)->bitmaps, 0, BUDDY_BITMAP_SIZE); 

		/* Stick the entire superpage into the buddy allocation scheme. */
		double_list_insert_front((*super)->page_pool, &((*super)->buddy[BUDDY_ORDER_MAX - 1].free_list));
		(*super)->largest_free_order = BUDDY_ORDER_MAX - 1;

		double_list_insert_front(*super, &superpage_list);
	}
}

static inline void* supermap(size_t size)
{
	superpage_t* super;
	void* pages;

	spin_lock(&super_lock);

	get_free_superpage(&super, size);

	/* Allocate pages from the superpage. */
	pages = buddy_alloc_pages(super, size);

	/* UGLY: Write the superpage header pointer into the pageblock header. We 
	 * shouldn't "know" about pageblock at this point, but this is the easiest 
	 * way for superunmap() to know where the superpage header is for the 
	 * freed superpage. */
	((pageblock_t*)pages)->sph = super;

	spin_unlock(&super_lock);

	return pages;
}

static inline void superunmap(void* start, size_t length)
{
	superpage_t* super;

	spin_lock(&super_lock);

	super = ((pageblock_t*)start)->sph;
	buddy_free_pages(super, start, length);

	spin_unlock(&super_lock);
}

/* If not using headers, registers pages in appropriate data structure. 
 * We assume that num_pages is a multiple of PAGE_SIZE. */
static inline void register_pages(void* start, int num_pages, pageblock_t* pageblock, short large_bit)
{
#ifdef RADIX_TREE
	radix_register(start, num_pages, pageblock, large_bit);
#elif BIBOP
	int i;
	unsigned long page = (unsigned long)start;
	for (i = 0; i < num_pages; ++i) {
		bibop[page / PAGE_SIZE].large = large_bit;
		bibop[page / PAGE_SIZE].offset = (page - (unsigned long)start) / PAGE_SIZE;
		page += PAGE_SIZE;
	}
#endif
}

/* Makes a request to whoever manages pages (us or kernel). */
static inline void* request_pages(size_t object_size, size_t alignment, size_t size)
{
	void* addr;

	if (object_size <= PAGE_SIZE >> 1) {
#ifdef SUPERPAGES
		addr = supermap(size);
#else
		addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		/*
		fprintf(stderr, "%d: mmap() = %p (small)\n", thread_id, addr);
		fflush(stderr);
		*/
		if (addr == MAP_FAILED) {
			fprintf(stderr, "request_pages() first mmap of size %zd failed\n", size);
			fflush(stderr);
			exit(1);
		}
#endif
	}
	else {
		addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		/*
		fprintf(stderr, "%d: mmap() = %p (large)\n", thread_id, addr);
		fflush(stderr);
		*/
		if (addr == MAP_FAILED) {
			fprintf(stderr, "request_pages() second mmap of size %zd failed\n", size);
			fflush(stderr);
			exit(1);
		}
	}

	return((void *)addr);
}

/* Frees pages to whoever manages pages (us or kernel). */
static inline void free_pages(void* start, size_t length)
{
#ifdef SUPERPAGES
	superunmap(start, length);
#else
	munmap(start, length);
#endif
}

/* Gets a large amount of memory from the OS and tags it appropriately. */
static inline void* large_memory_request(size_t size)
{
	void* mem;
	
	/* need to round size up to the nearest page */
	size = ceil((double)size / PAGE_SIZE) * PAGE_SIZE;

	mem = request_pages(size, HEADER_SIZE, size);

	/* Optimization: since we really only care about the first
	 * page with large objects (that's the only page that free() 
	 * ever gets), we only need to register the first page. */
	register_pages(mem, 1, NULL, 1);

	mem += HEADER_SIZE - sizeof(header_t);
	((header_t *)mem)->size = size;

#ifdef HEADERS
	((header_t *)mem)->large = 1;
#endif

	mem += sizeof(header_t);
	return mem;
}

/* Adds a pageblock to one of the global lists, or frees it to the OS/page manager. */
static inline void insert_global_free_pageblocks(pageblock_t* pageblock)
{
	int size_index = quick_log2((pageblock->mem_pool_size + (unsigned long)pageblock->mem_pool - (unsigned long)pageblock) / PAGE_SIZE) - quick_log2(MIN_PAGEBLOCK_SIZE / PAGE_SIZE);

	if (global_free_pageblocks[size_index].count >= MAX_GLOBAL_INACTIVE) {
		memory_add(&num_active_pageblocks, -(pageblock->mem_pool_size + ((unsigned long)pageblock->mem_pool - (unsigned long)pageblock)));
		free_pages(pageblock, pageblock->mem_pool_size + CACHE_LINE_SIZE);
	}
	else {
		atmc_add(&global_free_pageblocks[size_index].count, 1);
		lf_lifo_enqueue(&global_free_pageblocks[size_index].queue, pageblock);
	}
}

static inline void insert_global_partial_pageblocks(pageblock_t* pageblock, int class_index)
{
	atmc_add(&global_partial_pageblocks[class_index].count, 1);
	lf_lifo_enqueue(&global_partial_pageblocks[class_index].queue, pageblock);
}

/* Attempts to get a global pageblock. */
static inline pageblock_t* remove_global_pageblocks(int class_index, int pageblock_size)
{
	pageblock_t* pageblock = (pageblock_t*)lf_lifo_dequeue(&global_partial_pageblocks[class_index].queue);
	if (pageblock) {
		atmc_add(&global_partial_pageblocks[class_index].count, -1);
	}
	else {
		int size_index = quick_log2(pageblock_size / PAGE_SIZE) - quick_log2(MIN_PAGEBLOCK_SIZE / PAGE_SIZE);

		pageblock = (pageblock_t*)lf_lifo_dequeue(&global_free_pageblocks[size_index].queue);
		if (pageblock) {
			atmc_add(&global_free_pageblocks[size_index].count, -1);
		}
	}

	return pageblock;
}

/* Places new_node at the front of the list. */
static inline void double_list_insert_front(void* new_node, double_list_t* list)
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
static inline void double_list_rotate_back(double_list_t* list)
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
static inline void double_list_remove(void* node, double_list_t* list)
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
static inline void garbage_collect(pageblock_t* collectee)
{
	unsigned int chain;
	queue_node_t header;
	unsigned short index;

	chain = lf_lifo_chain_dequeue_nABA32((unsigned int *)&(collectee->garbage_head));
	header = *((queue_node_t*)&chain);
	index = header.next;
	collectee->freed = index;
	collectee->num_free_objects += ((queue_node_t *)&header)->count;
}

void streamflow_thread_finalize(void)
{
	int i;
	pageblock_t *pageblock, *next_pageblock;

	for (i = 0; i < (PAGE_SIZE >> 1) / OBJECT_GRANULARITY; ++i) {
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

static inline int compute_pageblock_size(int index)
{
	/* Make sure that the suggestion is a page multiple. */
	unsigned int suggestion = ceil((double)((index + 1) * OBJECT_GRANULARITY * OBJECTS_PER_PAGEBLOCK) / PAGE_SIZE) * PAGE_SIZE;

	/* 2^pow is the closest power-of-2 to suggestion. */
	/* NOTE: gcc 4 on the IBM doesn't seem to have logl. I'm going to change it to 
	 * just plain log, and hope that the cast to a long double will keep the precision. 
	 * Need to check to see if it still works on an Intel box. */
	unsigned int pow = (unsigned int)((log((long double)suggestion) / log((long double)2)) + 0.5);

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
		pageblock = (pageblock_t*)request_pages((index + 1) * OBJECT_GRANULARITY, PAGE_SIZE, pageblock_size);
		register_pages(pageblock, pageblock_size / PAGE_SIZE, pageblock, 0);
		memory_add(&num_active_pageblocks, pageblock_size);

		lf_lifo_queue_init_nABA32((unsigned int *)&(pageblock->garbage_head));
		pageblock->freed = 0;
		pageblock->unallocated = 1;
		pageblock->object_size = (index + 1) * OBJECT_GRANULARITY; 
		pageblock->mem_pool = (char*)((unsigned long)pageblock + (unsigned long)ceil((double)sizeof(pageblock_t) / CACHE_LINE_SIZE) * CACHE_LINE_SIZE);
		pageblock->mem_pool_size = pageblock_size - (unsigned long)pageblock->mem_pool + (unsigned long)pageblock;
		pageblock->num_free_objects = pageblock->mem_pool_size / pageblock->object_size;
	}
	else if (pageblock->object_size != (index + 1) * OBJECT_GRANULARITY) {
		pageblock->freed = 0;
		pageblock->unallocated = 1;
		pageblock->object_size = (index + 1) * OBJECT_GRANULARITY;
		pageblock->num_free_objects = pageblock->mem_pool_size / pageblock->object_size;
	}

	/* Claim ownership of the pageblock. */
	pageblock->owning_heap = local_heap;
	pageblock->owning_thread = thread_id;

	/* New pageblock goes to front of active list. */
	double_list_insert_front(pageblock, &heap->active_pageblocks);

	return pageblock;
}

void timer_handler(int sig)
{
	fprintf(stderr, "totsmall %u totlarge %u actpageblocks %u actsmall %u actlarge %u avgsmall %f avglarge %f frees %u remote %u\n",
			num_total_small, num_total_large, num_active_pageblocks, num_active_small, num_active_large,
			avg_small, avg_large, num_frees, num_remote_frees);
	fflush(stderr);
}

/* Checks to see if memory stuff has been initialized. If we're
 * not collecting memory stats, does nothing. */
static inline void memory_init_check()
{
#ifdef MEMORY
	if (!init_flag) {
		spin_lock(&init_lock);
		if (!init_flag) {
			struct sigaction act;
			
			bzero(&act, sizeof(act));
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
static inline void headerize_object(void** object, pageblock_t* pageblock)
{
#ifdef HEADERS
	*object += HEADER_SIZE - sizeof(header_t);
	((header_t *)*object)->pageblock = 0;
	((header_t *)*object)->pageblock = (unsigned long)pageblock >> PAGE_BITS;
	((header_t *)*object)->large = 0;
	*object += sizeof(header_t);
#endif
}

void* malloc(size_t requested_size)
{
	int index;
	void* pointer = NULL;
	pageblock_t *pageblock;
	heap_t *heap;

	memory_init_check();

	if (requested_size == 0) {
		return NULL;
	}

#ifdef HEADERS
	requested_size += HEADER_SIZE;
#endif

	/* We forward "large" objects directly to the OS. We define
	 * "large" as anything larger than half a page. */
	if (requested_size > PAGE_SIZE >> 1) {
		memory_average(&avg_large, &num_total_large, &num_active_large, 
				ceil((double)(requested_size + HEADER_SIZE) / PAGE_SIZE) * PAGE_SIZE);

		return large_memory_request(requested_size + HEADER_SIZE);
	}

	memory_average(&avg_small, &num_total_small, &num_active_small, requested_size);

	index = ceil((double)(requested_size) / OBJECT_GRANULARITY) - 1;
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

	headerize_object(&pointer, pageblock);

	return pointer;
}

static inline void local_free(void* object, pageblock_t* pageblock, heap_t* my_heap)
{
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
	else if (pageblock != (pageblock_t*)my_heap->active_pageblocks.head && pageblock->num_free_objects == 1) {
		double_list_remove(pageblock, &my_heap->active_pageblocks);
		double_list_insert_front(pageblock, &my_heap->active_pageblocks);
	}
}

static inline void adopt_pageblock(void* object, pageblock_t* pageblock, heap_t* my_heap)
{
	/* So we try to adopt it. If we succeed, treat it like our own. If we fail, 
	 * let the new parent deal with it. */
	if (compare_and_swap32(&pageblock->owning_thread, ORPHAN, thread_id)) {
		double_list_insert_front(pageblock, &my_heap->active_pageblocks);
		local_free(object, pageblock, my_heap);
	}
	else {
		remote_free(object, pageblock, my_heap);
	}
}

static inline void remote_free(void* object, pageblock_t* pageblock, heap_t* my_heap)
{
	queue_node_t temp_head, index;
	unsigned int temp_id;
	unsigned long long old_value;
	unsigned long long new_value;

	memory_add(&num_remote_frees, 1);

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
}

/* Extracts the meta information for an object for free(). */
static inline void object_extract_free(void** object, pageblock_t** pageblock, short* is_large)
{
#ifdef HEADERS
	*is_large = ((header_t *)(*object - sizeof(header_t)))->large;

	if (!(*is_large)) {
		*object -= sizeof(header_t);
		*pageblock = (pageblock_t *)(((header_t *)*object)->pageblock << PAGE_BITS);
		*object -= HEADER_SIZE - sizeof(header_t);
	}
#elif RADIX_TREE
	radix_extract(*object, pageblock, is_large);
#elif BIBOP
	unsigned long page = (unsigned long)*object & ~(PAGE_SIZE - 1);
	*is_large = bibop[page / PAGE_SIZE].large;

	if (!(*is_large)) {
		*pageblock = (pageblock_t*)(page - (bibop[page / PAGE_SIZE].offset * PAGE_SIZE));
	}
#endif
}

void free(void* object)
{
	size_t size;
	pageblock_t *pageblock = NULL;
	short is_large;
	
	if (!object) {
		return;
	}

	memory_add(&num_frees, 1);

	object_extract_free(&object, &pageblock, &is_large);

	/* Figure out if this object belongs in one of our pageblocks, or if it's a huge
	 * object requested directly from the OS. */
	if (is_large) {
		object -= sizeof(header_t);
		size = ((header_t *)object)->size;
		object -= HEADER_SIZE - sizeof(header_t);

		munmap(object, size);

		memory_add(&num_active_large, -size);
		return;
	}

	memory_add(&num_active_small, -pageblock->object_size);

	heap_t* my_heap = &local_heap[(pageblock->object_size / OBJECT_GRANULARITY) - 1];

	/* If we own the pageblock, then we can handle the object free right away. */
	if (pageblock->owning_thread == thread_id) {
		local_free(object, pageblock, my_heap);
	}

	/* No one owns the pageblock. */
	else if (pageblock->owning_thread == ORPHAN) {
		adopt_pageblock(object, pageblock, my_heap);
	}
		
	/* Someone else owns the pageblock. */
	else {
		remote_free(object, pageblock, my_heap);
	}
}

void *calloc(size_t nmemb, size_t size)
{
	void *ptr;
	
	/*
	fprintf(stderr, "calloc\n");
	fflush(stderr);
	*/

	ptr = malloc(nmemb * size);
	if (!ptr) {
		return NULL;
	}

	return memset(ptr, 0, nmemb * size);
}

void *valloc(size_t size)
{
	fprintf(stderr, "valloc() called. Not implemented! Exiting.\n");
	fflush(stderr);
	exit(1);
}

void *memalign(size_t boundary, size_t size)
{
	void *p;

	/*
	fprintf(stderr, "memalign\n");
	fflush(stderr);
	*/

	p = malloc((size + boundary - 1) & ~(boundary - 1));
	if (!p) {
		return NULL;
	}

	return(void*)(((unsigned long)p + boundary - 1) & ~(boundary - 1)); 
}

int posix_memalign1(void **memptr, size_t alignment, size_t size)
{
	/*
	fprintf(stderr, "posix_memalign\n");
	fflush(stderr);
	*/

	*memptr = memalign(alignment, size);
	if (*memptr) {
		return 0;
	}
	else {
		/* We have to "personalize" the return value according to the error */
		return -1;
	}
}

/* Extracts the object's meta-info for realloc(). */
static inline void object_extract_realloc(void** object, pageblock_t** pageblock, short* is_large, size_t* size)
{
#ifdef HEADERS
	*is_large = ((header_t *)(*object - sizeof(header_t)))->large;

	if (!(*is_large)) {
		*pageblock = (pageblock_t *)(((header_t *)((unsigned long)*object - sizeof(header_t)))->pageblock << PAGE_BITS);
		*size += HEADER_SIZE;
	}
#elif RADIX_TREE
	radix_extract(*object, pageblock, is_large);
#elif BIBOP
	unsigned long page = (unsigned long)*object & ~(PAGE_SIZE - 1);
	*is_large = bibop[page / PAGE_SIZE].large;

	if (!(*is_large)) {
		*pageblock = (pageblock_t*)(page - (bibop[page / PAGE_SIZE].offset * PAGE_SIZE));
	}
#endif
}

void *realloc(void *ptr, size_t size)
{
	size_t old_size;
	pageblock_t *pageblock = NULL;
	short is_large;
	void *modified_ptr, *new_ptr;

	if (ptr == NULL) {
		return malloc(size);
	}
	if (size == 0) {
		free(ptr);
		return NULL;
	}

	object_extract_realloc(&ptr, &pageblock, &is_large, &size);

	/* Object was previously large */
	if (is_large == 1) {
		modified_ptr = ptr - sizeof(header_t);
		old_size = ((header_t *)modified_ptr)->size;	
		modified_ptr -= HEADER_SIZE - sizeof(header_t);
	
		/* Used to be large object, will become small object */
		if (size <= PAGE_SIZE >> 1) {
			new_ptr = malloc(size);
			if (new_ptr == NULL) {
				return ((void *)MAP_FAILED);
			}
			memcpy(new_ptr, ptr, size);
			munmap(modified_ptr, old_size);
			return new_ptr;
		}
	
		size += HEADER_SIZE;
		/* need to round size up to the nearest page */
		size = ceil((double)size / PAGE_SIZE) * PAGE_SIZE;

		if (size == old_size) {
			return ptr;
		}

		/* Do not remap large objects, until the new size is less than half the old size */ 
		if (size < old_size) {
			if (size > old_size / 2) {
				return ptr;
			}
			
			/* Was and remains large object */
			/* Remap without allowing virt. address change */
			mremap(modified_ptr, old_size, size, 0);
			modified_ptr += HEADER_SIZE - sizeof(header_t);
			((header_t *)modified_ptr)->size = size;
			modified_ptr += sizeof(header_t);
			return modified_ptr;
		}

		/* Object has to expand */
		new_ptr = mremap(modified_ptr, old_size, size, MREMAP_MAYMOVE);
		if (new_ptr != MAP_FAILED) {
			modified_ptr = new_ptr;
			new_ptr += HEADER_SIZE - sizeof(header_t);
			((header_t *)new_ptr)->size = size;
			new_ptr += sizeof(header_t);

			/* Update the page map to declare the pages as headered ones */
			/* Possible optimization here... If the address has not changed *
			 * we can set only new pages					*/
			register_pages(modified_ptr, size / PAGE_SIZE, NULL, 1);
		}

		return new_ptr;
	}

	/* Object was previously small */
	old_size = pageblock->object_size;

	if (ceil((double)(old_size) / OBJECT_GRANULARITY) == ceil((double)(size) / OBJECT_GRANULARITY)) {
		return ptr;
	}
	if (size < old_size) {
			/* Do not remap small objects, until the new size is less than half the old size */ 
			if (size > old_size / 2) {
				return ptr;
			}
			new_ptr = malloc(size);
			if (new_ptr == NULL) {
				return (void *)MAP_FAILED;
			}
			memcpy(new_ptr, ptr, size);
			free(ptr);

			return new_ptr;
	}
	
	/* The object will grow */
	
	/* The object will remain a small object */
	if (size <= PAGE_SIZE >> 1) {
		new_ptr = malloc(size);
		if (new_ptr == NULL) {
			return (void *)MAP_FAILED;
		}
		memcpy(new_ptr, ptr, old_size);
		free(ptr);
		return new_ptr;
	}

	/* The object will become a large object */
	size += HEADER_SIZE;
	/* need to round size up to the nearest page */
	size = ceil((double)size / PAGE_SIZE) * PAGE_SIZE;
	new_ptr = large_memory_request(size);
	if (new_ptr == NULL) {
		return (void *)MAP_FAILED;
	}

	memcpy(new_ptr, ptr, old_size);
	free(ptr);

	return new_ptr;
}

