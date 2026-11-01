/* No glibc backtrace on the 360: crash reports walk the stack in xenon/platform. */
#ifndef XE_EXECINFO_H
#define XE_EXECINFO_H
static __inline int backtrace(void** buffer, int size) { (void)buffer; (void)size; return 0; }
static __inline char** backtrace_symbols(void* const* buffer, int size) { (void)buffer; (void)size; return 0; }
static __inline void backtrace_symbols_fd(void* const* buffer, int size, int fd) { (void)buffer; (void)size; (void)fd; }
#endif
