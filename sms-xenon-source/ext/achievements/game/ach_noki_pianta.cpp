// Achievements (ext/achievements/README.md): Noki Bay's and Pianta Village's
// challenges.
//
//   123 Leave No Trace: Noki Bay episode 1 without revealing a hidden
//       platform (the seven wall rocks under the goop) or touching goop
//   124 The Long Way Round: Noki Bay episode 2 without revealing the hidden path
//       (the sinking walls the sprayed painting sets off) or carrying the
//       springboard
//   125 High and Dry: Il Piantissimo's race of Noki Bay episode 5 under
//       40 seconds, without swimming or hovering
//   126 Ropes Are Optional: into The Shell's Secret from Noki Bay episode 6
//       without a rope or the Rocket Nozzle
//   127 Bare Hands: The Shell's Secret's Shine without the pole, the rope
//       or F.L.U.D.D.
//   131 Cannonball: from the Shine platform straight into the hot
//       spring, by day in Pianta Village
//   132 Gentle Touch: Pianta Village episode 1
//       without stunning a Chain Chomplet
//   133 Trunk Line: the flagpole of Pianta Village episode 2, only on tree
//       trunks past the first Pianta
//   134 Above It All: Pianta Village episode 3 without going
//       under the village or taking damage
//   135 Bath Time: the Chain Chomp's Shine of Pianta Village
//       episode 4 appears while Mario stands on the dirt path
//   136 Keep Your Cool: Secret of the Village Underside's Shine without
//       enraging a Chuckster
//   137 Treetop Rescue: every Pianta of Pianta Village episode 6 rescued,
//       only on treetops and mushrooms past the bridge
//
// An attempt starts as its stage is set up (entering the episode, or coming
// back to it after a lost life) and ends for the visit when the player does
// what it forbids; the reference set's rules, positions and ids are kept.
#include "achievements.h"

#include "port_ext.h"

#include <Enemy/Conductor.hpp>
#include <Enemy/EnemyManager.hpp>
#include <Enemy/FireWanwan.hpp>
#include <GC2D/GCConsole2.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <JSystem/JDrama/JDRViewObjPtrList.hpp>
#include <Map/MapData.hpp>
#include <Map/MapEventMare.hpp>
#include <MoveBG/Item.hpp>
#include <NPC/NpcBase.hpp>
#include <Player/Mario.hpp>
#include <Player/MarioStatus.hpp>
#include <Player/WaterGun.hpp>
#include <System/Application.hpp>
#include <System/FlagManager.hpp>
#include <System/MarDirector.hpp>

