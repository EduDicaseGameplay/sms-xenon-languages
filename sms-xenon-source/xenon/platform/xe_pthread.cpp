// Bodies of xenon/msvc/include/pthread.h on the XDK's Win32 threads.
#include <pthread.h>
#include <errno.h>

namespace {

struct Entry {
	pthread_t id;
	HANDLE h;
};
const int kMaxThreads = 64;
Entry s_threads[kMaxThreads];
CRITICAL_SECTION s_lock;
volatile LONG s_lockState;

void lazy_cs(volatile LONG* state, CRITICAL_SECTION* cs)
{
	if (*state == 2) {
		__lwsync();
		return;
	}
	if (InterlockedCompareExchange(state, 1, 0) == 0) {
		InitializeCriticalSection(cs);
		__lwsync();
		*state = 2;
		return;
	}
	while (*state != 2)
		SwitchToThread();
	__lwsync();
}

void table_lock() { lazy_cs(&s_lockState, &s_lock); EnterCriticalSection(&s_lock); }
void table_unlock() { LeaveCriticalSection(&s_lock); }

HANDLE take(pthread_t t)
{
	HANDLE h = 0;
	table_lock();
	for (int i = 0; i < kMaxThreads; i++)
		if (s_threads[i].h && s_threads[i].id == t) {
			h = s_threads[i].h;
			s_threads[i].h = 0;
			break;
		}
	table_unlock();
	return h;
}

struct Start {
	void* (*fn)(void*);
	void* arg;
};
DWORD WINAPI entry(LPVOID p)
{
	Start s = *(Start*)p;
	delete (Start*)p;
	s.fn(s.arg);
	return 0;
}

void lazy_cond(pthread_cond_t* c)
{
	if (c->state == 2) {
		__lwsync();
		return;
	}
	if (InterlockedCompareExchange(&c->state, 1, 0) == 0) {
		c->sem     = CreateSemaphore(NULL, 0, 0x7FFFFFFF, NULL);
		c->waiters = 0;
		__lwsync();
		c->state = 2;
		return;
	}
	while (c->state != 2)
		SwitchToThread();
	__lwsync();
}

DWORD ms_until(const struct timespec* abstime)
{
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	long long ms = (long long)(abstime->tv_sec - now.tv_sec) * 1000 + (abstime->tv_nsec - now.tv_nsec) / 1000000;
	return ms <= 0 ? 0 : (DWORD)ms;
}

}  // namespace

extern "C" volatile LONG g_xeCondSignals, g_xeCondWaits;  // [perf]: guest thread switches go through these
volatile LONG g_xeCondSignals, g_xeCondWaits;

namespace {

int cond_wait_ms(pthread_cond_t* c, pthread_mutex_t* m, DWORD ms)
{
	lazy_cond(c);
	InterlockedIncrement(&g_xeCondWaits);
	InterlockedIncrement(&c->waiters);
	pthread_mutex_unlock(m);
	bool got = WaitForSingleObject(c->sem, ms) == WAIT_OBJECT_0;
	if (!got) {
		LONG w;
		do {
			w = c->waiters;
			if (w <= 0) { // a signal already counted this waiter: take its release
				WaitForSingleObject(c->sem, INFINITE);
				got = true;
				break;
			}
		} while (InterlockedCompareExchange(&c->waiters, w - 1, w) != w);
	}
	pthread_mutex_lock(m);
	return got ? 0 : ETIMEDOUT;
}

} // namespace

