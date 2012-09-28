/*
 * override.c
 *
 * All system routines that we have to coopt in order for 
 * Streamflow to work correctly.
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

#include <pthread.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <ctype.h>

#include <sys/types.h>
#include <linux/unistd.h>

#include "streamflow.h"

typedef int (*create_fun_t)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
typedef void (*exit_fun_t)(void*);
typedef void* (*start_fun_t)(void*);

typedef struct {
	start_fun_t	app_start;
	void*		app_arg;
} wrapper_args_t;

void discover_cpu();

static void* wrapper(void* wargs)
{
	void* result;
	start_fun_t start_routine = ((wrapper_args_t*)wargs)->app_start;
	void* arg = ((wrapper_args_t*)wargs)->app_arg;

	thread_id = atmc_fetch_and_add(&global_id_counter, 1);

#ifdef NUMA
	/*
	discover_cpu();
	*/
#endif

	munmap(wargs, sizeof(wrapper_args_t));
	result = start_routine(arg);
	streamflow_thread_finalize();

	return result;
}

int pthread_create(pthread_t * thread, const pthread_attr_t * attr, void * (*start_routine)(void *), void * arg)
{
	static create_fun_t real_create = NULL;
	wrapper_args_t* wargs;
	int result;

	if (real_create == NULL) {
		char* error;

		dlerror();
		real_create = (create_fun_t)dlsym(RTLD_NEXT, "pthread_create");
		if ((error = dlerror()) != NULL)  {
			fprintf(stderr, "pthread_create: %s\n", error);
			exit(1);
		}
	}

	/* There are three options for allocating the 8 bytes necessary to pass 
	 * on the application function and arguments:
	 * 	1. Allocate it on the stack, which requires using a barrier in 
	 * 	   this function to ensure that wrapper sees the values before 
	 * 	   the stack is destroyed.
	 * 	2. Use (our) malloc. This might not work if we need to 
	 * 	   initialize the allocator upon thread creation, and has 
	 * 	   circular dependencies that make me worry.
	 * 	3. Use mmap. This is heavy-weight, but it works and avoids 
	 * 	   circular dependencies and any synchronization. 
	 * I chose option 3. */
	wargs = (wrapper_args_t*)mmap(NULL, sizeof(wrapper_args_t), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, -1, 0);
	if (wargs == MAP_FAILED) {
		fprintf(stderr, "pthread_create() mmap failed\n");
		fflush(stderr);
		exit(1);
	}

	wargs->app_start = start_routine;
	wargs->app_arg = arg;
	result = real_create(thread, attr, wrapper, wargs);

	return result;
}

void pthread_exit(void* arg)
{
	static exit_fun_t real_exit = NULL;

	if (real_exit == NULL) {
		char* error;

		dlerror();
		real_exit = (exit_fun_t)dlsym(RTLD_NEXT, "pthread_exit");
		if ((error = dlerror()) != NULL)  {
			fprintf(stderr, "pthread_exit: %s\n", error);
			exit(1);
		}
	}

	streamflow_thread_finalize();
	real_exit(arg);

	/* This should never be reached; real_exit() 
	 * should exit for us. */
	exit(0);
}

#ifdef NUMA
/* Discover, through the /proc filesystem, what cpu we're on. */
void discover_cpu()
{
	FILE* stats;
	unsigned int cpu;
	int dint;
	char tcomm[16];
	char stat;
	long dlong;
	unsigned long dulong;
	unsigned long long dullong;
	char buffer[512];
	char proc[] = "/proc/self/task/";

	strcpy(buffer, proc);
	sprintf(buffer + strlen(proc), "%lu", syscall(__NR_gettid));
	strcpy(buffer + strlen(buffer), "/stat");

	if ((stats = fopen(buffer, "r")) == NULL) {
		perror("discover_cpu");
		exit(1);
	}

	fscanf(stats, "%d %s %c %d %d %d %d %d %lu %lu %lu %lu %lu %lu %lu %ld %ld %ld %ld %d %ld %llu %lu %ld %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %d %d %lu %lu\n",
			&dint,
			tcomm,
			&stat,
			&dint, &dint, &dint, &dint, &dint,
			&dulong, &dulong, &dulong, &dulong, &dulong, &dulong, &dulong,
			&dlong, &dlong, &dlong, &dlong,
			&dint, 
			&dlong,
			&dullong,
			&dulong,
			&dlong,
			&dulong, &dulong, &dulong, &dulong, &dulong, &dulong, &dulong, 
			&dulong, &dulong, &dulong, &dulong, &dulong, &dulong,
			&dint, 
			&cpu, 
			&dulong,
			&dulong);

	printf("thread %d on cpu %d and node%d\n", thread_id, cpu, cpu_to_node[cpu]);
	fflush(stdout);

	fclose(stats);
}

/* Determine the cpu-to-node mapping, bind the first thread. */
void numa_start()
{
	DIR* root_dir;
	struct dirent* root_entry;
	char path[NAME_MAX];
	size_t root_path_length;

	if (numa_available() < 0) {
		fprintf(stderr, "No NUMA support. Exiting.\n");
		exit(1);
	}

	strcpy(path, NODE_MAP_PATH);
	root_path_length = strlen(path);
	root_dir = opendir(path);
	if (!root_dir) {
		perror("numa_start");
		exit(1);
	}

	while ((root_entry = readdir(root_dir)) != NULL) {
		DIR* node_dir;
		struct dirent* node_entry;
		unsigned long node;

		if (strncmp(root_entry->d_name, "node", 4)) {
			continue;
		}
		
		node = strtoul(root_entry->d_name + 4, NULL, 0);
		strcpy(path + root_path_length, root_entry->d_name);
		node_dir = opendir(path);
		if (!root_dir) {
			perror("numa_start");
			exit(1);
		}

		while ((node_entry = readdir(node_dir)) != NULL) {
			unsigned long cpu;

			if (strncmp(node_entry->d_name, "cpu", 3) || isalpha(node_entry->d_name[3])) {
				continue;
			}

			cpu = strtoul(node_entry->d_name + 3, NULL, 0);
			cpu_to_node[cpu] = node;
		}

	}

	discover_cpu();
}
#endif

