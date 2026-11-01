// ROM-free crash probe. Link the runtime with --gc-sections so only its crash
// reporting is used. The Windows logger can be linked without the runtime.
#include "../crash.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <stdlib.h>
#include "port_win64_stack.h"
#else
#include <pthread.h>
#include <unistd.h>
extern "C" int gcdisc_self_path(char*, uint32_t) { return 0; }
#endif

#ifndef _WIN32
__attribute__((noinline)) static unsigned exhaust_stack(unsigned depth)
{
	volatile unsigned char local[4096];
	memset((void*)local, depth, sizeof local);
	return exhaust_stack(depth + 1) + local[depth % sizeof local];
}
#endif

static void* fault(void*)
{
#ifndef _WIN32
	port_crash_thread_init();
#endif
	*(volatile int*)(uintptr_t)1 = 42;
	return NULL;
}

#ifndef _WIN32
static void* overflow(void*)
{
	port_crash_thread_init();
	return (void*)(uintptr_t)exhaust_stack(0);
}
#endif

#ifdef _WIN32
// The handle "held" leaves open, as the game leaves its disc open.
static HANDLE held_file;

static DWORD WINAPI end_probe(void*)
{
	Sleep(2000);
	TerminateProcess(GetCurrentProcess(), 3);
	return 0;
}
#endif

int main(int argc, char** argv)
{
#ifdef _WIN32
	// A copy of the probe named by SMS_ADDR2LINE stands in for a stuck addr2line
	// (the crash report runs it with -f): it says whether it got the probe's
	// open file, then never finishes.
	if (argc > 1 && !strcmp(argv[1], "-f")) {
		const char* held = getenv("SMS_PROBE_HELD");
		char path[MAX_PATH];
		DWORD flags;
		HANDLE handle = held ? (HANDLE)(uintptr_t)strtoull(held, NULL, 16) : NULL;
		bool inherited = handle && GetHandleInformation(handle, &flags) && GetFileType(handle) == FILE_TYPE_DISK &&
			GetFinalPathNameByHandleA(handle, path, sizeof path, 0) && strstr(path, "held");
		fprintf(stderr, inherited ? "[probe] symboliser inherited the held file\n" : "[probe] symboliser holds no game file\n");
		fflush(stderr);
		Sleep(INFINITE);
	}
	port_install_windows_exception_logger();
	SetErrorMode(SEM_NOGPFAULTERRORBOX);
#else
	port_install_crash_handlers();
#endif
	if (argc < 2) return 2;
	if (!strcmp(argv[1], "access")) fault(NULL);
#ifdef _WIN32
	if (!strcmp(argv[1], "heap")) RaiseException(0xC0000374, EXCEPTION_NONCONTINUABLE, 0, NULL);
	// What Windows raises when an unwind cannot follow the stack; the logger
	// once skipped it, so the Eclipse stage-exit crash printed nothing.
	if (!strcmp(argv[1], "unwind")) RaiseException(0xC00000FF, EXCEPTION_NONCONTINUABLE, 0, NULL);
	// Crash with a file open, as the game has its disc. "exit" ends the probe
	// while the report waits for addr2line, as closing a crashed game does.
	if (!strcmp(argv[1], "held") && argc > 3) {
		FILE* file = fopen(argv[2], "wb");
		if (!file) return 3;
		held_file = (HANDLE)_get_osfhandle(_fileno(file));
		char value[32];
		snprintf(value, sizeof value, "%llx", (unsigned long long)(uintptr_t)held_file);
		SetEnvironmentVariableA("SMS_PROBE_HELD", value);
		if (!strcmp(argv[3], "exit")) CloseHandle(CreateThread(NULL, 0, end_probe, NULL, 0, NULL));
		fault(NULL);
	}
	// The report abort() gets, without ending the probe.
	if (!strcmp(argv[1], "report")) {
		port_windows_report_current("probe report");
		return 0;
	}
#ifdef _WIN64
	if (!strcmp(argv[1], "low-stack")) {
		void* stack = VirtualAlloc((void*)0x10000000, 1 << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		if (!stack) return 3;
		port_win64_stack_call(stack, 1 << 20, fault, NULL);
	}
#endif
#else
	if (!strcmp(argv[1], "abort")) raise(SIGABRT);
	if (!strcmp(argv[1], "thread")) {
		pthread_t thread;
		if (pthread_create(&thread, NULL, fault, NULL)) return 3;
		pthread_join(thread, NULL);
	}
	if (!strcmp(argv[1], "overflow")) {
		pthread_attr_t attributes;
		pthread_attr_init(&attributes);
		pthread_attr_setstacksize(&attributes, 256 * 1024);
		pthread_t thread;
		if (pthread_create(&thread, &attributes, overflow, NULL)) return 3;
		pthread_attr_destroy(&attributes);
		pthread_join(thread, NULL);
	}
#endif
	return 4; // A requested crash should never return.
}
