// Desktop Win32 calls the port's _WIN32 branches make that the XDK declares but
// does not implement. Defined without xtl.h, whose declaration differs in form.
#include <string.h>

// The executable's path (gcdisc.cpp looks for a disc image bundled after it):
// the 360's title is always game:\default.xex.
extern "C" unsigned long GetModuleFileNameA(void*, char* path, unsigned long size)
{
	const char* p = "game:\\default.xex";
	unsigned long n = (unsigned long)strlen(p);
	if (!size)
		return 0;
	if (n >= size)
		n = size - 1;
	memcpy(path, p, n);
	path[n] = 0;
	return n;
}
