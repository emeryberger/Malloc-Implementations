/* -*- C++ -*- */

/**
 * @file   libcama.cpp
 * @brief  Replaces malloc and friends.
 * @author Emery Berger <http://www.cs.umass.edu/~emery>
 * @note   Copyright (C) 2005-2011 by Emery Berger, University of Massachusetts Amherst.
 */

// The undef below ensures that any pthread_* calls get strong
// linkage.  Otherwise, our versions here won't replace them.  It is
// IMPERATIVE that this line appear before any files get included.

#undef __GXX_WEAK__ 

#include <assert.h>
#include <stdlib.h>
#include <pthread.h>
#include <new>

#include "heaplayers.h"

#include "camarea.h"


// Temporarily testing spin locks.

class PosixSpinLockType {
public:

  PosixSpinLockType()
  {
    pthread_spin_init (&_lock, 0);
  }

  ~PosixSpinLockType() {
    pthread_spin_destroy (&_lock);
  }

  void lock() {
    pthread_spin_lock (&_lock);
  }

  void unlock() {
    pthread_spin_unlock (&_lock);
  }


private:
  pthread_spinlock_t _lock;
};


class CAMAHeap {
public:

  enum { Alignment = 8 };

  CAMAHeap() {
    cainit();
  }

  void * malloc (size_t sz) {
#if 0
    // Round up to the next multiple of Alignment if necessary.
    if (sz % Alignment != 0) {
      sz = sz + (Alignment - (sz % Alignment));
    }
#endif
    void * ptr = camalloc (sz, 1); // for now, just use one region.
    assert (getSize(ptr) >= sz);
    if ((size_t) ptr % Alignment != 0) {
      size_t align = (size_t) ptr % Alignment;
      printf ("CRAP: %u, %u\n", sz, align);
    }
    assert ((size_t) ptr % Alignment == 0);
    return ptr;
  }
  
  void free (void * ptr) {
    cafree (ptr);
  }

  size_t getSize (void * ptr) {
    return camsize (ptr);
  }

};

typedef
 ANSIWrapper<
  LockedHeap<PosixLockType,
	     CAMAHeap> >
TheCustomHeap;

class TheCustomHeapType : public TheCustomHeap {};

inline static TheCustomHeapType * getCustomHeap (void) {
  static char buf[sizeof(TheCustomHeapType)];
  static TheCustomHeapType * _theCustomHeap = 
    new (buf) TheCustomHeapType;
  return _theCustomHeap;
}

#if defined(_WIN32)
#pragma warning(disable:4273)
#endif

extern "C" {

  void * xxmalloc (size_t sz) {
    return getCustomHeap()->malloc (sz);
  }

  void xxfree (void * ptr) {
    getCustomHeap()->free (ptr);
  }

  size_t xxmalloc_usable_size (void * ptr) {
    return getCustomHeap()->getSize (ptr);
  }

  void xxmalloc_lock() {
    getCustomHeap()->lock();
  }

  void xxmalloc_unlock() {
    getCustomHeap()->unlock();
  }

}
