/* test.c
An example of how to use nedalloc
(C) 2005 Niall Douglas
*/

#include <stdio.h>
#include <stdlib.h>
#include "nedmalloc.c"

#define THREADS 2
#define RECORDS (100000/THREADS)

static int whichmalloc;
static struct threadstuff_t
{
	int ops;
	unsigned int *toalloc;
	void **allocs;
	char cachesync[128];
} threadstuff[THREADS];

static void threadcode(int);

#ifdef WIN32
static DWORD WINAPI _threadcode(LPVOID a)
{
	threadcode((int)(size_t) a);
	return 0;
}
#define THREADVAR HANDLE
#define THREADINIT(v, id) (*v=CreateThread(NULL, 0, _threadcode, (LPVOID)(size_t) id, 0, NULL))
#define THREADSLEEP(v) SleepEx(v, FALSE)
#define THREADWAIT(v) (WaitForSingleObject(v, INFINITE), 0)

typedef unsigned __int64 usCount;
static FORCEINLINE usCount GetUsCount()
{
	static LARGE_INTEGER ticksPerSec;
	static double scalefactor;
	LARGE_INTEGER val;
	if(!scalefactor)
	{
		if(QueryPerformanceFrequency(&ticksPerSec))
			scalefactor=ticksPerSec.QuadPart/1000000000000.0;
		else
			scalefactor=1;
	}
	if(!QueryPerformanceCounter(&val))
		return (usCount) GetTickCount() * 1000000000;
	return (usCount) (val.QuadPart/scalefactor);
}

static HANDLE win32heap;
static void *win32malloc(size_t size)
{
	return HeapAlloc(win32heap, 0, size);
}
static void win32free(void *mem)
{
	HeapFree(win32heap, 0, mem);
}

static void *(*const mallocs[])(size_t size)={ malloc, nedmalloc, win32malloc };
static void (*const frees[])(void *mem)={ free, nedfree, win32free };
#else
static void *_threadcode(void *a)
{
	threadcode((int)(size_t) a);
	return 0;
}
#define THREADVAR pthread_t
#define THREADINIT(v, id) pthread_create(v, NULL, _threadcode, (void *)(size_t) id)
#define THREADSLEEP(v) usleep(v*1000)
#define THREADWAIT(v) pthread_join(v, NULL)

typedef unsigned long long usCount;
static FORCEINLINE usCount GetUsCount()
{
	struct timeval tv;
	gettimeofday(&tv, 0);
	return ((usCount) tv.tv_sec*1000000000000LL)+tv.tv_usec*1000000LL;
}

static void *(*const mallocs[])(size_t size)={ malloc, nedmalloc };
static void (*const frees[])(void *mem)={ free, nedfree };
#endif
static usCount times[THREADS];


static FORCEINLINE unsigned int myrandom(unsigned int *seed)
{
	*seed=1664525UL*(*seed)+1013904223UL;
	return *seed;
}

static void threadcode(int threadidx)
{
	int n;
	unsigned int *toallocptr=threadstuff[threadidx].toalloc;
	void **allocptr=threadstuff[threadidx].allocs;
	unsigned int seed=threadidx;
	usCount start;
	/*neddisablethreadcache(0);*/
	THREADSLEEP(100);
	start=GetUsCount();
	for(n=0; n<RECORDS;)
	{
#if 1
		unsigned int r=myrandom(&seed);
		if(n && (r & 65535)<32760) /*<32760)*/
		{	/* free */
			--toallocptr;
			--allocptr;
			--n;
			frees[whichmalloc](*allocptr);
			*allocptr=0;
		}
		else
#endif
		{
            *allocptr=mallocs[whichmalloc](*toallocptr);
			toallocptr++;
			allocptr++;
			n++;
			threadstuff[threadidx].ops++;
		}
	}
	for(n=0; n<RECORDS; n++)
	{
		frees[whichmalloc](*--allocptr);
	}
	times[threadidx]=GetUsCount()-start;
	neddisablethreadcache(0);
}

static double runtest()
{
	unsigned int seed=1;
	int n;
	THREADVAR threads[THREADS];
	for(n=0; n<THREADS; n++)
	{
		unsigned int *toallocptr;
		int m;
		threadstuff[n].ops=0;
		times[n]=0;
		threadstuff[n].toalloc=toallocptr=calloc(RECORDS, sizeof(unsigned int));
		threadstuff[n].allocs=calloc(RECORDS, sizeof(void *));
		for(m=0; m<RECORDS; m++)
		{
			unsigned int size=myrandom(&seed);
			if(size<(1<<30))
			{   /* Make it two power multiple of less than 512 bytes to
				model frequent C++ new's */
				size=4<<(size & 7);
			}
			else
			{
				size&=0x3FFF;             /* < 16Kb */
				/*size&=0x1FFF;*/			  /* < 8Kb */
				/*size=(1<<6)<<(size & 7);*/  /* < 8Kb */
			}
			*toallocptr++=size;
		}
	}
	for(n=0; n<THREADS; n++)
	{
		THREADINIT(&threads[n], n);
	}
	for(n=THREADS-1; n>=0; n--)
	{
		THREADWAIT(threads[n]);
		free(threadstuff[n].allocs); threadstuff[n].allocs=0;
		free(threadstuff[n].toalloc); threadstuff[n].toalloc=0;
		threads[n]=0;
	}
	{
		double opspersec;
		usCount totaltime=0;
		int totalops=0;
		for(n=0; n<THREADS; n++)
		{
			totaltime+=times[n];
			totalops+=threadstuff[n].ops;
		}
		opspersec=1000000000000.0*totalops/totaltime*THREADS;
		printf("This allocator achieves %lfops/sec under %d threads\n", opspersec, THREADS);
		return opspersec;
	}
}

int main(void)
{
	double std=0, ned=0;

#if 0
	{
		usCount start, end;
		start=GetUsCount();
		THREADSLEEP(5000);
		end=GetUsCount();
		printf("Wait was %lf\n", (end-start)/1000000000000.0);
	}
#endif
#ifdef WIN32
	{	/* Force load of user32.dll so we can debug */
		BOOL v;
		SystemParametersInfo(SPI_GETBEEP, 0, &v, 0);
	}
#endif

	if(0)
	{
		printf("\nTesting standard allocator with %d threads ...\n", THREADS);
		std=runtest();
	}
	if(1)
	{
		printf("\nTesting nedmalloc with %d threads ...\n", THREADS);
		whichmalloc=1;
		ned=runtest();
	}
#ifdef WIN32
	if(0)
	{
		ULONG data=2;
		win32heap=HeapCreate(0, 0, 0);
		HeapSetInformation(win32heap, HeapCompatibilityInformation, &data, sizeof(data));
		HeapQueryInformation(win32heap, HeapCompatibilityInformation, &data, sizeof(data), NULL);
		if(2!=data)
		{
			printf("The win32 low frag allocator won't work under a debugger!\n");
		}
		else
		{
			printf("Testing win32 low frag allocator with %d threads ...\n\n", THREADS);
			whichmalloc=2;
			runtest();
		}
		HeapDestroy(win32heap);
	}
#endif
	if(std && ned)
	{	// ned should have more ops/sec
		printf("\n\nnedmalloc allocator is %lf times faster than standard\n", ned/std);
	}
	printf("\nPress a key to trim\n");
	getchar();
	nedmalloc_trim(0);
#ifdef _MSC_VER
	printf("\nPress a key to end\n");
	getchar();
#endif
	return 0;
}
