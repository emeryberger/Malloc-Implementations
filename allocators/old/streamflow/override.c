/*
 * override.c
 *
 * All system routines that we have to coopt in order for 
 * Streamflow to work correctly.
 */

#include <pthread.h>
#include <dlfcn.h>
#include <sys/mman.h>

#include "streamflow.h"

typedef int (*create_fun_t)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
typedef void (*exit_fun_t)(void*);
typedef void* (*start_fun_t)(void*);

typedef struct {
	start_fun_t	app_start;
	void*		app_arg;
} wrapper_args_t;

static void* wrapper(void* wargs)
{
	void* result;
	start_fun_t start_routine = ((wrapper_args_t*)wargs)->app_start;
	void* arg = ((wrapper_args_t*)wargs)->app_arg;

	thread_id = atmc_fetch_and_add(&global_id_counter, 1);

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

