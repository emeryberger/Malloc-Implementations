#include <stdlib.h>
#include <new>
#include <cstdio>
using namespace std;

#include "ottomallocheap.h"

using namespace HL;
volatile int anyThreadCreated = 0;

class TheCustomHeapType : public OttoMallocHeap {};

inline static TheCustomHeapType * getCustomHeap() {
	static char thBuf[sizeof(TheCustomHeapType)];
	static TheCustomHeapType * th = new (thBuf) TheCustomHeapType;
	return th;
}

#include "wrapper.cpp"
