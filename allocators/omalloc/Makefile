LIB = libomalloc.so

CFLAGS = -fPIC -O3 -DNDEBUG
CC = gcc
CXX = g++

SRCS = omalloc.c random.c issetugid.c
OBJS = omalloc.o random.o issetugid.o

%.o: %.c
	$(CC) $(CFLAGS) -c $<

$(LIB): gnuwrapper.cpp $(OBJS)
	$(CXX) $(CFLAGS) -shared gnuwrapper.cpp $(OBJS) -o $(LIB)

clean:
	-rm $(LIB) $(OBJS)


