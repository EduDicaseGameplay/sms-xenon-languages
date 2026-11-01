// Achievements (ext/achievements/README.md): 1-Ups placed in the stages,
// one or all of an area's in one visit. Which 1-Ups is the reference set's
// choice: each is the item manager's object at a slot (its place in the
// manager's list, which the port keeps in the game's order), and a 1-Up
// mushroom (TMushroom1up) has been taken once its unk13A is 1. An area's
// objects are made again each time it loads, so "in one visit" is simply all
// of them taken at once.
//
// SMS_ACHIEVEMENTS_DEBUG=1 lists each stage's 1-Ups with their slots.
#include "achievements.h"

#include "port_ext.h"

#include <MoveBG/ItemManager.hpp>
#include <MoveBG/MapObjItem2.hpp>
#include <stdio.h>

namespace {

// The reference set names an object by its slot as the GameCube's memory has
// it: 4 bytes a slot, from kSlotBase.
const int kSlotBase = 0xAC; // every slot below is checked by the tour (SMS_ACHIEVEMENTS_TEST=tour)
int index(int slot)
{
	return (slot - kSlotBase) / 4;
}

const int ANY = -1;

struct OneUps {
	int id;      // the achievement
	int area;    // ach::Area
	int episode; // the scenario, or ANY
	bool all;    // all of them (else the first one)
	int slots[4];
};

using namespace ach;

// Episode N is scenario N-1; the plaza's and Pinna Park's states and the
// casino's are their scenarios too.
const OneUps kOneUps[] = {
	{ 63, PLAZA, 0, true, { 0x280, 0x284, 0x288, 0x3a0 } },
	{ 63, PLAZA, 1, true, { 0x280, 0x284, 0x288, 0x3a4 } },
	{ 63, PLAZA, 5, true, { 0x234, 0x238, 0x23c, 0x368 } },
	{ 63, PLAZA, 6, true, { 0x280, 0x284, 0x288, 0x3b8 } },
	// the reference has 0x3b8 and 0x3d0 for these two; the tour found each fourth 1-Up a slot later
	{ 63, PLAZA, 7, true, { 0x280, 0x284, 0x288, 0x3bc } },
	{ 63, PLAZA, 8, true, { 0x298, 0x29c, 0x2a0, 0x3d4 } },
	{ 64, LILY_PAD_RIDE, ANY, false, { 0xcc } },
	{ 80, BIANCO, 7, true, { 0x1fc, 0x20c, 0x2a8 } },
	{ 81, HILLSIDE_CAVE, ANY, true, { 0xcc, 0x128 } },
	{ 82, DIRTY_LAKE_SECRET, ANY, true, { 0xcc, 0xd0 } },
	{ 90, RICCO, 7, false, { 0x294 } }, // the duck-ring riders' reward
	{ 91, RICCO, 7, true, { 0x294, 0x2a4, 0x32c } },
	{ 92, RICCO_TOWER_SECRET, ANY, true, { 0xcc, 0xd0 } },
	{ 99, GELATO, 0, false, { 0x2cc } },
	{ 99, GELATO, 1, false, { 0x2fc } },
	{ 99, GELATO, 2, false, { 0x2b4 } },
	{ 99, GELATO, 3, false, { 0x2c8 } },
	{ 99, GELATO, 4, false, { 0x2a8 } },
	{ 99, GELATO, 5, false, { 0x2d4 } },
	{ 99, GELATO, 6, false, { 0x330 } },
	{ 99, GELATO, 7, false, { 0x29c } },
	{ 100, SAND_CASTLE_SECRET, ANY, true, { 0xcc, 0x128 } },
	{ 101, SAND_BIRD, ANY, false, { 0xc0 } },
	{ 106, PINNA_BEACH, 0, false, { 0xd4 } },
	{ 106, PINNA_BEACH, 1, false, { 0xd4 } },
	{ 106, PINNA_BEACH, 2, false, { 0xd8 } },
	{ 106, PINNA_BEACH, 3, false, { 0xd4 } },
	{ 106, PINNA_BEACH, 4, false, { 0xd8 } },
	{ 106, PINNA_BEACH, 5, false, { 0xd8 } },
	{ 106, PINNA_BEACH, 6, false, { 0xd8 } },
	{ 106, PINNA_BEACH, 7, false, { 0xd8 } },
	{ 107, PINNA_PARK, 0, false, { 0x1bc } },
	{ 107, PINNA_PARK, 1, false, { 0x1e4 } },
	{ 107, PINNA_PARK, 2, false, { 0x1b4 } },
	{ 107, PINNA_PARK, 3, false, { 0x1b8 } },
	{ 107, PINNA_PARK, 4, false, { 0x1b4 } },
	{ 107, PINNA_PARK, 5, false, { 0x204 } },
	{ 108, BEACH_CANNON, ANY, true, { 0xd4, 0x134 } },
	{ 109, YOSHI_GO_ROUND, ANY, true, { 0xcc, 0x128, 0x12c } },
	{ 113, SIRENA_BEACH, 0, false, { 0x1c0 } },
	{ 113, SIRENA_BEACH, 1, false, { 0x11c } },
	{ 113, SIRENA_BEACH, 2, false, { 0x11c } },
	{ 113, SIRENA_BEACH, 3, false, { 0x11c } },
	{ 113, SIRENA_BEACH, 4, false, { 0x11c } },
	{ 113, SIRENA_BEACH, 5, false, { 0x12c } },
	{ 113, SIRENA_BEACH, 6, false, { 0x120 } },
	{ 113, SIRENA_BEACH, 7, false, { 0x120 } },
	{ 114, CASINO, 0, false, { 0x104 } },
	{ 114, CASINO, 1, false, { 0x124 } },
	{ 115, HOTEL_LOBBY_SECRET, ANY, true, { 0xe8, 0x14c, 0x150 } },
	{ 116, CASINO_SECRET, ANY, true, { 0xcc, 0xd0 } },
	{ 120, NOKI, 3, true, { 0x298, 0x29c, 0x2a0, 0x2a4 } },
	{ 120, NOKI, 4, true, { 0x298, 0x29c, 0x2a0, 0x2a4 } },
	{ 120, NOKI, 6, true, { 0x29c, 0x2a0, 0x2a4, 0x2a8 } },
	{ 120, NOKI, 7, true, { 0x298, 0x29c, 0x2a0, 0x2a4 } },
	{ 121, NOKI, 5, false, { 0x374 } },
	{ 122, SHELL_SECRET, ANY, true, { 0x124, 0x128, 0x12c } },
	{ 128, PIANTA, 4, true, { 0x18c, 0x190, 0x194, 0x198 } },
	{ 129, PIANTA, 2, false, { 0x1a4 } },
	{ 130, VILLAGE_UNDERSIDE, ANY, true, { 0xe8, 0xec, 0xf0 } },
	{ 138, CORONA, ANY, true, { 0x13c, 0x1b8, 0x138 } },
};
const int kCount = sizeof kOneUps / sizeof kOneUps[0];

// The 1-Up at `slot` in this stage, or null if the slot holds something else.
TMushroom1up* oneUp(int slot)
{
	if (!gpItemManager)
		return nullptr;
	const int i = index(slot);
	if (i < 0 || i >= gpItemManager->getObjNum())
		return nullptr;
	return dynamic_cast<TMushroom1up*>(gpItemManager->getObj(i));
}

// What this stage's rules saw taken at the last look (bit per slot), so each unlocks as it happens.
unsigned s_taken[sizeof kOneUps / sizeof kOneUps[0]];

unsigned takenNow(const OneUps& r, bool* complete)
{
	unsigned bits = 0;
	int n = 0, have = 0;
	for (int k = 0; k < 4 && r.slots[k]; ++k) {
		++n;
		TMushroom1up* m = oneUp(r.slots[k]);
		if (m && m->unk13A == 1) {
			bits |= 1u << k;
			++have;
		}
	}
	*complete = r.all ? have == n && n > 0 : (bits & 1) != 0;
	return bits;
}

bool applies(const OneUps& r)
{
	return ach::area() == r.area && (r.episode == ANY || ach::episode() == r.episode);
}

void stageSetup()
{
	if (!ach::on())
		return;
	for (int i = 0; i < kCount; ++i) {
		bool done;
		s_taken[i] = applies(kOneUps[i]) ? takenNow(kOneUps[i], &done) : 0;
		if (applies(kOneUps[i]))
			for (int k = 0; k < 4 && kOneUps[i].slots[k]; ++k) {
				char what[48];
				snprintf(what, sizeof what, "the 1-Up at slot %#x", kOneUps[i].slots[k]);
				ach::require(kOneUps[i].id, what, oneUp(kOneUps[i].slots[k]) != nullptr);
			}
	}
	if (ach::debug() && gpItemManager)
		for (int i = 0; i < gpItemManager->getObjNum(); ++i)
			if (dynamic_cast<TMushroom1up*>(gpItemManager->getObj(i)))
				ach::log("1-Up at slot %#x (item %d)", kSlotBase + i * 4, i);
}

void afterMovement()
{
	if (!ach::on() || !ach::playing())
		return;
	for (int i = 0; i < kCount; ++i) {
		const OneUps& r = kOneUps[i];
		if (!applies(r))
			continue;
		bool done;
		const unsigned now = takenNow(r, &done);
		if (now != s_taken[i] && done)
			ach::unlock(r.id);
		s_taken[i] = now;
	}
}

PortExtension s_extension;

struct Register {
	Register()
	{
		s_extension.name          = "achievements-oneups";
		s_extension.stageSetup    = stageSetup;
		s_extension.afterMovement = afterMovement;
		port_ext_register(&s_extension);
	}
} s_register;

} // namespace
