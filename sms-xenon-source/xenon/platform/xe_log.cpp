// game:\sms.log: where stderr and stdout of the port's code go on the 360 (xe_prelude.h).
// Opened on first use, from any thread. Not _IONBF: the XDK CRT then writes single characters as the
// high (zero) byte of an int, so the file fills with NULs. Fully buffered instead, flushed by a thread
// every 100 ms (a flushed write costs ~30 ms on the console, so callers must not pay it).
// ponytail: a crash loses up to 100 ms of log; flush from the crash path when one exists.
#include "../msvc/include/xe_win.h"
#include <stdio.h>

#undef stderr
#undef stdout

static DWORD WINAPI flusher(void*)
{
	for (;;) {
		Sleep(100);
		fflush(xe_logfile());
	}
}

extern "C" FILE* xe_logfile(void)
{
	static FILE* volatile s_log;
	static volatile LONG s_state;
	if (s_state == 2)
		return s_log;
	if (InterlockedCompareExchange(&s_state, 1, 0) == 0) {
		FILE* f = fopen("game:\\sms.log", "w");
		if (!f)
			f = fopen("NUL", "w");
		setvbuf(f, NULL, _IOFBF, 64 * 1024);
		s_log = f;
		__lwsync();
		s_state = 2;
		fputs("[xenon] sms.log open\n", f);
		CreateThread(NULL, 0, flusher, NULL, 0, NULL);
		return f;
	}
	while (s_state != 2)
		SwitchToThread();
	return s_log;
}
