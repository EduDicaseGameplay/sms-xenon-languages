// Achievements (ext/achievements/README.md), Super Mario Eclipse's: the ones
// that only read the save file. Shines (an area's all, the plaza's 60, the
// trails' 9, Episode 7 everywhere, Il Piantissimo's races, Corona Mountain's),
// blue coins (an area's all) and the counts of both.
//
// Where Eclipse keeps them is its pause menu tracker's (Eclipse's
// src/stage/tracker.cpp): Sunshine's areas keep their Shines' flags (0x10000
// on, ten an area, the 100-coin ones from 0x10064, the plaza's in several
// runs) and blue coins (50 bools an area from 0x10078, by Shine stage); Eclipse's
// own areas have Shines 120 to 239 (TFlagManager::getShineFlag, which
// BetterSunshineEngine takes past 120) and 20 blue coin bools each from
// 0x1023A. A blue coin's number within its area's bools isn't its order
// (Corona Mountain's are 20 to 29), so blue coins are counted, as the tracker
// counts them, not named.
//
// As Sunshine's (ach_flags.cpp): an achievement unlocks when what it counts
// reaches its target while the player plays. Counts are taken when a stage is
// set up and compared every tick after, and as a Shine's flag is set (the
// ending sets the last one outside any stage); a file that already has it all
// unlocks nothing by being loaded.
#include "achievements.h"

#include "port_ext.h"

#include <System/FlagManager.hpp>

