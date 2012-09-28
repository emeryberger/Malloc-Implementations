/*
 * Copyright (C) 2007  Scott Schneider, Christos Antonopoulos
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

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

