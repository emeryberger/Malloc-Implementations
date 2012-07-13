#ifndef _PHKMALLOCHEAP_H_
#define _PHKMALLOCHEAP_H_

extern "C" {
	void * phkmalloc(size_t);
	void phkfree(void *);
	//void * phkrealloc(void *, size_t);
	size_t phkgetsize(void *);
}

namespace HL {

class PhkMallocHeap {
public:
	inline void * malloc(size_t sz) {
		return phkmalloc(sz);
	}

	inline void free(void * ptr) {
		phkfree(ptr);
	}
/*
	inline void * realloc(void * ptr, size_t sz) {
		return phkrealloc(ptr, sz);
	}
*/
	inline size_t getSize(void * ptr) {
		return phkgetsize(ptr);
	}
};	//end of class PhkMallocHeap

};	//end of namespace HL

#endif