namespace {

enum Kind {
	FLAG,   // save flags first..first+n-1 (bools from 0x10000)
	SHINE,  // Shines first..first+n-1 (getShineFlag)
	COUNTER // the counter at first, its value
};

struct Run {
	unsigned char kind;
	unsigned first;
	int n;
};

struct Rule {
	int id;
	int need;
	Run runs[7];
};

// A vanilla area's blue coins: 50 bools by Shine stage (TFlagManager::getBlueCoinFlag).
#define BLUES(stage) { FLAG, 0x10078 + ((stage) - 1) * 50, 50 }
// One of Eclipse's areas: 20 bools from 0x1023A by its place (Eclipse's tracker's getBlueCoinFlag).
#define ECLIPSE_BLUES(index, n) { FLAG, 0x1023A + (index) * 20, n }

const Rule kRules[] = {
	// story and sets of Shines
	{ 2, 11, { { FLAG, 0x10006, 1 }, { FLAG, 0x10010, 1 }, { FLAG, 0x1001A, 1 }, { FLAG, 0x10024, 1 },
	           { FLAG, 0x1002E, 1 }, { FLAG, 0x10038, 1 }, { FLAG, 0x10042, 1 } } }, // Episode 7, Sunshine's areas...
	{ 3, 1, { { FLAG, 0x10077, 1 } } },                                               // Corona Mountain's Shine
	{ 4, 4, { { FLAG, 0x10018, 1 }, { FLAG, 0x10041, 1 }, { FLAG, 0x10036, 1 }, { SHINE, 135, 1 } } }, // the races, as Eclipse unlocks Il Piantissimo
	{ 5, 1, { { FLAG, 0x10056, 1 } } },                                               // the airstrip's first
	// Sunshine's areas: 10 Shines and the 100-coin one; 30 blue coins
	{ 6, 11, { { FLAG, 0x10000, 10 }, { FLAG, 0x10064, 1 } } },
	{ 7, 30, { BLUES(2) } },
	{ 8, 11, { { FLAG, 0x1000A, 10 }, { FLAG, 0x10065, 1 } } },
	{ 9, 30, { BLUES(3) } },
	{ 10, 11, { { FLAG, 0x10014, 10 }, { FLAG, 0x10066, 1 } } },
	{ 11, 30, { BLUES(4) } },
	{ 12, 11, { { FLAG, 0x1001E, 10 }, { FLAG, 0x10067, 1 } } },
	{ 13, 30, { BLUES(5) } },
	{ 14, 11, { { FLAG, 0x10028, 10 }, { FLAG, 0x10068, 1 } } },
	{ 15, 30, { BLUES(6) } },
	{ 16, 11, { { FLAG, 0x1003C, 10 }, { FLAG, 0x1006A, 1 } } }, // Pianta Village
	{ 17, 30, { BLUES(7) } },
	{ 18, 11, { { FLAG, 0x10032, 10 }, { FLAG, 0x10069, 1 } } }, // Noki Bay
	{ 19, 30, { BLUES(8) } },
	// the plaza and the places it leads to: the tracker's 61 less Corona Mountain's
	{ 20, 60, { { FLAG, 0x10046, 16 }, { FLAG, 0x1006C, 8 }, { SHINE, 221, 18 }, { FLAG, 0x10057, 13 },
	            { FLAG, 0x10074, 3 }, { FLAG, 0x1006B, 1 }, { SHINE, 239, 1 } } },
	{ 21, 20, { BLUES(1) } },
	{ 22, 10, { BLUES(9) } }, // Corona Mountain
	// Eclipse's areas: Shines from 120 and their 100-coin ones from 207; blue coins
	{ 23, 11, { { SHINE, 120, 10 }, { SHINE, 207, 1 } } }, // Erto Rock
	{ 24, 20, { ECLIPSE_BLUES(0, 20) } },
	{ 25, 11, { { SHINE, 130, 10 }, { SHINE, 208, 1 } } }, // Lighthouse Island
	{ 26, 20, { ECLIPSE_BLUES(1, 20) } },
	{ 27, 11, { { SHINE, 140, 10 }, { SHINE, 209, 1 } } }, // Warship Island
	{ 28, 20, { ECLIPSE_BLUES(2, 20) } },
	{ 29, 11, { { SHINE, 150, 10 }, { SHINE, 210, 1 } } }, // Hotel Lacrima
	{ 30, 20, { ECLIPSE_BLUES(3, 20) } },
	{ 31, 7, { { SHINE, 160, 6 }, { SHINE, 211, 1 } } }, // Pianta Pit
	{ 32, 20, { ECLIPSE_BLUES(4, 20) } },
	{ 33, 6, { { SHINE, 166, 5 }, { SHINE, 212, 1 } } }, // Daisy Cruiser
	{ 34, 10, { ECLIPSE_BLUES(5, 10) } },
	{ 35, 5, { { SHINE, 171, 4 }, { SHINE, 213, 1 } } }, // Mario's Dream
	{ 36, 10, { ECLIPSE_BLUES(6, 10) } },
	{ 37, 5, { { SHINE, 175, 4 }, { SHINE, 214, 1 } } }, // Lancia Fredda Peaks
	{ 38, 10, { ECLIPSE_BLUES(7, 10) } },
	{ 39, 5, { { SHINE, 179, 4 }, { SHINE, 215, 1 } } }, // Vaporwave
	{ 40, 10, { ECLIPSE_BLUES(8, 10) } },
	{ 41, 5, { { SHINE, 183, 4 }, { SHINE, 216, 1 } } }, // Yoshi Village
	{ 42, 10, { ECLIPSE_BLUES(9, 10) } },
	{ 43, 5, { { SHINE, 187, 4 }, { SHINE, 217, 1 } } }, // Red Lily City (the unused area 73 takes no place)
	{ 44, 10, { ECLIPSE_BLUES(10, 10) } },
	{ 45, 5, { { SHINE, 191, 4 }, { SHINE, 218, 1 } } }, // Peach Beach
	{ 46, 10, { ECLIPSE_BLUES(11, 10) } },
	{ 47, 5, { { SHINE, 195, 4 }, { SHINE, 219, 1 } } }, // Spettro Casino
	{ 48, 10, { ECLIPSE_BLUES(12, 10) } },
	{ 49, 9, { { SHINE, 199, 8 }, { SHINE, 220, 1 } } }, // the trails, and the train car's
	// the counts
	{ 50, 240, { { COUNTER, 0x40000, 1 } } },
	{ 51, 420, { { COUNTER, 0x40001, 1 } } },
};
const int kRuleCount = sizeof kRules / sizeof kRules[0];

// Episode 7 in Eclipse's own four areas: Shines 126, 136, 146 and 156 (their seventh scenario).
const Run kEpisode7Eclipse[] = { { SHINE, 126, 1 }, { SHINE, 136, 1 }, { SHINE, 146, 1 }, { SHINE, 156, 1 } };

int s_was[sizeof kRules / sizeof kRules[0]];
bool s_known;

int countRun(const Run& r)
{
	TFlagManager* flags = TFlagManager::getInstance();
	if (r.kind == COUNTER)
		return flags->getFlag(r.first);
	int n = 0;
	for (int i = 0; i < r.n; ++i)
		n += (r.kind == SHINE ? flags->getShineFlag(r.first + i) : flags->getFlag(r.first + i)) != 0;
	return n;
}

int count(const Rule& r)
{
	int n = 0;
	for (int k = 0; k < 7 && r.runs[k].n; ++k)
		n += countRun(r.runs[k]);
	if (r.id == 2)
		for (int k = 0; k < 4; ++k)
			n += countRun(kEpisode7Eclipse[k]);
	return n;
}

void look(bool grant)
{
	if (!TFlagManager::getInstance())
		return;
	for (int i = 0; i < kRuleCount; ++i) {
		const int now = count(kRules[i]);
		if (grant && s_was[i] < kRules[i].need && now >= kRules[i].need)
			ach::unlock(kRules[i].id);
		s_was[i] = now;
	}
}

void stageSetup()
{
	if (!ach::eclipseOn())
		return;
	look(false);
	s_known = true;
}

void afterMovement()
{
	if (ach::eclipseOn() && s_known && ach::playing())
		look(true);
}

// A Shine's flag was set: compare at once (the ending sets Corona Mountain's where no stage ticks).
void shineCollected(int, int)
{
	if (ach::eclipseOn() && s_known)
		look(true);
}

void afterFileSelect()
{
	s_known = false;
}

PortExtension s_extension;

struct Register {
	Register()
	{
		s_extension.name            = "achievements-eclipse-flags";
		s_extension.stageSetup      = stageSetup;
		s_extension.afterMovement   = afterMovement;
		s_extension.shineCollected  = shineCollected;
		s_extension.afterFileSelect = afterFileSelect;
		port_ext_register(&s_extension);
	}
} s_register;

} // namespace
