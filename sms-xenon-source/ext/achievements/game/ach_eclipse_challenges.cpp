// Achievements (ext/achievements/README.md), Super Mario Eclipse's: 1-Ups,
// Power Stars and its challenges. Areas and episodes are Eclipse's
// (stageArc.bin; Eclipse's src/stage.cpp names them): 61 Erto Rock, 62
// Lighthouse Island, 63 Warship Island, 64 to 66 Hotel Lacrima (the beach,
// the hotel, the back house), 67 Pianta Pit, 68 Daisy Cruiser, 69 Mario's
// Dream, 70 Lancia Fredda Peaks, 71 Vaporwave, 72 Yoshi Village, 74 Red Lily
// City, 75 Peach Beach, 76 Spettro Casino, 77 Peach's Castle (episodes 0 to 4
// the grounds and floors, 5 to 34 its paintings), 78 the trails, 80 the
// Tutorial, 81 to 92 the secrets; 49 is Corona Mountain's Fire Shrine.
//
//   1-Ups (52-75, 106-117): so many of the stage's 1-Ups taken in one visit,
//     counted as Sunshine's are (ach_oneups.cpp), among all of them, the ones a
//     box or an event lets out too: where a stage has more than the
//     achievement asks, any of them count.
//   Power Stars (76-105): the paintings' Shines (event 86, a Shine the file
//     has, so nothing is saved) but the 100-coin one, taken this session (since
//     the game started), as many as the achievement asks.
//   1 Trials Passed: the Tutorial's temple gate opens (Eclipse sets its stage
//     bool 0x50062, src/stage/tutorial.cpp).
//   118 Hands-On Approach: the airstrip's first Shine (0x10056) while Mario
//     hasn't had F.L.U.D.D. this visit.
//   119 Back Door: the flooded plaza (episode 9, dolpic9) reached while the
//     Gatekeeper's flag (0x10384) isn't set.
//   120 Sinking Feeling: a death in goop in Erto Rock's Episode 7.
//   121 Minefield Mistake: 8 health or more lost at once in Warship Island's
//     Episode 1 (its mines take that much; nothing else there does).
//   122 Hot Delivery: Hotel Lacrima's Episode 8 timer stopped (the delivery)
//     with 1 second or less left.
//   123 Cleaning House: a Hinokuri2 (the goop monster) defeated in Hotel Lacrima.
//   124 Too Hot to Handle: a talk in Yoshi Village's Episode 1 (the burning
//     house's Yoshi; APPROXIMATE: any talk there).
//   125 Banana Dash: Peach Beach's Episode 2 timer stopped under 30 seconds.
//   126 No Fan Club: a talk at Ricco Station (APPROXIMATE: any talk there).
//   127 Mirror Match: a race's timer started while playing as Il Piantissimo.
//   128 Player Two: Luigi near the courtyard's star statue (its plaque,
//     peachCastle0's "backsign", at -230, 12640) in the castle grounds.
//
// Which character is played (Eclipse's character select): the player's
// archive, mounted as "mario" whichever it is, has a file only Luigi's
// (bck/ma_jump.bck) or Il Piantissimo's (bck/ma_hot_wait.bck) has.
#include "achievements.h"

#include "port_ext.h"

#include <Enemy/Hinokuri2.hpp>
#include <JSystem/JKernel/JKRFileLoader.hpp>
#include <MoveBG/ItemManager.hpp>
#include <MoveBG/MapObjItem2.hpp>
#include <Player/Mario.hpp>
#include <System/FlagManager.hpp>
#include <math.h>
#include <string.h>

