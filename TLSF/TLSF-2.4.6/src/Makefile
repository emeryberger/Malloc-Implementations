WARNINGS = -Wextra -Wall -Wwrite-strings -Wstrict-prototypes \
           -Wmissing-prototypes -Wno-long-long -Wstrict-aliasing=2

HEAP_LAYERS = ../../../Heap-Layers

CFLAGS+= -g -O2 -fPIC  -I$(HEAP_LAYERS) # $(WARNINGS)
CFLAGS+= -DTLSF_USE_LOCKS=1
CFLAGS+= -DUSE_MMAP=1
# CFLAGS+= -DUSE_SBRK=1

# CFLAGS+=-ftest-coverage -fprofile-arcs
CC=gcc

all: tlsf.o libtlsf.so

tlsf.o: tlsf.h tlsf.c Makefile

tlsf-shim.o: tlsf-shim.cpp
	g++ $(CFLAGS) -c tlsf-shim.cpp

libtlsf.so: tlsf-shim.o tlsf.o
	g++ $(CFLAGS) -shared tlsf-shim.o tlsf.o $(HEAP_LAYERS)/wrappers/gnuwrapper.cpp -o libtlsf.so -ldl

clean:
	$(RM) -rf *.o *~ *.c.gcov *.gcda *.gcno

indent:
	indent -kr -l120 -br -ce -npsl -nut -i4 *.[hc]

