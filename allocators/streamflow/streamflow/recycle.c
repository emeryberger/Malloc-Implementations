/*
 * recycle.c
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

#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
/* EDB DISABLED #include <numa.h> */

size_t min_size;
size_t max_size;
int iterations = (int)1e8;
int rate;

double random_number()
{
	static long int seed = 547845897;
	static long int m = 2147483647;         // m is the modulus, m = 2 ^ 31 - 1
	static long int a = 16807;              // a is the multiplier, a = 7 ^ 5
	static long int q = 127773;             // q is the floor of m / a
	static long int r = 2836;               // r is m mod a

	long int temp = a * (seed % q) - r * (seed / q);

	if (temp > 0) {
		seed = temp;
	}
	else {
		seed = temp + m;
	}

	return (double)seed / (double)m;
}

void* simulate_work(void* arg)
{
	unsigned long** reserve = malloc(rate * sizeof(void*));
	int i;
	int j;
	double rand;
	size_t object_size;

	for (i = 0; i < iterations; ++i) {

		if (i % rate == 0 && i != 0) {
			for (j = 0; j < rate; ++j) {
				free(reserve[j]);
			}
		}

		rand = random_number();
		object_size = min_size + (rand * (max_size - min_size));
		reserve[i % rate] = malloc(object_size);
	}

	free(reserve);

	return NULL;
}

void numa_start(void);

int main(int argc, char* argv[])
{
	pthread_t* threads;

	//numa_start();

	if (argc < 5) {
		printf("correct usage: recycle <num threads> <min alloc size> <max alloc size> <alloc rate>\n");
		exit(0);
	}

	int num_threads = atoi(argv[1]);
	min_size = atoi(argv[2]);
	max_size = atoi(argv[3]);
	rate = atoi(argv[4]);

	iterations /= num_threads;

	threads = (pthread_t*)malloc(num_threads * sizeof(pthread_t));

	int i;
	for (i = 0; i < num_threads-1; ++i) {
		pthread_create(&threads[i], NULL, simulate_work, NULL);
	}

	simulate_work(NULL);
	
	for (i = 0; i < num_threads-1; ++i) {
		pthread_join(threads[i], NULL);
	}

	return 0;
}

