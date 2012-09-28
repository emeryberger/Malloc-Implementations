/// @author Emery Berger

#include <iostream>
using namespace std;

#include <string.h>

extern "C" {
#include "camarea.h"
}

int
main()
{
  cainit();
  for (int i = 0; i < 100; i++) {
    for (int j = 0; j < 100; j++) {
      size_t sz = 16 * (i+1);
      void * ptr = camalloc (sz, 1);
      memset (ptr, 0, sz);
      cout << "size = " << camsize(ptr) << endl;
      if (ptr != NULL) {
	cafree (ptr);
      }
    }
  }
  return 0;
}
