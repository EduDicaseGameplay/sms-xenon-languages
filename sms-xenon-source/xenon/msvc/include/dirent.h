/* opendir/readdir over FindFirstFile, for the port's directory scans. */
#ifndef XE_DIRENT_H
#define XE_DIRENT_H
#include "xe_win.h"
#include <stdlib.h>
#include <string.h>

#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8
struct dirent {
	unsigned char d_type;
	char d_name[MAX_PATH];
};
typedef struct {
	HANDLE h;
	WIN32_FIND_DATAA fd;
	int first;
	struct dirent ent;
} DIR;

static __inline DIR* opendir(const char* path)
{
	char pat[MAX_PATH];
	size_t n;
	DIR* d;
	xe_path(path, pat, sizeof pat - 3); /* relative to game:\, '\' separators (xe_files.cpp) */
	n = strlen(pat);
	if (n && pat[n - 1] != '\\' && pat[n - 1] != '/')
		pat[n++] = '\\';
	pat[n++] = '*';
	pat[n]   = 0;
	for (n = 0; pat[n]; n++)
		if (pat[n] == '/')
			pat[n] = '\\';
	d    = (DIR*)malloc(sizeof(DIR));
	d->h = FindFirstFileA(pat, &d->fd);
	if (d->h == INVALID_HANDLE_VALUE) {
		free(d);
		return NULL;
	}
	d->first = 1;
	return d;
}
static __inline struct dirent* readdir(DIR* d)
{
	if (!d->first && !FindNextFileA(d->h, &d->fd))
		return NULL;
	d->first = 0;
	strncpy(d->ent.d_name, d->fd.cFileName, MAX_PATH - 1);
	d->ent.d_name[MAX_PATH - 1] = 0;
	d->ent.d_type = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? DT_DIR : DT_REG;
	return &d->ent;
}
static __inline int closedir(DIR* d)
{
	FindClose(d->h);
	free(d);
	return 0;
}
#endif
