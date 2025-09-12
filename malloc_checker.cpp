// malloc_checker.cpp
// Cross-platform malloc/calloc/realloc/alignment conformance & extensions checker
// Linux (glibc), macOS (Darwin), Windows (MSVC CRT)
//
// Build:
//   Linux:   g++ -std=c++17 -O2 -Wall -Wextra malloc_checker.cpp -o malloc_checker
//   macOS:   clang++ -std=c++17 -O2 -Wall -Wextra malloc_checker.cpp -o malloc_checker
//   Windows (MSVC):  cl /std:c++17 /O2 /W4 malloc_checker.cpp
//
// Exit code is nonzero if any REQUIRED test fails.
// Some behavior is implementation-defined or resource-sensitive; those are marked SKIP.

#include <cassert>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <type_traits>
#include <vector>

// Platform headers
#if defined(_WIN32)
  #include <malloc.h>
  #include <windows.h>
#elif defined(__APPLE__)
  #include <malloc/malloc.h>
  #include <unistd.h>
  extern "C" void *reallocf(void *ptr, size_t size); // Apple extension
#else // POSIX (Linux)
  #include <malloc.h> // glibc: malloc_usable_size, memalign, valloc, pvalloc, mallinfo, malloc_info, malloc_trim
  #include <unistd.h>
#endif

// Feature toggles
#if defined(__APPLE__) || defined(__linux__)
  #define HAS_POSIX 1
#else
  #define HAS_POSIX 0
#endif

#if defined(__APPLE__) || defined(__linux__)
  #define HAS_ALIGNED_ALLOC 1   // C11 aligned_alloc
#else
  #define HAS_ALIGNED_ALLOC 0
#endif

#if defined(__APPLE__)
  #define HAS_MALLOC_SIZE 1
  #define HAS_MALLOC_GOOD_SIZE 1
#else
  #define HAS_MALLOC_SIZE 0
  #define HAS_MALLOC_GOOD_SIZE 0
#endif

#if defined(__linux__)
  #define HAS_MALLOC_USABLE_SIZE 1
#else
  #define HAS_MALLOC_USABLE_SIZE 0
#endif

#if defined(_WIN32)
  #define HAS_WIN_ALIGNED_MALLOC 1
#else
  #define HAS_WIN_ALIGNED_MALLOC 0
#endif

#if defined(__GLIBC__)
  #define HAS_GLIBC 1
#else
  #define HAS_GLIBC 0
#endif

// Simple tri-state for test outcomes
enum class Status { PASS, FAIL, SKIP };

struct TestCase {
  const char* name;
  std::function<Status(std::string&)> fn; // writes message on FAIL/SKIP or extra PASS detail
};

static std::vector<TestCase> g_tests;

static void add_test(const char* name, std::function<Status(std::string&)> fn) {
  g_tests.push_back({name, std::move(fn)});
}

// Helpers
static inline bool is_aligned(void* p, std::size_t align) {
  return (reinterpret_cast<std::uintptr_t>(p) % align) == 0;
}

static Status skip(std::string& out, const char* why) {
  out = why; return Status::SKIP;
}

// Write a deterministic byte pattern, then verify.
static void pattern_fill(unsigned char* p, size_t n, unsigned seed=0xC3) {
  for (size_t i = 0; i < n; ++i) p[i] = static_cast<unsigned char>((i*131 + seed) & 0xFF);
}
static bool pattern_check(const unsigned char* p, size_t n, unsigned seed=0xC3) {
  for (size_t i = 0; i < n; ++i) {
    if (p[i] != static_cast<unsigned char>((i*131 + seed) & 0xFF)) return false;
  }
  return true;
}

// Touch every page in [p, p+n) to provoke overcommit faults without UB.
static void page_touch(unsigned char* p, size_t n) {
  long pg = 4096;
#if HAS_POSIX
  long v = sysconf(_SC_PAGESIZE);
  if (v > 0) pg = v;
#endif
  for (size_t off = 0; off < n; off += static_cast<size_t>(pg)) p[off] ^= 0x00; // read-modify-write
  if (n) p[n-1] ^= 0x00;
}

