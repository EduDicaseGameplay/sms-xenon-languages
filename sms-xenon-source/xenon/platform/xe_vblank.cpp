// Retraces from the TV's vertical blank (port patch 08): D3D calls back at each vblank (a DPC,
// which may only signal), and a thread hands the retrace to platform/vi/vi.cpp. Until the
// device exists, or if vblanks never come, a 17 ms wait stands in for them.
#include "../msvc/include/xe_win.h"
#include <stdio.h>

extern "C" void port_vi_vblank(void);

namespace {

HANDLE g_event;
volatile LONG g_seen;  // a real vblank has arrived: no more stand-ins

DWORD WINAPI vblank_thread(void*)
{
	DWORD t0 = 0;
	for (unsigned n = 0;; n++) {
		const bool real = WaitForSingleObject(g_event, g_seen ? INFINITE : 17) == WAIT_OBJECT_0;
		port_vi_vblank();
		if (real && !t0) t0 = GetTickCount(), n = 0;
		if (t0 && n == 600)  // once: the display's rate as the retraces see it
			fprintf(stderr, "[vi] vertical blank: 600 retraces in %lu ms\n", (unsigned long)(GetTickCount() - t0));
	}
}

}  // namespace

// D3D's vertical blank callback (set by xe_gx_render.cpp's deviceInit)
extern "C" volatile long g_xeVblanks;  // the TV's vblanks so far (xe_bench.cpp)
volatile long g_xeVblanks;

void WINAPI xe_vblank(D3DVBLANKDATA*)
{
	InterlockedIncrement(&g_xeVblanks);
	g_seen = 1;
	SetEvent(g_event);
}

extern "C" int xe_vi_start_vblank(void)
{
	g_event = CreateEvent(NULL, FALSE, FALSE, NULL);
	HANDLE th = CreateThread(NULL, 0, vblank_thread, NULL, 0, NULL);
	if (!g_event || !th) return 0;
	SetThreadPriority(th, THREAD_PRIORITY_TIME_CRITICAL);
	CloseHandle(th);
	return 1;
}
