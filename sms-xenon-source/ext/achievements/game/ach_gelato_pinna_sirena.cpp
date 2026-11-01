// Achievements (ext/achievements/README.md): the challenges of Gelato Beach,
// Pinna Park and Sirena Beach.
//
//   102 Palm Tree Blast: Gelato 1, open the sand castle while standing on a
//       palm tree, then go in without touching the sand
//   103 Grass Is for Losers: Gelato 5, beat Il Piantissimo under 35 seconds without
//       touching grass or using F.L.U.D.D.
//   104 Duck Parade: Gelato 8, 10 Cataquacks
//       chasing at once
//   105 Duck Pond: Gelato 8, 15 Cataquacks sunk
//       in the sand at once
//   110 One Lap Wonder: Pinna 1, beat Mecha-Bowser within one lap of the
//       roller coaster
//   111 No Bombs Required: Pinna 2, go into the Beach Cannon's Secret
//       without picking up a Bob-omb
//   112 Fence-Free Flowers: Pinna 5, take its Shine without climbing a fence
//   117 Stairs Are Overrated: Sirena 2, go into the Hotel Lobby's Secret without a wall
//       kick or F.L.U.D.D.
//   118 Fruit Salad: Sirena 5, King Boo painted with all three fruits' colours
//   119 Spotless: Sirena 6, the beach clean with a minute left on
//       the timer, without refilling F.L.U.D.D.
//
// Every attempt lasts one visit (a stage load starts it again), as the
// reference set's resets on a loading screen do; King Boo's colours are kept
// while the player stays in his fight. Ways into a secret are seen as the
// stage leaves for it (moveStage).
#include "achievements.h"

#include "port_ext.h"

#include <Enemy/BossTelesaObj.hpp>
#include <Enemy/Conductor.hpp>
#include <Enemy/EnemyManager.hpp>
#include <Enemy/PoiHana.hpp>
#include <Enemy/TinKoopa.hpp>
#include <GC2D/GCConsole2.hpp>
#include <M3DUtil/MActor.hpp>
#include <Map/MapData.hpp>
#include <MoveBG/Item.hpp>
#include <MoveBG/MapObjMamma.hpp>
#include <MoveBG/MapObjManager.hpp>
#include <MoveBG/MapObjMonte.hpp>
#include <Player/Mario.hpp>
#include <Player/MarioStatus.hpp>
#include <Player/WaterGun.hpp>
#include <System/Application.hpp>
#include <System/MarDirector.hpp>

