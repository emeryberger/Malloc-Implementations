phk:
	gcc -g -fPIC -pipe -O3 -finline-limit=5000 -fkeep-inline-functions -finline-functions -ffast-math -DNDEBUG  -UDEBUG -D_REENTRANT=1 -c ottomalloc_linux.c
	gcc -g -fPIC -pipe -O3 -finline-limit=5000 -fkeep-inline-functions -finline-functions -ffast-math -DNDEBUG  -UDEBUG -D_REENTRANT=1 -c arc4random.c
	g++ -g -fPIC -pipe -O3 -finline-limit=5000 -fkeep-inline-functions -finline-functions -ffast-math -DNDEBUG  -UDEBUG -D_REENTRANT=1 -I../../heaplayers -I../../heaplayers/util -shared -D'CUSTOM_PREFIX(x)=otto_##x' gnuwrapper.cpp arc4random.o ottomalloc_linux.o  -o libottomalloc.so -ldl 

phk_objs:
	gcc -O3 -finline-functions -fkeep-inline-functions -ffast-math -fomit-frame-pointer -pipe -DNDEBUG -UDEBUG -c phkmalloc_linux.c
	g++ -O3 -finline-functions -fkeep-inline-functions -ffast-math -fomit-frame-pointer -pipe -DNDEBUG -UDEBUG -I../../heaplayers -c libphkmalloc.cpp
	g++ -O3 -finline-functions -fkeep-inline-functions -ffast-math -fomit-frame-pointer -pipe -DNDEBUG -UDEBUG -shared libphkmalloc.o phkmalloc_linux.o -o libphkmalloc.so

