// Achievements (ext/achievements/README.md): enemy combos and the 1-Ups they
// pay, and Mario's tricks, anywhere they can be done.
//
//   139 Stu Stack  stomp 3 flipped Strollin' Stus at once, take the 1-Up
//   140 Stu Smash            ground pound 3 flipped Strollin' Stus at once, take the 1-Up
//   141 Stu Stampede                          ground pound many of them at once in Sirena Beach, take the 1-Up
//   142 Hot Stuff                        stomp 3 flipped Smolderin' Stus at once in Pinna Park, take the 1-Up
//   143 Cloud Wash                 spray a Coo Coo until it drops its 1-Up, take it
//   144 Bill Stomper                  stomp a Guided Bullet Bill in Corona Mountain, take its 1-Up
//   145 Belly Flop                      slide at full speed on flat ground for a second
//   146 Refill on the Go                   FLUDD goes from empty to filled while sliding
//   147 Special Delivery                a Chuckster throws Mario while he rides Yoshi
//   148 Sprinkler Spin             a spin jump while the Turbo Nozzle runs
//
// The reference set tells the combo 1-Ups apart by their item slots; here
// each is known by what made it: a flipped Stu hit by the player kills every
// flipped Stu of its kind and, with 3 or more, makes a 1-Up appear in the same
// call (THamuKuriManager::requestSerialKill), so a 1-Up that appears while the
// player's stomp or ground pound reaches a Stu is that combo's. A Coo Coo
// carries its own 1-Up (TKukku::mOneUp), and a Guided Bullet Bill drops one
// where it breaks (TBathtubKillerManager::generateMushroom).
#include "achievements.h"

#include "port_ext.h"
#include "port_framerate.h"

#include <Enemy/BathtubKiller.hpp>
#include <Enemy/HamuKuri.hpp>
#include <Enemy/KukkuNerve.hpp>
#include <MoveBG/MapObjItem2.hpp>
#include <NPC/NpcBase.hpp>
#include <Player/Mario.hpp>
#include <Player/MarioFlags.hpp>
#include <Player/MarioStatus.hpp>
#include <Player/WaterGun.hpp>
#include <Player/Yoshi.hpp>
#include <Map/MapData.hpp>

