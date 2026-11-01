// Achievements (ext/achievements/README.md): the shared state and helpers of
// achievements.h. Whether the play counts: a save state loaded or the warp
// menu used since the file was chosen, or a cheat on during this stage,
// keeps achievements from unlocking. The host half (host/ach_store.cpp) keeps
// what was unlocked and tells the launcher.
#include "achievements.h"

#include "port_ext.h"

#include <Player/Mario.hpp>
#include <System/FlagManager.hpp>
#include <System/Application.hpp>
#include <System/MarDirector.hpp>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
// host/ach_store.cpp
int sms_ach_store_on(void);
int sms_ach_store_eclipse(void);
int sms_ach_store_has(int id);
int sms_ach_store_add(int id);
// platform/misc/port_cheats.cpp
extern int port_cheat_god_mode, port_cheat_movement_speed, port_cheat_jump_height, port_cheat_infinite_lives,
	port_cheat_infinite_fludd, port_cheat_coin_multiplier, port_cheat_infinite_air, port_cheat_low_gravity;
// Counts warps (the warp menu) and save state loads (platform/misc/port_cheats.cpp).
extern int port_play_unchecked;
}

namespace ach {

namespace {

unsigned s_visit;
int s_uncheckedAtFile = -1; // port_play_unchecked when the file was chosen
bool s_cheatedThisStage;
int s_debug = -1;

// Stages or the game's code replaced (the world editor's levels, DVD mods, code mods): nothing counts.
bool modded()
{
	static int s_modded = -1;
	if (s_modded < 0) {
		const char* stage = getenv("SMS_MOD");
		const char* code  = getenv("SMS_CODE_MODS");
		s_modded = (stage && *stage && strcmp(stage, "none") != 0 && strcmp(stage, "0") != 0)
		        || (code && *code && strcmp(code, "none") != 0 && strcmp(code, "0") != 0);
		if (s_modded && sms_ach_store_on())
			log("the stages or the game's code are modded (SMS_MOD, SMS_CODE_MODS): achievements don't unlock");
	}
	return s_modded > 0;
}

// What require() saw, for the tour's summary: each (achievement, thing) once.
struct Need {
	int id;
	char what[48];
	int checks, misses;
};
Need s_needs[256];
int s_needCount;
unsigned s_warnedVisit[256];

// The tour (a test aid, with SMS_ACHIEVEMENTS_DEBUG): SMS_ACHIEVEMENTS_TEST=tour
// warps through every stage and episode where an achievement needs something
// (Episode N is scenario N-1), a second and a half each, and says what was missing.
struct Stop {
	int area, episode;
};
const Stop kTour[] = {
	{ PLAZA, 0 }, { PLAZA, 1 }, { PLAZA, 2 }, { PLAZA, 5 }, { PLAZA, 6 }, { PLAZA, 7 }, { PLAZA, 8 }, { PLAZA, 9 },
	{ LILY_PAD_RIDE, 0 }, { BIANCO, 4 }, { BIANCO, 7 }, { HILLSIDE_CAVE, 0 }, { DIRTY_LAKE_SECRET, 0 }, { RICCO, 4 },
	{ RICCO, 7 }, { RICCO_TOWER_SECRET, 0 }, { GELATO, 0 }, { GELATO, 1 }, { GELATO, 2 }, { GELATO, 3 }, { GELATO, 4 },
	{ GELATO, 5 }, { GELATO, 6 }, { GELATO, 7 }, { SAND_CASTLE_SECRET, 0 }, { SAND_BIRD, 0 }, { PINNA_BEACH, 0 },
	{ PINNA_BEACH, 1 }, { PINNA_BEACH, 2 }, { PINNA_BEACH, 3 }, { PINNA_BEACH, 4 }, { PINNA_BEACH, 5 }, { PINNA_BEACH, 6 },
	{ PINNA_BEACH, 7 }, { PINNA_PARK, 0 }, { PINNA_PARK, 1 }, { PINNA_PARK, 2 }, { PINNA_PARK, 3 }, { PINNA_PARK, 4 },
	{ PINNA_PARK, 5 }, { BEACH_CANNON, 0 }, { YOSHI_GO_ROUND, 0 }, { ROLLER_COASTER, 1 }, { SIRENA_BEACH, 0 },
	{ SIRENA_BEACH, 1 }, { SIRENA_BEACH, 2 }, { SIRENA_BEACH, 3 }, { SIRENA_BEACH, 4 }, { SIRENA_BEACH, 5 },
	{ SIRENA_BEACH, 6 }, { SIRENA_BEACH, 7 }, { CASINO, 0 }, { CASINO, 1 }, { HOTEL_LOBBY_SECRET, 0 },
	{ CASINO_SECRET, 0 }, { KING_BOO_FIGHT, 0 }, { NOKI, 0 }, { NOKI, 1 }, { NOKI, 3 }, { NOKI, 4 }, { NOKI, 5 },
	{ NOKI, 6 }, { NOKI, 7 }, { SHELL_SECRET, 0 }, { PIANTA, 0 }, { PIANTA, 2 }, { PIANTA, 4 }, { VILLAGE_UNDERSIDE, 0 },
	{ CORONA, 0 },
};
const int kTourStops = sizeof kTour / sizeof kTour[0];
// Super Mario Eclipse's: every area of its own and every episode (the castle's
// 35: grounds, floors and paintings), the Tutorial and Corona Mountain's
// secrets (Eclipse's Fire Shrine is one).
const Stop kEclipseTour[] = {
	{ 61, 0 }, { 61, 1 }, { 61, 5 }, { 61, 6 }, { 61, 7 }, { 62, 0 }, { 62, 1 }, { 62, 5 }, { 62, 6 }, { 62, 7 },
	{ 63, 0 }, { 63, 1 }, { 63, 2 }, { 63, 5 }, { 63, 6 }, { 63, 7 }, { 64, 0 }, { 64, 1 }, { 64, 5 }, { 64, 6 },
	{ 65, 0 }, { 65, 1 }, { 65, 6 }, { 65, 7 }, { 66, 0 }, { 66, 1 }, { 66, 6 }, { 67, 0 }, { 68, 0 }, { 68, 4 },
	{ 69, 0 }, { 69, 1 }, { 70, 0 }, { 71, 0 }, { 72, 0 }, { 72, 1 }, { 74, 0 }, { 74, 1 }, { 75, 0 }, { 75, 1 },
	{ 75, 2 }, { 76, 0 }, { 77, 0 }, { 77, 1 }, { 77, 2 }, { 77, 3 }, { 77, 4 }, { 77, 5 }, { 77, 6 }, { 77, 7 },
	{ 77, 8 }, { 77, 9 }, { 77, 10 }, { 77, 11 }, { 77, 12 }, { 77, 13 }, { 77, 14 }, { 77, 15 }, { 77, 16 },
	{ 77, 17 }, { 77, 18 }, { 77, 19 }, { 77, 20 }, { 77, 21 }, { 77, 22 }, { 77, 23 }, { 77, 24 }, { 77, 25 },
	{ 77, 26 }, { 77, 27 }, { 77, 28 }, { 77, 29 }, { 77, 30 }, { 77, 31 }, { 77, 32 }, { 77, 33 }, { 77, 34 },
	{ 78, 0 }, { 78, 1 }, { 78, 2 }, { 78, 3 }, { 78, 4 }, { 78, 5 }, { 78, 6 }, { 78, 7 }, { 78, 8 }, { 79, 1 },
	{ 80, 0 }, { 81, 0 }, { 82, 0 }, { 83, 0 }, { 84, 0 }, { 85, 0 }, { 86, 0 }, { 87, 0 }, { 88, 0 }, { 89, 0 },
	{ 90, 0 }, { 91, 0 }, { 92, 0 }, { 0x2E, 0 }, { 0x2F, 0 }, { 0x30, 0 }, { 0x31, 0 }, { 0x32, 0 }, { 0x33, 0 },
	{ 0x34, 0 },
};
const int kEclipseTourStops = sizeof kEclipseTour / sizeof kEclipseTour[0];
bool s_tour, s_survey;
int s_tourNext = -1, s_tourTicks;

// As the warp menu does (patch zzz-warp-01): the episode in the file's flag, then the stage.
void warpTo(const Stop& to)
{
	TFlagManager::getInstance()->setFlag(0x40003, to.episode);
	gpMarDirector->setNextStage((u16)(((to.area + 1) << 8) | to.episode), 0);
}

const Stop* tourStops(int* n)
{
	*n = eclipse() ? kEclipseTourStops : kTourStops;
	return eclipse() ? kEclipseTour : kTour;
}

void tourReport()
{
	int checks = 0, misses = 0;
	for (int i = 0; i < s_needCount; ++i) {
		checks += s_needs[i].checks;
		misses += s_needs[i].misses;
	}
	int stops;
	tourStops(&stops);
	log("tour done: %d stops, %d checks of %d things achievements need, %d missing", stops, checks, s_needCount, misses);
	for (int i = 0; i < s_needCount; ++i)
		if (s_needs[i].misses)
			log("tour: MISSING achievement %d: %s (%d of %d stops)", s_needs[i].id, s_needs[i].what, s_needs[i].misses,
			    s_needs[i].checks);
}

void tourTick()
{
	static unsigned s_warpedAt; // the visit when the last warp was asked for
	int stops;
	const Stop* tour = tourStops(&stops);
	if (!s_tour || s_tourNext > stops || !gpMarDirector)
		return;
	// wait for the stage asked for to load, then give it a second and a half; a
	// stage that didn't come (a talk or a cutscene held the warp) is asked for
	// again every ten seconds, and passed over after three times
	static int s_waited, s_asked;
	if (s_tourNext > 0 && s_visit == s_warpedAt) {
		if (++s_waited < 300)
			return;
		s_waited = 0;
		const Stop& again = tour[s_tourNext - 1];
		if (++s_asked < 3) {
			log("tour: area %#x scenario %d didn't come (director state %d), asking again", again.area, again.episode,
			    (int)gpMarDirector->mState);
			warpTo(again);
			return;
		}
		log("tour: area %#x scenario %d didn't come, passed over", again.area, again.episode);
		s_warpedAt = s_visit - 1; // go on
	}
	s_waited = 0;
	if (++s_tourTicks < 180)
		return;
	s_tourTicks = 0;
	if (s_tourNext > 0) {
		const Stop& was = tour[s_tourNext - 1];
		if (area() != was.area || episode() != was.episode)
			log("tour: asked for area %#x scenario %d, got %#x %d", was.area, was.episode, area(), episode());
	}
	if (s_tourNext > 0 && s_survey)
		survey();
	if (s_tourNext < 0) {
		// SMS_ACHIEVEMENTS_TOUR_FROM=N starts at stop N (after a stage that crashed)
		const char* from = getenv("SMS_ACHIEVEMENTS_TOUR_FROM");
		s_tourNext       = from ? atoi(from) : 0;
	}
	if (s_tourNext == stops) {
		tourReport();
		++s_tourNext;
		return;
	}
	const Stop& to = tour[s_tourNext++];
	s_asked        = 0;
	log("tour: stop %d of %d: area %#x (%d) scenario %d", s_tourNext, stops, to.area, to.area, to.episode);
	s_warpedAt     = s_visit;
	warpTo(to);
}

bool cheating()
{
	return port_cheat_god_mode || port_cheat_movement_speed != 1 || port_cheat_jump_height != 1
	    || port_cheat_infinite_lives || port_cheat_infinite_fludd || port_cheat_coin_multiplier != 1
	    || port_cheat_infinite_air || port_cheat_low_gravity != 1;
}

void frame()
{
	if (sms_ach_store_on() && cheating())
		s_cheatedThisStage = true;
}

void stageSetup()
{
	// Super Mario Eclipse chooses the file its own way (TCardLoad's end, where
	// afterFileSelect is called, isn't reached): the first stage after the
	// title screen (or the first, Eclipse's title having no stage) is where its play starts.
	static int s_lastArea = -1;
	if (eclipse() && (s_lastArea == TITLE || s_lastArea < 0) && area() != TITLE)
		s_uncheckedAtFile = port_play_unchecked;
	s_lastArea = area();
	++s_visit;
	s_cheatedThisStage = cheating();
	if (debug())
		log("stage %#x scenario %d (visit %u)%s", area(), episode(), s_visit, tainted() ? ", doesn't count" : "");
}

// A test aid (with SMS_ACHIEVEMENTS_DEBUG): SMS_ACHIEVEMENTS_TEST=setflag@N:ID sets
// save flag ID (hex) after N ticks of the first stage, as if the player had.
int s_testTick = -1, s_testFlag, s_ticks;

void afterMovement()
{
	tourTick();
	if (s_testTick < 0 || !playing())
		return;
	if (++s_ticks == s_testTick) {
		log("test: setting flag %#x", s_testFlag);
		TFlagManager::getInstance()->setFlag(s_testFlag, 1);
	}
}

void afterFileSelect()
{
	s_uncheckedAtFile = port_play_unchecked;
}

PortExtension s_extension;

struct Register {
	Register()
	{
		s_extension.name            = "achievements";
		s_extension.frame           = frame;
		s_extension.stageSetup      = stageSetup;
		s_extension.afterFileSelect = afterFileSelect;
		s_extension.afterMovement   = afterMovement;
		port_ext_register(&s_extension);
		const char* test = getenv("SMS_ACHIEVEMENTS_TEST");
		if (test && debug()) {
			s_survey = strcmp(test, "survey") == 0;
			s_tour   = s_survey || strcmp(test, "tour") == 0;
			sscanf(test, "setflag@%d:%x", &s_testTick, &s_testFlag);
			int id = 0;
			if (sscanf(test, "banner:%d", &id) == 1)
				banner(id); // shown once a stage is playing, as an unlock's is
		}
	}
} s_register;

} // namespace

bool eclipse()
{
	return sms_ach_store_eclipse() != 0;
}

bool on()
{
	return sms_ach_store_on() != 0 && !eclipse();
}

bool eclipseOn()
{
	return sms_ach_store_on() != 0 && eclipse();
}

bool tainted()
{
	// before any file select (a warp straight into a stage, SMS_WARP) nothing counts
	return s_uncheckedAtFile < 0 || port_play_unchecked != s_uncheckedAtFile || s_cheatedThisStage || cheating() || modded();
}

void unlock(int id)
{
	if (id < 1 || id > (eclipse() ? ECLIPSE_COUNT : COUNT) || !sms_ach_store_on() || sms_ach_store_has(id))
		return;
	if (tainted()) {
		if (debug())
			log("%d earned, but this play doesn't count (save state, warp or cheat)", id);
		return;
	}
	if (!sms_ach_store_add(id))
		return;
	banner(id);
}

void require(int id, const char* what, bool found)
{
	if (!sms_ach_store_on())
		return;
	int i = 0;
	while (i < s_needCount && !(s_needs[i].id == id && strcmp(s_needs[i].what, what) == 0))
		++i;
	if (i == s_needCount && s_needCount < (int)(sizeof s_needs / sizeof s_needs[0])) {
		s_needs[i].id = id;
		snprintf(s_needs[i].what, sizeof s_needs[i].what, "%s", what);
		++s_needCount;
	}
	if (i < s_needCount) {
		++s_needs[i].checks;
		s_needs[i].misses += !found;
	}
	if (!found && !(i < s_needCount && s_warnedVisit[i] == s_visit)) {
		if (i < s_needCount)
			s_warnedVisit[i] = s_visit;
		log("warning: achievement %d can't find %s in area %#x scenario %d, so it can't be earned here", id, what,
		    area(), episode());
	}
}

bool unlocked(int id)
{
	return sms_ach_store_has(id);
}

int area()
{
	return SMSGetApplication()->mCurrArea.getStage();
}

int episode()
{
	return SMSGetApplication()->mCurrArea.getScenario();
}

bool playing()
{
	return gpMarDirector && gpMarioOriginal && area() != TITLE;
}

unsigned visit()
{
	return s_visit;
}

TMario* mario()
{
	return gpMarioOriginal;
}

bool debug()
{
	if (s_debug < 0) {
		const char* v = getenv("SMS_ACHIEVEMENTS_DEBUG");
		s_debug       = v && *v && strcmp(v, "0") != 0;
	}
	return s_debug > 0;
}

void log(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	fputs("[achievements] ", stderr);
	vfprintf(stderr, fmt, args);
	fputc('\n', stderr);
	va_end(args);
}

} // namespace ach
