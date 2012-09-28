#ifndef __SYNCHRO_ATOMIC_H__
#define __SYNCHRO_ATOMIC_H__

#define __fool_gcc(x) (*(struct {unsigned int a[100];} *)x)

/***************************************************************************/

#define fetch_and_store(address, value) \
({ \
 volatile unsigned long ret_val; \
\
 __asm__ __volatile__ ("lock; xchgl %0, %1" \
 		       :"=r" ((ret_val)) \
 		       :"m" (__fool_gcc((address))), "0" ((value)) \
 		       :"memory"); \
 ret_val; \
})

#define atmc_add32(address, value) \
({ \
  register volatile long val = (value); \
\
  __asm__ __volatile__ ("lock; addl %1, %0" \
  			: "=m" (*(address)) \
  			: "r" (val)); \
})

/* This version does not work due to a bug in 3.3.x gcc compilers... *  \
 * Using an uninitialized register as an argument to xaddl if -O2 or *  \
 * -O3 optimization levels are used				     */ \
/*
#define atmc_fetch_and_add(address, value) \
({ \
  register volatile unsigned long val = (value); \
\
  __asm__ __volatile__ ("lock; xaddl %0, %1" \
  			: "=r" (val), "=m" (*(address)) \
  			: "r" (val) \
  			: "memory", "cc");\
\
 val; \
})
*/

#define atmc_fetch_and_add(address, value) \
({ \
  register volatile unsigned long val = (value); \
\
  __asm__ __volatile__ ("lock\n\t" \
  			"xaddl %%ecx, %1" \
  			: "=c" (val), "=m" (*(address)) \
  			: "c" (val) \
  			: "memory", "cc");\
\
 (val + (value)); \
})

#define compare_and_swap32(address, old_value, new_value) \
({ \
  unsigned long ret_val = 0; \
  __asm__ __volatile__ ("lock\n\t" \
  			"cmpxchgl %2, (%1)\n\t" \
  			"sete (%3)\n\t" \
  			: \
  			: "a" (old_value), "r" (address), "r" (new_value), \
  			  "r" (&ret_val) \
  			: "memory"); \
  ret_val; \
})

#define compare_and_swap64(address, old_value, new_value) \
({ \
	unsigned long ret_val = 0; \
	__asm__ __volatile__ ("lock\n\t" \
			"cmpxchg8b (%0)\n\t" \
			"sete (%1)\n\t" \
			: \
			: "r" (address), "r" (&ret_val), \
			"d" (*(((unsigned int*)&(old_value))+1)), "a" (*(((unsigned int*)&(old_value))+0)), \
			"c" (*(((unsigned int*)&(new_value))+1)), "b" (*(((unsigned int*)&(new_value))+0)) \
			: "memory"); \
			ret_val; \
})

#define compare_and_swap_ptr(address, old_value, new_value) compare_and_swap32(address, old_value, new_value)

static inline void atmc_add64(volatile unsigned long long* address, unsigned long long value)
{
	unsigned long long oldval;
	unsigned long long newval;
	do {
		oldval = *address;
		newval = oldval + value;
	} while (!compare_and_swap64(address, oldval, newval));
}

/*
#define __compare_and_swap64(address, old_value_low, old_value_high, \
			   new_value_low, new_value_high) \
({ \
  unsigned long ret_val = 0; \
  __asm__ __volatile__ ("lock\n\t" \
  			"cmpxchg8b (%0)\n\t" \
  			"sete (%1)\n\t" \
  			: \
  			: "r" (address), "r" (&ret_val), \
  			  "d" (old_value_high), "a" (old_value_low), \
  			  "c" (new_value_high), "b" (new_value_low) \
  			: "memory"); \
  ret_val; \
 })
 */

#endif

