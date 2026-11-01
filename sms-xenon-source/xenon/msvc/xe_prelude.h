/* Force-included before src/port_compat.h in the Xbox 360 build (VS2010, XDK):
 * the GCC spellings the port's shared headers use, in MSVC terms. Nothing in
 * upstream's files changes for this; see xenon/README.md. */
#ifndef XE_PRELUDE_H
#define XE_PRELUDE_H

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <ppcintrinsics.h>
#ifdef __cplusplus
#include <new> /* placement new: xrewrite.py builds singletons in zeroed memory */
#endif

#ifndef __cplusplus
#define inline __inline /* VS2010's C is C89 */
#endif
#define __attribute__(x)
/* GCC's byte-order macros. Undefined, `#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__` reads
 * 0 == 0 and is true: JASSeqParser's note-command bitfields then took the little-endian
 * layout, while this compiler fills bitfields from the high bit as MWCC does (the intro
 * movie's music ran out of tracks and wrote through a null child). */
#define __ORDER_LITTLE_ENDIAN__ 1234
#define __ORDER_BIG_ENDIAN__ 4321
#define __BYTE_ORDER__ __ORDER_BIG_ENDIAN__
#define __extension__
#define __inline__ __inline
#define __restrict__ __restrict
#define thread_local __declspec(thread) /* C++11; the XDK compiler has the Microsoft spelling */
#define __asm__(x)

/* The XDK compiler has Gekko-named intrinsics of its own; port_compat.h
 * defines functions of these names with the Gekko's exact results. */
#define __frsqrte xe_gekko_frsqrte
#define __fres xe_gekko_fres
#ifdef __cplusplus
#define __typeof__(x) decltype(x)
#define __typeof(x) decltype(x)
/* GCC compound literals, (T){ ... }, rewritten by xenon/tools/xrewrite.py
 * into xe_cl<T>(...): aggregate initialisation through a function. */