extern "C" {

int pthread_attr_init(pthread_attr_t* a) { a->stacksize = 0; a->detached = 0; return 0; }
int pthread_attr_destroy(pthread_attr_t*) { return 0; }
int pthread_attr_setstacksize(pthread_attr_t* a, size_t size) { a->stacksize = size; return 0; }
int pthread_attr_setdetachstate(pthread_attr_t* a, int state) { a->detached = state; return 0; }

int pthread_create(pthread_t* t, const pthread_attr_t* a, void* (*fn)(void*), void* arg)
{
	Start* s = new Start;
	s->fn    = fn;
	s->arg   = arg;
	DWORD id = 0;
	HANDLE h = CreateThread(NULL, a && a->stacksize ? a->stacksize : 256 * 1024, entry, s, CREATE_SUSPENDED, &id);
	if (!h) {
		delete s;
		return EAGAIN;
	}
	*t = id;
	bool kept = false;
	if (!a || !a->detached) {
		table_lock();
		int i = 0;
		while (i < kMaxThreads && s_threads[i].h)
			i++;
		if (i < kMaxThreads) {
			s_threads[i].id = id;
			s_threads[i].h  = h;
			kept = true;
		} // else the table is full: the thread runs, but cannot be joined
		table_unlock();
	}
	// SMS_XE_THREADS=spread: off the creator's hardware thread (CreateThread's default), round
	// robin over 2-4 (5 is XAudio2's). Measured worse in Delfino Plaza (game 48-54 ms a frame
	// against 30-47 ms), so threads stay where they were made unless asked.
	static int spread = -1;
	if (spread < 0) spread = getenv("SMS_XE_THREADS") && !strcmp(getenv("SMS_XE_THREADS"), "spread");
	static volatile LONG s_next;
	if (spread) XSetThreadProcessor(h, 2 + DWORD(InterlockedIncrement(&s_next) % 3));
	ResumeThread(h);
	if (!kept)
		CloseHandle(h); // only after ResumeThread: the console does not resume a closed handle (Xenia does)
	return 0;
}

HANDLE xe_pthread_handle(pthread_t t)
{
	HANDLE h = 0;
	table_lock();
	for (int i = 0; i < kMaxThreads; i++)
		if (s_threads[i].h && s_threads[i].id == t)
			h = s_threads[i].h;
	table_unlock();
	return h;
}

int pthread_join(pthread_t t, void** ret)
{
	HANDLE h = take(t);
	if (!h)
		return ESRCH;
	WaitForSingleObject(h, INFINITE);
	CloseHandle(h);
	if (ret)
		*ret = 0;
	return 0;
}

int pthread_tryjoin_np(pthread_t t, void** ret)
{
	HANDLE h = xe_pthread_handle(t);
	if (!h)
		return ESRCH;
	if (WaitForSingleObject(h, 0) != WAIT_OBJECT_0)
		return EBUSY;
	return pthread_join(t, ret);
}

int pthread_detach(pthread_t t)
{
	HANDLE h = take(t);
	if (h)
		CloseHandle(h);
	return 0;
}

pthread_t pthread_self(void) { return GetCurrentThreadId(); }
// The calling thread onto hardware thread `hw` (0-5), e.g. the DSP mixer's (port patch 14)
void xe_pin_thread(int hw) { XSetThreadProcessor(GetCurrentThread(), DWORD(hw)); }
int pthread_equal(pthread_t a, pthread_t b) { return a == b; }
void pthread_exit(void*) { ExitThread(0); }

int pthread_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t*)
{
	m->state = 0;
	lazy_cs(&m->state, &m->cs);
	return 0;
}
int pthread_mutex_destroy(pthread_mutex_t* m)
{
	if (m->state == 2)
		DeleteCriticalSection(&m->cs);
	m->state = 0;
	return 0;
}
int pthread_mutex_lock(pthread_mutex_t* m)
{
	lazy_cs(&m->state, &m->cs);
	EnterCriticalSection(&m->cs);
	return 0;
}
int pthread_mutex_trylock(pthread_mutex_t* m)
{
	lazy_cs(&m->state, &m->cs);
	return TryEnterCriticalSection(&m->cs) ? 0 : EBUSY;
}
int pthread_mutex_unlock(pthread_mutex_t* m)
{
	LeaveCriticalSection(&m->cs);
	return 0;
}

int pthread_cond_init(pthread_cond_t* c, const pthread_condattr_t*)
{
	c->state = 0;
	lazy_cond(c);
	return 0;
}
int pthread_cond_destroy(pthread_cond_t* c)
{
	if (c->state == 2)
		CloseHandle(c->sem);
	c->state = 0;
	return 0;
}
int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) { return cond_wait_ms(c, m, INFINITE); }
int pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m, const struct timespec* abstime)
{
	return cond_wait_ms(c, m, ms_until(abstime));
}
int pthread_cond_signal(pthread_cond_t* c)
{
	InterlockedIncrement(&g_xeCondSignals);
	lazy_cond(c);
	LONG w;
	do {
		w = c->waiters;
		if (w <= 0)
			return 0;
	} while (InterlockedCompareExchange(&c->waiters, w - 1, w) != w);
	ReleaseSemaphore(c->sem, 1, NULL);
	return 0;
}
int pthread_cond_broadcast(pthread_cond_t* c)
{
	lazy_cond(c);
	LONG n = InterlockedExchange(&c->waiters, 0);
	if (n > 0)
		ReleaseSemaphore(c->sem, n, NULL);
	return 0;
}

} // extern "C"
