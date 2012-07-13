/*
 * $Id: thread-st.h,v 1.1 2003/10/06 20:42:48 emery Exp $
 * by Wolfram Gloger 2001
 */

#include <stdio.h>

#include "thread-m.h"

#if USE_PTHREADS /* Posix threads */

#include <pthread.h>

pthread_cond_t finish_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t finish_mutex = PTHREAD_MUTEX_INITIALIZER;

#ifndef USE_PTHREADS_STACKS
#define USE_PTHREADS_STACKS 0
#endif

#endif /* USE_PTHREADS */

#ifndef STACKSIZE
#define STACKSIZE	32768
#endif

struct thread_st {
	char *sp;							/* stack pointer, can be 0 */
	void (*func)(struct thread_st* st);	/* must be set by user */
	thread_id id;
	int flags;
	struct user_data u;
};

static void
thread_init(void)
{
#if USE_PTHREADS
	printf("Using posix threads.\n");
	pthread_cond_init(&finish_cond, NULL);
	pthread_mutex_init(&finish_mutex, NULL);
#elif USE_THR
	printf("Using Solaris threads.\n");
#elif USE_SPROC
	printf("Using sproc() threads.\n");
#else
	printf("No threads.\n");
#endif
}

#if USE_PTHREADS || USE_THR
static void *
thread_wrapper(void *ptr)
#else
static void
thread_wrapper(void *ptr, size_t stack_len)
#endif
{
	struct thread_st *st = (struct thread_st*)ptr;

	/*printf("begin %p\n", st->sp);*/
	st->func(st);
#if USE_PTHREADS
	pthread_mutex_lock(&finish_mutex);
	st->flags = 1;
	pthread_mutex_unlock(&finish_mutex);
	pthread_cond_signal(&finish_cond);
#endif
	/*printf("end %p\n", st->sp);*/
#if USE_PTHREADS || USE_THR
	return NULL;
#endif
}

/* Create a thread. */
static int
thread_create(struct thread_st *st)
{
	st->flags = 0;
#if USE_PTHREADS
	{
		pthread_attr_t* attr_p = 0;
#if USE_PTHREADS_STACKS
		pthread_attr_t attr;

		pthread_attr_init (&attr);
		if(!st->sp)
			st->sp = malloc(STACKSIZE+16);
		if(!st->sp)
			return -1;
		if(pthread_attr_setstacksize(&attr, STACKSIZE))
			fprintf(stderr, "error setting stacksize");
		else
			pthread_attr_setstackaddr(&attr, st->sp + STACKSIZE);
		/*printf("create %p\n", st->sp);*/
		attr_p = &attr;
#endif
		return pthread_create(&st->id, attr_p, thread_wrapper, st);
	}
#elif USE_THR
	if(!st->sp)
		st->sp = malloc(STACKSIZE);
	if(!st->sp) return -1;
	thr_create(st->sp, STACKSIZE, thread_wrapper, st, THR_NEW_LWP, &st->id);
#elif USE_SPROC
	if(!st->sp)
		st->sp = malloc(STACKSIZE);
	if(!st->sp) return -1;
	st->id = sprocsp(thread_wrapper, PR_SALL, st, st->sp+STACKSIZE, STACKSIZE);
	if(st->id < 0) {
		return -1;
	}
#else /* NO_THREADS */
	st->id = 1;
	st->func(st);
#endif
	return 0;
}

/* Wait for one of several subthreads to finish. */
static void
wait_for_thread(struct thread_st st[], int n_thr,
				int (*end_thr)(struct thread_st*))
{
	int i;
#if USE_SPROC || USE_THR
	thread_id id;
#endif

#if USE_PTHREADS
	pthread_mutex_lock(&finish_mutex);
	for(;;) {
		int term = 0;
		for(i=0; i<n_thr; i++)
			if(st[i].flags) {
				/*printf("joining %p\n", st[i].sp);*/
				if(pthread_join(st[i].id, NULL) == 0) {
					st[i].flags = 0;
					if(end_thr)
						end_thr(&st[i]);
				} else
					fprintf(stderr, "can't join\n");
				++term;
			}
		if(term > 0)
			break;
		pthread_cond_wait(&finish_cond, &finish_mutex);
	}
	pthread_mutex_unlock(&finish_mutex);
#elif USE_THR
	thr_join(0, &id, NULL);
	for(i=0; i<n_thr; i++)
		if(id == st[i].id) {
			if(end_thr)
				end_thr(&st[i]);
			break;
		}
#elif USE_SPROC
	{
		int status = 0;
		id = wait(&status);
		if(status != 0) {
			if(WIFSIGNALED(status))
				printf("thread %id terminated by signal %d\n",
					   id, WTERMSIG(status));
			else
				printf("thread %id exited with status %d\n",
					   id, WEXITSTATUS(status));
		}
		for(i=0; i<n_thr; i++)
			if(id == st[i].id) {
				if(end_thr)
					end_thr(&st[i]);
				break;
			}
	}
#else /* NO_THREADS */
	for(i=0; i<n_thr; i++)
		if(end_thr)
			end_thr(&st[i]);
#endif
}

/*
 * Local variables:
 * tab-width: 4
 * End:
 */
