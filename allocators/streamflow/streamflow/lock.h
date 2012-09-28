/*
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

#ifndef __LOCK_H_
#define __LOCK_H_

#include "atomic.h"

typedef volatile unsigned int lock_t;

static void spin_init(lock_t *lock);
static void spin_lock(lock_t *lock);
static void spin_unlock(lock_t *lock);

typedef struct {
	unsigned long num_threads;
	volatile unsigned long arrived;
	volatile unsigned long global_sense;	
} barrier_t;

static void bar_init(barrier_t *barr, unsigned long num_threads);
static void bar(barrier_t *barr);

static inline void spin_init(lock_t* lock)
{
	*(lock) = 0;
}

static inline void spin_lock(lock_t* lock)
{
	while (fetch_and_store((volatile unsigned int *)(lock), 1)) {
		while(*(lock));
	}
}

static inline void spin_unlock(lock_t* lock)
{
	*(lock) = 0;
}

static inline void bar_init(barrier_t* barr, unsigned long num_threads)
{
	barr->global_sense = 0;
	barr->arrived = 0;
	barr->num_threads = num_threads;
}

static inline void bar(barrier_t* barr)
{
	unsigned long local_sense, my_num;

	local_sense = barr->global_sense;
	my_num = fetch_and_add(&(barr->arrived), 1);
	if (my_num == barr->num_threads - 1) {
		barr->arrived = 0;
		barr->global_sense = !barr->global_sense;
	}
	else {
		while(local_sense == barr->global_sense);
	}
}

#endif