#include <utility>
/* Aggregates are initialised from the list; std::pair, not one, through its constructor. */
template <class T> struct xe_make {
	static T run() { T t = {}; return t; }
	template <class A0> static T run(A0 a0) { T t = { a0 }; return t; }
	template <class A0, class A1> static T run(A0 a0, A1 a1) { T t = { a0, a1 }; return t; }
	template <class A0, class A1, class A2> static T run(A0 a0, A1 a1, A2 a2) { T t = { a0, a1, a2 }; return t; }
	template <class A0, class A1, class A2, class A3> static T run(A0 a0, A1 a1, A2 a2, A3 a3) { T t = { a0, a1, a2, a3 }; return t; }
	template <class A0, class A1, class A2, class A3, class A4> static T run(A0 a0, A1 a1, A2 a2, A3 a3, A4 a4) { T t = { a0, a1, a2, a3, a4 }; return t; }
	template <class A0, class A1, class A2, class A3, class A4, class A5> static T run(A0 a0, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5) { T t = { a0, a1, a2, a3, a4, a5 }; return t; }
	template <class A0, class A1, class A2, class A3, class A4, class A5, class A6> static T run(A0 a0, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6) { T t = { a0, a1, a2, a3, a4, a5, a6 }; return t; }
	template <class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7> static T run(A0 a0, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7) { T t = { a0, a1, a2, a3, a4, a5, a6, a7 }; return t; }
	template <class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8> static T run(A0 a0, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7, A8 a8) { T t = { a0, a1, a2, a3, a4, a5, a6, a7, a8 }; return t; }
	template <class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9> static T run(A0 a0, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7, A8 a8, A9 a9) { T t = { a0, a1, a2, a3, a4, a5, a6, a7, a8, a9 }; return t; }
	template <class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9, class A10> static T run(A0 a0, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7, A8 a8, A9 a9, A10 a10) { T t = { a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10 }; return t; }
	template <class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9, class A10, class A11> static T run(A0 a0, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7, A8 a8, A9 a9, A10 a10, A11 a11) { T t = { a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11 }; return t; }
	template <class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9, class A10, class A11, class A12> static T run(A0 a0, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7, A8 a8, A9 a9, A10 a10, A11 a11, A12 a12) { T t = { a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12 }; return t; }
	template <class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9, class A10, class A11, class A12, class A13> static T run(A0 a0, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7, A8 a8, A9 a9, A10 a10, A11 a11, A12 a12, A13 a13) { T t = { a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13 }; return t; }
	template <class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9, class A10, class A11, class A12, class A13, class A14> static T run(A0 a0, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7, A8 a8, A9 a9, A10 a10, A11 a11, A12 a12, A13 a13, A14 a14) { T t = { a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14 }; return t; }
	template <class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9, class A10, class A11, class A12, class A13, class A14, class A15> static T run(A0 a0, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7, A8 a8, A9 a9, A10 a10, A11 a11, A12 a12, A13 a13, A14 a14, A15 a15) { T t = { a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15 }; return t; }
};
template <class X, class Y> struct xe_make<std::pair<X, Y> > {
	template <class A0, class A1> static std::pair<X, Y> run(A0 a0, A1 a1) { return std::pair<X, Y>(a0, a1); }
};
template <class T> inline T xe_cl() { return xe_make<T>::run(); }
template <class T, class A0> inline T xe_cl(A0 a0) { return xe_make<T>::run(a0); }
template <class T, class A0, class A1> inline T xe_cl(A0 a0, A1 a1) { return xe_make<T>::run(a0, a1); }
template <class T, class A0, class A1, class A2> inline T xe_cl(A0 a0, A1 a1, A2 a2) { return xe_make<T>::run(a0, a1, a2); }
template <class T, class A0, class A1, class A2, class A3> inline T xe_cl(A0 a0, A1 a1, A2 a2, A3 a3) { return xe_make<T>::run(a0, a1, a2, a3); }
template <class T, class A0, class A1, class A2, class A3, class A4> inline T xe_cl(A0 a0, A1 a1, A2 a2, A3 a3, A4 a4) { return xe_make<T>::run(a0, a1, a2, a3, a4); }
template <class T, class A0, class A1, class A2, class A3, class A4, class A5> inline T xe_cl(A0 a0, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5) { return xe_make<T>::run(a0, a1, a2, a3, a4, a5); }
template <class T, class A0, class A1, class A2, class A3, class A4, class A5, class A6> inline T xe_cl(A0 a0, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6) { return xe_make<T>::run(a0, a1, a2, a3, a4, a5, a6); }
template <class T, class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7> inline T xe_cl(A0 a0, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7) { return xe_make<T>::run(a0, a1, a2, a3, a4, a5, a6, a7); }
template <class T, class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8> inline T xe_cl(A0 a0, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7, A8 a8) { return xe_make<T>::run(a0, a1, a2, a3, a4, a5, a6, a7, a8); }
template <class T, class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9> inline T xe_cl(A0 a0, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7, A8 a8, A9 a9) { return xe_make<T>::run(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9); }
template <class T, class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9, class A10> inline T xe_cl(A0 a0, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7, A8 a8, A9 a9, A10 a10) { return xe_make<T>::run(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10); }
template <class T, class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9, class A10, class A11> inline T xe_cl(A0 a0, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7, A8 a8, A9 a9, A10 a10, A11 a11) { return xe_make<T>::run(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11); }
template <class T, class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9, class A10, class A11, class A12> inline T xe_cl(A0 a0, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7, A8 a8, A9 a9, A10 a10, A11 a11, A12 a12) { return xe_make<T>::run(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12); }
template <class T, class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9, class A10, class A11, class A12, class A13> inline T xe_cl(A0 a0, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7, A8 a8, A9 a9, A10 a10, A11 a11, A12 a12, A13 a13) { return xe_make<T>::run(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13); }
template <class T, class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9, class A10, class A11, class A12, class A13, class A14> inline T xe_cl(A0 a0, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7, A8 a8, A9 a9, A10 a10, A11 a11, A12 a12, A13 a13, A14 a14) { return xe_make<T>::run(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14); }
template <class T, class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9, class A10, class A11, class A12, class A13, class A14, class A15> inline T xe_cl(A0 a0, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7, A8 a8, A9 a9, A10 a10, A11 a11, A12 a12, A13 a13, A14 a14, A15 a15) { return xe_make<T>::run(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15); }
/* Range-based for, rewritten by xenon/tools/xrewrite.py into an iterator loop. */
#include <type_traits>
template <class C> inline auto xe_begin(C& c) -> decltype(c.begin()) { return c.begin(); }
template <class C> inline auto xe_end(C& c) -> decltype(c.end()) { return c.end(); }
template <class C> inline auto xe_begin(const C& c) -> decltype(c.begin()) { return c.begin(); }
template <class C> inline auto xe_end(const C& c) -> decltype(c.end()) { return c.end(); }
template <class T, size_t N> inline T* xe_begin(T (&a)[N]) { return a; }
template <class T, size_t N> inline T* xe_end(T (&a)[N]) { return a + N; }
#endif

