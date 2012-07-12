phk:
	gcc -pipe -mcpu=pentiumpro -O3 -finline-limit=5000 -fkeep-inline-functions -finline-functions -ffast-math -fomit-frame-pointer -DNDEBUG  -UDEBUG -D_REENTRANT=1 -c phkmalloc_linux.c
	g++ -pipe -mcpu=pentiumpro -O3 -finline-limit=5000 -fkeep-inline-functions -finline-functions -ffast-math -fomit-frame-pointer -DNDEBUG  -UDEBUG -D_REENTRANT=1 -I../../heaplayers -shared libphkmalloc.cpp phkmalloc_linux.o -o libphkmalloc.so

phk_sc:
	gcc -pipe -mcpu=pentiumpro -O3 -finline-limit=5000 -fkeep-inline-functions -finline-functions -ffast-math -fomit-frame-pointer -DNDEBUG  -UDEBUG -D_REENTRANT=1 -c phkmalloc_sc.c
	g++ -pipe -mcpu=pentiumpro -O3 -finline-limit=5000 -fkeep-inline-functions -finline-functions -ffast-math -fomit-frame-pointer -DNDEBUG  -UDEBUG -D_REENTRANT=1 -I../../heaplayers -shared libphkmalloc.cpp phkmalloc_sc.o -o libphkmalloc_sc.so

phk_reap:
	gcc -pipe -mcpu=pentiumpro -O3 -finline-limit=5000 -fkeep-inline-functions -finline-functions -ffast-math -fomit-frame-pointer -DNDEBUG  -UDEBUG -D_REENTRANT=1 -c phkmalloc_reap.c
	g++ -pipe -mcpu=pentiumpro -O3 -finline-limit=5000 -fkeep-inline-functions -finline-functions -ffast-math -fomit-frame-pointer -DNDEBUG  -UDEBUG -D_REENTRANT=1 -I../../heaplayers -shared libphkmalloc.cpp phkmalloc_reap.o -o libphkmalloc_reap.so

phk_sc_reap:
	gcc -pipe -mcpu=pentiumpro -O3 -finline-limit=5000 -fkeep-inline-functions -finline-functions -ffast-math -fomit-frame-pointer -DNDEBUG  -UDEBUG -D_REENTRANT=1 -c phkmalloc_sc_reap.c
	g++ -pipe -mcpu=pentiumpro -O3 -finline-limit=5000 -fkeep-inline-functions -finline-functions -ffast-math -fomit-frame-pointer -DNDEBUG  -UDEBUG -D_REENTRANT=1 -I../../heaplayers -shared libphkmalloc.cpp phkmalloc_sc_reap.o -o libphkmalloc_sc_reap.so

debug:
	gcc -g -c phkmalloc_linux_vmcomm.c
	g++ -g -DDEBUG -I../../heaplayers -shared libphkmalloc.cpp phkmalloc_linux_vmcomm.o -o libdebug.so
#	gcc -g -c phkmalloc_linux.c
#	g++ -g -DDEBUG -I../../heaplayers -shared libphkmalloc.cpp phkmalloc_linux.o -o libphkmalloc.so

clean:
	rm *.so *.o

phk_objs:
	gcc -O3 -finline-functions -fkeep-inline-functions -ffast-math -fomit-frame-pointer -pipe -DNDEBUG -UDEBUG -c phkmalloc_linux.c
	g++ -O3 -finline-functions -fkeep-inline-functions -ffast-math -fomit-frame-pointer -pipe -DNDEBUG -UDEBUG -I../../heaplayers -c libphkmalloc.cpp
	g++ -O3 -finline-functions -fkeep-inline-functions -ffast-math -fomit-frame-pointer -pipe -DNDEBUG -UDEBUG -shared libphkmalloc.o phkmalloc_linux.o -o libphkmalloc.so

