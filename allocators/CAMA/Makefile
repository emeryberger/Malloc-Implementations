CC  = gcc -Wall -W -g -std=c99 -DNDEBUG -O3 -fPIC
CXX = g++ -Wall -W -g -DNDEBUG -O3 -fPIC

libcama.so: libcama.cpp camarea.o
	$(CXX) -IHeap-Layers -shared libcama.cpp Heap-Layers/wrappers/gnuwrapper.cpp camarea.o -o libcama.so -lpthread -ldl

#cama-driver: cama-driver.cpp camarea.o
#	$(CXX) cama-driver.cpp camarea.o -o cama-driver

camarea.o: camarea.h camarea2.c
	$(CC) -c camarea2.c -o camarea.o

.PHONY: clean

clean:
	rm -f cama-driver camarea.o
