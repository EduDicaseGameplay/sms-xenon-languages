// File access for the port's own code on the 360. The XDK CRT's fd and stat calls never reach the
// kernel, and the console has no current directory: a relative path ("settings.txt", "rom/x.iso",
// "../../mods") means the title's own folder, game:\. xenon/tools/xrewrite.py sends the port's calls
// here (xe_fopen, xe_open, ...); paths take '\' and lose leading "./" and "../" (nothing lies above game:\).
#include "../msvc/include/xe_win.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifndef INVALID_FILE_ATTRIBUTES
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#endif

extern "C" const char* xe_path(const char* in, char* out, size_t size)
{
	char tmp[MAX_PATH];
	size_t n = 0;
	const char* p = in;
	bool rooted = strchr(in, ':') != NULL;
	if (!rooted) {
		while (*p == '/' || *p == '\\')
			p++;
		for (;;) { // drop "./" and "../": a relative path starts at game:\, and there is nothing above it
			if (p[0] == '.' && (p[1] == '/' || p[1] == '\\'))
				p += 2;
			else if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\\'))
				p += 3;
			else if (p[0] == '.' && p[1] == '.' && !p[2])
				p += 2;
			else
				break;
		}
		const char* root = "game:\\";
		while (*root && n + 1 < sizeof tmp)
			tmp[n++] = *root++;
	}
	for (; *p && n + 1 < sizeof tmp; p++)
		tmp[n++] = *p == '/' ? '\\' : *p;
	while (n > 0 && tmp[n - 1] == '\\' && (n < 2 || tmp[n - 2] != ':'))
		n--; // no trailing separator: the kernel does not trim one
	tmp[n] = 0;
	// "." and ".." inside the path (dvd.cpp reads <files>/../sys/fst.bin): the kernel resolves neither
	if (char* root = strchr(tmp, ':')) {
		char* w = root + 1;  // where the next component goes
		if (*w == '\\') w++;
		const char* r = w;
		for (bool more = *r != 0; more;) {
			const char* e = r;
			while (*e && *e != '\\') e++;
			size_t len = size_t(e - r);
			more = *e != 0;  // before the copy below may write over *e
			const char* next = e + 1;
			if (len == 1 && r[0] == '.') {
			} else if (len == 2 && r[0] == '.' && r[1] == '.') {
				if (w > root + 2) {  // drop the last component written, and its separator
					w--;
					while (w > root + 2 && w[-1] != '\\') w--;
				}
			} else {
				memmove(w, r, len);
				w += len;
				*w++ = '\\';
			}
			r = next;
		}
		if (w > root + 2 && w[-1] == '\\') w--;
		*w = 0;
	}
	strncpy(out, tmp, size - 1);
	out[size - 1] = 0;
	return out;
}

extern "C" FILE* xe_fopen(const char* path, const char* mode)
{
	char p[MAX_PATH];
	return fopen(xe_path(path, p, sizeof p), mode);
}

extern "C" FILE* xe_wfopen(const wchar_t* path, const wchar_t* mode)
{
	char p[MAX_PATH], m[16];
	size_t i;
	for (i = 0; path[i] && i + 1 < sizeof p; i++)
		p[i] = (char)path[i];
	p[i] = 0;
	for (i = 0; mode[i] && i + 1 < sizeof m; i++)
		m[i] = (char)mode[i];
	m[i] = 0;
	return xe_fopen(p, m);
}

extern "C" int xe_remove(const char* path)
{
	char p[MAX_PATH];
	xe_path(path, p, sizeof p);
	if (DeleteFileA(p) || RemoveDirectoryA(p))
		return 0;
	errno = ENOENT;
	return -1;
}

extern "C" int xe_rename(const char* from, const char* to)
{
	char a[MAX_PATH], b[MAX_PATH];
	xe_path(from, a, sizeof a);
	xe_path(to, b, sizeof b);
	DeleteFileA(b); // POSIX rename replaces
	return MoveFileA(a, b) ? 0 : -1;
}

