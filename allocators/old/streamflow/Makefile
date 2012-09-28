# How do we figure out the meta information for an object? 
# Options:
#	- HEADERS
#	- BIBOP
#	- RADIX_TREE
META_METHOD	= BIBOP

# What architecture are we on?
# Options:
# 	- ppc64 (IBM PowerPC, 64-bit)
#	- x86 (Intel x86, 32-bit)
ASM		= x86

ifeq ($(ASM), ppc64)
	BITS = 64
	FPIC = -fPIC
endif
ifeq ($(ASM), x86)
	BITS = 32
endif
ifndef BITS
	$(error Need to define ASM.)
endif

CC		= gcc
CXX		= g++

LDFLAGS		= -lpthread -lm -ldl
CFLAGS		= -D_REENTRANT -D$(ASM) -D$(META_METHOD)
#CFLAGS		+= -DSUPERPAGES
#CFLAGS		+= -DMEMORY
#CFLAGS		+= -DCOLOR

GCC_CFLAGS	= -D_GNU_SOURCE -Wall -m$(BITS) -fno-strict-aliasing $(FPIC)
GCC_OPT		= -O3 -pipe -finline-functions -fomit-frame-pointer

ICC_CFLAGS	= -wd279 -wd981 -wd1418 -wd1469 -wd383 -wd869 -wd522 -wd810 -wd1684 -wd1338 -wd1684 -D_GNU_SOURCE
ICC_OPT		= -O3 -pipe -finline-functions -fomit-frame-pointer 

XLC_CFLAGS	= -q64 -qabi_version=2 -qasm=gcc 
XLC_OPT		= -O4

ifeq ($(CC), gcc)
	OPT = $(GCC_OPT)
	CFLAGS += $(GCC_CFLAGS)
endif
ifeq ($(CC), icc)
	OPT = $(ICC_OPT)
	CFLAGS += $(ICC_CFLAGS)
endif
ifeq ($(CC), xlc_r)
	OPT = $(XLC_OPT)
	CFLAGS += $(XLC_CFLAGS)
endif
ifndef OPT
	$(error Need to define CC.)
endif

# Rules.

all:	libstreamflow.so recycle larson

clean:
	rm -f *.o *.so recycle larson 

streamflow.o:		streamflow.h streamflow.c
			$(CC) $(CFLAGS) $(OPT) -Iinclude-$(ASM) -c streamflow.c 

malloc_new.o:		malloc_new.cpp streamflow.h
			$(CXX) $(CFLAGS) $(OPT) -Iinclude-$(ASM) -c malloc_new.cpp

override.o:		override.c streamflow.h
			$(CC) $(CFLAGS) $(OPT) -Iinclude-$(ASM) -c override.c

libstreamflow.so:	malloc_new.o streamflow.o override.o
			$(CXX) $(CFLAGS) $(OPT) override.o streamflow.o malloc_new.o -o libstreamflow.so $(LDFLAGS) -lstdc++ -shared 

recycle:		recycle.c 
			$(CC) $(CFLAGS) $(OPT) -o recycle recycle.c $(LDFLAGS)

larson:			larson.cpp 
			$(CXX) $(CFLAGS) $(OPT) -o larson larson.cpp $(LDFLAGS)

