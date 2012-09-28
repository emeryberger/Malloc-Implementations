#include <new>

extern "C" {
	void* malloc(size_t);
	void free(void*);
}

void* operator new(size_t sz) throw (std::bad_alloc)
{
	return malloc(sz);
}

void * operator new(size_t sz, const std::nothrow_t&) throw()
{
	return malloc(sz);
}

void operator delete(void * ptr)
{
	free(ptr);
}

void* operator new[](size_t sz) throw (std::bad_alloc)
{
	return malloc(sz);
}

void * operator new[](size_t sz, const std::nothrow_t&) throw()
{
	return malloc(sz);
}

void operator delete[](void * ptr)
{
	free(ptr);
}

