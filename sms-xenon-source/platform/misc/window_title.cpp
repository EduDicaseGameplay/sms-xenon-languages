// Window title: the game's name and version, so a player can tell which build
// they are playing (and say so in a report), with Super Mario Eclipse named
// when it is built in: "Super Mario Sunshine 2026.10.21 (f5b6846)". The
// version is the launcher's name for this game (SMS_GAME_VERSION, as it
// passes for multiplayer), else the last commit's date, as ext/ext.cmake bakes
// it in with the commit (sms_game_build.h). The debug overlay (`) shows it too.
#include "port_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_include)
#if __has_include("sms_game_build.h")
#include "sms_game_build.h"
#endif
#endif
#ifndef SMS_GAME_BUILD
#define SMS_GAME_BUILD ""
#endif
#ifndef SMS_GAME_VERSION
#define SMS_GAME_VERSION ""
#endif

extern "C" __attribute__((weak)) void GXPC_SetWindowTitle(const char* title);

// The title for this build: `version` the launcher's (may be null or empty),
// `baked` the build's own (the commit's date), `build` the commit, as ext.cmake names it (12 digits, "-eclipse" with
// Eclipse built in; "unknown" outside a git checkout).
extern "C" void port_window_title(char* out, size_t size, const char* version, const char* baked, const char* build)
{
	if (!version || !*version)
		version = baked;
	char commit[16] = "";
	if (build && *build && strcmp(build, "unknown") != 0) {
		size_t n = strcspn(build, "-");
		if (n > 7)
			n = 7;
		memcpy(commit, build, n);
		commit[n] = '\0';
	}
#ifdef SMS_ECLIPSE
	const bool eclipse = true;
#else
	const bool eclipse = build && strstr(build, "-eclipse") != nullptr;
#endif
	int at = snprintf(out, size, "Super Mario Sunshine");
	if (at >= 0 && (size_t)at < size && version && *version)
		at += snprintf(out + at, size - at, " %s", version);
	if (at >= 0 && (size_t)at < size && *commit)
		at += snprintf(out + at, size - at, version && *version ? " (%s)" : " %s", commit);
	if (at >= 0 && (size_t)at < size && eclipse)
		snprintf(out + at, size - at, " + Super Mario Eclipse");
}

extern "C" void port_window_title_init(void)
{
	char title[160];
	port_window_title(title, sizeof title, getenv("SMS_GAME_VERSION"), SMS_GAME_VERSION, SMS_GAME_BUILD);
	port_log("[port] version: %s\n", title);
	if (GXPC_SetWindowTitle)
		GXPC_SetWindowTitle(title);
}
