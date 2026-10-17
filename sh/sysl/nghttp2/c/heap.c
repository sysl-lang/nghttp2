// How many bytes of the C heap are in use, for the tests that prove a peer cannot make a session
// hold more than its header-list limit. It is the one C file in the package and binds nothing of
// nghttp2's: the number has to come from the allocator, which only C can ask. It answers 0 where
// the allocator has no way to say, and the tests that use it skip there.

#include <stddef.h>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif

size_t sysl_nghttp2_heap_in_use(void) {
#if defined(__APPLE__)
  return mstats().bytes_used;
#elif defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
  return mallinfo2().uordblks;
#else
  return 0;
#endif
}
