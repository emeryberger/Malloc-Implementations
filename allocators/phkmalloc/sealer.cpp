#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#undef strcpy

extern "C" {
  typedef int snprintf_function_type (char *, size_t, const char *, ...);
  typedef int sprintf_function_type (char *, const char *, ...);
  typedef char * strcat_function_type (char *, const char *);
  typedef char * strncat_function_type (char *, const char *, size_t);
  typedef char * strcpy_function_type (char *, const char *);
  typedef char * strncpy_function_type (char *, const char *, size_t);
  typedef char * fgets_function_type (char *, int, FILE *);
  typedef void * memcpy_function_type (void *, const void *, size_t);
  typedef size_t fread_function_type (void *, size_t, size_t, FILE *);
  size_t phkmalloc_usable_size (void * ptr);
}

static sprintf_function_type * my_sprintf_fn;
static snprintf_function_type * my_snprintf_fn;
static strcat_function_type * my_strcat_fn;
static strncat_function_type * my_strncat_fn;
static strcpy_function_type * my_strcpy_fn;
static strncpy_function_type * my_strncpy_fn;
static fgets_function_type * my_fgets_fn;
static memcpy_function_type * my_memcpy_fn;
static fread_function_type * my_fread_fn;


static bool initialized = false;

class Initialize {
public:
  Initialize (void)
  {
    if (!initialized) {
      initialize();
      initialized = true;
    }
  }
private:
  static void initialize (void) {
    my_sprintf_fn = (sprintf_function_type *) dlsym (RTLD_NEXT, "sprintf");
    my_snprintf_fn = (snprintf_function_type *) dlsym (RTLD_NEXT, "snprintf");
    my_strcat_fn = (strcat_function_type *) dlsym (RTLD_NEXT, "strcat");
    my_strncat_fn = (strncat_function_type *) dlsym (RTLD_NEXT, "strncat");
    my_strcpy_fn = (strcpy_function_type *) dlsym (RTLD_NEXT, "strcpy");
    my_strncpy_fn = (strncpy_function_type *) dlsym (RTLD_NEXT, "strncpy");
    my_fgets_fn = (fgets_function_type *) dlsym (RTLD_NEXT, "fgets");
    my_memcpy_fn = (memcpy_function_type *) dlsym (RTLD_NEXT, "memcpy");
    my_fread_fn = (fread_function_type *) dlsym (RTLD_NEXT, "fread");
  }
};



extern "C" {

  size_t fread (void * ptr, size_t size, size_t nmemb, FILE * stream)
  {
    Initialize me;
    size_t sz = phkmalloc_usable_size (ptr);
    if (sz == -1) {
      return (*my_fread_fn)(ptr, size, nmemb, stream);
    } else {
      size_t total = size * nmemb;
      size_t s = (total < sz) ? total : sz;
      return (*my_fread_fn)(ptr, size, s / size, stream);
    }
  }

  void * memcpy (void * dest, const void * src, size_t n) {
    Initialize me;
    size_t sz = phkmalloc_usable_size (dest);
    size_t s = (n < sz) ? n : sz;
    return (*my_memcpy_fn) (dest, src, s);
  }

  int sprintf (char * str, const char * format, ...) {
    Initialize me;
    va_list ap;
    va_start (ap, format);
    size_t sz = phkmalloc_usable_size (str);
    if (sz == -1) {
      int r = vsprintf (str, format, ap);
      va_end (ap);
      return r;
    } else {
      int r = vsnprintf (str, sz, format, ap);
      va_end (ap);
      return r;
    }
  }

  int snprintf (char * str, size_t n, const char * format, ...) {
    Initialize me;
    va_list ap;
    va_start (ap, format);
    size_t sz = phkmalloc_usable_size (str);
    size_t s = (n < sz) ? n : sz;
    int r = vsnprintf (str, s, format, ap);
    va_end (ap);
    return r;
  }

  char * fgets (char * s, int size, FILE * stream) {
    Initialize me;
    size_t sz = phkmalloc_usable_size (s);
    if (sz == -1) {
      return (*my_fgets_fn) (s, size, stream);
    } else {
      size_t min = (size < sz) ? size : sz;
      return (*my_fgets_fn) (s, min, stream);
    }
  }

  char * gets (char * s) {
    Initialize me;
    return (*my_fgets_fn) (s, phkmalloc_usable_size(s), stdin);
  }

  char * strcpy (char * dest, const char * src) {
    Initialize me;
    size_t sz = phkmalloc_usable_size (dest);
    if (sz == -1) {
      return (*my_strcpy_fn) (dest, src);
    } else {
      size_t s = strlen(src) + 1;
      size_t n = (s < sz) ? s : sz;
      return (*my_strncpy_fn) (dest, src, n);
    }
  }

  char * strncpy (char * dest, const char * src, size_t n) {
    Initialize me;
    size_t sz = phkmalloc_usable_size (dest);
    size_t s = (n < sz) ? n : sz;
    return (*my_strncpy_fn) (dest, src, s);
  }

  char * strcat (char * dest, const char * src) {
    Initialize me;
    size_t sz = phkmalloc_usable_size (dest);
    if (sz == -1) {
      return (*my_strcat_fn) (dest, src);
    } else {
      return (*my_strncat_fn) (dest, src, sz);
    }
  }

  char * strncat (char * dest, const char * src, size_t n) {
    Initialize me;
    size_t sz = phkmalloc_usable_size (dest);
    size_t s = (n < sz) ? n : sz;
    return (*my_strncat_fn) (dest, src, s);
  }

}