namespace {

using namespace ach;

// Actor types (THitActor::getActorType) of what Mario holds or hangs on.
const unsigned ACTOR_SPRING = 0x40000017; // the springboard he can carry
const unsigned ACTOR_POLE   = 0x4000002F;
const unsigned ACTOR_ROPE   = 0x40000098;

// The ground's kind of sound (TBGCheckData::unk6, the low byte of
// TMario::mSoundFlags), which tells what it is made of.
enum {
	GROUND_WOOD       = 0x07,
	GROUND_DIRT       = 0x08,
	GROUND_TRUNK      = 0x09,
	GROUND_LEAF       = 0x0E,
	GROUND_RIGID_LEAF = 0x15,
	GROUND_MUSHROOM   = 0x1E,
};

// The stage's flags (TFlagManager), reset as each stage loads.
const unsigned FLAG_RACE_GOAL       = 0x50005; // Mario touched a race's goal flag (TGoalFlag)
const unsigned FLAG_PIANTAS_RESCUED = 0x60002; // Pianta Village episode 6's count

// The Shines (TShine's event id, the file's Shine number).
enum {
	SHINE_NOKI_1   = 0x32,
	SHINE_NOKI_2   = 0x33,
	SHINE_NOKI_6   = 0x37,
	SHINE_PIANTA_1 = 0x3C,
	SHINE_PIANTA_3 = 0x3E,
	SHINE_PIANTA_5 = 0x40,
};

// Il Piantissimo's time in Noki Bay, in the HUD timer's hundredths.
const int NOKI_RACE_TIME = 4000;

// Pianta Village's places, as the reference set marks them.
const float SHINE_PLATFORM_Y = 9500.0f;  // at or above: the Shine's platform
const float HOT_SPRING_Z     = -5500.0f; // at or below: the hot spring
const float FIRST_PIANTA_Z   = 10025.0f; // below: past episode 2's first Pianta
const float UNDERSIDE_Y      = -1100.0f; // below: under the village
const float BRIDGE_Z         = 11550.0f; // below: past episode 6's bridge

const int kMaxRocks     = 8;
const int kMaxWalls     = 4;
const int kMaxChomplets = 8;
const int kMaxNpcs      = 32;

// This visit's attempts (each on while it can still be earned) and what they watch.
struct Visit {
	bool noki1, noki2, noki5, noki6, shell, pianta1, pianta2, pianta3, underside, pianta6;
	bool onShinePlatform; // 131: came from the Shine platform, on wood since
	bool raceGoal;        // FLAG_RACE_GOAL at the last look
	int rescued;          // FLAG_PIANTAS_RESCUED at the last look
	int health;           // Mario's health at the last look (-1 before the first)
	TMareWallRock* rocks[kMaxRocks];
	int rockCount;
	TMareEventDepressWall* walls[kMaxWalls];
	int wallCount;
	TFireWanwan* chomplets[kMaxChomplets];
	int chompletCount;
	TBaseNPC* npcs[kMaxNpcs];
	int npcCount;
};
Visit s;

void fail(bool& attempt, int id, const char* why)
{
	if (!attempt)
		return;
	attempt = false;
	if (debug())
		log("achievement %d: not this time (%s)", id, why);
}

// Mario stands on the ground (exactly on it, as the reference set has it).
bool grounded(const TMario* m)
{
	return m->mPosition.y == m->mFloorPosition.y;
}

// What the ground under Mario is made of (GROUND_*), or -1.
int ground(const TMario* m)
{
	return m->mGroundPlane ? m->mGroundPlane->unk6 : -1;
}

bool swimming(const TMario* m)
{
	return m->mStatus >= MARIO_STATUS_SWIM_START && m->mStatus <= MARIO_STATUS_SWIM_DIVE;
}

// F.L.U.D.D. sprays, hovers or launches.
bool pumping(const TMario* m)
{
	return m->mUpperState == TMario::UPPER_STATE_PUMPING;
}

// F.L.U.D.D. has `nozzle` on and is sending water out of it.
bool usingNozzle(const TMario* m, int nozzle)
{
	const TWaterGun* gun = m->mWaterGun;
	return gun && gun->mCurrentNozzle == nozzle && gun->mIsEmitWater == 1;
}

// Mario climbs or hangs on an object of `type`.
bool hangingOn(const TMario* m, unsigned type)
{
	return m->mHolder && m->mHolder->getActorType() == type;
}

bool carrying(const TMario* m, unsigned type)
{
	return m->mHeldObject && m->mHeldObject->getActorType() == type;
}

// The stage's map group (Noki Bay's wall rocks and sinking walls are in it).
JDrama::TViewObjPtrListT<JDrama::TViewObj>* mapGroup()
{
	if (!JDrama::TNameRefGen::getInstance() || !JDrama::TNameRefGen::getInstance()->getRootNameRef())
		return nullptr;
	// "マップグループ", as the disc spells it (Shift-JIS)
	return JDrama::TNameRefGen::search<JDrama::TViewObjPtrListT<JDrama::TViewObj> >(
	    "\x83\x7d\x83\x62\x83\x76\x83\x4f\x83\x8b\x81\x5b\x83\x76");
}

// Noki Bay's hidden platforms (the wall rocks that rise once their goop is
// cleaned) and hidden paths (the walls that sink once a painting is sprayed).
void findMareObjects()
{
	JDrama::TViewObjPtrListT<JDrama::TViewObj>* group = mapGroup();
	if (!group)
		return;
	JGadget::TList_pointer<JDrama::TViewObj*>& objs = group->getChildren();
	for (JGadget::TList_pointer<JDrama::TViewObj*>::iterator it = objs.begin(); it != objs.end(); ++it) {
		if (TMareWallRock* rock = dynamic_cast<TMareWallRock*>(*it)) {
			if (s.rockCount < kMaxRocks)
				s.rocks[s.rockCount++] = rock;
		} else if (TMareEventDepressWall* wall = dynamic_cast<TMareEventDepressWall*>(*it)) {
			if (s.wallCount < kMaxWalls)
				s.walls[s.wallCount++] = wall;
		}
	}
	if (debug())
		log("Noki Bay: %d wall rocks, %d sinking walls", s.rockCount, s.wallCount);
}

// Every enemy of the stage's managers that is a T (the Chain Chomplets, the NPCs).
template <class T> int findEnemies(T** out, int max)
{
	int n = 0;
	if (!gpConductor)
		return 0;
	for (JGadget::TList<TEnemyManager*>::iterator it = gpConductor->unk20.begin(); it != gpConductor->unk20.end();
	     ++it) {
		TEnemyManager* mgr = *it;
		if (!mgr)
			continue;
		for (int i = 0; i < mgr->getObjNum() && n < max; ++i)
			if (T* actor = dynamic_cast<T*>(mgr->getObj(i)))
				out[n++] = actor;
	}
	return n;
}

void stageSetup()
{
	s = Visit();
	s.health = -1;
	if (!on())
		return;
	const int a = area(), e = episode();
	s.noki1     = a == NOKI && e == 0;
	s.noki2     = a == NOKI && e == 1;
	s.noki5     = a == NOKI && e == 4;
	s.noki6     = a == NOKI && e == 5;
	s.shell     = a == SHELL_SECRET;
	s.pianta1   = a == PIANTA && e == 0;
	s.pianta2   = a == PIANTA && e == 1;
	s.pianta3   = a == PIANTA && e == 2;
	s.underside = a == VILLAGE_UNDERSIDE;
	s.pianta6   = a == PIANTA && e == 5;
	if (s.noki1 || s.noki2) {
		findMareObjects();
		if (s.noki1)
			require(123, "the 7 hidden wall rocks", s.rockCount == 7);
		if (s.noki2)
			require(124, "the 3 sinking walls", s.wallCount == 3);
	}
	if (s.pianta1) {
		s.chompletCount = findEnemies<TFireWanwan>(s.chomplets, kMaxChomplets);
		require(132, "the Chain Chomplets", s.chompletCount > 0);
		if (debug())
			log("Pianta Village: %d Chain Chomplets", s.chompletCount);
	}
	if (s.underside) {
		s.npcCount = findEnemies<TBaseNPC>(s.npcs, kMaxNpcs);
		require(136, "the Chucksters and Piantas", s.npcCount > 0);
		if (debug())
			log("Village Underside: %d NPCs", s.npcCount);
	}
	if (TFlagManager* flags = TFlagManager::getInstance()) {
		s.raceGoal = flags->getBool(FLAG_RACE_GOAL);
		s.rescued  = flags->getFlag(FLAG_PIANTAS_RESCUED);
	}
}

void afterMovement()
{
	if (!on() || !playing())
		return;
	TMario* m = mario();
	TFlagManager* flags = TFlagManager::getInstance();
	if (!m || !flags)
		return;

	// The race's goal flag was touched this tick.
	const bool raceGoal    = flags->getBool(FLAG_RACE_GOAL);
	const bool reachedGoal = raceGoal && !s.raceGoal;
	s.raceGoal             = raceGoal;
	const bool hurt        = s.health >= 0 && m->mHealth < s.health;
	s.health               = m->mHealth;
	const bool onGround    = grounded(m);
	const int kind         = ground(m);

	if (s.noki1) {
		if (m->mDirty != 0.0f)
			fail(s.noki1, 123, "touched goop");
		for (int i = 0; i < s.rockCount; ++i)
			if (s.rocks[i]->unkF4 != 0) // 0 until its goop is cleaned and it rises
				fail(s.noki1, 123, "revealed a hidden platform");
	}

	if (s.noki2) {
		if (carrying(m, ACTOR_SPRING))
			fail(s.noki2, 124, "grabbed the springboard");
		for (int i = 0; i < s.wallCount; ++i)
			if (s.walls[i]->unk44 != 0) // 0 until its painting is sprayed
				fail(s.noki2, 124, "revealed the hidden path");
	}

	if (s.noki5) {
		if (swimming(m))
			fail(s.noki5, 125, "swam");
		if (usingNozzle(m, TWaterGun::Hover))
			fail(s.noki5, 125, "hovered");
		TGCConsole2* hud = gpMarDirector ? gpMarDirector->getConsole() : nullptr;
		if (s.noki5 && reachedGoal && hud && hud->getFinishedTime() < NOKI_RACE_TIME)
			unlock(125);
	}

	if (s.noki6) {
		if (hangingOn(m, ACTOR_ROPE))
			fail(s.noki6, 126, "used a rope");
		if (usingNozzle(m, TWaterGun::Rocket))
			fail(s.noki6, 126, "used the Rocket Nozzle");
	}

	if (s.shell) {
		if (hangingOn(m, ACTOR_POLE) || hangingOn(m, ACTOR_ROPE))
			fail(s.shell, 127, "used the pole or the rope");
		if (pumping(m))
			fail(s.shell, 127, "used F.L.U.D.D.");
	}

	// 131: by day (episodes 2, 4, 6 and 8), from the Shine platform on wood alone into the hot spring
	if (area() == PIANTA && (episode() & 1)) {
		if ((onGround && kind != GROUND_WOOD) || pumping(m)) {
			s.onShinePlatform = false;
		} else {
			if (onGround && kind == GROUND_WOOD && m->mPosition.y >= SHINE_PLATFORM_Y)
				s.onShinePlatform = true;
			if (s.onShinePlatform && swimming(m) && m->mPosition.z <= HOT_SPRING_Z)
				unlock(131);
		}
	}

	if (s.pianta1)
		for (int i = 0; i < s.chompletCount; ++i)
			if (s.chomplets[i]->isFreeze()) // landed dizzy after being thrown
				fail(s.pianta1, 132, "stunned a Chain Chomplet");

	if (s.pianta2) {
		if (onGround && kind != GROUND_TRUNK && m->mPosition.z < FIRST_PIANTA_Z)
			fail(s.pianta2, 133, "touched something besides a tree trunk");
		if (s.pianta2 && reachedGoal)
			unlock(133);
	}

	if (s.pianta3) {
		if (onGround && m->mPosition.y < UNDERSIDE_Y)
			fail(s.pianta3, 134, "went under the village");
		if (hurt)
			fail(s.pianta3, 134, "took damage");
	}

	if (s.underside)
		for (int i = 0; i < s.npcCount; ++i)
			if (s.npcs[i]->checkLiveFlag(LIVE_FLAG_UNK2000000)) // TBaseNPC::npcMadIn
				fail(s.underside, 136, "enraged a Chuckster");

	const int rescued = flags->getFlag(FLAG_PIANTAS_RESCUED);
	if (s.pianta6) {
		if (onGround && m->mPosition.z < BRIDGE_Z && kind != GROUND_LEAF && kind != GROUND_RIGID_LEAF
		    && kind != GROUND_MUSHROOM)
			fail(s.pianta6, 137, "touched something besides treetops and mushrooms");
		if (s.pianta6 && s.rescued == 9 && rescued == 10)
			unlock(137);
	}
	s.rescued = rescued;
}

void shine(void* actor, int what)
{
	if (!on())
		return;
	if (what == PORT_SHINE_APPEAR) {
		// 135: the Chain Chomp's jump into the spa is what brings its Shine out
		TMario* m = mario();
		if (area() == PIANTA && episode() == 3 && playing() && m && grounded(m) && ground(m) == GROUND_DIRT)
			unlock(135);
		return;
	}
	if (what != PORT_SHINE_GET)
		return;
	TShine* taken = dynamic_cast<TShine*>(static_cast<THitActor*>(actor));
	if (!taken)
		return;
	const int id = taken->mEventId & 0xFFFF;
	if (s.noki1 && id == SHINE_NOKI_1)
		unlock(123);
	if (s.noki2 && id == SHINE_NOKI_2)
		unlock(124);
	if (s.shell && id == SHINE_NOKI_6)
		unlock(127);
	if (s.pianta1 && id == SHINE_PIANTA_1)
		unlock(132);
	if (s.pianta3 && id == SHINE_PIANTA_3)
		unlock(134);
	if (s.underside && id == SHINE_PIANTA_5)
		unlock(136);
}

// 126: leaving Noki Bay episode 6 for The Shell's Secret (its warp).
void moveStage()
{
	if (on() && s.noki6 && SMSGetApplication()->mNextArea.getStage() == SHELL_SECRET)
		unlock(126);
}

PortExtension s_extension;

struct Register {
	Register()
	{
		s_extension.name          = "achievements-noki-pianta";
		s_extension.stageSetup    = stageSetup;
		s_extension.afterMovement = afterMovement;
		s_extension.shine         = shine;
		s_extension.moveStage     = moveStage;
		port_ext_register(&s_extension);
	}
} s_register;

} // namespace