namespace {

using namespace ach;

// The ground's sound material (TBGCheckData::unk6), as the reference names them.
enum { GROUND_SAND = 0x05, GROUND_TREE_LEAF = 0x0E, GROUND_GRASS = 0x10 };

const int CATAQUACK      = 0x10000015; // TPoiHana's actor type
const int BOBOMB         = 0x1000001E;
const unsigned GOAL_FLAG = 0x50005;    // the stage's bool: the race's goal flag was reached
const u32 MSG_GOAL       = 0xE;        // what the goal flag sends whoever touches it

// Shines (TShine::mEventId)
const u32 SHINE_PINNA_5  = 0x22;
const u32 SHINE_SIRENA_6 = 0x2D;

// Cataquack animations (TSmallEnemy::mCurrentBckAnm): dashing after Mario,
// stomped while stuck, stuck in the sand.
enum { POIHANA_DASH = 0, POIHANA_STOMPED = 3, POIHANA_STUCK = 4 };

// King Boo's colours (TBossTelesa::unk348) after each fruit.
struct Colour {
	u8 r, g, b;
};
const Colour kKingBooColours[] = {
	{ 230, 100, 180 }, // coconut
	{ 230, 180, 0 },   // papaya, pineapple
	{ 150, 50, 230 },  // durian
};
const int kKingBooColourCount = sizeof kKingBooColours / sizeof kKingBooColours[0];

// This visit's attempts.
int s_area = -1, s_episode = -1;
unsigned s_visit;

TSandCastle* s_castle;   // 102: Gelato 1's sand castle
u16 s_castleState;       // its state at the last look
bool s_castleFromTree;   // opened from a palm tree, no sand touched since
bool s_raceSpoiled;      // 103: grass or F.L.U.D.D.
bool s_goalJustSet;      // the goal flag's bool went up this tick (who touched it comes next)
TTinKoopa* s_mecha;      // 110
bool s_lapped;           // the coaster went round once
float s_railFrame;       // where the coaster was at the last look
bool s_railKnown;
int s_mechaStage;        // Mecha-Bowser's plates knocked off at the last look
bool s_heldBobomb;       // 111
bool s_climbedFence;     // 112
bool s_hotelSpoiled;     // 117: wall kick or F.L.U.D.D.
TBossTelesa* s_kingBoo;  // 118
unsigned s_kingBooSeen;  // the colours he has worn (bit per kKingBooColours), kept through the fight
bool s_refilled;         // 119
int s_water;             // F.L.U.D.D.'s water at the last look
bool s_waterKnown;
bool s_timerStarted;     // the HUD's timer started this visit

bool in(int area, int episode)
{
	return s_area == area && s_episode == episode;
}

TMario* player()
{
	return ach::mario();
}

bool grounded(const TMario* m)
{
	const float d = m->mPosition.y - m->mFloorPosition.y;
	return d < 1.0f && d > -1.0f;
}

// Mario stands on ground of this sound material.
bool standsOn(const TMario* m, int material)
{
	return m->mGroundPlane && grounded(m) && m->mGroundPlane->unk6 == material;
}

// Mario is using F.L.U.D.D. (the trigger held, or water coming out).
bool usesFludd(const TMario* m)
{
	return m->mUpperState == TMario::UPPER_STATE_PUMPING;
}

bool isFenceStatus(unsigned status)
{
	return status == MARIO_STATUS_FENCE_CATCH || status == MARIO_STATUS_FENCE_JUMP_CATCH
	    || status == MARIO_STATUS_FENCE_MOVE || status == MARIO_STATUS_FENCE_PUNCH;
}

// The first enemy of type T the stage's managers have.
template <class T> T* findEnemy()
{
	if (!gpConductor)
		return nullptr;
	for (JGadget::TList<TEnemyManager*>::iterator it = gpConductor->unk20.begin(); it != gpConductor->unk20.end(); ++it)
		for (int i = 0; *it && i < (*it)->getObjNum(); ++i)
			if (T* e = dynamic_cast<T*>((*it)->getObj(i)))
				return e;
	return nullptr;
}

TSandCastle* findSandCastle()
{
	if (!gpMapObjManager)
		return nullptr;
	for (int i = 0; i < gpMapObjManager->getObjNum(); ++i)
		if (TSandCastle* c = dynamic_cast<TSandCastle*>(gpMapObjManager->getObj(i)))
			return c;
	return nullptr;
}

// The HUD's timer as it shows (hundredths of a second), and whether it runs.
int timerShown()
{
	return gpMarDirector && gpMarDirector->mConsole ? gpMarDirector->mConsole->getFinishedTime() : 0;
}

bool timerRunning()
{
	return gpMarDirector && gpMarDirector->mConsole && gpMarDirector->mConsole->unk4A == 1;
}

int allCataquacks();

void stageSetup()
{
	if (!ach::on())
		return;
	const int area = ach::area();
	if (area != KING_BOO_FIGHT || s_area != KING_BOO_FIGHT)
		s_kingBooSeen = 0;
	s_area    = area;
	s_episode = ach::episode();
	s_visit   = ach::visit();

	s_castle         = in(GELATO, 0) ? findSandCastle() : nullptr;
	s_castleState    = s_castle ? s_castle->mState : 0;
	s_castleFromTree = false;
	s_raceSpoiled    = false;
	s_goalJustSet    = false;
	s_mecha          = in(ROLLER_COASTER, 1) ? findEnemy<TTinKoopa>() : nullptr;
	s_mechaStage     = s_mecha ? s_mecha->mDamageStage : 0;
	s_lapped         = false;
	s_railKnown      = false;
	s_heldBobomb     = false;
	s_climbedFence   = false;
	s_hotelSpoiled   = false;
	s_kingBoo        = area == KING_BOO_FIGHT ? findEnemy<TBossTelesa>() : nullptr;
	s_refilled       = false;
	s_waterKnown     = false;
	s_timerStarted   = false;

	if (in(GELATO, 0))
		ach::require(102, "the sand castle", s_castle != nullptr);
	if (in(ROLLER_COASTER, 1))
		ach::require(110, "Mecha-Bowser", s_mecha != nullptr);
	if (area == KING_BOO_FIGHT)
		ach::require(118, "King Boo", s_kingBoo != nullptr);
	if (in(GELATO, 7)) {
		const int n = allCataquacks();
		ach::require(104, "10 Cataquacks", n >= 10);
		ach::require(105, "15 Cataquacks", n >= 15);
	}
}

// 102: the castle opens as its bud has taken enough water (grow: FIRING).
void lookSandCastle(TMario* m)
{
	if (!s_castle)
		return;
	const u16 state = s_castle->mState;
	if (state == TSandBombBase::STATE_FIRING && s_castleState != TSandBombBase::STATE_FIRING) {
		s_castleFromTree = standsOn(m, GROUND_TREE_LEAF);
		if (ach::debug())
			ach::log("102: sand castle opened %s", s_castleFromTree ? "from a palm tree" : "not from a palm tree");
	}
	s_castleState = state;
	if (s_castleFromTree && standsOn(m, GROUND_SAND)) {
		s_castleFromTree = false;
		if (ach::debug())
			ach::log("102: touched the sand");
	}
}

// 104, 105: the Cataquacks the stage has (alive or waiting in their managers' pools).
int allCataquacks()
{
	int n = 0;
	if (!gpConductor)
		return 0;
	for (JGadget::TList<TEnemyManager*>::iterator it = gpConductor->unk20.begin(); it != gpConductor->unk20.end(); ++it)
		if (dynamic_cast<TPoiHanaManager*>(*it))
			for (int i = 0; i < (*it)->getObjNum(); ++i) {
				TPoiHana* p = dynamic_cast<TPoiHana*>((*it)->getObj(i));
				n += p && p->mActorType == CATAQUACK;
			}
	return n;
}

// 104, 105: the Cataquacks chasing Mario and stuck in the sand, now.
void countCataquacks()
{
	if (!gpConductor)
		return;
	int chasing = 0, sunk = 0;
	for (JGadget::TList<TEnemyManager*>::iterator it = gpConductor->unk20.begin(); it != gpConductor->unk20.end(); ++it) {
		if (!dynamic_cast<TPoiHanaManager*>(*it))
			continue;
		for (int i = 0; i < (*it)->getObjNum(); ++i) {
			TPoiHana* p = dynamic_cast<TPoiHana*>((*it)->getObj(i));
			if (!p || p->mActorType != CATAQUACK || p->checkLiveFlag(LIVE_FLAG_DEAD | LIVE_FLAG_HIDDEN))
				continue;
			if (p->mCurrentBckAnm == POIHANA_DASH)
				++chasing;
			else if (p->mCurrentBckAnm == POIHANA_STOMPED || p->mCurrentBckAnm == POIHANA_STUCK)
				++sunk;
		}
	}
	if (chasing >= 10)
		ach::unlock(104);
	if (sunk >= 15)
		ach::unlock(105);
}

// 110: the coaster's place on the rail (its animation's frame) only goes
// back as it starts a new lap.
void lookMechaBowser(TMario* m)
{
	if (!s_mecha)
		return;
	MActor* rail = m->mKoopaRail ? m->mKoopaRail : s_mecha->mTruckMActor;
	if (rail && rail->getFrameCtrl(ANM_TYPE_BCK)) {
		const float frame = rail->getFrameCtrl(ANM_TYPE_BCK)->getFrame();
		if (s_railKnown && frame < s_railFrame && !s_lapped) {
			s_lapped = true;
			if (ach::debug())
				ach::log("110: the coaster went round once");
		}
		s_railFrame = frame;
		s_railKnown = true;
	}
	const int stage = s_mecha->mDamageStage;
	if (stage == 4 && s_mechaStage != 4 && !s_lapped)
		ach::unlock(110);
	s_mechaStage = stage;
}

// 118: each colour he wears once counts.
void lookKingBoo()
{
	if (!s_kingBoo)
		return;
	const GXColor c = s_kingBoo->unk348;
	for (int i = 0; i < kKingBooColourCount; ++i)
		if (c.r == kKingBooColours[i].r && c.g == kKingBooColours[i].g && c.b == kKingBooColours[i].b
		    && !(s_kingBooSeen & 1u << i)) {
			s_kingBooSeen |= 1u << i;
			if (ach::debug())
				ach::log("118: King Boo painted colour %d", i);
		}
	if (s_kingBooSeen == (1u << kKingBooColourCount) - 1)
		ach::unlock(118);
}

// 119: any water F.L.U.D.D. gains is a refill.
void lookWater(TMario* m)
{
	if (!m->mWaterGun)
		return;
	const int water = m->mWaterGun->mCurrentWater;
	if (s_waterKnown && water > s_water && !s_refilled) {
		s_refilled = true;
		if (ach::debug())
			ach::log("119: F.L.U.D.D. refilled");
	}
	s_water      = water;
	s_waterKnown = true;
}

void afterMovement()
{
	s_goalJustSet = false;
	if (!ach::on() || !ach::playing() || s_visit != ach::visit())
		return;
	TMario* m = player();
	if (!m)
		return;

	if (in(GELATO, 0))
		lookSandCastle(m);
	if (in(GELATO, 4) && !s_raceSpoiled && (standsOn(m, GROUND_GRASS) || usesFludd(m))) {
		s_raceSpoiled = true;
		if (ach::debug())
			ach::log("103: %s", usesFludd(m) ? "used F.L.U.D.D." : "touched grass");
	}
	if (in(GELATO, 7))
		countCataquacks();
	if (in(ROLLER_COASTER, 1))
		lookMechaBowser(m);
	if (in(PINNA_BEACH, 1) && !s_heldBobomb && m->mHeldObject && m->mHeldObject->mActorType == BOBOMB) {
		s_heldBobomb = true;
		if (ach::debug())
			ach::log("111: picked up a Bob-omb");
	}
	if (in(SIRENA_HOTEL, 0) && !s_hotelSpoiled && (usesFludd(m) || m->mStatus == MARIO_STATUS_WALL_JUMP)) {
		s_hotelSpoiled = true;
		if (ach::debug())
			ach::log("117: %s", usesFludd(m) ? "used F.L.U.D.D." : "wall kick");
	}
	if (s_area == KING_BOO_FIGHT)
		lookKingBoo();
	if (in(SIRENA_BEACH, 5))
		lookWater(m);
}

void playerStatusChanged(void* mario, unsigned from, unsigned to, unsigned arg)
{
	(void)from;
	(void)arg;
	if (!ach::on() || mario != player())
		return;
	if (s_area == PINNA_PARK && isFenceStatus(to) && !s_climbedFence) {
		s_climbedFence = true;
		if (ach::debug())
			ach::log("112: climbed a fence");
	}
	if (in(SIRENA_HOTEL, 0) && to == MARIO_STATUS_WALL_JUMP && !s_hotelSpoiled) {
		s_hotelSpoiled = true;
		if (ach::debug())
			ach::log("117: wall kick");
	}
}

// The ways into the secrets.
void moveStage()
{
	if (!ach::on() || !ach::playing() || s_visit != ach::visit())
		return;
	const int next = gpApplication.mNextArea.getStage();
	if (in(GELATO, 0) && next == SAND_CASTLE_SECRET && s_castleFromTree)
		ach::unlock(102);
	if (in(PINNA_BEACH, 1) && next == BEACH_CANNON && !s_heldBobomb)
		ach::unlock(111);
	if (in(SIRENA_HOTEL, 0) && next == HOTEL_LOBBY_SECRET && !s_hotelSpoiled)
		ach::unlock(117);
}

// 103: the goal flag's bool goes up as the first to reach it touches it.
int flagChange(unsigned flag, int old, int* value)
{
	if (flag == GOAL_FLAG && old == 0 && *value != 0 && ach::on() && in(GELATO, 4))
		s_goalJustSet = true;
	return 0;
}

// ...and right after, the flag tells whoever touched it: the player (not Il
// Piantissimo) won the race if it is him.
int playerMessage(void* mario, void* sender, unsigned message)
{
	if (!s_goalJustSet || message != MSG_GOAL || mario != player() || !sender
	    || !dynamic_cast<TGoalFlag*>((THitActor*)sender))
		return 0;
	s_goalJustSet = false;
	const int time = timerShown();
	if (ach::debug())
		ach::log("103: reached the goal at %d.%02d%s%s", time / 100, time % 100, s_raceSpoiled ? ", spoiled" : "",
		         timerRunning() ? "" : ", timer not running");
	if (!s_raceSpoiled && timerRunning() && time < 3500)
		ach::unlock(103);
	return 0;
}

void timer(int what, int time)
{
	(void)time;
	if (what == PORT_TIMER_START)
		s_timerStarted = true;
}

void shine(void* actor, int what)
{
	if (!ach::on() || !ach::playing() || s_visit != ach::visit())
		return;
	TShine* s = dynamic_cast<TShine*>((THitActor*)actor);
	if (!s)
		return;
	// 112: Pinna 5's Shine taken
	if (what == PORT_SHINE_GET && s_area == PINNA_PARK && s->mEventId == SHINE_PINNA_5 && !s_climbedFence)
		ach::unlock(112);
	// 119: the beach is clean as its Shine appears
	if (what == PORT_SHINE_APPEAR && in(SIRENA_BEACH, 5) && s->mEventId == SHINE_SIRENA_6) {
		const int left = timerShown();
		if (ach::debug())
			ach::log("119: beach clean with %d.%02d left%s", left / 100, left % 100, s_refilled ? ", refilled" : "");
		if (s_timerStarted && !s_refilled && left >= 6000)
			ach::unlock(119);
	}
}

PortExtension s_extension;

struct Register {
	Register()
	{
		s_extension.name                = "achievements-gelato-pinna-sirena";
		s_extension.stageSetup          = stageSetup;
		s_extension.afterMovement       = afterMovement;
		s_extension.moveStage           = moveStage;
		s_extension.playerStatusChanged = playerStatusChanged;
		s_extension.flagChange          = flagChange;
		s_extension.playerMessage       = playerMessage;
		s_extension.timer               = timer;
		s_extension.shine               = shine;
		port_ext_register(&s_extension);
	}
} s_register;

} // namespace
