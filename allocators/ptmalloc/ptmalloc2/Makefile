# Makefile for ptmalloc, version 2
# by Wolfram Gloger 1996-1999, 2001, 2002

DIST_FILES0 = ChangeLog thread-m.h malloc.h malloc.c arena.c hooks.c \
 tst-mallocstate.c
DIST_FILES1 = COPYRIGHT README Makefile \
 $(DIST_FILES0) \
 lran2.h t-test.h thread-st.h t-test1.c t-test2.c
DIST_FILES2 = $(DIST_FILES1) Makefile.glibc glibc-include RCS/*,v

#CC = /pkg/gcc-2.95.2-wg/bin/gcc
CC = cc

SYS_FLAGS  =
OPT_FLAGS  = -g -O # -O2
WARN_FLAGS = #-Wall -Wstrict-prototypes
SH_FLAGS   = -shared

# Flags for the test programs
T_FLAGS   = -DUSE_MALLOC=1 -DTEST=1

# Flags for the compilation of malloc.c
M_FLAGS   = -DTHREAD_STATS=1 #-DMALLOC_DEBUG=1

# Flags for the threads package configuration.  Adjust this for your
# architecture, or see the platform-specific targets below.
# Valid discerning symbols are:
# USE_PTHREADS=1 for Posix threads
# USE_THR=1      for Solaris threads
# USE_SPROC=1    for SGI sprocs
THR_FLAGS = -DUSE_PTHREADS=1 -DUSE_TSD_DATA_HACK -D_REENTRANT
THR_LIBS  = -lpthread

RM        = rm -f

MALLOC = malloc.o

T_SUF =
TESTS = t-test1$(T_SUF) t-test2$(T_SUF) tst-mallocstate$(T_SUF)

CFLAGS = $(SYS_FLAGS) $(OPT_FLAGS) $(WARN_FLAGS) $(THR_FLAGS)

.c.o:
	$(CC) -c $(CFLAGS) $<

all: $(TESTS)

shared: malloc.so

malloc.o: malloc.c malloc.h thread-m.h
	$(CC) -c $(CFLAGS) $(M_FLAGS) $<

malloc.so: malloc.c malloc.h thread-m.h
	$(CC) $(SH_FLAGS) $(CFLAGS) $(M_FLAGS) $< -o $@

again:
	$(RM) $(TESTS)
	$(MAKE) $(TESTS)

clean:
	$(RM) malloc.o malloc.so $(TESTS) core core.[0-9]*

t-test1$(T_SUF): t-test1.c t-test.h thread-m.h $(MALLOC)
	$(CC) $(CFLAGS) $(T_FLAGS) t-test1.c $(MALLOC) $(THR_LIBS) -o $@

t-test2$(T_SUF): t-test2.c t-test.h thread-m.h $(MALLOC)
	$(CC) $(CFLAGS) $(T_FLAGS) t-test2.c $(MALLOC) $(THR_LIBS) -o $@

tst-mallocstate$(T_SUF): tst-mallocstate.c thread-m.h $(MALLOC)
	$(CC) $(CFLAGS) $(T_FLAGS) tst-mallocstate.c $(MALLOC) $(THR_LIBS) \
 -o $@

############################################################################
# Platform-specific targets. The ones ending in `-libc' are provided
# to enable comparison with the standard malloc implementation from
# the system's native C library.  The option USE_TSD_DATA_HACK is now
# the default for pthreads systems, as most (Irix 6, Solaris 2) seem
# to need it.  Try with USE_TSD_DATA_HACK undefined only if you're
# confident that your systems's thread specific data functions do not
# use malloc themselves.

posix:
	$(MAKE) THR_FLAGS='-DUSE_PTHREADS=1 -DUSE_TSD_DATA_HACK -D_REENTRANT' \
 OPT_FLAGS='$(OPT_FLAGS)' SYS_FLAGS='$(SYS_FLAGS)' CC='$(CC)' \
 THR_LIBS=-lpthread

posix-with-tsd:
	$(MAKE) THR_FLAGS='-DUSE_PTHREADS=1 -D_REENTRANT' THR_LIBS=-lpthread

posix-libc:
	$(MAKE) THR_FLAGS='-DUSE_PTHREADS=1 -D_REENTRANT' THR_LIBS=-lpthread \
	 MALLOC= T_FLAGS= T_SUF=-libc

linux-pthread:
	$(MAKE) SYS_FLAGS='-D_GNU_SOURCE=1' \
 WARN_FLAGS='-Wall -Wstrict-prototypes' \
 OPT_FLAGS='$(OPT_FLAGS)' THR_FLAGS='-DUSE_PTHREADS=1 -DUSE_TSD_DATA_HACK'

sgi:
	$(MAKE) THR_FLAGS='-DUSE_SPROC=1' THR_LIBS='' \
 OPT_FLAGS='$(OPT_FLAGS)' CC='$(CC)'

sgi-shared:
	$(MAKE) THR_FLAGS='-DUSE_SPROC=1' THR_LIBS= \
	 SH_FLAGS='-shared -check_registry /usr/lib/so_locations' \
	  MALLOC=malloc.so shared all

sgi-libc:
	$(MAKE) THR_FLAGS='-DUSE_SPROC=1' THR_LIBS= MALLOC= T_FLAGS= \
	 T_SUF=-libc

solaris:
	$(MAKE) THR_FLAGS='-DUSE_THR=1 -D_REENTRANT' OPT_FLAGS='$(OPT_FLAGS)' \
 THR_LIBS=-lthread

solaris-libc:
	$(MAKE) THR_FLAGS='-DUSE_THR=1 -D_REENTRANT' OPT_FLAGS='$(OPT_FLAGS)' \
 THR_LIBS=-lthread MALLOC= T_FLAGS= T_SUF=-libc

nothreads:
	$(MAKE) OPT_FLAGS='$(OPT_FLAGS)' SYS_FLAGS='$(SYS_FLAGS)' \
 THR_FLAGS='' THR_LIBS=''

gcc-nothreads:
	$(MAKE) CC='gcc' WARN_FLAGS='-Wall' OPT_FLAGS='$(OPT_FLAGS)' \
 SYS_FLAGS='$(SYS_FLAGS)' THR_FLAGS='' THR_LIBS='' M_FLAGS='$(M_FLAGS)'

linux-nothreads:
	$(MAKE) CC='gcc' WARN_FLAGS='-Wall' OPT_FLAGS='$(OPT_FLAGS)' \
 SYS_FLAGS='-D_GNU_SOURCE' THR_FLAGS='' THR_LIBS='' M_FLAGS='$(M_FLAGS)'

traditional:
	$(MAKE) THR_FLAGS='' THR_LIBS='' CC='gcc -traditional'

glibc-test:
	$(MAKE) THR_FLAGS='-DUSE_PTHREADS=1 -D_LIBC' \
 SYS_FLAGS='-D_GNU_SOURCE=1 -Iglibc-include -include glibc-include/libc-symbols.h' \
 WARN_FLAGS='-Wall -Wstrict-prototypes -Wbad-function-cast -Wmissing-noreturn -Wmissing-prototypes -Wmissing-declarations -Wcomment -Wcomments -Wtrigraphs -Wmultichar -Wstrict-prototypes -Winline' \
 malloc.o && mv malloc.o malloc-glibc.o

############################################################################

check: $(TESTS)
	./t-test1
	./t-test2

snap:
	cd ..; tar --numeric-owner -c -f - $(DIST_FILES1:%=ptmalloc2/%) | \
	 gzip -9 >ptmalloc2-current.tar.gz

dist:
	cd ..; tar --numeric-owner -c -f - $(DIST_FILES2:%=ptmalloc2/%) | \
	 gzip -9 >ptmalloc2.tar.gz

Makefile.glibc.diff: Makefile.glibc
	-diff -u /mount/public/export/glibc/cvs/libc/malloc/Makefile \
 Makefile.glibc >$@

dist-glibc: Makefile.glibc.diff
	tar cf - $(DIST_FILES0) Makefile.glibc.diff | \
	 gzip -9 >../libc.malloc.tar.gz

# dependencies
malloc.o: arena.c hooks.c
