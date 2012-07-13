/*

  HeapShield
  
  Prevents library-based heap overflow attacks
  for any memory allocator that maintains object size
  GIVEN AN INTERIOR POINTER.
 
  Copyright (C) 2000-2010 by Emery Berger
  http://www.cs.umass.edu/~emery
  emery@cs.umass.edu
  
  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.
  
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
  
  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

*/

/*

fprintf
fputs
fscanf
fwrite
printf
puts
sscanf

strcpy, strlen, sprintf

 */

#if defined(_WIN32)
//#error "This file does not work for Windows. Sorry."
#endif

#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <limits.h>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

#ifndef CUSTOM_PREFIX
#define CUSTOM_PREFIX(x) x
#endif

#undef memcpy
#undef strcpy

extern bool malloc_hooked;

inline static 
bool onStack (void * ptr) {
  volatile int a;
  volatile int b;
  if ((size_t) &b > (size_t) &a) {
    // Stack grows up.
    return ((size_t) ptr < (size_t) &a);
  } else {
    // Stack grows down.
    return ((size_t) ptr > (size_t) &b);
  }
}

extern "C" {
  size_t CUSTOM_PREFIX(malloc_usable_size)(void *);
}


static size_t local_strlen (const char * str)
{
  int len = 0;
  char * ch = (char *) str;
  size_t maxLen;
  if (onStack((void *) str) || !malloc_hooked) {
    maxLen = UINT_MAX;
  } else {
    maxLen = CUSTOM_PREFIX(malloc_usable_size) ((void *) str);
  }
  const char *p = ch;
  /* Loop over the data in s.  */
  while (*p != '\0')
    p++;
  len = (size_t)(p - ch);
  if (len > maxLen) {
    len = maxLen;
  }
  return len;
}


static char * local_strcat (char * dest, const char * src) 
{
  size_t d = local_strlen (dest);
  char * dptr = dest + d;
  size_t s = local_strlen (src);
  char * sptr = (char *) src;
  for (int i = 0; i <= s; i++) {
    *dptr++ = *sptr++;
  }
  return dest;
}

static char * local_strncat (char * dest, const char * src, size_t sz)
{
  size_t d = local_strlen (dest);
  char * dptr = dest + d;
  size_t s = local_strlen (src);
  char * sptr = (char *) src;
  for (int i = 0; i <= s && i < sz; i++) {
    *dptr++ = *sptr++;
  }
  // Add a trailing nul.
  *dptr = '\0';
  return dest;
}

static char * local_strncpy (char * dest, const char * src, size_t n)
{
  char * sptr = (char *) src;
  char * dptr = dest;
  int destLength;
  if (!malloc_hooked) {
    destLength = n;
  } else {
    destLength = CUSTOM_PREFIX(malloc_usable_size) (dest) - 1;
  }
  int lengthToCopy = (n < destLength) ? n : destLength;
  while (lengthToCopy && (*dptr++ = *sptr++)) {
    lengthToCopy--;
  }
  *dptr = '\0';
  return dest;
}


static char * local_strcpy (char * dest, const char * src) 
{
  return local_strncpy (dest, src, INT_MAX);
}

static void * local_strdup (const char * s) {
  size_t len = local_strlen((char *) s);
  char * n = (char *) malloc (len + 1);
  if (n) {
    local_strncpy (n, s, len);
  }
  return n;
}

static void * local_memcpy (void * dest, const void * src, size_t n)
{
  char * dptr = (char *) dest;
  char * sptr = (char *) src;
  for (int i = 0; i < n; i++) {
    dptr[i] = sptr[i];
  }
  return dest;
}

static void * local_memset (void * dest, int val, size_t n)
{
  char * dptr = (char *) dest;
  for (int i = 0; i < n; i++) {
    dptr[i] = val;
  }
  return dest;
}