#define __builtin_expect(x, y) (x)
#define __builtin_unreachable() __assume(0)
#define __builtin_memcpy memcpy
#define __builtin_memset memset
#define __builtin_ldexp ldexp
#define __builtin_clz(x) ((int)_CountLeadingZeros(x))
#define __builtin_bswap16(x) _byteswap_ushort(x)
#define __builtin_bswap32(x) _byteswap_ulong(x)
#define __builtin_bswap64(x) _byteswap_uint64(x)
#define __builtin_signbit(x) xe_signbit((double)(x))
static __inline int xe_signbit(double d)
{
	unsigned __int64 u;
	memcpy(&u, &d, sizeof u);
	return (int)(u >> 63);
}

/* VS2010's CRT has _snprintf, which leaves the buffer unterminated when it
 * overflows and returns -1; C99's snprintf terminates and returns the length
 * it would have written. */
static __inline int xe_vsnprintf(char* buf, size_t n, const char* fmt, va_list ap)
{
	int r = n ? _vsnprintf(buf, n, fmt, ap) : -1;
	if (n && (r < 0 || (size_t)r >= n))
		buf[n - 1] = 0;
	if (r < 0)
		r = _vscprintf(fmt, ap);
	return r;
}
static __inline int xe_snprintf(char* buf, size_t n, const char* fmt, ...)
{
	va_list ap;
	int r;
	va_start(ap, fmt);
	r = xe_vsnprintf(buf, n, fmt, ap);
	va_end(ap);
	return r;
}
#define snprintf xe_snprintf
#define vsnprintf xe_vsnprintf

#include <ctype.h>
#ifdef __cplusplus
#include <cctype>
namespace std { using ::tolower; using ::toupper; }
#endif