namespace {

using namespace ach;

const int ANY = -1;

struct OneUps {
	int id, area, episode, need;
};
const OneUps kOneUps[] = {
	{ 52, 80, ANY, 4 }, // the Tutorial
	{ 53, 49, ANY, 2 }, // Corona Mountain's Fire Shrine (coro_ex3)
	{ 54, 61, ANY, 2 },  { 55, 81, ANY, 2 }, // Erto Rock, the Secret of the Shed
	{ 56, 62, ANY, 3 },  { 57, 82, ANY, 2 }, // Lighthouse Island, the Secret of the Basement
	{ 58, 63, ANY, 4 },  { 59, 83, ANY, 2 }, // Warship Island, the (Sub)Marine Biology Secret
	{ 60, 64, ANY, 1 },  { 61, 65, ANY, 1 }, // Lacrima's beach, the hotel
	{ 62, 89, ANY, 3 },                      // the Secret of the Air Vent
	{ 63, 67, ANY, 6 },                      // Pianta Pit
	{ 64, 68, ANY, 4 },  { 65, 85, ANY, 2 }, // Daisy Cruiser, Planes and Trains
	{ 66, 69, ANY, 4 },                      // Mario's Dream
	{ 67, 70, ANY, 4 },  { 68, 86, ANY, 2 }, // Lancia Fredda Peaks, the Secret of the Ice Cave
	{ 69, 71, ANY, 1 },  { 70, 87, ANY, 2 }, // Vaporwave, Memphis Mania
	{ 71, 72, ANY, 12 },                     // Yoshi Village
	{ 72, 74, ANY, 4 },                      // Red Lily City
	{ 73, 75, ANY, 5 },                      // Peach Beach
	{ 74, 76, ANY, 2 },  { 75, 92, ANY, 2 }, // Spettro Casino, the Secret of the Haunted Rook
	// the castle: its basement and paintings
	{ 106, 77, 4, 2 },   { 107, 77, 7, 1 },  { 108, 77, 8, 2 },  { 109, 77, 9, 1 },  { 110, 77, 10, 9 },
	{ 111, 77, 16, 3 },  { 112, 77, 17, 1 }, { 113, 77, 19, 1 }, { 114, 77, 20, 3 }, { 115, 77, 22, 7 },
	{ 116, 77, 31, 1 },  { 117, 77, 34, 2 },
};
const int kOneUpCount = sizeof kOneUps / sizeof kOneUps[0];

// The castle's paintings: episode (and another with the same painting), Power Stars.
struct Stars {
	int id, episode, alsoEpisode, need;
};
const Stars kStars[] = {
	{ 76, 3, ANY, 1 },   { 77, 6, ANY, 1 },   { 78, 7, ANY, 1 },   { 79, 8, ANY, 1 },   { 80, 9, ANY, 1 },
	{ 81, 10, ANY, 4 },  { 82, 11, ANY, 1 },  { 83, 12, ANY, 1 },  { 84, 13, ANY, 1 },  { 85, 14, ANY, 1 },
	{ 86, 15, ANY, 1 },  { 87, 16, ANY, 2 },  { 88, 17, ANY, 3 },  { 89, 18, ANY, 1 },  { 90, 19, ANY, 1 },
	{ 91, 20, ANY, 1 },  { 92, 21, ANY, 1 },  { 93, 22, ANY, 3 },  { 94, 23, ANY, 1 },  { 95, 24, ANY, 1 },
	{ 96, 25, ANY, 2 },  { 97, 26, ANY, 2 },  { 98, 27, ANY, 1 },  { 99, 28, ANY, 1 },  { 100, 29, ANY, 2 },
	{ 101, 30, ANY, 1 }, { 102, 31, ANY, 2 }, { 103, 32, ANY, 1 },
	{ 104, 33, 5, 1 }, // Click Clock Wood: the castle has its painting twice
	{ 105, 34, ANY, 2 },
};
const int kStarCount = sizeof kStars / sizeof kStars[0];

const int CASTLE = 77;

enum Character { MARIO, LUIGI, PIANTISSIMO };

Character character()
{
	if (JKRGetResource("/mario/bck/ma_jump.bck"))
		return LUIGI;
	if (JKRGetResource("/mario/bck/ma_hot_wait.bck"))
		return PIANTISSIMO;
	return MARIO;
}

bool in(int a, int e)
{
	return area() == a && (e == ANY || episode() == e);
}

// --- 1-Ups ---------------------------------------------------------------

int s_oneUpsWas[sizeof kOneUps / sizeof kOneUps[0]];

int oneUpsTaken()
{
	int n = 0;
	if (gpItemManager)
		for (int i = 0; i < gpItemManager->getObjNum(); ++i)
			if (TMushroom1up* m = dynamic_cast<TMushroom1up*>(gpItemManager->getObj(i)))
				n += m->unk13A == 1;
	return n;
}

// --- Power Stars -----------------------------------------------------------

// Each painting's Power Stars taken this session, by where they were.
struct Taken {
	int n;
	float x[8], z[8];
};
Taken s_stars[35];

// The 100-coin Shine: its name, "シャイン（１００枚コイン用）" in Shift-JIS, has "１００".
struct Named : JDrama::TNameRef {
	static const char* of(const JDrama::TNameRef* r) { return static_cast<const Named*>(r)->mName; }
};

bool hundredCoinShine(void* shine)
{
	const char* name = Named::of(static_cast<THitActor*>(shine));
	return name && strstr(name, "\x82\x50\x82\x4f\x82\x4f");
}

void powerStar(void* shine)
{
	const int e = episode();
	if (area() != CASTLE || e < 0 || e >= 35 || hundredCoinShine(shine))
		return;
	THitActor* actor = static_cast<THitActor*>(shine);
	Taken& t         = s_stars[e];
	for (int k = 0; k < t.n; ++k)
		if (fabsf(t.x[k] - actor->mPosition.x) < 50 && fabsf(t.z[k] - actor->mPosition.z) < 50)
			return; // that one again
	if (t.n < 8) {
		t.x[t.n] = actor->mPosition.x;
		t.z[t.n] = actor->mPosition.z;
		++t.n;
	}
	for (int i = 0; i < kStarCount; ++i)
		if ((kStars[i].episode == e || kStars[i].alsoEpisode == e) && t.n >= kStars[i].need)
			unlock(kStars[i].id);
	if (debug())
		log("Power Star %d of painting %d", t.n, e);
}

// --- the challenges ----------------------------------------------------------

bool s_gateWas;     // 1: the temple gate's bool at the last look
bool s_airstripWas; // 118: the airstrip's first Shine at the last look
bool s_hadFludd;    // 118: Mario had F.L.U.D.D. this visit
int s_health = -1;  // 121
int s_timerLimit;   // the timer's limit as it started (0 counting up)

void stageSetup()
{
	if (!eclipseOn())
		return;
	for (int i = 0; i < kOneUpCount; ++i)
		s_oneUpsWas[i] = 0;
	TFlagManager* flags = TFlagManager::getInstance();
	s_gateWas           = flags->getFlag(0x50062) != 0;
	s_airstripWas       = flags->getFlag(0x10056) != 0;
	s_hadFludd          = false;
	s_health            = -1;
	// 119: the flooded plaza, the Gatekeeper still there
	if (in(1, 9) && !flags->getFlag(0x10384))
		unlock(119);
	if (debug() && area() >= 61)
		log("eclipse: playing as %s", character() == LUIGI ? "Luigi" : character() == PIANTISSIMO ? "Il Piantissimo" : "Mario");
}

void afterMovement()
{
	if (!eclipseOn() || !playing())
		return;
	TMario* m = mario();
	// 1-Ups
	const int taken = oneUpsTaken();
	for (int i = 0; i < kOneUpCount; ++i) {
		const OneUps& r = kOneUps[i];
		if (!in(r.area, r.episode))
			continue;
		if (s_oneUpsWas[i] < r.need && taken >= r.need)
			unlock(r.id);
		s_oneUpsWas[i] = taken;
	}
	TFlagManager* flags = TFlagManager::getInstance();
	// 1: the temple gate
	const bool gate = flags->getFlag(0x50062) != 0;
	if (in(80, ANY) && gate && !s_gateWas)
		unlock(1);
	s_gateWas = gate;
	// 118: the airstrip's first Shine without F.L.U.D.D.
	if (m && m->checkFlag(MARIO_FLAG_HAS_FLUDD))
		s_hadFludd = true;
	const bool airstrip = flags->getFlag(0x10056) != 0;
	if (in(0, ANY) && airstrip && !s_airstripWas && !s_hadFludd)
		unlock(118);
	s_airstripWas = airstrip;
	// 121: a mine's 8 health at once
	if (m) {
		if (in(63, 0) && s_health >= 0 && s_health - m->mHealth >= 8)
			unlock(121);
		s_health = m->mHealth;
	}
	// 128: Luigi at the courtyard's star statue
	if (m && (in(CASTLE, 0) || in(CASTLE, 1))) {
		const float dx = m->mPosition.x - -230.0f, dz = m->mPosition.z - 12640.0f;
		if (dx * dx + dz * dz < 600.0f * 600.0f && character() == LUIGI)
			unlock(128);
	}
}

void shine(void* s, int what)
{
	if (eclipseOn() && what == PORT_SHINE_GET)
		powerStar(s);
}

void playerDeath(void*, int cause)
{
	if (eclipseOn() && cause == PORT_DEATH_GOOP && in(61, 6))
		unlock(120);
}

void actorDeath(void* actor, int)
{
	if (!eclipseOn() || area() < 64 || area() > 66)
		return;
	if (dynamic_cast<THinokuri2*>(static_cast<THitActor*>(actor)))
		unlock(123);
}

void talk(int what, void*, unsigned*, unsigned)
{
	if (!eclipseOn() || what != PORT_TALK_START)
		return;
	if (in(72, 0))
		unlock(124);
	if (in(78, 7))
		unlock(126);
}

void timer(int what, int time)
{
	if (!eclipseOn())
		return;
	if (what == PORT_TIMER_START) {
		s_timerLimit = time;
		// 127: a race (counting up) as Il Piantissimo, in his races' stages
		if (time == 0 && character() == PIANTISSIMO
		    && (in(4, ANY) || in(8, ANY) || in(9, ANY) || in(62, ANY)))
			unlock(127);
		return;
	}
	if (what != PORT_TIMER_STOP)
		return;
	const int elapsed = s_timerLimit > 0 ? s_timerLimit - time : time;
	if (debug())
		log("timer stopped at %d.%02d (limit %d)", time / 100, time % 100, s_timerLimit);
	// 122: the pizza delivered with 1 second or less left
	if (s_timerLimit > 0 && time <= 100 && (in(64, 7) || in(65, 7) || in(66, 7)))
		unlock(122);
	// 125: Peach Beach's race under 30 seconds
	if (in(75, 1) && elapsed < 3000)
		unlock(125);
}

PortExtension s_extension;

struct Register {
	Register()
	{
		s_extension.name          = "achievements-eclipse";
		s_extension.stageSetup    = stageSetup;
		s_extension.afterMovement = afterMovement;
		s_extension.shine         = shine;
		s_extension.playerDeath   = playerDeath;
		s_extension.actorDeath    = actorDeath;
		s_extension.talk          = talk;
		s_extension.timer         = timer;
		port_ext_register(&s_extension);
	}
} s_register;

} // namespace