extern "C" {

#if 0
  size_t fread (void * ptr, size_t size, size_t nmemb, FILE * stream)
  {
    if (!malloc_hooked) {
      return (*my_fread_fn)(ptr, size, nmemb, stream);
    }
    size_t sz = CUSTOM_PREFIX(malloc_usable_size) (ptr);
    if (sz == -1) {
      return (*my_fread_fn)(ptr, size, nmemb, stream);
    } else {
      size_t total = size * nmemb;
      size_t s = (total < sz) ? total : sz;
      if (sz < total) {
	// Overflow.
	//fprintf (stderr, "Overflow detected in fread!\n");
      }
      return (*my_fread_fn)(ptr, 1, s, stream);
    }
  }
#endif

  void * memcpy (void * dest, const void * src, size_t n) {
    size_t sz;
    if (onStack(dest) || !malloc_hooked) {
      sz = n;
    } else {
      sz = CUSTOM_PREFIX(malloc_usable_size) (dest);
      if (sz == -1) {
	sz = n;
      } else {
	size_t s = (n < sz) ? n : sz;
	if (sz < n) {
#if 0
	  // Overflow.
	  fprintf (stderr, "Overflow detected in memcpy: dest (%x) size = %d, n = %d\n", dest, sz, n);
#endif
	}
	sz = s;
      }
    }
    return local_memcpy (dest, src, sz);
  }

  void * memset (void * dest, int val, size_t n) {
    size_t sz;
    if (onStack(dest) || !malloc_hooked) {
      sz = n;
    } else {
      sz = CUSTOM_PREFIX(malloc_usable_size) (dest);
      if (sz == -1) {
	sz = n;
      } else {
	size_t s = (n < sz) ? n : sz;
	if (sz < n) {
#if 1
	  // Overflow.
	  fprintf (stderr, "Overflow detected in memset: dest (%x) size = %d, n = %d\n", dest, sz, n);
#endif
	}
	sz = s;
      }
    }
    return local_memset (dest, val, sz);
  }

#if 0
  // Crap. something going on here with stack pushing of str.
  int sprintf (char * str, const char * format, ...) {
    va_list ap;
    va_start (ap, format);
    if (onStack(str) || !malloc_hooked) {
      int r = vsprintf (str, format, ap);
      va_end (ap);
      return r;
    }
    else {
      int sz = CUSTOM_PREFIX(malloc_usable_size) (str);
      if (sz < 0) {
	// Not from here.
	int r = vsprintf (str, format, ap);
	va_end (ap);
	return r;
      } else {
	int r = vsnprintf (str, sz, format, ap);
	va_end (ap);
	return r;
      }
    }
  }
#endif

  int snprintf (char * str, size_t n, const char * format, ...) {
    va_list ap;
    va_start (ap, format);
    if (onStack(str) || !malloc_hooked) {
      int r = vsnprintf (str, n, format, ap);
      va_end (ap);
      return r;
    }
    size_t sz = CUSTOM_PREFIX(malloc_usable_size) (str);
    if (sz == -1) {
      int r = vsnprintf (str, n, format, ap);
      va_end (ap);
      return r;
    }
    size_t s = (n < sz) ? n : sz;
    int r = vsnprintf (str, s, format, ap);
    va_end (ap);
    return r;
  }

  char * gets (char * s) {
    size_t sz;
    if (onStack(s)) {
      sz = INT_MAX;
    } else {
      sz = CUSTOM_PREFIX(malloc_usable_size)(s);
    }
    return fgets (s, sz, stdin);
  }

#if 1
  char * strcpy (char * dest, const char * src) {
    if (onStack(dest) || !malloc_hooked) {
      return local_strcpy (dest, src);
    }
    size_t sz = CUSTOM_PREFIX(malloc_usable_size) (dest);
    if (sz == -1) {
      return local_strcpy (dest, src);
    } else {
      // DieFast: check for an error (would we have overflowed?)
#if 0
      if (sz < strlen(src) + 1) {
	fprintf (stderr, "Overflow detected in strcpy! dest (%x) size = %d, src (%x) length = %d (%s)\n", dest, sz, src, strlen(src), src); 
      }
#endif
      return local_strncpy (dest, src, sz);
    }
  }
#endif

  char * strncpy (char * dest, const char * src, size_t n) {
    if (onStack(dest) || !malloc_hooked) {
      return local_strncpy (dest, src, n);
    }
    size_t sz = CUSTOM_PREFIX(malloc_usable_size) (dest);
    if (sz == -1) {
      return local_strncpy (dest, src, n);
    } else {
      size_t s = (n < sz) ? n : sz;
      // DieFast: check for an error (would we have overflowed?)
      if (n > s) {
	// fprintf (stderr, "Overflow detected in strncpy!\n");
      }
      return local_strncpy (dest, src, s);
    }
  }

  char * strcat (char * dest, const char * src) {
    if (onStack(dest) || !malloc_hooked) {
      return local_strncat (dest, src, INT_MAX);
    }
    size_t sz = CUSTOM_PREFIX(malloc_usable_size) (dest + strlen(dest));
    if (sz == -1) {
      return local_strncat (dest, src, INT_MAX);
    } else {
      // DieFast: check for an error (would we have overflowed?)
      //if (strlen(src) + strlen(dest) > sz) {
      //	fprintf (stderr, "Overflow detected in strcat!\n");
      //}
      return local_strncat (dest, src, sz);
    }
  }

  char * strncat (char * dest, const char * src, size_t n) {
    if (onStack(dest) || !malloc_hooked) {
      return local_strncat (dest, src, n);
    }      
    size_t sz = CUSTOM_PREFIX(malloc_usable_size) (dest + strlen(dest));
    if (sz == -1) {
      return local_strncat (dest, src, n);
    } else {
      size_t s = (n < (sz-1)) ? n : (sz-1);
#if 0
      // DieFast: check for an error (would we have overflowed?)
      if (strlen(dest) + s > sz) {
	// fprintf (stderr, "Overflow detected in strncat!\n");
      }
#endif
      return local_strncat (dest, src, s);
    }
  }


}