namespace {

using namespace ach;

// A Stu's bck animation while it lies flipped (THamuKuri::isHitValid).
const int kFlippedAnm = 3;
// Stus in one combo for each achievement: the game's own bonus is 3
// (mSLTrampleBonusNum); "many" is our choice.
const int kComboStus = 3;
const int kManyStus  = 5;
// Full sliding speed (TMario::mForwardVel).
const float kFullSlideSpeed = 100.0f;
// A floor whose normal points up this much is flat.
const float kFlatNormalY = 0.9999f;

// --- what a hit in progress is doing, from objectHit/objectStomp to objectHitDone

enum Combo { COMBO_NONE, COMBO_STU_STOMP, COMBO_STU_POUND, COMBO_FIRE_STU_STOMP };

struct Hit {
	void* target; // the THitActor being hit, or null
	Combo combo;
	int stus; // flipped Stus the hit kills, the hit one too
};
Hit s_hit;

// --- the 1-Ups we are waiting for the player to take

enum Reward { REWARD_STU_STOMP, REWARD_STU_POUND, REWARD_FIRE_STU_STOMP, REWARD_COO_COO, REWARD_BULLET_BILL };

struct OneUp {
	TMushroom1up* item;
	Reward reward;
	int stus;   // for the Stu combos
	bool taken; // unk13A == 1 at the last look
};
const int kMaxOneUps = 16;
OneUp s_oneUps[kMaxOneUps];
int s_oneUpCount;

// Guided Bullet Bills the player broke this visit, and whether by stomping
// (rather than spraying).
struct Killer {
	TBathtubKiller* killer;
	bool stomped;
};
const int kMaxKillers = 16;
Killer s_killers[kMaxKillers];
int s_killerCount;

// --- Mario's tricks

int s_flatSlideTicks; // ticks of this slide at full speed on flat ground
int s_lastWater;      // FLUDD's water at the last tick, or -1
bool s_chucked;       // a Chuckster threw Mario this tick while he rode Yoshi

unsigned s_visit;

void reset()
{
	s_hit            = Hit();
	s_oneUpCount     = 0;
	s_killerCount    = 0;
	s_flatSlideTicks = 0;
	s_lastWater      = -1;
	s_chucked        = false;
	s_visit          = ach::visit();
}

// Per-visit state is the stage's: forget it when the stage changes.
bool ready()
{
	if (!ach::on() || !ach::playing())
		return false;
	if (s_visit != ach::visit())
		reset();
	return true;
}

bool isPlayer(void* actor)
{
	TMario* m = ach::mario();
	return m && actor == static_cast<THitActor*>(m);
}

bool inSirena()
{
	switch (ach::area()) {
	case SIRENA_BEACH:
	case SIRENA_HOTEL:
	case CASINO:
	case HOTEL_LOBBY_SECRET:
	case CASINO_SECRET:
	case KING_BOO_FIGHT:
		return true;
	}
	return false;
}

bool inPinna()
{
	switch (ach::area()) {
	case PINNA_BEACH:
	case PINNA_PARK:
	case BEACH_CANNON:
	case YOSHI_GO_ROUND:
		return true;
	}
	return false;
}

bool inCorona()
{
	return ach::area() == CORONA || ach::area() == CORONA_BOSS;
}

// Remembers a 1-Up to watch; the same object again (the game reuses them) takes the new reward.
void watchOneUp(TMushroom1up* item, Reward reward, int stus)
{
	OneUp* slot = nullptr;
	for (int i = 0; i < s_oneUpCount && !slot; ++i)
		if (s_oneUps[i].item == item)
			slot = &s_oneUps[i];
	if (slot && reward == REWARD_COO_COO && slot->reward == REWARD_COO_COO)
		return; // the same Coo Coo sprayed again
	if (!slot) {
		if (s_oneUpCount == kMaxOneUps)
			return;
		slot = &s_oneUps[s_oneUpCount++];
	}
	slot->item   = item;
	slot->reward = reward;
	slot->stus   = stus;
	slot->taken  = item->unk13A == 1;
	if (ach::debug())
		ach::log("anywhere: watching 1-Up %p (reward %d, %d Stus)", (void*)item, reward, stus);
}

// The flipped Stus a hit on `stu` kills, the hit one too (as requestSerialKill counts them); 0 if it isn't flipped.
int comboStus(THamuKuri* stu)
{
	if (!stu->isBckAnm(kFlippedAnm))
		return 0;
	THamuKuriManager* manager = stu->getManager();
	if (!manager)
		return 1;
	int n = 1;
	for (int i = 0; i < manager->getActiveObjNum(); ++i) {
		THamuKuri* other = static_cast<THamuKuri*>(manager->getObj(i));
		if (other != stu && other->isBckAnm(kFlippedAnm))
			++n;
	}
	return n;
}

// A Bullet Bill on its way out (TBathtubKiller's BKIsDying): hits don't break it again.
bool killerDying(TBathtubKiller* k)
{
	TSpineBase<TLiveActor>* spine = k->getSpine();
	if (!spine)
		return false;
	return spine->getCurrentNerve() == &TNerveBathtubKillerBreak::theNerve()
	       || spine->getCurrentNerve() == &TNerveBathtubKillerExplosion::theNerve();
}

// A guided Bullet Bill the player hits with `message`: the hit that breaks it decides how it was beaten.
void killerHit(TBathtubKiller* k, unsigned message, bool fromStomp)
{
	if (k->unk194 != 1 || killerDying(k))
		return;
	const bool stomped = fromStomp || message == HIT_MESSAGE_HIP_DROP || message == HIT_MESSAGE_SUPER_HIP_DROP;
	if (!stomped && message != HIT_MESSAGE_SPRAYED_BY_WATER && message != HIT_MESSAGE_TRAMPLE)
		return; // doesn't break it
	for (int i = 0; i < s_killerCount; ++i)
		if (s_killers[i].killer == k) {
			s_killers[i].stomped = stomped;
			return;
		}
	if (s_killerCount < kMaxKillers) {
		s_killers[s_killerCount].killer  = k;
		s_killers[s_killerCount].stomped = stomped;
		++s_killerCount;
	}
}

// The player starts a hit on `object` (a THitActor) with `message`: `fromStomp` for a stomp
// (TMario::trampleExec), not the trample a dive or a catch sends.
void startHit(void* object, unsigned message, bool fromStomp)
{
	THitActor* actor = static_cast<THitActor*>(object);
	if (TKukku* kukku = dynamic_cast<TKukku*>(actor)) {
		if (message == HIT_MESSAGE_SPRAYED_BY_WATER)
			if (TMushroom1up* item = dynamic_cast<TMushroom1up*>(kukku->mOneUp))
				watchOneUp(item, REWARD_COO_COO, 0);
		return;
	}
	if (TBathtubKiller* k = dynamic_cast<TBathtubKiller*>(actor)) {
		killerHit(k, message, fromStomp);
		return;
	}
	THamuKuri* stu = dynamic_cast<THamuKuri*>(actor);
	if (!stu)
		return;
	TMario* m       = ach::mario();
	const bool fire = dynamic_cast<TFireHamuKuri*>(stu) != nullptr;
	Combo combo     = COMBO_NONE;
	if (fromStomp)
		combo = fire ? COMBO_FIRE_STU_STOMP : COMBO_STU_STOMP;
	else if ((message == HIT_MESSAGE_HIP_DROP || message == HIT_MESSAGE_SUPER_HIP_DROP) && !fire && m
	         && m->mStatus == MARIO_STATUS_HIP_DROP)
		combo = COMBO_STU_POUND;
	if (combo == COMBO_NONE)
		return;
	const int stus = comboStus(stu);
	if (stus < kComboStus)
		return;
	s_hit.target = object;
	s_hit.combo  = combo;
	s_hit.stus   = stus;
}

int objectStomp(void* object, void* mario)
{
	if (ready() && isPlayer(mario))
		startHit(object, HIT_MESSAGE_TRAMPLE, true);
	return 0;
}

int objectHit(void* object, void* sender, unsigned message)
{
	if (ready())
		startHit(object, message, false);
	return 0;
}

void objectHitDone(void* object, void*, unsigned, int)
{
	if (s_hit.target == object)
		s_hit = Hit();
}

// A 1-Up appears: a combo's if a combo hit is under way, a Bullet Bill's if one broke where it stands.
void actorSpawn(void* actor)
{
	if (!ready())
		return;
	TMushroom1up* item = dynamic_cast<TMushroom1up*>(static_cast<THitActor*>(actor));
	if (!item)
		return;
	if (s_hit.target) {
		const Reward r = s_hit.combo == COMBO_STU_STOMP ? REWARD_STU_STOMP
		                 : s_hit.combo == COMBO_STU_POUND ? REWARD_STU_POUND
		                                                  : REWARD_FIRE_STU_STOMP;
		watchOneUp(item, r, s_hit.stus);
		return;
	}
	for (int i = 0; i < s_killerCount; ++i) {
		TBathtubKiller* k = s_killers[i].killer;
		if (s_killers[i].stomped && killerDying(k) && k->mPosition.x == item->mPosition.x
		    && k->mPosition.z == item->mPosition.z) {
			watchOneUp(item, REWARD_BULLET_BILL, 0);
			return;
		}
	}
}

// A watched 1-Up was taken.
void oneUpTaken(const OneUp& o)
{
	switch (o.reward) {
	case REWARD_STU_STOMP:
		ach::unlock(139);
		break;
	case REWARD_STU_POUND:
		ach::unlock(140);
		if (o.stus >= kManyStus && inSirena())
			ach::unlock(141);
		break;
	case REWARD_FIRE_STU_STOMP:
		if (inPinna())
			ach::unlock(142);
		break;
	case REWARD_COO_COO:
		ach::unlock(143);
		break;
	case REWARD_BULLET_BILL:
		if (inCorona())
			ach::unlock(144);
		break;
	}
}

void checkOneUps()
{
	for (int i = 0; i < s_oneUpCount; ++i) {
		OneUp& o         = s_oneUps[i];
		const bool taken = o.item->unk13A == 1;
		if (taken && !o.taken)
			oneUpTaken(o);
		o.taken = taken;
	}
}

// 145: a second of one slide at full speed on flat ground (frames on a slope don't count, leaving the slide resets).
void checkFlatSlide(TMario* m)
{
	if (m->mStatus != MARIO_STATUS_CATCH) {
		s_flatSlideTicks = 0;
		return;
	}
	const TBGCheckData* floor = m->mGroundPlane;
	if (floor && floor->getNormal().y >= kFlatNormalY && m->mForwardVel >= kFullSlideSpeed) {
		const int second = port_active_frame_rate > 0 ? port_active_frame_rate : 30;
		if (++s_flatSlideTicks >= second)
			ach::unlock(145);
	}
}

// 146: FLUDD empty last tick, not now, and Mario sliding.
void checkReload(TMario* m)
{
	TWaterGun* gun = m->mWaterGun;
	if (!gun || !m->checkFlag(MARIO_FLAG_HAS_FLUDD)) {
		s_lastWater = -1;
		return;
	}
	const int water = gun->getCurrentWater();
	if (s_lastWater == 0 && water > 0 && m->mStatus == MARIO_STATUS_CATCH)
		ach::unlock(146);
	s_lastWater = water;
}

// 148: a spin jump while the Turbo Nozzle runs.
void checkTurboSpin(TMario* m)
{
	TWaterGun* gun = m->mWaterGun;
	if (!gun || !m->checkFlag(MARIO_FLAG_HAS_FLUDD))
		return;
	if ((m->mStatus == MARIO_STATUS_LEFT_ROTATE_JUMP || m->mStatus == MARIO_STATUS_RIGHT_ROTATE_JUMP)
	    && gun->mCurrentNozzle == TWaterGun::Turbo && gun->isEmitWater() == 1)
		ach::unlock(148);
}

bool ridingYoshi(TMario* m)
{
	return m->mYoshi && m->mYoshi->onYoshi();
}

// 147: a Chuckster (an NPC's throw, TNpcThrow) sends the player flying while he rides Yoshi.
int playerMessage(void* mario, void* sender, unsigned message)
{
	if (!ready() || !isPlayer(mario) || message != HIT_MESSAGE_THROWN)
		return 0;
	TMario* m = ach::mario();
	if (dynamic_cast<TBaseNPC*>(static_cast<THitActor*>(sender)) && ridingYoshi(m))
		s_chucked = true;
	return 0;
}

void playerStatusChanged(void* mario, unsigned, unsigned to, unsigned)
{
	if (!ready() || !isPlayer(mario))
		return;
	if (to == MARIO_STATUS_THROWN_DOWN && s_chucked && ridingYoshi(ach::mario()))
		ach::unlock(147);
}

void afterMovement()
{
	if (!ready())
		return;
	s_hit     = Hit(); // a hit another extension stopped has no objectHitDone
	s_chucked = false;
	checkOneUps();
	TMario* m = ach::mario();
	if (!m)
		return;
	checkFlatSlide(m);
	checkReload(m);
	checkTurboSpin(m);
}

void stageSetup()
{
	if (ach::on())
		reset();
}

PortExtension s_extension;

struct Register {
	Register()
	{
		s_extension.name                = "achievements-anywhere";
		s_extension.stageSetup          = stageSetup;
		s_extension.afterMovement       = afterMovement;
		s_extension.objectStomp         = objectStomp;
		s_extension.objectHit           = objectHit;
		s_extension.objectHitDone       = objectHitDone;
		s_extension.actorSpawn          = actorSpawn;
		s_extension.playerMessage       = playerMessage;
		s_extension.playerStatusChanged = playerStatusChanged;
		port_ext_register(&s_extension);
	}
} s_register;

} // namespace
