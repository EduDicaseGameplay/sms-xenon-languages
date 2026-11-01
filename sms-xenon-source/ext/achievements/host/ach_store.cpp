// Achievements (ext/achievements/README.md): the host half keeps what was
// unlocked and tells the launcher. SMS_ACHIEVEMENTS names the file they are
// kept in (the launcher's, one per player, not per save file); unset, they are
// off. Each unlock is a line "<id> <unix time>" added to it, and a line
//   [achievement] {"id":12,"time":1761170000,"game":"sunshine"}
// on stdout, which the launcher reads as it does [presence] lines and shows at
// once. A Super Mario Eclipse build keeps Eclipse's (game "eclipse").
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef SMS_HAVE_GAME_BUILD_H
#include "sms_game_build.h"
#endif
#ifndef SMS_GAME_BUILD
#define SMS_GAME_BUILD "unknown"
#endif

namespace {

// Super Mario Eclipse has its own achievements (eclipse.json), numbered from 1
// as Sunshine's are: the launcher gives each its own file.
#ifdef SMS_ECLIPSE
const bool kEclipse = true;
#else
const bool kEclipse = strstr(SMS_GAME_BUILD, "-eclipse") != nullptr;
#endif
enum { SUNSHINE_COUNT = 148, ECLIPSE_COUNT = 128, MAX_COUNT = 148 };
const int COUNT = kEclipse ? ECLIPSE_COUNT : SUNSHINE_COUNT;

std::mutex s_lock;
int s_on = -1;
bool s_have[MAX_COUNT + 1];
char s_path[1024];

void load()
{
	const char* path = getenv("SMS_ACHIEVEMENTS");
	s_on             = path && *path;
	if (!s_on)
		return;
	snprintf(s_path, sizeof s_path, "%s", path);
	FILE* f = fopen(s_path, "r");
	int n   = 0;
	if (f) {
		char line[128];
		while (fgets(line, sizeof line, f)) {
			int id = atoi(line);
			if (id >= 1 && id <= COUNT && !s_have[id]) {
				s_have[id] = true;
				++n;
			}
		}
		fclose(f);
	}
	fprintf(stderr, "[achievements] on: %d of %s's %d unlocked (%s)\n", n, kEclipse ? "Super Mario Eclipse" : "Sunshine", COUNT,
	        s_path);
}

} // namespace

// 1 in a Super Mario Eclipse build: its achievements, not Sunshine's.
extern "C" int sms_ach_store_eclipse(void)
{
	return kEclipse;
}

extern "C" int sms_ach_store_on(void)
{
	std::lock_guard<std::mutex> l(s_lock);
	if (s_on < 0)
		load();
	return s_on;
}

extern "C" int sms_ach_store_has(int id)
{
	if (!sms_ach_store_on() || id < 1 || id > COUNT)
		return 0;
	std::lock_guard<std::mutex> l(s_lock);
	return s_have[id];
}

// 1 when `id` is newly unlocked (kept and announced), 0 when it was already.
extern "C" int sms_ach_store_add(int id)
{
	if (!sms_ach_store_on() || id < 1 || id > COUNT)
		return 0;
	std::lock_guard<std::mutex> l(s_lock);
	if (s_have[id])
		return 0;
	s_have[id]     = true;
	const long now = (long)time(nullptr);
	if (FILE* f = fopen(s_path, "a")) {
		fprintf(f, "%d %ld\n", id, now);
		fclose(f);
	} else {
		fprintf(stderr, "[achievements] couldn't write %s: %d is unlocked for this session only\n", s_path, id);
	}
	printf("[achievement] {\"id\":%d,\"time\":%ld,\"game\":\"%s\"}\n", id, now, kEclipse ? "eclipse" : "sunshine");
	fflush(stdout);
	return 1;
}
