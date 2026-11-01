// Host entry point: sets up the platform layer, then enters the game's main().
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "port_platform.h"
#include "port_win64_stack.h"
#include "os/crash.h"

extern void SMS_main(void);

struct Args {
	int argc;
	char** argv;
};

static void* run_game(void* p)
{
	port_crash_thread_init();
	Args* a = (Args*)p;
	port_init(a->argc, a->argv);
	std::fprintf(stderr, "[port] entering SMS_main\n");
	SMS_main();
	std::fprintf(stderr, "[port] SMS_main returned\n");
	return NULL;
}

#if UINTPTR_MAX > 0xFFFFFFFFu
#include <pthread.h>
#if defined(__APPLE__) && defined(__aarch64__)
#include <cstring>
#include <mach-o/dyld.h>
#include <spawn.h>
extern char** environ;

// Native arm64 macOS loads every executable at a random address, which the
// linker cannot turn off there; save states hold pointers to the game's
// globals and vtables, and need them at the same addresses in a later run (as
// the other builds' fixed-address executables have them). So the process
// starts itself again without address randomisation (as lldb launches), once:
// SMS_NO_ASLR marks the second start. If that fails, the game runs as it is.
static void restart_without_aslr(char** argv)
{
	if (getenv("SMS_NO_ASLR"))
		return;
	setenv("SMS_NO_ASLR", "1", 1);
	char path[4096];
	uint32_t size = sizeof path;
	if (_NSGetExecutablePath(path, &size) != 0)
		return;
	posix_spawnattr_t attr;
	if (posix_spawnattr_init(&attr) != 0)
		return;
	const short kDisableAslr = 0x0100;  // _POSIX_SPAWN_DISABLE_ASLR
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETEXEC | kDisableAslr);
	pid_t pid;
	int err = posix_spawn(&pid, path, NULL, &attr, argv, environ);  // returns only on failure
	posix_spawnattr_destroy(&attr);
	std::fprintf(stderr, "[port] cannot restart without address randomisation (%s); save states will not load in a later run\n",
		std::strerror(err));
}
#endif

// 64-bit hosts: the game keeps pointers to its locals in u32 slots, so the
// boot thread (which becomes the default OSThread) runs on a stack below
// 2 GiB instead of the process's main stack.
int main(int argc, char** argv)
{
#if defined(__APPLE__) && defined(__aarch64__)
	restart_without_aslr(argv);
#endif
	port_install_crash_handlers();
	const unsigned long size = 8ul << 20;
	void* stack = port_low_alloc(size);
	if (!stack) {
		std::fprintf(stderr, "[port] no memory below 2 GiB (of the game's address window) for the game's stack\n");
		return 1;
	}
	Args a = {argc, argv};
#ifdef _WIN64
	port_win64_stack_call(stack, size, run_game, &a);
	return 0;
#endif
#if defined(__APPLE__) && defined(__x86_64__)
	// AppKit (SDL's window and event pump) only works on the process's main
	// thread, so instead of a second thread, switch this one onto the low
	// stack. Callee-saved rbx holds the old rsp across the call.
	void* top = (char*)stack + size;
	__asm__ volatile(
		"mov %%rsp, %%rbx\n\t"
		"mov %0, %%rsp\n\t"
		"mov %2, %%rdi\n\t"
		"call *%1\n\t"
		"mov %%rbx, %%rsp"
		:
		: "r"(top), "r"(run_game), "r"(&a)
		: "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
		  "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
		  "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15",
		  "memory", "cc");
	return 0;
#elif defined(__APPLE__) && defined(__aarch64__)
	// The same on arm64: callee-saved x19 holds the old sp across the call
	// (x18 is the platform's, x29/fp the callee keeps).
	void* top = (char*)stack + size;
	__asm__ volatile(
		"mov x19, sp\n\t"
		"mov sp, %0\n\t"
		"mov x0, %2\n\t"
		"blr %1\n\t"
		"mov sp, x19"
		:
		: "r"(top), "r"(run_game), "r"(&a)
		: "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14",
		  "x15", "x16", "x17", "x19", "x30",
		  "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v16", "v17", "v18", "v19", "v20", "v21", "v22",
		  "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31",
		  "memory", "cc");
	return 0;
#endif
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstack(&attr, stack, size);
	pthread_t th;
	if (pthread_create(&th, &attr, run_game, &a) != 0) {
		std::fprintf(stderr, "[port] cannot start the game thread\n");
		return 1;
	}
	pthread_attr_destroy(&attr);
	pthread_join(th, NULL);
	return 0;
}
#else
int main(int argc, char** argv)
{
	port_install_crash_handlers();
	Args a = {argc, argv};
	run_game(&a);
	return 0;
}
#endif
