/* The pthread subset the port's platform layer uses (os_thread.cpp, vi.cpp),
 * on the XDK's Win32 threads, as winpthreads serves the MinGW build. Static
 * initialisers work: mutexes and condition variables create their kernel
 * objects on first use. xenon/platform/xe_pthread.cpp has the bodies. */
#ifndef XE_PTHREAD_H
#define XE_PTHREAD_H
#include <time.h>
#include "xe_win.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef DWORD pthread_t; /* the thread id; the handle lives in a table */
typedef struct {
	size_t stacksize;
	int detached;
} pthread_attr_t;
typedef struct {
	volatile LONG state; /* 0 fresh, 1 initialising, 2 ready */
	CRITICAL_SECTION cs;
} pthread_mutex_t;
typedef struct {
	volatile LONG state;
	HANDLE sem;
	volatile LONG waiters;
} pthread_cond_t;
typedef int pthread_mutexattr_t;
typedef int pthread_condattr_t;

#define PTHREAD_MUTEX_INITIALIZER { 0 }
#define PTHREAD_COND_INITIALIZER { 0 }
#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

int pthread_attr_init(pthread_attr_t* a);
int pthread_attr_destroy(pthread_attr_t* a);
int pthread_attr_setstacksize(pthread_attr_t* a, size_t size);
int pthread_attr_setdetachstate(pthread_attr_t* a, int state);
int pthread_create(pthread_t* t, const pthread_attr_t* a, void* (*fn)(void*), void* arg);
int pthread_join(pthread_t t, void** ret);
int pthread_tryjoin_np(pthread_t t, void** ret);
int pthread_detach(pthread_t t);
pthread_t pthread_self(void);
int pthread_equal(pthread_t a, pthread_t b);
void pthread_exit(void* ret);
/* The Win32 handle of a thread this layer created (0 for others), so the
 * platform layer can place it on a hardware thread. */
HANDLE xe_pthread_handle(pthread_t t);

int pthread_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t* a);
int pthread_mutex_destroy(pthread_mutex_t* m);
int pthread_mutex_lock(pthread_mutex_t* m);
int pthread_mutex_trylock(pthread_mutex_t* m);
int pthread_mutex_unlock(pthread_mutex_t* m);

int pthread_cond_init(pthread_cond_t* c, const pthread_condattr_t* a);
int pthread_cond_destroy(pthread_cond_t* c);
int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m);
/* abstime is CLOCK_REALTIME, as POSIX's default */
int pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m, const struct timespec* abstime);
int pthread_cond_signal(pthread_cond_t* c);
int pthread_cond_broadcast(pthread_cond_t* c);

#ifdef __cplusplus
}
#endif
#endif
