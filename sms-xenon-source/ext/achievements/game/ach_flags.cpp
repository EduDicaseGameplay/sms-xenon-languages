// Achievements (ext/achievements/README.md): the ones that only read the save
// file: Shines (one, an area's eight episodes or two secrets, the plaza's ten,
// the boathouse's trades, the Shine count), blue coins, nozzle boxes opened
// for good, Yoshi, story flags. The table (ach_flag_rules.inc) is generated
// from the reference set by tools/gen_flag_rules.py.
//
// An achievement unlocks when what it counts reaches its target while the
// player plays: each rule's count is taken when a stage is set up and checked
// every tick after, and a Shine's is checked as its flag is set (the ending
// sets the last one outside any stage). A file that already has it all
// unlocks nothing by being loaded.
#include "achievements.h"

#include "port_ext.h"

#include <System/FlagManager.hpp>

namespace {

enum Kind { FLAGS_ALL, FLAGS_SHINES };

struct Rule {
	int id;
	Kind kind;
	int need;  // how many of its flags (FLAGS_SHINES: Shines on the file)
	int first; // its flags in kPool
	int count;
};

const Rule kRules[] = {
#include "ach_flag_rules.inc"
};
const unsigned kPool[] = { ACH_FLAG_POOL };
const int kRuleCount   = sizeof kRules / sizeof kRules[0];

int s_was[sizeof kRules / sizeof kRules[0]]; // each rule's count at the last look
bool s_known;                                // s_was holds this stage's counts

const unsigned SHINE_COUNT = 0x40000; // TFlagManager's Shines on the file

int count(const Rule& r)
{
	TFlagManager* flags = TFlagManager::getInstance();
	if (r.kind == FLAGS_SHINES)
		return flags->getFlag(SHINE_COUNT);
	int n = 0;
	for (int i = 0; i < r.count; ++i)
		n += flags->getFlag(kPool[r.first + i]) != 0;
	return n;
}

// Every rule's count now; those that reached their target since the last look unlock.
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
	if (!ach::on())
		return;
	look(false);
	s_known = true;
}

void afterMovement()
{
	if (ach::on() && s_known && ach::playing())
		look(true);
}

// A Shine's flag was set for the first time: the rules it counts towards are
// checked at once, against the count without it (the ending sets the final
// Shine's flag where no stage ticks).
void shineCollected(int shine, int total)
{
	if (!ach::on())
		return;
	const unsigned flag = 0x10000 + (unsigned)shine;
	for (int i = 0; i < kRuleCount; ++i) {
		const Rule& r = kRules[i];
		int now = count(r), before = now;
		if (r.kind == FLAGS_SHINES) {
			now    = total;
			before = total - 1;
		} else {
			bool counts = false;
			for (int k = 0; k < r.count; ++k)
				counts |= kPool[r.first + k] == flag;
			if (!counts)
				continue;
			before = now - 1;
		}
		if (before < r.need && now >= r.need)
			ach::unlock(r.id);
		if (s_known)
			s_was[i] = now;
	}
}

// After a file is chosen the counts are its own: nothing is compared until a stage is set up.
void afterFileSelect()
{
	s_known = false;
}

PortExtension s_extension;

struct Register {
	Register()
	{
		s_extension.name            = "achievements-flags";
		s_extension.stageSetup      = stageSetup;
		s_extension.afterMovement   = afterMovement;
		s_extension.shineCollected  = shineCollected;
		s_extension.afterFileSelect = afterFileSelect;
		port_ext_register(&s_extension);
	}
} s_register;

} // namespace