extern "C" int xe_mkdir(const char* path, ...)
{
	char p[MAX_PATH];
	xe_path(path, p, sizeof p);
	if (CreateDirectoryA(p, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) {
		if (GetLastError() == ERROR_ALREADY_EXISTS)
			errno = EEXIST;
		return GetLastError() == ERROR_ALREADY_EXISTS ? -1 : 0;
	}
	errno = ENOENT;
	return -1;
}

extern "C" int xe_wmkdir(const wchar_t* path)
{
	char p[MAX_PATH];
	size_t i;
	for (i = 0; path[i] && i + 1 < sizeof p; i++)
		p[i] = (char)path[i];
	p[i] = 0;
	return xe_mkdir(p);
}

extern "C" int xe_access(const char* path, int)
{
	char p[MAX_PATH];
	if (GetFileAttributesA(xe_path(path, p, sizeof p)) == INVALID_FILE_ATTRIBUTES) {
		errno = ENOENT;
		return -1;
	}
	return 0;
}

extern "C" int xe_stat(const char* path, struct stat* st)
{
	char p[MAX_PATH];
	WIN32_FILE_ATTRIBUTE_DATA a;
	if (!GetFileAttributesExA(xe_path(path, p, sizeof p), GetFileExInfoStandard, &a)) {
		errno = ENOENT;
		return -1;
	}
	memset(st, 0, sizeof *st);
	st->st_mode = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? (_S_IFDIR | 0755) : (_S_IFREG | 0644);
	st->st_size = (_off_t)a.nFileSizeLow;
	unsigned long long t = ((unsigned long long)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime);
	st->st_mtime = (time_t)(t / 10000000ULL - 11644473600ULL);
	return 0;
}

// POSIX descriptors over Win32 handles (the disc image is read through them).
namespace {
const int kFds = 64;
HANDLE s_fd[kFds];
CRITICAL_SECTION s_fdLock;
volatile LONG s_fdInit;
void fd_lock()
{
	if (InterlockedCompareExchange(&s_fdInit, 1, 0) == 0) {
		InitializeCriticalSection(&s_fdLock);
		s_fdInit = 2;
	}
	while (s_fdInit != 2)
		SwitchToThread();
	EnterCriticalSection(&s_fdLock);
}
HANDLE fd_handle(int fd) { return fd >= 3 && fd < kFds ? s_fd[fd] : NULL; }
} // namespace

extern "C" int xe_open(const char* path, int flags, ...)
{
	char p[MAX_PATH];
	xe_path(path, p, sizeof p);
	DWORD access = (flags & (_O_WRONLY | _O_RDWR)) ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
	DWORD disp = (flags & _O_CREAT) ? ((flags & _O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS) : ((flags & _O_TRUNC) ? TRUNCATE_EXISTING : OPEN_EXISTING);
	HANDLE h = CreateFileA(p, access, FILE_SHARE_READ, NULL, disp, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		errno = ENOENT;
		return -1;
	}
	fd_lock();
	int fd = 3;
	while (fd < kFds && s_fd[fd])
		fd++;
	if (fd < kFds)
		s_fd[fd] = h;
	LeaveCriticalSection(&s_fdLock);
	if (fd >= kFds) {
		CloseHandle(h);
		errno = EMFILE;
		return -1;
	}
	return fd;
}

extern "C" int xe_close(int fd)
{
	fd_lock();
	HANDLE h = fd_handle(fd);
	if (h)
		s_fd[fd] = NULL;
	LeaveCriticalSection(&s_fdLock);
	return h && CloseHandle(h) ? 0 : -1;
}

extern "C" long long xe_lseeki64(int fd, long long off, int whence)
{
	HANDLE h = fd_handle(fd);
	LARGE_INTEGER d, r;
	d.QuadPart = off;
	if (!h || !SetFilePointerEx(h, d, &r, whence == SEEK_SET ? FILE_BEGIN : whence == SEEK_CUR ? FILE_CURRENT : FILE_END))
		return -1;
	return r.QuadPart;
}

extern "C" long xe_lseek(int fd, long off, int whence) { return (long)xe_lseeki64(fd, off, whence); }

extern "C" int xe_read(int fd, void* buf, unsigned count)
{
	HANDLE h = fd_handle(fd);
	DWORD got = 0;
	if (!h || !ReadFile(h, buf, count, &got, NULL))
		return -1;
	return (int)got;
}

extern "C" int xe_write(int fd, const void* buf, unsigned count)
{
	HANDLE h = fd_handle(fd);
	DWORD put = 0;
	if (!h || !WriteFile(h, buf, count, &put, NULL))
		return -1;
	return (int)put;
}
