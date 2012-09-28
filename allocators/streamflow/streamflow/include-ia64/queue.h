#ifndef __QUEUE_H_
#define __QUEUE_H_

#include "atomic.h"
#include <stdlib.h>

typedef struct {
	volatile void* top;
} top_aba_t;

// Pseudostructure for lock-free list elements.
// The only requirement is that the 5th-8th byte of
// each element should be available to be used as
// the pointer for the implementation of a singly-linked
// list. 
struct queue_elem_t {
	char 				*_dummy;
	volatile struct queue_elem_t 	*next;
};

typedef struct {
	unsigned long long 	_pad0[8];
	top_aba_t		both;
	unsigned long long 	_pad1[8];
} lf_lifo_queue_t;

#define LF_FIFO_QUEUE_STATIC_INIT	{0, 0, 0, 0, 0, 0, 0, 0,\
					 NULL,\
					 0,\
					 0, 0, 0, 0, 0, 0, 0, 0}
					  
/******************************************************************************/

static __inline__ void lf_lifo_queue_init(lf_lifo_queue_t *queue);
static __inline__ int lf_lifo_enqueue(lf_lifo_queue_t *queue, void *element);
static __inline__ void *lf_lifo_dequeue(lf_lifo_queue_t *queue);

/******************************************************************************/

static __inline__ void lf_lifo_queue_init(lf_lifo_queue_t *queue)
{
	queue->both.top = 0;
}

/******************************************************************************/

static __inline__ void *lf_lifo_dequeue(lf_lifo_queue_t *queue)
{
	top_aba_t head;
	top_aba_t next;

	while(1) {
		head.top = queue->both.top;
		if (head.top == 0)
			return NULL;
		next.top = (unsigned long)((struct queue_elem_t *)head.top)->next;
		if (compare_and_swap64((unsigned long long *)&(queue->both), *((unsigned long long*)&head), *((unsigned long long*)&next))) {
			return((void *)head.top);
		}
	}
}

/******************************************************************************/

static __inline__ int lf_lifo_enqueue(lf_lifo_queue_t *queue, void *element)
{
	top_aba_t old_top;
	top_aba_t new_top;
	
	while(1) {
		old_top.top = queue->both.top;

		((struct queue_elem_t *)element)->next = (struct queue_elem_t *)old_top.top;
		new_top.top = (unsigned long)element;
		if (compare_and_swap64((unsigned long long *)&(queue->both), *((unsigned long long*)&old_top), *((unsigned long long*)&new_top))) {
			return 0;
		}
	}
}

/******************************************************************************/
/******************************************************************************/

/* Non ABA-safe lock-free LIFO queues. They can be safely used whenever we *
 * have single thread that performs all dequeue operations 		   */
 
static __inline__ void lf_lifo_queue_init_nABA(volatile struct queue_elem_t **head);
static __inline__ int lf_lifo_enqueue_nABA(volatile struct queue_elem_t **head, void *element);
static __inline__ void *lf_lifo_dequeue_nABA(volatile struct queue_elem_t **head);
static __inline__ void *lf_lifo_chain_dequeue_nABA(volatile struct queue_elem_t **head);

/******************************************************************************/

static __inline__ void lf_lifo_queue_init_nABA(volatile struct queue_elem_t **head)
{
	*head = NULL;
}

static __inline__ void lf_lifo_queue_init_nABA32(volatile unsigned int *head)
{
	*head = 0;
}
/******************************************************************************/

static __inline__ void *lf_lifo_dequeue_nABA(volatile struct queue_elem_t **head)
{
	struct queue_elem_t 	*top, *next;

	while(1) {
		top = *(struct queue_elem_t **)head;
		if (top == NULL)
			return(NULL);
		next = (struct queue_elem_t *)top->next;
		if (compare_and_swap_ptr(head, top, next)) {
			return((void *)top);
		}
	}
}

/******************************************************************************/

static __inline__ void *lf_lifo_chain_dequeue_nABA(volatile struct queue_elem_t **head)
{
	struct queue_elem_t 	*top;

	while(1) {
		top = *(struct queue_elem_t **)head;
		if (top == NULL)
			return(NULL);
		if (compare_and_swap_ptr(head, top, 0)) {
			return((void *)top);
		}
	}
}

static __inline__ unsigned int lf_lifo_chain_dequeue_nABA32(volatile unsigned int *head)
{
	unsigned int top;

	while(1) {
		top = *head;
		if (top == 0)
			return 0;
		if (compare_and_swap32(head, top, 0)) {
			return top;
		}
	}
}

/******************************************************************************/
static __inline__ int lf_lifo_enqueue_nABA(volatile struct queue_elem_t **head, void *element)
{
	struct queue_elem_t 	*top;
	
	while(1) {
		top = *(struct queue_elem_t **)head;
		((struct queue_elem_t *)element)->next = top;
		if (compare_and_swap_ptr(head, top, element)) {
			return(0);
		}
	}
}

/******************************************************************************/
/******************************************************************************/

/* Non-protected, single-threaded LIFO queues				      */

static __inline__ void seq_lifo_queue_init(struct queue_elem_t **head);
static __inline__ int seq_lifo_enqueue(struct queue_elem_t **head, void *element);
static __inline__ void *seq_lifo_dequeue(struct queue_elem_t **head);

/******************************************************************************/

static __inline__ void seq_lifo_queue_init(struct queue_elem_t **head)
{
	*head = NULL;
}

/******************************************************************************/

static __inline__ void *seq_lifo_dequeue(struct queue_elem_t **head)
{
	struct queue_elem_t 	*top;

	top = *head;
	if (top == NULL)
		return(NULL);
	*head = (struct queue_elem_t *)top->next;
	return((void *)top);
}

/******************************************************************************/

static __inline__ int seq_lifo_enqueue(struct queue_elem_t **head, void *element)
{
	((struct queue_elem_t *)element)->next = *head;
	*head = (struct queue_elem_t *)element;
	return(0);
}

/******************************************************************************/
#endif
