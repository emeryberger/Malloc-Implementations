#ifndef __SYNCHRO_ATOMIC_H__
#define __SYNCHRO_ATOMIC_H__

#define mb()		__asm__ __volatile__ ("sync" : : : "memory")
#define EIEIO_ON_SMP	"eieio\n"
#define ISYNC_ON_SMP	"\n\tisync"

static inline unsigned long fetch_and_store(volatile unsigned int *address, unsigned int value)
{
	volatile unsigned long dummy = 0;

	__asm__ __volatile__(
	EIEIO_ON_SMP
"1:	lwarx %0,0,%3		# __xchg_u32\n\
	stwcx. %2,0,%3\n\
2:	bne- 1b"
	ISYNC_ON_SMP
 	: "=&r" (dummy), "=m" (*address)
	: "r" (value), "r" (address)
	: "cc", "memory");

	return dummy;
}

static inline int atmc_fetch_and_add(volatile unsigned int *address, int value)
{
	int t;

	__asm__ __volatile__(
"1:	lwarx	%0,0,%2		# atomic_add_return\n\
	add	%0,%1,%0\n\
	stwcx.	%0,0,%2\n\
	bne-	1b"
	ISYNC_ON_SMP
	: "=&r" (t)
	: "r" (value), "r" (address)
	: "cc", "memory");

	return t;
}

static inline void atmc_add32(volatile unsigned int* address, int value)
{
	int t;

	__asm__ __volatile__(
"1:	lwarx	%0,0,%3		# atomic_add\n\
	add	%0,%2,%0\n\
	stwcx.	%0,0,%3\n\
	bne-	1b"
	: "=&r" (t), "=m" (*address)
	: "r" (value), "r" (address), "m" (*address)
	: "cc");
}

static inline void atmc_add64(volatile unsigned long long* address, unsigned long long value)
{
	int t;

	__asm__ __volatile__(
"1:	ldarx	%0,0,%3		# atomic_add\n\
	add	%0,%2,%0\n\
	stdcx.	%0,0,%3\n\
	bne-	1b"
	: "=&r" (t), "=m" (*address)
	: "r" (value), "r" (address), "m" (*address)
	: "cc");
}

static inline unsigned int compare_and_swap32(volatile unsigned int *address, unsigned int old_value, unsigned int new_value)
{
	unsigned int prev;

	__asm__ __volatile__ (
	EIEIO_ON_SMP
"1:	lwarx	%0,0,%2		# __cmpxchg_u32\n\
	cmpw	0,%0,%3\n\
	bne-	2f\n\
	stwcx.	%4,0,%2\n\
	bne-	1b"
	ISYNC_ON_SMP
	"\n\
2:"
	: "=&r" (prev), "=m" (*address)
	: "r" (address), "r" (old_value), "r" (new_value), "m" (*address)
	: "cc", "memory");

	return prev == old_value;
}

static inline unsigned int compare_and_swap64(volatile unsigned long *address, 
						unsigned long old_value, unsigned long new_value)
{ 
	unsigned long prev = 0;

	__asm__ __volatile__ (
	EIEIO_ON_SMP
"1:	ldarx	%0,0,%2		# __cmpxchg_u64\n\
	cmpd	0,%0,%3\n\
	bne-	2f\n\
	stdcx.	%4,0,%2\n\
	bne-	1b"
	ISYNC_ON_SMP
	"\n\
2:"
	: "=&r" (prev), "=m" (*address)
	: "r" (address), "r" (old_value), "r" (new_value), "m" (*address)
	: "cc", "memory");

	return prev == old_value;
}

static inline unsigned long compare_and_swap_ptr(volatile void *address, void* old_ptr, void* new_ptr)
{
	return compare_and_swap64((volatile unsigned long *)address, (unsigned long)old_ptr, (unsigned long)new_ptr); 
}

#endif

