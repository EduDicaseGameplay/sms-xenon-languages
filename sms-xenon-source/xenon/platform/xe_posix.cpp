// POSIX pieces the port's code calls that the XDK's CRT lacks: clock_gettime and
// getenv. The console has no process environment, so getenv answers from
// game:\sms_env.txt (KEY=VALUE lines, # comments), read once: the SMS_* switches
// of the PC build work the same way there.
#include "../msvc/include/xe_win.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <map>
#include <string>

extern "C" int clock_gettime(int clk, struct timespec* ts)
{
	if (clk == CLOCK_MONOTONIC) {
		static LARGE_INTEGER f;
		if (!f.QuadPart)
			QueryPerformanceFrequency(&f);
		LARGE_INTEGER c;
		QueryPerformanceCounter(&c);
		ts->tv_sec  = (time_t)(c.QuadPart / f.QuadPart);
		ts->tv_nsec = (long)(c.QuadPart % f.QuadPart * 1000000000LL / f.QuadPart);
		return 0;
	}
	FILETIME ft; // 100 ns ticks since 1601
	GetSystemTimeAsFileTime(&ft);
	long long t = ((long long)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000LL;
	ts->tv_sec  = (time_t)(t / 10000000);
	ts->tv_nsec = (long)(t % 10000000 * 100);
	return 0;
}

namespace {
// Fixed tables, no allocation: getenv is called from static constructors (ach_core.cpp),
// before the CRT has finished starting, where the STL containers crashed.
const int kEnvMax = 128;
char s_keys[kEnvMax][64];
char s_vals[kEnvMax][256];
int s_count = -1; // -1: game:\sms_env.txt not read yet

void copy(char* to, const char* from, size_t size)
{
	size_t n = strlen(from);
	if (n >= size)
		n = size - 1;
	memcpy(to, from, n);
	to[n] = 0;
}

int find(const char* name)
{
	for (int i = 0; i < s_count; i++)
		if (!strcmp(s_keys[i], name))
			return i;
	return -1;
}

void put(const char* name, const char* value)
{
	int i = find(name);
	if (i < 0) {
		if (s_count >= kEnvMax)
			return;
		i = s_count++;
		copy(s_keys[i], name, sizeof s_keys[i]);
	}
	copy(s_vals[i], value, sizeof s_vals[i]);
}

void default_env();
} // namespace
extern "C" void xe_bench_env(const char* list, void (*put)(const char*, const char*));  // xe_bench.cpp
namespace {

void load_env()
{
	s_count = 0;
	FILE* f = fopen("game:\\sms_env.txt", "r");
	if (!f) {
		default_env();
		return;
	}
	char line[512];
	while (fgets(line, sizeof line, f)) {
		char* e = line + strlen(line);
		while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
			*--e = 0;
		char* s = line;
		while (*s == ' ' || *s == '\t')
			s++;
		if (!*s || *s == '#')
			continue;
		char* eq = strchr(s, '=');
		if (!eq)
			continue;
		*eq = 0;
		put(s, eq + 1);
	}
	fclose(f);
	if (find("SMS_XE_BENCH") >= 0) {  // a bench run's lines go over the file's
		char list[256];
		copy(list, s_vals[find("SMS_XE_BENCH")], sizeof list);
		xe_bench_env(list, put);
	}
	default_env();
}

// Defaults the console needs. Where the PC build keeps ~/.local/share/sms-port (the memory card,
// HUD settings): the title's folder. ponytail: plain files, as homebrew run from HDD/USB can write there; XContent saves
// (per profile, device selector) if the port ever runs from a read-only place.
void default_env()
{
	if (find("XDG_DATA_HOME") < 0) put("XDG_DATA_HOME", "game:\\data");
	if (find("SMS_WIDESCREEN") < 0) put("SMS_WIDESCREEN", "16:9");  // 720p screens are 16:9
	if (find("SMS_FRAME_RATE") < 0) put("SMS_FRAME_RATE", "30");  // the GameCube's: 60 is out of the console's reach
	// The game: sms_extract.py's folders (game:\files, game:\sys), or a GMSE01.iso beside the xex.
	if (find("SMS_DISC_IMAGE") < 0 && find("SMS_DISC_ROOT") < 0) {
		if (GetFileAttributesA("game:\\GMSE01.iso") != (DWORD)-1) put("SMS_DISC_IMAGE", "game:\\GMSE01.iso");
		else put("SMS_DISC_ROOT", "game:\\files");
	}
}
} // namespace

extern "C" char* getenv(const char* name)
{
	// The first call happens on the boot thread, before the game starts others.
	if (s_count < 0)
		load_env();
	int i = find(name);
	return i < 0 ? NULL : s_vals[i];
}

extern "C" int setenv(const char* name, const char* value, int overwrite)
{
	if (s_count < 0)
		load_env();
	if (overwrite || find(name) < 0)
		put(name, value ? value : "");
	return 0;
}

extern "C" int _putenv_s(const char* name, const char* value)
{
	if (s_count < 0)
		load_env();
	int i = find(name);
	if (!value || !*value) {
		if (i >= 0) { // remove: move the last entry into its place
			s_count--;
			memcpy(s_keys[i], s_keys[s_count], sizeof s_keys[i]);
			memcpy(s_vals[i], s_vals[s_count], sizeof s_vals[i]);
		}
		return 0;
	}
	return setenv(name, value, 1);
}

namespace {
std::string narrow(const wchar_t* w)
{
	std::string s;
	for (; *w; w++)
		s += *w < 0x80 ? (char)*w : '?';
	return s;
}
} // namespace

extern "C" wchar_t* _wgetenv(const wchar_t* name)
{
	static std::map<std::string, std::wstring> wide; // kept so the pointer stays valid
	const char* v = getenv(narrow(name).c_str());
	if (!v)
		return NULL;
	std::wstring& w = wide[narrow(name)];
	w.assign(v, v + strlen(v));
	return (wchar_t*)w.c_str();
}

// The settings writer's atomic replace: the XDK has MoveFile, but not the Ex form.
extern "C" int MoveFileExW(const wchar_t* from, const wchar_t* to, unsigned long flags)
{
	std::string f = narrow(from), t = narrow(to);
	if (flags & MOVEFILE_REPLACE_EXISTING)
		DeleteFileA(t.c_str());
	return MoveFileA(f.c_str(), t.c_str()) ? 1 : 0;
}
