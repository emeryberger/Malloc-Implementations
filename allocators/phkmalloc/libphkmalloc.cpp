#include <stdlib.h>
#include <new>

#include "heaplayers.h"
#include "phkmallocheap.h"

using namespace HL;
volatile int anyThreadCreated = 0;


#ifdef DEBUG
class TheCustomHeapType : public ANSIWrapper<SanityCheckHeap<PhkMallocHeap> > {};
#else
class TheCustomHeapType : public ANSIWrapper<PhkMallocHeap> {};
#endif

inline static TheCustomHeapType * getCustomHeap() {
	static char thBuf[sizeof(TheCustomHeapType)];
	static TheCustomHeapType * th = new (thBuf) TheCustomHeapType;
	return th;
}

#include "wrapper.cpp"
