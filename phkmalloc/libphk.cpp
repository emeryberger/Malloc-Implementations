#include <stdlib.h>

extern "C" {
  void * phkmalloc (size_t);
  void phkfree (void *);
  size_t phkmalloc_usable_size (void *);
}


class TheCustomHeapType {
public:
  inline void * malloc (size_t sz) {
    return phkmalloc (sz);
  }
  inline void free (void * ptr) {
    phkfree (ptr);
  }
  inline size_t getSize (void * ptr) {
    return phkmalloc_usable_size (ptr);
  }
};

TheCustomHeapType * getCustomHeap (void) {
  static TheCustomHeapType tHeap;
  return &tHeap;
}

#include "wrapper.cpp"