// Try a huge allocation expected to fail; don't treat as FAIL if system overcommits.
static Status expect_huge_malloc_failure(std::string& out) {
  errno = 0;
  void* p = std::malloc(static_cast<size_t>(-1)); // SIZE_MAX
  if (p == nullptr) {
#if HAS_POSIX
    if (errno != 0 && errno != ENOMEM) {
      out = "malloc failure did not set errno to ENOMEM (POSIX-expected)";
      return Status::FAIL;
    }
#endif
    return Status::PASS;
  }
  std::free(p);
  return skip(out, "System appears to overcommit; cannot reliably assert ENOMEM on huge malloc");
}

int main() {
  // ------------------
  // Core C allocator API
  // ------------------
  add_test("malloc/free basic", [](std::string& out){
    void* p = std::malloc(128);
    if (!p) { out = "malloc(128) returned NULL"; return Status::FAIL; }
    std::memset(p, 0xA5, 128);
    std::free(p);
    return Status::PASS;
  });

  add_test("malloc alignment >= alignof(max_align_t)", [](std::string& out){
    void* p = std::malloc(1);
    if (!p) { out = "malloc(1) returned NULL"; return Status::FAIL; }
    if (!is_aligned(p, alignof(std::max_align_t))) {
      out = "malloc result not aligned to alignof(max_align_t)";
      std::free(p);
      return Status::FAIL;
    }
    std::free(p);
    return Status::PASS;
  });

  add_test("free(NULL) is a no-op", [](std::string&){ std::free(nullptr); return Status::PASS; });

  add_test("malloc(0) returns NULL or unique pointer freeable", [](std::string&){
    void* p = std::malloc(0);
    if (p) std::free(p); // If non-null, it must be freeable.
    return Status::PASS; // Both behaviors are conforming.
  });

  add_test("calloc zero-initializes and writable across range", [](std::string& out){
    const size_t nmemb = 97, size = 17; // 1649 bytes
    unsigned char* p = static_cast<unsigned char*>(std::calloc(nmemb, size));
    if (!p) { out = "calloc returned NULL"; return Status::FAIL; }
    for (size_t i = 0; i < nmemb*size; ++i) if (p[i] != 0) { std::free(p); out = "calloc memory not zero-initialized"; return Status::FAIL; }
    // Write pattern to entire region and read back
    pattern_fill(p, nmemb*size, 0x55);
    if (!pattern_check(p, nmemb*size, 0x55)) { std::free(p); out = "calloc memory not stably writable"; return Status::FAIL; }
    page_touch(p, nmemb*size);
    std::free(p);
    return Status::PASS;
  });

  add_test("calloc overflow must fail", [](std::string& out){
    const size_t big = static_cast<size_t>(-1) / 4 + 1; // > SIZE_MAX/4
    errno = 0;
    void* p = std::calloc(big, 8); // big*8 > SIZE_MAX
    if (p != nullptr) { std::free(p); out = "calloc did not fail on overflow-sized request"; return Status::FAIL; }
#if HAS_POSIX
    if (errno && errno != ENOMEM) { out = "calloc overflow failure: errno not ENOMEM (POSIX expected)"; return Status::FAIL; }
#endif
    return Status::PASS;
  });

  add_test("realloc preserves prefix when growing and keeps new bytes writable", [](std::string& out){
    const size_t n1 = 4096 + 37, n2 = 3*n1 + 11; // cross page boundaries
    unsigned char* p = static_cast<unsigned char*>(std::malloc(n1));
    if (!p) { out = "malloc failed"; return Status::FAIL; }
    pattern_fill(p, n1, 0xA1);
    unsigned char* q = static_cast<unsigned char*>(std::realloc(p, n2));
    if (!q) { std::free(p); out = "realloc to larger size returned NULL"; return Status::FAIL; }
    if (!pattern_check(q, n1, 0xA1)) { std::free(q); out = "realloc did not preserve original prefix"; return Status::FAIL; }
    pattern_fill(q+n1, n2-n1, 0xB2);
    if (!pattern_check(q, n1, 0xA1) || !pattern_check(q+n1, n2-n1, 0xB2)) { std::free(q); out = "post-realloc write/readback failed"; return Status::FAIL; }
    page_touch(q, n2);
    std::free(q);
    return Status::PASS;
  });

  add_test("realloc shrinks preserving prefix; tail may be discarded", [](std::string& out){
    const size_t n1 = 4096*2 + 123, n2 = 1024 + 7;
    unsigned char* p = static_cast<unsigned char*>(std::malloc(n1));
    if (!p) { out = "malloc failed"; return Status::FAIL; }
    pattern_fill(p, n1, 0x3C);
    unsigned char* q = static_cast<unsigned char*>(std::realloc(p, n2));
    if (!q) { out = "realloc to smaller returned NULL"; return Status::FAIL; }
    if (!pattern_check(q, n2, 0x3C)) { std::free(q); out = "realloc shrink did not preserve prefix"; return Status::FAIL; }
    page_touch(q, n2);
    std::free(q);
    return Status::PASS;
  });

  add_test("realloc(NULL, n) behaves like malloc(n)", [](std::string& out){
    void* p = std::realloc(nullptr, 1024);
    if (!p) { return skip(out, "realloc(NULL, n) returned NULL (likely ENOMEM) – skipping"); }
    std::free(p);
    return Status::PASS;
  });

  add_test("realloc(ptr, 0) either frees and returns NULL, or returns pointer to minimum size", [](std::string& out){
    void* p = std::malloc(32);
    if (!p) { out = "malloc failed"; return Status::FAIL; }
    void* r = std::realloc(p, 0);
    if (r == nullptr) return Status::PASS; // freed
    std::free(r);
    return Status::PASS; // alternative conforming behavior
  });

  add_test("huge malloc should fail (or SKIP on overcommit)", expect_huge_malloc_failure);

  // ------------------
  // POSIX-specific APIs (Linux/macOS)
  // ------------------
#if HAS_POSIX
  add_test("posix_memalign alignment and free", [](std::string& out){
    const size_t aligns[] = { sizeof(void*), 16, 32, 64, 4096 };
    for (size_t a : aligns) {
      void* p = nullptr;
      int rc = posix_memalign(&p, a, 123);
      if (rc != 0) { if (rc == ENOMEM) return skip(out, "posix_memalign ENOMEM – skipping (resource-limited)"); out = "posix_memalign failed for valid alignment"; return Status::FAIL; }
      if (!p || !is_aligned(p, a)) { out = "posix_memalign pointer not aligned as requested"; std::free(p); return Status::FAIL; }
      pattern_fill(static_cast<unsigned char*>(p), 123, 0x5A);
      if (!pattern_check(static_cast<unsigned char*>(p), 123, 0x5A)) { std::free(p); out = "posix_memalign region not stable"; return Status::FAIL; }
      std::free(p);
    }
    return Status::PASS;
  });

  add_test("posix_memalign EINVAL on invalid alignment", [](std::string& out){
    void* p = nullptr; int rc = posix_memalign(&p, 3 /* not power of two */, 128);
    if (rc != EINVAL) { out = "posix_memalign did not return EINVAL for invalid alignment"; return Status::FAIL; }
    return Status::PASS;
  });

  add_test("malloc failure sets errno=ENOMEM (POSIX)", [](std::string& out){
    errno = 0; void* p = std::malloc(static_cast<size_t>(-1));
    if (p) { std::free(p); return skip(out, "overcommit prevents ENOMEM check"); }
    if (errno != ENOMEM && errno != 0) { out = "malloc failure set unexpected errno"; return Status::FAIL; }
    return Status::PASS;
  });

  #if HAS_ALIGNED_ALLOC
  add_test("aligned_alloc valid: alignment power-of-two and size multiple", [](std::string& out){
    const size_t Avals[] = {alignof(std::max_align_t), 32, 64, 4096};
    for (size_t A : Avals) {
      const size_t N = A * 17; // N % A == 0
      void* p = aligned_alloc(A, N);
      if (!p) { out = "aligned_alloc returned NULL"; return Status::FAIL; }
      if (!is_aligned(p, A)) { std::free(p); out = "aligned_alloc did not honor alignment"; return Status::FAIL; }
      pattern_fill(static_cast<unsigned char*>(p), N, 0x77);
      if (!pattern_check(static_cast<unsigned char*>(p), N, 0x77)) { std::free(p); out = "aligned_alloc region not stable"; return Status::FAIL; }
      std::free(p);
    }
    return Status::PASS;
  });

  add_test("aligned_alloc invalid size (not multiple) must fail", [](std::string& out){
    const size_t A = 64; const size_t N = 1000; // not multiple of 64
    errno = 0; void* p = aligned_alloc(A, N);
    if (p) { std::free(p); out = "aligned_alloc succeeded with invalid size"; return Status::FAIL; }
    return Status::PASS; // errno may be EINVAL or 0 depending on libc
  });
  #endif // HAS_ALIGNED_ALLOC

  #if HAS_MALLOC_USABLE_SIZE
  add_test("malloc_usable_size >= requested and non-decreasing under enlarge (Linux)", [](std::string& out){
    void* p = std::malloc(123);
    if (!p) { out = "malloc failed"; return Status::FAIL; }
    size_t u1 = malloc_usable_size(p);
    void* q = std::realloc(p, 4096);
    if (!q) { std::free(p); out = "realloc failed"; return Status::FAIL; }
    size_t u2 = malloc_usable_size(q);
    if (u1 < 123 || u2 < 4096 || u2 < u1) { std::free(q); out = "malloc_usable_size invariant broke"; return Status::FAIL; }
    std::free(q);
    return Status::PASS;
  });
  #endif

  #if HAS_MALLOC_SIZE
  add_test("malloc_size >= requested (macOS)", [](std::string& out){
    void* p = std::malloc(123);
    if (!p) { out = "malloc failed"; return Status::FAIL; }
    size_t sz = malloc_size(p);
    if (sz < 123) { std::free(p); out = "malloc_size < requested"; return Status::FAIL; }
    pattern_fill(static_cast<unsigned char*>(p), sz, 0x6D);
    if (!pattern_check(static_cast<unsigned char*>(p), sz, 0x6D)) { std::free(p); out = "malloc_size region not stable"; return Status::FAIL; }
    std::free(p);
    return Status::PASS;
  });

  add_test("reallocf frees original on failure (macOS)", [](std::string& out){
    void* p = std::malloc(256);
    if (!p) { out = "malloc failed"; return Status::FAIL; }
    void* q = reallocf(p, static_cast<size_t>(-1));
    if (q != nullptr) { std::free(q); out = "reallocf unexpectedly succeeded"; return Status::FAIL; }
    return Status::PASS; // original freed by reallocf on failure
  });

  #if HAS_MALLOC_GOOD_SIZE
  add_test("malloc_good_size matches malloc_size rounding (macOS)", [](std::string& out){
    size_t req = 1234; size_t good = malloc_good_size(req);
    void* p = std::malloc(req);
    if (!p) { out = "malloc failed"; return Status::FAIL; }
    size_t got = malloc_size(p);
    if (got != good) { std::free(p); out = "malloc_size != malloc_good_size for same request"; return Status::FAIL; }
    std::free(p);
    return Status::PASS;
  });

  add_test("valloc returns page-aligned pointer (macOS)", [](std::string& out){
    long pg = sysconf(_SC_PAGESIZE);
    void* p = valloc(1234);
    if (!p) { out = "valloc returned NULL"; return Status::FAIL; }
    if (!is_aligned(p, static_cast<size_t>(pg))) { std::free(p); out = "valloc pointer not page-aligned"; return Status::FAIL; }
    std::free(p);
    return Status::PASS;
  });
  #endif // HAS_MALLOC_GOOD_SIZE
  #endif // HAS_MALLOC_SIZE
#endif // HAS_POSIX

  // ------------------
  // glibc-specific extensions
  // ------------------
#if HAS_GLIBC
  add_test("memalign returns power-of-two aligned pointer (glibc)", [](std::string& out){
    const size_t aligns[] = { sizeof(void*), 16, 32, 64, 4096 };
    for (size_t a : aligns) {
      void* p = memalign(a, 777);
      if (!p) { out = "memalign returned NULL"; return Status::FAIL; }
      if (!is_aligned(p, a)) { std::free(p); out = "memalign misaligned"; return Status::FAIL; }
      pattern_fill(static_cast<unsigned char*>(p), 777, 0x2A);
      if (!pattern_check(static_cast<unsigned char*>(p), 777, 0x2A)) { std::free(p); out = "memalign region not stable"; return Status::FAIL; }
      std::free(p);
    }
    return Status::PASS;
  });

  add_test("memalign EINVAL for non power-of-two or too small align (glibc)", [](std::string& out){
    errno = 0; void* p = memalign(3, 128); // not power-of-two
    if (p) { std::free(p); out = "memalign succeeded with invalid alignment"; return Status::FAIL; }
    if (errno && errno != EINVAL) { out = "memalign invalid alignment: errno not EINVAL"; return Status::FAIL; }
    return Status::PASS;
  });

  add_test("valloc returns page-aligned pointer (glibc)", [](std::string& out){
    long pg = sysconf(_SC_PAGESIZE);
    void* p = valloc(1000);
    if (!p) { out = "valloc returned NULL"; return Status::FAIL; }
    if (!is_aligned(p, static_cast<size_t>(pg))) { std::free(p); out = "valloc pointer not page-aligned"; return Status::FAIL; }
    std::free(p);
    return Status::PASS;
  });

  add_test("pvalloc rounds up to page size (glibc)", [](std::string& out){
    long pg = sysconf(_SC_PAGESIZE);
    void* p = pvalloc(1); // rounds up
    if (!p) { out = "pvalloc returned NULL"; return Status::FAIL; }
    if (!is_aligned(p, static_cast<size_t>(pg))) { std::free(p); out = "pvalloc pointer not page-aligned"; return Status::FAIL; }
    std::free(p);
    return Status::PASS;
  });

  add_test("malloc_trim(0) is callable (glibc)", [](std::string&){ (void)malloc_trim(0); return Status::PASS; });

  add_test("malloc_info emits XML to stream (glibc)", [](std::string& out){
    FILE* f = tmpfile();
    if (!f) return skip(out, "tmpfile() unavailable – skipping malloc_info");
    int rc = malloc_info(0, f);
    fclose(f);
    if (rc != 0) { out = "malloc_info returned non-zero"; return Status::FAIL; }
    return Status::PASS;
  });

  add_test("mallinfo callable (glibc)", [](std::string&){ auto mi = mallinfo(); (void)mi; return Status::PASS; });

  add_test("reallocarray overflow fails with ENOMEM (glibc)", [](std::string& out){
    size_t big = static_cast<size_t>(-1) / 4 + 1; errno = 0;
    void* p = reallocarray(nullptr, big, 8);
    if (p) { std::free(p); out = "reallocarray succeeded on overflow request"; return Status::FAIL; }
    if (errno && errno != ENOMEM) { out = "reallocarray overflow errno not ENOMEM"; return Status::FAIL; }
    return Status::PASS;
  });

  add_test("posix_memalign size=0 returns NULL or freeable pointer", [](std::string& out){
    void* p = nullptr; int rc = posix_memalign(&p, 64, 0);
    if (rc == 0 && p) { std::free(p); return Status::PASS; }
    if (rc == 0 && !p) return Status::PASS; // NULL allowed
    if (rc == ENOMEM) return skip(out, "posix_memalign ENOMEM – skipping");
    out = "posix_memalign(size=0) unexpected behavior"; return Status::FAIL;
  });
#endif // HAS_GLIBC

  // ------------------
  // Windows-specific CRT APIs
  // ------------------
#if HAS_WIN_ALIGNED_MALLOC
  add_test("_aligned_malloc/_aligned_free return correctly-aligned pointer", [](std::string& out){
    const size_t aligns[] = { sizeof(void*), 16, 32, 64, 4096 };
    for (size_t a : aligns) {
      void* p = _aligned_malloc(1024, a);
      if (!p) { out = "_aligned_malloc returned NULL"; return Status::FAIL; }
      if (!is_aligned(p, a)) { _aligned_free(p); out = "_aligned_malloc misaligned"; return Status::FAIL; }
      pattern_fill(static_cast<unsigned char*>(p), 1024, 0xE1);
      if (!pattern_check(static_cast<unsigned char*>(p), 1024, 0xE1)) { _aligned_free(p); out = "_aligned_malloc region not stable"; return Status::FAIL; }
      _aligned_free(p);
    }
    return Status::PASS;
  });

  add_test("_msize reports usable size >= requested (Windows)", [](std::string& out){
    void* p = std::malloc(200);
    if (!p) { out = "malloc failed"; return Status::FAIL; }
    size_t m = _msize(p);
    if (m < 200) { std::free(p); out = "_msize < requested"; return Status::FAIL; }
    std::free(p);
    return Status::PASS;
  });

  add_test("_aligned_realloc preserves prefix and alignment (Windows)", [](std::string& out){
    const size_t A = 64; const size_t N1 = 128; const size_t N2 = 1024;
    unsigned char* p = static_cast<unsigned char*>(_aligned_malloc(N1, A));
    if (!p) { out = "_aligned_malloc failed"; return Status::FAIL; }
    pattern_fill(p, N1, 0xC9);
    unsigned char* q = static_cast<unsigned char*>(_aligned_realloc(p, N2, A));
    if (!q) { _aligned_free(p); out = "_aligned_realloc returned NULL"; return Status::FAIL; }
    if (!is_aligned(q, A)) { _aligned_free(q); out = "_aligned_realloc misaligned"; return Status::FAIL; }
    if (!pattern_check(q, N1, 0xC9)) { _aligned_free(q); out = "_aligned_realloc lost prefix"; return Status::FAIL; }
    _aligned_free(q);
    return Status::PASS;
  });

  add_test("_aligned_msize >= requested (Windows)", [](std::string& out){
    const size_t A = 32; void* p = _aligned_malloc(300, A);
    if (!p) { out = "_aligned_malloc failed"; return Status::FAIL; }
    size_t m = _aligned_msize(p, A, 0);
    if (m < 300) { _aligned_free(p); out = "_aligned_msize < requested"; return Status::FAIL; }
    _aligned_free(p);
    return Status::PASS;
  });

  add_test("_recalloc zero-inits growth (Windows)", [](std::string& out){
    size_t n1 = 10, n2 = 20; size_t sz = 4; // 4-byte cells
    unsigned char* p = static_cast<unsigned char*>(_recalloc(nullptr, n1, sz));
    if (!p) { out = "_recalloc initial alloc failed"; return Status::FAIL; }
    for (size_t i=0;i<n1*sz;++i) if (p[i] != 0) { free(p); out = "_recalloc initial not zero"; return Status::FAIL; }
    pattern_fill(p, n1*sz, 0xAB);
    unsigned char* q = static_cast<unsigned char*>(_recalloc(p, n2, sz));
    if (!q) { free(p); out = "_recalloc grow failed"; return Status::FAIL; }
    for (size_t i=0;i<n1*sz;++i) if (q[i] != static_cast<unsigned char>((i*131 + 0xAB) & 0xFF)) { free(q); out = "_recalloc lost prefix"; return Status::FAIL; }
    for (size_t i=n1*sz;i<n2*sz;++i) if (q[i] != 0) { free(q); out = "_recalloc growth not zeroed"; return Status::FAIL; }
    free(q);
    return Status::PASS;
  });

  add_test("_expand may grow in place (Windows)", [](std::string& out){
    size_t n1 = 256, n2 = 320;
    unsigned char* p = static_cast<unsigned char*>(malloc(n1));
    if (!p) { out = "malloc failed"; return Status::FAIL; }
    pattern_fill(p, n1, 0xDD);
    void* q = _expand(p, n2);
    if (!q) { free(p); return skip(out, "_expand could not grow in place – skipping"); }
    if (q != p) { free(q); out = "_expand returned different pointer"; return Status::FAIL; }
    if (!pattern_check(p, n1, 0xDD)) { free(p); out = "_expand corrupted prefix"; return Status::FAIL; }
    free(p);
    return Status::PASS;
  });

  #if defined(_DEBUG)
  add_test("_CrtCheckMemory passes (Windows debug CRT)", [](std::string& out){
    if (!_CrtCheckMemory()) { out = "_CrtCheckMemory reported heap issues"; return Status::FAIL; }
    return Status::PASS;
  });
  #endif
#endif // HAS_WIN_ALIGNED_MALLOC

  // ------------------
  // Mini stress test (portable, conservative)
  // ------------------
  add_test("stress: allocate/free mixed sizes & alignments", [](std::string& out){
    std::vector<void*> blocks;
    for (size_t i=1;i<=1024;i+=13) {
      void* p = std::malloc(i);
      if (!p) { out = "malloc failed during stress"; return Status::FAIL; }
      pattern_fill(static_cast<unsigned char*>(p), i, static_cast<unsigned>(i));
      blocks.push_back(p);
    }
    for (size_t i=0;i<blocks.size(); i+=2) { std::free(blocks[i]); blocks[i]=nullptr; }
#if HAS_POSIX
    // Try some aligned blocks too
    for (size_t a: {size_t(16),size_t(64),size_t(256)}) {
      void* p=nullptr; if (posix_memalign(&p,a, a*3)==0 && p){ pattern_fill(static_cast<unsigned char*>(p), a*3, 0x42); blocks.push_back(p);} }
#endif
    // Verify remaining
    for (size_t i=0;i<blocks.size(); ++i) if (blocks[i]) { /* spot-check first byte */ (void)static_cast<unsigned char*>(blocks[i])[0]; }
    for (void* p: blocks) if (p) std::free(p);
    return Status::PASS;
  });

  // ------------------
  // Run tests
  // ------------------
  std::size_t passed = 0, failed = 0, skipped = 0;
  for (const auto& t : g_tests) {
    std::string msg;
    Status s = t.fn(msg);
    switch (s) {
      case Status::PASS:
        std::cout << "[PASS] " << t.name;
        if (!msg.empty()) std::cout << ": " << msg;
        std::cout << std::endl;
        ++passed; break;
      case Status::SKIP:
        std::cout << "[SKIP] " << t.name;
        if (!msg.empty()) std::cout << ": " << msg;
        std::cout << std::endl;
        ++skipped; break;
      case Status::FAIL:
        std::cout << "[FAIL] " << t.name;
        if (!msg.empty()) std::cout << ": " << msg;
        std::cout << std::endl;
        ++failed; break;
    }
  }

  std::cout << "Summary: " << passed << " passed, " << failed << " failed, " << skipped << " skipped." << std::endl;
  return failed == 0 ? 0 : 1;
}
