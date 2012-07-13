#ifndef _OTTOMALLOCHEAP_H_
#define _OTTOMALLOCHEAP_H_

extern "C" {
	void * otto_malloc(size_t);
	void otto_free(void *);
	void * otto_realloc(void *, size_t);
  void * otto_calloc(size_t, size_t);
  size_t otto_getsize(void * ptr);
}

namespace HL {

class OttoMallocHeap {
public:
	inline void * malloc(size_t sz) {
		return otto_malloc(sz);
	}

	inline void free(void * ptr) {
		otto_free(ptr);
	}
  
  inline size_t getSize(void * ptr) {
    return otto_getsize(ptr);
  }
  
	inline void * realloc(void * ptr, size_t sz) {
		return otto_realloc(ptr, sz);
	}
};	//end of class OttoMallocHeap

};	//end of namespace HL

#endif
