#ifndef __LOCK_H_
#define __LOCK_H_

#include "atomic.h"

typedef volatile unsigned int lock_t;

void spin_init(lock_t *lock);
void spin_lock(register lock_t *lock);
void spin_unlock(register lock_t *lock);

struct incr_barrier {
	unsigned long num_threads;
	volatile unsigned long arrived;
	volatile unsigned long global_sense;	
};
typedef struct incr_barrier barrier_t;

void bar_init(barrier_t *barr, unsigned long num_threads);
void bar(barrier_t *barr);

#define spin_init(lock) \
{\
	*(lock) = 0; \
}

#define spin_lock(lock) \
{\
	while (fetch_and_store((volatile unsigned int *)(lock), 1)) { \
		while(*(lock)); \
	} \
}

#define spin_unlock(lock) \
{\
	*(lock) = 0; \
}

#define bar_init(barr, num_threads) \
{\
	(barr)->global_sense = 0; \
	(barr)->arrived = 0; \
	(barr)->num_threads = (num_threads); \
}

#define bar(barr) \
{\
	unsigned long local_sense, my_num; \
\
	local_sense = (barr)->global_sense; \
	my_num = fetch_and_add(&((barr)->arrived), 1); \
	if (my_num == (barr)->num_threads - 1) { \
		(barr)->arrived = 0; \
		(barr)->global_sense = !(barr)->global_sense; \
	} \
	else { \
		while(local_sense == (barr)->global_sense); \
	} \
}

#endif
