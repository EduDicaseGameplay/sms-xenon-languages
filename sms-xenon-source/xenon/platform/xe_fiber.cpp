// Fibers for the game's OS threads (port patch 10, platform/os/os_thread.cpp): one host thread runs
// them all and a thread switch is SwitchToFiber, not a host thread signalled and another blocked,
// which on the console cost ~11 ms a Delfino Plaza frame. os_thread.cpp cannot include the XDK
// headers (port_compat.h's __declspec), so the calls live here.
#include "../msvc/include/xe_win.h"
#include <stdlib.h>
#include <string.h>

extern "C" {

// SMS_OS_FIBERS=0: host threads as upstream
int xe_fibers_enabled(void)
{
	const char* e = getenv("SMS_OS_FIBERS");
	return !(e && e[0] == '0');
}

void* xe_fiber_from_thread(void) { return ConvertThreadToFiber(NULL); }

void* xe_fiber_create(unsigned stack, void (*fn)(void*), void* arg)
{
	return CreateFiber(stack, (LPFIBER_START_ROUTINE)fn, arg);
}

void xe_fiber_switch(void* f) { SwitchToFiber(f); }
void xe_fiber_delete(void* f) { DeleteFiber(f); }
void* xe_fiber_current(void) { return GetCurrentFiber(); }

}