/* C99 maths VS2010's CRT lacks. */
#include <float.h>
#ifdef __cplusplus
inline bool xe_isnan(double x) { return _isnan(x) != 0; }
inline bool xe_isinf(double x) { return !_finite(x) && !_isnan(x); }
#define isnan(x) xe_isnan((double)(x))
#define isinf(x) xe_isinf((double)(x))
#define isfinite(x) (_finite((double)(x)) != 0)
#else
#define isnan(x) _isnan((double)(x))
#define isinf(x) (!_finite((double)(x)) && !_isnan((double)(x)))
#define isfinite(x) _finite((double)(x))
#endif
static __inline float fminf(float a, float b) { return a < b ? a : b; }
static __inline float fmaxf(float a, float b) { return a > b ? a : b; }
static __inline double fmin(double a, double b) { return a < b ? a : b; }
static __inline double fmax(double a, double b) { return a > b ? a : b; }
static __inline long lroundf(float x) { return (long)(x < 0 ? x - 0.5f : x + 0.5f); }
static __inline long lround(double x) { return (long)(x < 0 ? x - 0.5 : x + 0.5); }
static __inline float roundf(float x) { return (float)(x < 0 ? -floor(-x + 0.5) : floor(x + 0.5)); }
static __inline double round(double x) { return x < 0 ? -floor(-x + 0.5) : floor(x + 0.5); }
/* Round to nearest even, as the default FPU mode does. */
static __inline long lrintf(float x)
{
	double f = floor((double)x), d = (double)x - f;
	long r = (long)f;
	if (d > 0.5 || (d == 0.5 && (r & 1)))
		r++;
	return r;
}
static __inline float truncf(float x) { return (float)(x < 0 ? ceil(x) : floor(x)); }
static __inline float log2f(float x) { return (float)(log((double)x) / 0.69314718055994530942); }
static __inline float exp2f(float x) { return (float)pow(2.0, (double)x); }
static __inline float copysignf(float x, float y) { return (float)_copysign(x, y); }

/* Fused multiply-adds, one rounding (xenon/platform/xe_fma.cpp). */
#ifdef __cplusplus
extern "C" {
#endif
float fmaf(float a, float b, float c);
double fma(double a, double b, double c);
#ifdef __cplusplus
}
#endif
#define __builtin_fma fma
#define __builtin_fmaf fmaf
#define __builtin_fabs fabs
#define __builtin_fabsf fabsf
static __inline long lrint(double x)
{
	double f = floor(x), d = x - f;
	long r = (long)f;
	if (d > 0.5 || (d == 0.5 && (r & 1)))
		r++;
	return r;
}
#define strtoull _strtoui64
#define strtoll _strtoi64
typedef long ssize_t;
#include <sys/stat.h>
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
#ifndef MOVEFILE_REPLACE_EXISTING
#define MOVEFILE_REPLACE_EXISTING 1
#endif
#if defined(__cplusplus)
/* VS2010 has no constexpr; its uses here are constants and trivial constructors. */
#define constexpr
#endif
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* POSIX time and environment the port's code uses; xenon/platform provides them. */
#include <time.h>
struct timespec {
	time_t tv_sec;
	long tv_nsec;
};
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#ifdef __cplusplus
extern "C" {
#endif
int clock_gettime(int clk, struct timespec* ts);
char* getenv(const char* name);
int setenv(const char* name, const char* value, int overwrite);
int _putenv_s(const char* name, const char* value);
wchar_t* _wgetenv(const wchar_t* name);
int MoveFileExW(const wchar_t* from, const wchar_t* to, unsigned long flags);
/* The port logs to stderr (and a little to stdout): on the 360 both go to game:\sms.log
 * (xenon/platform/xe_log.cpp); the CRT's own stderr writes to nothing there. */
FILE* xe_logfile(void);
#undef stderr
#undef stdout
#define stderr xe_logfile()
#define stdout xe_logfile()
/* File access (xenon/platform/xe_files.cpp); xrewrite.py sends the port's calls here. */
struct stat;
const char* xe_path(const char* in, char* out, size_t size);
FILE* xe_fopen(const char* path, const char* mode);
FILE* xe_wfopen(const wchar_t* path, const wchar_t* mode);
int xe_remove(const char* path);
int xe_rename(const char* from, const char* to);
int xe_mkdir(const char* path, ...);
int xe_wmkdir(const wchar_t* path);
int xe_access(const char* path, int mode);
int xe_stat(const char* path, struct stat* st);
int xe_open(const char* path, int flags, ...);
int xe_close(int fd);
long long xe_lseeki64(int fd, long long off, int whence);
long xe_lseek(int fd, long off, int whence);
int xe_read(int fd, void* buf, unsigned count);
int xe_write(int fd, const void* buf, unsigned count);
#ifdef __cplusplus
}
#endif

#endif
