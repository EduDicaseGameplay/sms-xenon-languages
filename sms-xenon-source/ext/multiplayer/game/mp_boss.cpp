// Bosses shared in multiplayer. Each boss is its own state machine, so each
// is taught here on its own: Petey Piranha, Gooper Blooper, Eely-Mouth,
// Phantamanta, Wiggler, King Boo, Mecha-Bowser, Bowser's hot tub, Shadow
// Mario's chases and the Gatekeepers (the goop piranha plants of the
// Airstrip, Bianco Hills, and the plaza's statue and its gates to Ricco
// Harbor and Gelato Beach). Il Piantissimo, a Shadow Mario of sorts, is left
// to each game: every player races their own.
//
// The host's boss is the real one. Its state rides at the end of the world
// snapshot as records, one per part ("unit": the boss, or one of
// Phantamanta's pieces), each [kind, unit, length, payload]; pieces take
// turns when there are many. A client's copy follows: it takes each new step
// of the boss's behaviour (nerve) when the host's does (only then, so its
// animations are not restarted every tick), and the values that make the
// fight (hit points, swallowed water, teeth, the weak segment...) whenever a
// record comes. Its own AI still runs between the host's steps: the boss
// aims at the player beside it in that game.
//
// What a client's player does to its copy is found by comparing the copy
// with what was last set from the host, reported (mpg_boss_hit: part, kind,
// count) and done to the real boss. A copy never goes down before the host's;
// when the host's does, every copy plays the defeat and brings out the
// boss's Shine, which anyone can get for everyone (mp_game.cpp's Shine Get).
#include "mp_boss.hpp"
#include "mp_mirror.hpp"
#include "../mp_game_api.h"
#include "port_framerate.h"

#include <Enemy/BossEel.hpp>
#include <Enemy/BossGesso.hpp>
#include <Enemy/BossGessoTentacle.hpp>
#include <Enemy/BossHanachan.hpp>
#include <Enemy/BossManta.hpp>
#include <Enemy/BossPakkun.hpp>
#include <Enemy/BossTelesa.hpp>
#include <Enemy/BossTelesaObj.hpp>
#include <Enemy/Conductor.hpp>
#include <Enemy/Emario.hpp>
#include <Enemy/EnemyMario.hpp>
#include <Enemy/Graph.hpp>
#include <Enemy/GateKeeper.hpp>
#include <Enemy/TinKoopa.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <MarioUtil/MathUtil.hpp>
#include <MoveBG/MapObjCorona.hpp>
#include <Player/Mario.hpp>
#include <Player/MarioRecord.hpp>
#include <Player/ModelWaterManager.hpp>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

enum Kind {
	KIND_PETEY   = 1,
	KIND_GESSO   = 2, // Gooper Blooper
	KIND_EEL     = 3, // Eely-Mouth
	KIND_MANTA   = 4, // Phantamanta, every piece a unit
	KIND_WIGGLER = 5,
	KIND_KINGBOO = 6,
	KIND_MECHA   = 7, // Mecha-Bowser (each player rides their own coaster)
	KIND_TUB     = 8, // Bowser's hot tub: its five corners
	KIND_SHADOW  = 9, // Shadow Mario's chases
	KIND_GATE    = 10, // the Gatekeepers, each a unit
};

enum Hit {
	HIT_WATER = 1, // Petey: water in his mouth
	HIT_HIP_DROP,  // Petey: a ground pound on his belly
	HIT_EYE,       // Gooper Blooper: an eye sprayed
	HIT_BEAK,      // Gooper Blooper: the beak pulled off
	HIT_TENTACLE,  // Gooper Blooper: tentacle `unit` torn off
	HIT_TOOTH,     // Eely-Mouth: tooth `unit` cleaned by `count`
	HIT_MANTA,     // Phantamanta: piece `unit` sprayed
	HIT_WIGGLER,   // Wiggler: the weak segment ground-pounded
	HIT_FRUIT,     // King Boo: hit by fruit while spicy
	HIT_SPICY,     // King Boo: ate a pepper
	HIT_ROCKET,    // Mecha-Bowser: a rocket on his plates
	HIT_GRIP,      // the hot tub: corner `unit` broken by a super ground pound
	HIT_SHADOW_WATER, // Shadow Mario: sprayed
	HIT_SHADOW_STOMP, // Shadow Mario: stomped while down
	HIT_GATE_SPRAY,   // Gatekeeper `unit`: sprayed (asleep, it wakes after enough)
	HIT_GATE_MOUTH,   // Gatekeeper `unit`: water in its mouth, `count` hit points
	HIT_TENTACLE_STUN, // Gooper Blooper: tentacle `unit` stomped flat
};

typedef const TNerveBase<TLiveActor>* Nerve;
typedef Nerve (*NerveTable)(int);

const int kMaxUnits = 128;
const int kMantaPerUpdate = 6;

bool debug() { return getenv("SMS_MULTIPLAYER_DEBUG") != nullptr; }

// ------------------------------------------------------------------ bytes

struct Out {
	u8* p;
	int n, cap;
	Out(u8* buf, int size) : p(buf), n(0), cap(size) {}
	bool room(int k) const { return n + k <= cap; }
	void bytes(const void* d, int k)
	{
		if (!room(k))
			return;
		memcpy(p + n, d, k);
		n += k;
	}
	void u8_(u8 v) { bytes(&v, 1); }
	void s16_(s16 v) { bytes(&v, 2); }
	void f32_(f32 v) { bytes(&v, 4); } // the same build on both sides: the same layout
};

struct In {
	const u8* p;
	int n, at;
	bool ok;
	In(const u8* buf, int size) : p(buf), n(size), at(0), ok(true) {}
	void bytes(void* d, int k)
	{
		if (at + k > n) {
			ok = false;
			memset(d, 0, k);
			return;
		}
		memcpy(d, p + at, k);
		at += k;
	}
	u8 u8_()
	{
		u8 v;
		bytes(&v, 1);
		return v;
	}
	s16 s16_()
	{
		s16 v;
		bytes(&v, 2);
		return v;
	}
	f32 f32_()
	{
		f32 v;
		bytes(&v, 4);
		return v;
	}
};

s16 yawOf(f32 degrees) { return (s16)(degrees * (65536.0f / 360.0f)); }
f32 degreesOf(s16 yaw) { return yaw * (360.0f / 65536.0f); }

f32 easeAngle(f32 have, f32 want, f32 k)
{
	f32 d = want - have;
	while (d > 180.0f)
		d -= 360.0f;
	while (d < -180.0f)
		d += 360.0f;
	return have + d * k;
}

void writeWhere(Out& o, TLiveActor* a)
{
	o.f32_(a->mPosition.x);
	o.f32_(a->mPosition.y);
	o.f32_(a->mPosition.z);
	o.s16_(yawOf(a->mRotation.y));
}

// Where the host's is, eased (a step's own motion carries it between); at
// once when far (it jumped, or a lost update or two left it behind).
// Host: reading its own last records into its reloaded boss (restoreKept):
// put where they say at once, and its hit points as they are (a copy's
// last-point rule is for copies)
bool s_restoring;

void readWhere(In& in, TLiveActor* a, f32 k = 0.25f)
{
	const f32 x = in.f32_(), y = in.f32_(), z = in.f32_();
	const s16 yaw = in.s16_();
	if (!in.ok)
		return;
	const f32 dx = x - a->mPosition.x, dy = y - a->mPosition.y, dz = z - a->mPosition.z;
	if (dx * dx + dy * dy + dz * dz > 800.0f * 800.0f || s_restoring)
		k = 1.0f;
	a->mPosition.x += (x - a->mPosition.x) * k;
	a->mPosition.y += (y - a->mPosition.y) * k;
	a->mPosition.z += (z - a->mPosition.z) * k;
	a->mRotation.y = easeAngle(a->mRotation.y, degreesOf(yaw), k);
}

// ------------------------------------------------------------------ nerves

int nerveIndex(NerveTable table, Nerve n)
{
	for (int i = 0; i < 64; i++) {
		Nerve t = table(i);
		if (!t)
			break;
		if (t == n)
			return i;
	}
	return 0xFF;
}

int s_hostNerve[kMaxUnits]; // the host's last step taken, per unit
int s_localNerve[kMaxUnits], s_localSince[kMaxUnits]; // the copy's step, and since when (tick)
int s_bossTick;
const int kOwnStepTicks = 12; // a step the copy took on its own (hit here) is let run this long

// A new step of the host's: take it. But not over a step this copy took on
// its own just now (hurt by a hit here, which the host has not had yet): the
// host's from before the hit would cut it short, and its own hurt step, once
// it comes, play it again. And a copy is not down (its `die` steps) before
// the host's is.
void followNerve(TLiveActor* a, int unit, int host, NerveTable table, int die1, int die2)
{
	if (host == 0xFF || !table(host))
		return;
	const int local = nerveIndex(table, a->getLatestNerve());
	if (local != s_localNerve[unit]) {
		s_localNerve[unit] = local;
		s_localSince[unit] = s_bossTick;
	}
	if (host != s_hostNerve[unit]) {
		if (local != host && local != s_hostNerve[unit] && s_hostNerve[unit] >= 0
		    && s_bossTick - s_localSince[unit] < kOwnStepTicks)
			return; // taken once it has run a little (the host's is kept until then)
		s_hostNerve[unit] = host;
		if (local != host) {
			a->mSpine->reset();
			a->mSpine->setNext(table(host));
		}
	} else if ((local == die1 || local == die2) && host != die1 && host != die2) {
		a->mSpine->reset();
		a->mSpine->setNext(table(host));
	}
}

Nerve peteyNerve(int i)
{
	switch (i) {
	case 0: return &TNerveBPWait::theNerve();
	case 1: return &TNerveBPCannon::theNerve();
	case 2: return &TNerveBPVomit::theNerve();
	case 3: return &TNerveBPTornado::theNerve();
	case 4: return &TNerveBPPivot::theNerve();
	case 5: return &TNerveBPSwallow::theNerve();
	case 6: return &TNerveBPTumbleIn::theNerve();
	case 7: return &TNerveBPTumble::theNerve();
	case 8: return &TNerveBPTumbleOut::theNerve();
	case 9: return &TNerveBPGetUp::theNerve();
	case 10: return &TNerveBPSwing::theNerve();
	case 11: return &TNerveBPStompReact::theNerve();
	case 12: return &TNerveBPJumpReact::theNerve();
	case 13: return &TNerveBPPreDie::theNerve();
	case 14: return &TNerveBPDie::theNerve();
	case 15: return &TNerveBPTakeOff::theNerve();
	case 16: return &TNerveBPFly::theNerve();
	case 17: return &TNerveBPTouchDown::theNerve();
	case 18: return &TNerveBPFlyCannon::theNerve();
	case 19: return &TNerveBPFlyPivot::theNerve();
	case 20: return &TNerveBPHover::theNerve();
	case 21: return &TNerveBPFall::theNerve();
	case 22: return &TNerveBPSleep::theNerve();
	case 23: return &TNerveBPBreakSleep::theNerve();
	default: return nullptr;
	}
}

Nerve gessoNerve(int i)
{
	switch (i) {
	case 0: return &TNerveBGWait::theNerve();
	case 1: return &TNerveBGEyeDamage::theNerve();
	case 2: return &TNerveBGBeakDamage::theNerve();
	case 3: return &TNerveBGTentacleDamage::theNerve();
	case 4: return &TNerveBGTug::theNerve();
	case 5: return &TNerveBGDie::theNerve();
	case 6: return &TNerveBGPollute::theNerve();
	case 7: return &TNerveBGPolDrop::theNerve();
	case 8: return &TNerveBGRoll::theNerve();
	default: return nullptr;
	}
}
const int kGessoEyeDamage = 1, kGessoDie = 5;

Nerve eelNerve(int i)
{
	switch (i) {
	case 0: return &TNerveBossEelWaitAppear::theNerve();
	case 1: return &TNerveBossEelFirstSpin::theNerve();
	case 2: return &TNerveBossEelSecondSpin::theNerve();
	case 3: return &TNerveBossEelAppear::theNerve();
	case 4: return &TNerveBossEelOutWait::theNerve();
	case 5: return &TNerveBossEelSlowBack::theNerve();
	case 6: return &TNerveBossEelQuickBack::theNerve();
	case 7: return &TNerveBossEelEat::theNerve();
	case 8: return &TNerveBossEelDie::theNerve();
	case 9: return &TNerveBossEelMouthOpenWait::theNerve();
	case 10: return &TNerveBossEelSleepOnBottom::theNerve();
	default: return nullptr;
	}
}
const int kEelDie = 8;

Nerve mantaNerve(int i)
{
	switch (i) {
	case 0: return &TNerveMantaMove::theNerve();
	case 1: return &TNerveMantaHitWater::theNerve();
	case 2: return &TNerveMantaSpawn::theNerve();
	case 3: return &TNerveMantaDeath::theNerve();
	case 4: return &TNerveMantaAppearDemo::theNerve();
	default: return nullptr;
	}
}
const int kMantaSpawn = 2, kMantaDeath = 3;

Nerve wigglerNerve(int i)
{
	switch (i) {
	case 0: return &TNerveBossHanachanGraphWander::theNerve();
	case 1: return &TNerveBossHanachanTumble::theNerve();
	case 2: return &TNerveBossHanachanDown::theNerve();
	case 3: return &TNerveBossHanachanGetUp::theNerve();
	case 4: return &TNerveBossHanachanDamage::theNerve();
	case 5: return &TNerveBossHanachanSnort::theNerve();
	case 6: return &TNerveBossHanachanDead::theNerve();
	default: return nullptr;
	}
}
const int kWigglerTumble = 1, kWigglerDown = 2, kWigglerDead = 6;

Nerve kingBooNerve(int i)
{
	switch (i) {
	case 0: return &TNerveBossTelesaAppear::theNerve();
	case 1: return &TNerveBossTelesaDie::theNerve();
	case 2: return &TNerveBossTelesaFallDemo::theNerve();
	case 3: return &TNerveBossTelesaFreeze::theNerve();
	case 4: return &TNerveBossTelesaHide::theNerve();
	case 5: return &TNerveBossTelesaHideWait::theNerve();
	case 6: return &TNerveBossTelesaPrepareSlot::theNerve();
	case 7: return &TNerveBossTelesaSlotStart::theNerve();
	case 8: return &TNerveBossTelesaSpit::theNerve();
	case 9: return &TNerveBossTelesaSpitSlotItem::theNerve();
	default: return nullptr;
	}
}
const int kKingBooDie = 1;

Nerve mechaNerve(int i)
{
	switch (i) {
	case 0: return &TNerveTinKoopaWait::theNerve();
	case 1: return &TNerveTinKoopaDamage::theNerve();
	case 2: return &TNerveTinKoopaBreak::theNerve();
	default: return nullptr;
	}
}

// ------------------------------------------------------------------ the stage's boss

int s_kind;
TSpineEnemy* s_boss;     // the boss (Phantamanta: none, see s_mantas)
TEnemyManager* s_mantas; // Phantamanta's pieces, by slot
TEnemyManager* s_gates;  // the Gatekeepers, by slot
TBathtub* s_tub;         // Bowser's hot tub
TEnemyMario* s_shadow;   // Shadow Mario
TEMarioManager* s_shadowList; // his hit actor's list: he is found through it once set up
TEMario* s_shadowAlone;       // or he is one on his own (the plaza's chase with Peach)
int s_next;              // the next piece to send
// client: what was last set from the host (to find this game's own hits)
int s_setHp;
int s_setValue[kMaxUnits];
u8 s_reported[kMaxUnits + 8]; // per unit; then Gooper Blooper's eye
bool s_followed;

// ------------------------------------------------------------------ the Gatekeepers

Nerve gateNerve(int i)
{
	switch (i) {
	case 0: return &TNerveBGKSleep::theNerve();
	case 1: return &TNerveBGKAppear::theNerve();
	case 2: return &TNerveBGKWait::theNerve();
	case 3: return &TNerveBGKWait2::theNerve();
	case 4: return &TNerveBGKSleepDamage::theNerve();
	case 5: return &TNerveBGKAwakeDamage::theNerve();
	case 6: return &TNerveBGKDie::theNerve();
	case 7: return &TNerveBGKDive::theNerve();
	case 8: return &TNerveBGKLaunchGoro::theNerve();
	case 9: return &TNerveBGKLaunchName::theNerve();
	default: return nullptr;
	}
}
const int kGateWait = 2, kGateWait2 = 3, kGateAwakeDamage = 5, kGateDie = 6;
const int kMaxGates = 8;
const int kGateRecord = 6;
int s_sprayWait[kMaxGates]; // client: frames until this game's spraying is reported again

TBiancoGateKeeper* gateAt(int unit)
{
	if (!s_gates || unit < 0 || unit >= s_gates->getObjNum() || unit >= kMaxGates)
		return nullptr;
	return dynamic_cast<TBiancoGateKeeper*>(s_gates->getObj(unit));
}

int clampByte(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

void writeGate(Out& o, int unit)
{
	TBiancoGateKeeper* g = gateAt(unit);
	o.u8_((u8)nerveIndex(gateNerve, g->getLatestNerve()));
	o.u8_(g->getHitPoints());
	o.u8_((u8)clampByte(g->unk17C)); // how awake: it comes up at 255
	o.u8_((u8)clampByte(g->unk28A)); // how many times it has come up
	o.u8_(g->checkLiveFlag(LIVE_FLAG_DEAD) ? 1 : 0);
	o.u8_((u8)g->unk296); // Ricco's and Pinna's come back once
}

void readGate(In& in, int unit)
{
	TBiancoGateKeeper* g = gateAt(unit);
	const int nerve = in.u8_(), hp = in.u8_(), wake = in.u8_(), appeared = in.u8_(), dead = in.u8_();
	const int revive = (s8)in.u8_();
	if (!in.ok || !g)
		return;
	if (g->checkLiveFlag(LIVE_FLAG_DEAD))
		return;
	const int local = nerveIndex(gateNerve, g->getLatestNerve());
	if (dead && local != kGateDie) { // gone at the host's before this game saw it go
		g->mStampEnabled = 0;
		g->kill();
		return;
	}
	if (s_followed) {
		// what this game's player did to its copy since the host's last say
		if (g->getHitPoints() < s_setValue[unit])
			mpg_boss_hit(unit, HIT_GATE_MOUTH, s_setValue[unit] - g->getHitPoints());
		// sprayed this tick (the move step marks it so for 16 ticks)
		if (s_sprayWait[unit] > 0)
			s_sprayWait[unit]--;
		if (g->unk17E == 0x0F && !s_sprayWait[unit]) {
			mpg_boss_hit(unit, HIT_GATE_SPRAY, 1);
			s_sprayWait[unit] = 8;
		}
	}
	followNerve(g, unit, nerve, gateNerve, kGateDie, kGateDie);
	// a copy never holds its last hit point: a hit here takes it to 1, not
	// to its defeat (the host's says when it falls)
	const int here = hp == 1 && !s_restoring ? 2 : hp;
	g->mHitPoints = (u8)here;
	g->unk17C     = (s16)wake;
	g->unk28A     = (s16)appeared;
	g->unk296     = (s8)revive;
	s_setValue[unit] = here;
}

void hitGate(int unit, int kind, int count)
{
	TBiancoGateKeeper* g = gateAt(unit);
	if (!g || g->checkLiveFlag(LIVE_FLAG_DEAD))
		return;
	if (kind == HIT_GATE_SPRAY) {
		g->unk17E = 0x10; // as the game marks its own water on it
		return;
	}
	const int nerve = nerveIndex(gateNerve, g->getLatestNerve());
	if (kind != HIT_GATE_MOUTH || (nerve != kGateWait && nerve != kGateWait2) || !g->getHitPoints())
		return; // asleep, diving or already hurt: as for this game's own water
	g->decHitPoints();
	g->mSpine->reset();
	g->mSpine->setNext(gateNerve(g->getHitPoints() ? kGateAwakeDamage : kGateDie));
}

// ------------------------------------------------------------------ Petey Piranha

void writePetey(Out& o)
{
	TBossPakkun* b = static_cast<TBossPakkun*>(s_boss);
	o.u8_((u8)nerveIndex(peteyNerve, b->getLatestNerve()));
	o.u8_((u8)b->mState);
	o.u8_(b->getHitPoints());
	o.s16_((s16)b->mWaterMark);
	o.u8_(b->unk17C ? 1 : 0);
	o.s16_(yawOf(b->mHeadYaw));
	writeWhere(o, b);
}

void readPetey(In& in)
{
	TBossPakkun* b = static_cast<TBossPakkun*>(s_boss);
	const int nerve = in.u8_(), state = (s8)in.u8_(), hp = in.u8_();
	const int water = in.s16_(), drain = in.u8_();
	const s16 headYaw = in.s16_();
	if (!in.ok)
		return;
	if (s_followed) {
		if (b->getHitPoints() < s_setHp) // a ground pound on his belly, here
			mpg_boss_hit(0, HIT_HIP_DROP, s_setHp - b->getHitPoints());
		if (b->mWaterMark > s_setValue[0]) // water in his mouth, here
			mpg_boss_hit(0, HIT_WATER, b->mWaterMark - s_setValue[0]);
	}
	followNerve(b, 0, nerve, peteyNerve, 13, 14);
	// his stomp reaction turns his head's hits off and on again only as its
	// animation ends: a copy taken on to the host's next step before then
	// kept it off, and his head swing never hit this game's player
	if (b->mHeadHit && b->getLatestNerve() != &TNerveBPStompReact::theNerve()
	    && b->getLatestNerve() != &TNerveBPPreDie::theNerve() && b->getLatestNerve() != &TNerveBPDie::theNerve())
		b->mHeadHit->offHitFlag(HIT_FLAG_NO_COLLISION);
	b->mState     = (s8)state;
	b->mHitPoints = (u8)hp;
	b->mWaterMark = water;
	b->unk17C     = (s8)drain;
	b->mHeadYaw   = degreesOf(headYaw);
	readWhere(in, b);
	s_setHp       = hp;
	s_setValue[0] = water;
}

void hitPetey(int, int kind, int count)
{
	TBossPakkun* b = static_cast<TBossPakkun*>(s_boss);
	if (kind == HIT_WATER)
		for (int i = 0; i < count; i++)
			b->gotWaterDamage();
	else if (kind == HIT_HIP_DROP && b->mState == BOSSPAKU_STATE_BELLY_UP)
		b->gotHipDropDamage();
}

// ------------------------------------------------------------------ Gooper Blooper

const int kTentacleNodes = 6; // TBossGesso's (new TBGTentacle(this, 6, i))
f32 clampNode(f32 v) { return v < -32000.0f ? -32000.0f : v > 32000.0f ? 32000.0f : v; }
// client: the host's tentacles' nodes (from his middle) and when they came
JGeometry::TVec3<f32> s_hostNodes[4][kTentacleNodes];
int s_hostNodesAt[4] = {-1000, -1000, -1000, -1000};

void writeGesso(Out& o)
{
	TBossGesso* b = static_cast<TBossGesso*>(s_boss);
	o.u8_((u8)nerveIndex(gessoNerve, b->getLatestNerve()));
	o.u8_(b->getHitPoints());
	o.u8_((u8)b->mAttackMode);
	o.u8_(b->unk190.color.a); // the ink on him
	for (int i = 0; i < TBossGesso::TENTACLE_NUM; i++)
		o.u8_(b->mTentacles[i] ? (u8)b->mTentacles[i]->getState() : 0);
	writeWhere(o, b);
	o.u8_(b->checkLiveFlag(LIVE_FLAG_DEAD) ? 1 : 0);
	// two tentacles' shapes a time (their nodes, from his middle): everyone
	// sees the same tentacles, swinging at whoever they swing at here
	static int next;
	o.u8_((u8)next);
	for (int k = 0; k < 2; k++) {
		TBGTentacle* t = b->mTentacles[(next + k) % TBossGesso::TENTACLE_NUM];
		for (int n = 0; n < kTentacleNodes; n++) {
			const JGeometry::TVec3<f32> at = t && n < t->mNodeNum ? t->mNodes[n].getPosition() : b->mPosition;
			o.s16_((s16)clampNode(at.x - b->mPosition.x));
			o.s16_((s16)clampNode(at.y - b->mPosition.y));
			o.s16_((s16)clampNode(at.z - b->mPosition.z));
		}
	}
	next = (next + 2) % TBossGesso::TENTACLE_NUM;
}

// A tentacle's states (TBGTentacle::changeStateAndFixNodes): held (3, pulled
// by a player), torn off (4), stomped flat (5), growing back (6), guarding (10).
const int kTentacleHeld = 3, kTentacleTorn = 4, kTentacleStunned = 5, kTentacleRegrow = 6, kTentacleGuard = 10;
// torn off or growing back: the host's says
bool tornOrRegrowing(int state) { return state == kTentacleTorn || state == kTentacleRegrow; }
// one a stomp can flatten (TBGTakeHit::receiveMessage)
bool stompable(int state)
{
	return state != kTentacleTorn && state != kTentacleStunned && state != kTentacleHeld && state != kTentacleRegrow
	       && state != kTentacleGuard;
}
int s_gessoDying; // client: ticks this copy has been in its defeat since the host's was gone
bool s_newWorld;  // client: this tick's boss record is a new one (not the last again)

// client, per tentacle: the host's state last read, this copy's state as the
// last read left it (so what this game's own player did since shows as a
// change), when this game's player last tore or stomped it, and since when
// it has been torn here while the host's is whole
int s_tentHost[TBossGesso::TENTACLE_NUM];
int s_tentHere[TBossGesso::TENTACLE_NUM];
int s_tentOwnAt[TBossGesso::TENTACLE_NUM];
int s_tentApartSince[TBossGesso::TENTACLE_NUM];

void resetTentacles()
{
	for (int i = 0; i < TBossGesso::TENTACLE_NUM; i++) {
		s_tentHost[i] = s_tentHere[i] = -1;
		s_tentOwnAt[i] = s_tentApartSince[i] = -100000;
	}
}

int gessoFps() { return port_active_frame_rate > 0 ? port_active_frame_rate : 30; }

// A tentacle torn off in another game, torn off here: nobody here holds it,
// so it does not flail where it came off for the half minute the tearer's
// does in their hand; it blinks out as a dropped one does at its end
// (TBGTentacle::perform: the last 240 ticks of its amputee time).
void tearFromAfar(TBGTentacle* t)
{
	if (t->getState() == kTentacleTorn || t->getState() == kTentacleRegrow)
		return;
	t->changeStateAndFixNodes(kTentacleTorn);
	if (t->mTakeHit->getHolder())
		return; // this game's player holds it: theirs to flail
	const int blink = t->getOwner()->getSaveParam2()->getSLAmputeeTime() - 240;
	if (t->mTimeInCurrentState < blink)
		t->mTimeInCurrentState = blink;
}

// A tentacle as the host's: in its step (swinging, resting, guarding, held
// by someone there), and its shape, eased in. Not one this game's own
// player holds (they pull it here), nor one torn off or growing back
// (those steps are the host's, above), nor one this game's player has just
// stomped flat (flat until the host's is, or the host's stays up a while).
void followTentacle(TBossGesso* b, int i, int hostState)
{
	TBGTentacle* t = b->mTentacles[i];
	const int fps  = gessoFps();
	if (!t || s_bossTick - s_hostNodesAt[i] > fps)
		return;
	const int here = t->getState();
	if (here == kTentacleHeld || tornOrRegrowing(here) || tornOrRegrowing(hostState) || hostState == kTentacleStunned)
		return;
	if (here == kTentacleStunned && s_bossTick - s_tentOwnAt[i] < 3 * fps)
		return;
	if (hostState != here && hostState != kTentacleHeld && hostState >= 0 && hostState <= kTentacleGuard)
		t->changeStateAndFixNodes(hostState);
	// node 0 is at his shoulder (TBossGesso::perform puts it there)
	static f32 apart; // debug: how far from the host's they are, lately
	static int counted;
	for (int n = 1; n < t->mNodeNum && n < kTentacleNodes; n++) {
		const JGeometry::TVec3<f32>& rel = s_hostNodes[i][n];
		const JGeometry::TVec3<f32> want(b->mPosition.x + rel.x, b->mPosition.y + rel.y, b->mPosition.z + rel.z);
		JGeometry::TVec3<f32> at = t->mNodes[n].getPosition();
		const f32 dx = want.x - at.x, dy = want.y - at.y, dz = want.z - at.z;
		if (debug()) {
			apart += sqrtf(dx * dx + dy * dy + dz * dz);
			if (++counted % 3000 == 0) {
				fprintf(stderr, "[multiplayer] Gooper Blooper's tentacles: %.0f units from the host's on average\n", apart / 3000);
				apart = 0.0f;
			}
		}
		at.x += dx * 0.5f;
		at.y += dy * 0.5f;
		at.z += dz * 0.5f;
		t->mNodes[n].setPosition(at);
	}
}

// A tentacle's life: whole, torn off (4, flailing for its amputee time),
// gone (6, until the beak's pull brings all four back). Each game runs that
// clock itself and the host's runs a moment behind or ahead of each copy's,
// so a copy follows the host's changes as they come (an edge in the host's
// states), never its state as such: a copy whose torn tentacle has gone
// before the host's has is not torn off again, and the host's tentacle that
// is gone is not reported torn off again by a copy whose is still torn (which
// tore the host's tentacle off again and again, his tentacle-hurt step over
// and over, so the beak could not be pulled: gotBeakDamage refuses in it).
// Only what this game's player did since the last read (a change here that
// no read made) is reported.
void readTentacle(TBossGesso* b, int i, int host)
{
	TBGTentacle* t = b->mTentacles[i];
	if (!t)
		return;
	const int fps      = gessoFps();
	const int prevHost = s_tentHost[i];
	const int prevHere = s_tentHere[i];
	int here           = t->getState();
	if (s_followed && prevHere >= 0 && here != prevHere) {
		if (here == kTentacleTorn && !tornOrRegrowing(prevHere) && !tornOrRegrowing(host)) {
			mpg_boss_hit(i, HIT_TENTACLE, 1); // torn off, here
			s_tentOwnAt[i] = s_bossTick;
			if (debug())
				fprintf(stderr, "[multiplayer] Gooper Blooper: tentacle %d torn off here\n", i);
		}
		if (here == kTentacleStunned && stompable(host)) {
			mpg_boss_hit(i, HIT_TENTACLE_STUN, 1); // stomped flat here: flat in the host's too (so in everyone's)
			s_tentOwnAt[i] = s_bossTick;
		}
	}
	if (s_restoring) {
		// the host's own reloaded boss: as its last record says
		if (host == kTentacleTorn)
			tearFromAfar(t);
		else if (host == kTentacleRegrow || host == kTentacleStunned)
			t->changeStateAndFixNodes(host);
	} else if (host == kTentacleTorn && prevHost != kTentacleTorn && !tornOrRegrowing(here)) {
		tearFromAfar(t); // torn off in the host's game just now (by whoever)
	} else if (host == kTentacleRegrow && here != kTentacleRegrow
	           && !(here == kTentacleTorn && t->mTakeHit->getHolder())) {
		t->changeStateAndFixNodes(kTentacleRegrow); // gone there: gone here (one this game's player holds goes at its own time)
	} else if (!tornOrRegrowing(host) && tornOrRegrowing(here)) {
		// whole again in the host's game (the beak pulled), or whole there
		// for a while though torn here (a tear the host never had)
		if (s_tentApartSince[i] < 0 || here != prevHere)
			s_tentApartSince[i] = s_bossTick;
		if ((prevHost >= 0 && tornOrRegrowing(prevHost))
		    || (s_bossTick - s_tentApartSince[i] > 5 * fps && s_bossTick - s_tentOwnAt[i] > 5 * fps))
			t->changeStateAndFixNodes(0);
	} else if (host == kTentacleStunned && prevHost != kTentacleStunned && stompable(here)) {
		t->changeStateAndFixNodes(kTentacleStunned); // stomped flat in the host's game just now
	}
	if (!(!tornOrRegrowing(host) && tornOrRegrowing(t->getState())))
		s_tentApartSince[i] = -1;
	followTentacle(b, i, host);
	if (debug() && (host != prevHost || t->getState() != prevHere))
		fprintf(stderr, "[multiplayer] Gooper Blooper: tentacle %d host %d -> %d, here %d -> %d\n", i, prevHost, host, prevHere,
		        t->getState());
	s_tentHost[i] = host;
	s_tentHere[i] = t->getState();
}

void readGesso(In& in)
{
	TBossGesso* b = static_cast<TBossGesso*>(s_boss);
	const int nerve = in.u8_(), hp = in.u8_(), mode = in.u8_(), ink = in.u8_();
	int tentacles[TBossGesso::TENTACLE_NUM];
	for (int i = 0; i < TBossGesso::TENTACLE_NUM; i++)
		tentacles[i] = in.u8_();
	if (!in.ok)
		return;
	In where = in;
	in.f32_(), in.f32_(), in.f32_(), in.s16_();
	const bool hostDead = in.u8_() != 0 && in.ok;
	const int first     = in.u8_();
	JGeometry::TVec3<f32> nodes[2][kTentacleNodes];
	for (int k = 0; k < 2; k++)
		for (int n = 0; n < kTentacleNodes; n++) {
			nodes[k][n].x = in.s16_();
			nodes[k][n].y = in.s16_();
			nodes[k][n].z = in.s16_();
		}
	if (in.ok && first < TBossGesso::TENTACLE_NUM && s_newWorld)
		for (int k = 0; k < 2; k++) {
			const int t = (first + k) % TBossGesso::TENTACLE_NUM;
			memcpy(s_hostNodes[t], nodes[k], sizeof nodes[k]);
			s_hostNodesAt[t] = s_bossTick;
		}
	const int local = nerveIndex(gessoNerve, b->getLatestNerve());
	if (s_followed) {
		if (b->getHitPoints() < s_setHp) // the beak pulled, here
			mpg_boss_hit(0, HIT_BEAK, s_setHp - b->getHitPoints());
		if (local == kGessoEyeDamage && nerve != kGessoEyeDamage && !s_reported[kMaxUnits]) {
			mpg_boss_hit(0, HIT_EYE, 1); // an eye sprayed, here
			s_reported[kMaxUnits] = 1;
		}
	}
	if (local != kGessoEyeDamage)
		s_reported[kMaxUnits] = 0;
	// down in the host's game while this copy is not (it missed the step):
	// it goes down here too
	if (hostDead && local != kGessoDie && !b->checkLiveFlag(LIVE_FLAG_DEAD)) {
		fprintf(stderr, "[multiplayer] Gooper Blooper is down in the host's game: down here too\n");
		b->mHitPoints = 0;
		b->mSpine->reset();
		b->mSpine->setNext(gessoNerve(kGessoDie));
		return;
	}
	followNerve(b, 0, nerve, gessoNerve, kGessoDie, kGessoDie);
	// a copy never holds its last hit point: the beak pulled here takes it to
	// 1, not to its defeat, whose fall, camera and Shine come once the host's
	// goes down (each game plays its own)
	const int here    = hp == 1 && !s_restoring ? 2 : hp;
	b->mHitPoints     = (u8)here;
	b->mAttackMode    = mode;
	b->unk190.color.a = (u8)ink;
	// torn off, gone and stomped flat as the host's; holding and pulling
	// stay with the player who does them, in their own game
	for (int i = 0; i < TBossGesso::TENTACLE_NUM; i++)
		readTentacle(b, i, tentacles[i]);
	// its fall into the harbour is its own (eased toward the host's, it
	// would be held where the host's sank, short of where this one aims)
	if (local == kGessoDie || b->checkLiveFlag(LIVE_FLAG_DEAD)) {
		if (hostDead)
			s_gessoDying++; // the host's is gone: this one has a while to follow
	} else {
		s_gessoDying = 0;
		readWhere(where, b);
	}
	s_setHp = here;
}

// Every game, every tick: a player holding his beak pulls it, though another
// player's tear has him in his tentacle-hurt step (alone, the player who tore
// it is holding the torn tentacle then, not the beak), in which
// gotBeakDamage refuses the pull. Holding the beak takes him to his tug.
void beakOverTentacle()
{
	TBossGesso* b = static_cast<TBossGesso*>(s_boss);
	if (!b || b->checkLiveFlag(LIVE_FLAG_DEAD) || !b->mBeak || !b->mBeak->getHolder())
		return;
	if (b->getLatestNerve() != &TNerveBGTentacleDamage::theNerve())
		return;
	if (debug())
		fprintf(stderr, "[multiplayer] Gooper Blooper: his beak is held, out of his tentacle-hurt step\n");
	b->mSpine->reset();
	b->mSpine->setNext(&TNerveBGTug::theNerve());
	b->mSpine->pushAfterCurrent(&TNerveBGWait::theNerve());
}

// Client, every tick: a defeat that never ends here (its fall was cut short)
// ends a while after the host's is over (the fall itself takes half a minute).
void finishGesso()
{
	TBossGesso* b = static_cast<TBossGesso*>(s_boss);
	const int fps = port_active_frame_rate > 0 ? port_active_frame_rate : 30;
	if (b && !b->checkLiveFlag(LIVE_FLAG_DEAD) && s_gessoDying > 20 * fps) {
		fprintf(stderr, "[multiplayer] Gooper Blooper's fall never ended here: gone\n");
		b->changeAllTentacleState(0);
		b->kill();
	}
}

void hitGesso(int unit, int kind, int count)
{
	TBossGesso* b = static_cast<TBossGesso*>(s_boss);
	if (kind == HIT_EYE)
		b->gotEyeDamage();
	else if (kind == HIT_BEAK) {
		// pulled in another game while another player's tear has him hurt
		// here: the pull counts (gotBeakDamage refuses in that step)
		if (b->getLatestNerve() == &TNerveBGTentacleDamage::theNerve())
			b->mSpine->reset();
		for (int i = 0; i < count && b->getHitPoints() > 0; i++)
			b->gotBeakDamage();
	} else if (kind == HIT_TENTACLE && unit >= 0 && unit < TBossGesso::TENTACLE_NUM && b->mTentacles[unit])
		tearFromAfar(b->mTentacles[unit]); // not one already torn off or gone (gone: torn here long ago)
	else if (kind == HIT_TENTACLE_STUN && unit >= 0 && unit < TBossGesso::TENTACLE_NUM && b->mTentacles[unit]
	         && stompable(b->mTentacles[unit]->getState()))
		b->mTentacles[unit]->changeStateAndFixNodes(kTentacleStunned);
}

// ------------------------------------------------------------------ Eely-Mouth

void writeEel(Out& o)
{
	TBossEel* b = static_cast<TBossEel*>(s_boss);
	o.u8_((u8)nerveIndex(eelNerve, b->getLatestNerve()));
	o.u8_((b->mToothDamaged ? 1 : 0) | (b->mToothBroken ? 2 : 0));
	o.f32_(b->mAppearOffset);
	for (int i = 0; i < 8; i++)
		o.s16_(b->mTeeth[i] ? (s16)b->mTeeth[i]->mHitPoints : 0);
	writeWhere(o, b);
}

// A tooth's dirt shows its hit points (TBossEelTooth::receiveMessage).
void setToothHp(TBossEel* b, TBossEelTooth* t, int hp)
{
	const int max = b->getBossEelParams().mSLToothMaxHitPoint.get();
	t->mHitPoints = hp;
	t->mColor.a   = hp <= 1 ? 0 : (u8)((f32)(hp * 0xFF) / (f32)(max > 0 ? max : 1));
}

void readEel(In& in)
{
	TBossEel* b = static_cast<TBossEel*>(s_boss);
	const int nerve = in.u8_(), flags = in.u8_();
	const f32 appear = in.f32_();
	int teeth[8];
	for (int i = 0; i < 8; i++)
		teeth[i] = in.s16_();
	if (!in.ok)
		return;
	for (int i = 0; i < 8; i++) {
		TBossEelTooth* t = b->mTeeth[i];
		if (!t)
			continue;
		if (s_followed && t->mHitPoints < s_setValue[i]) // cleaned, here
			mpg_boss_hit(i, HIT_TOOTH, s_setValue[i] - t->mHitPoints);
		if (teeth[i] == 1 && t->mHitPoints > 1) {
			// clean in the host's game: it comes off here as it did there
			const bool broken = b->mToothBroken;
			b->mToothBroken   = false;
			for (int k = 0; k < 4096 && t->mHitPoints > 1; k++) {
				t->mDamageCooldown = 0;
				t->receiveMessage(gpMarioOriginal, HIT_MESSAGE_SPRAYED_BY_WATER);
			}
			b->mToothBroken = broken;
		} else if (t->mHitPoints != teeth[i]) {
			setToothHp(b, t, teeth[i]);
		}
		s_setValue[i] = teeth[i];
	}
	followNerve(b, 0, nerve, eelNerve, kEelDie, kEelDie);
	b->mToothDamaged = (flags & 1) != 0;
	b->mToothBroken  = (flags & 2) != 0;
	b->mAppearOffset = appear;
	readWhere(in, b);
}

void hitEel(int unit, int kind, int count)
{
	TBossEel* b = static_cast<TBossEel*>(s_boss);
	if (kind != HIT_TOOTH || unit < 0 || unit >= 8 || !b->mTeeth[unit])
		return;
	for (int i = 0; i < count; i++) {
		b->mTeeth[unit]->mDamageCooldown = 0;
		b->mTeeth[unit]->receiveMessage(gpMarioOriginal, HIT_MESSAGE_SPRAYED_BY_WATER);
	}
}

// ------------------------------------------------------------------ Phantamanta

// One piece (a slot of its manager): whether it is out, its generation (its
// size), its hits, its pause after a hit, its step, where it is and faces.
const int kMantaRecord = 5 + 12 + 12;

void writeManta(Out& o, int slot)
{
	TBossManta* m = static_cast<TBossManta*>(s_mantas->getObj(slot));
	o.u8_(m->checkLiveFlag(LIVE_FLAG_DEAD) ? 0 : 1);
	o.u8_((u8)m->mGeneration);
	o.u8_((u8)m->unk19C);
	o.u8_((u8)m->unk1A0);
	o.u8_((u8)nerveIndex(mantaNerve, m->getLatestNerve()));
	o.f32_(m->mPosition.x);
	o.f32_(m->mPosition.y);
	o.f32_(m->mPosition.z);
	o.f32_(m->unk170.x);
	o.f32_(m->unk170.y);
	o.f32_(m->unk170.z);
}

void readManta(In& in, int slot)
{
	if (!s_mantas || slot >= s_mantas->getObjNum() || slot >= kMaxUnits)
		return;
	TBossManta* m = static_cast<TBossManta*>(s_mantas->getObj(slot));
	const int alive = in.u8_(), gen = in.u8_(), hits = in.u8_(), pause = in.u8_(), nerve = in.u8_();
	const f32 x = in.f32_(), y = in.f32_(), z = in.f32_();
	const f32 fx = in.f32_(), fy = in.f32_(), fz = in.f32_();
	if (!in.ok || !m)
		return;
	const bool here = !m->checkLiveFlag(LIVE_FLAG_DEAD);
	if (here && alive && m->mGeneration == gen && s_reported[slot] && m->unk19C > s_setValue[slot])
		mpg_boss_hit(slot, HIT_MANTA, m->unk19C - s_setValue[slot]); // sprayed, here
	if (alive && (!here || m->mGeneration != gen)) {
		// out (or another size) in the host's game: here too, where it is there
		m->initNthGeneration(gen);
		m->mPosition.set(x, y, z);
		// out between this frame's movement and its drawing: not drawn until
		// its view is worked out (next frame), its shadow pass crashed on it
		m->onLiveFlag(LIVE_FLAG_CLIPPED_OUT);
		s_hostNerve[slot] = -1;
	} else if (!alive && here) {
		m->kill();
	}
	if (!alive) {
		s_reported[slot] = 0;
		return;
	}
	m->unk19C = hits;
	m->unk1A0 = pause;
	followNerve(m, slot, nerve, mantaNerve, kMantaSpawn, kMantaDeath);
	const f32 k = s_restoring ? 1.0f : 0.3f;
	m->mPosition.x += (x - m->mPosition.x) * k;
	m->mPosition.y += (y - m->mPosition.y) * k;
	m->mPosition.z += (z - m->mPosition.z) * k;
	m->unk170.set(fx, fy, fz);
	s_setValue[slot] = hits;
	s_reported[slot] = 1; // followed once: its hits from now on are news
}

void hitManta(int unit, int kind, int count)
{
	if (kind != HIT_MANTA || !s_mantas || unit < 0 || unit >= s_mantas->getObjNum())
		return;
	TBossManta* m = static_cast<TBossManta*>(s_mantas->getObj(unit));
	for (int i = 0; m && i < count && !m->checkLiveFlag(LIVE_FLAG_DEAD); i++) {
		m->unk1A0 = 0; // its pause after a hit was the client's
		m->collidedWithWater();
	}
}

// ------------------------------------------------------------------ Wiggler

const u32 kWigglerFlags = 0x10000 | 0x20000 | 0x40000 | 0x100000;

void writeWiggler(Out& o)
{
	TBossHanachan* b = static_cast<TBossHanachan*>(s_boss);
	o.u8_((u8)nerveIndex(wigglerNerve, b->getLatestNerve()));
	o.u8_(b->getHitPoints());
	o.u8_((u8)b->mWeakBodyIndex);
	const u32 flags = b->mLiveFlag & kWigglerFlags;
	o.bytes(&flags, 4);
	o.s16_(yawOf(b->mRotation.x));
	o.s16_(yawOf(b->mRotation.z));
	writeWhere(o, b);
}

void readWiggler(In& in)
{
	TBossHanachan* b = static_cast<TBossHanachan*>(s_boss);
	const int nerve = in.u8_(), hp = in.u8_(), weak = in.u8_();
	u32 flags;
	in.bytes(&flags, 4);
	const s16 rx = in.s16_(), rz = in.s16_();
	if (!in.ok)
		return;
	if (s_followed && b->getHitPoints() < s_setHp) // its weak segment pounded, here
		mpg_boss_hit(0, HIT_WIGGLER, s_setHp - b->getHitPoints());
	followNerve(b, 0, nerve, wigglerNerve, kWigglerDead, kWigglerDead);
	b->mHitPoints     = (u8)hp;
	b->mWeakBodyIndex = weak; // the segment that can be hit is the host's
	b->mLiveFlag      = (b->mLiveFlag & ~kWigglerFlags) | (flags & kWigglerFlags);
	b->mRotation.x    = easeAngle(b->mRotation.x, degreesOf(rx), 0.25f);
	b->mRotation.z    = easeAngle(b->mRotation.z, degreesOf(rz), 0.25f);
	readWhere(in, b);
	s_setHp = hp;
}

void hitWiggler(int, int kind, int count)
{
	TBossHanachan* b = static_cast<TBossHanachan*>(s_boss);
	const int nerve  = nerveIndex(wigglerNerve, b->getLatestNerve());
	if (kind == HIT_WIGGLER && (nerve == kWigglerTumble || nerve == kWigglerDown))
		for (int i = 0; i < count && b->getHitPoints() > 0; i++)
			b->execDamage();
}

// ------------------------------------------------------------------ King Boo

void writeKingBoo(Out& o)
{
	TBossTelesa* b = static_cast<TBossTelesa*>(s_boss);
	o.u8_((u8)nerveIndex(kingBooNerve, b->getLatestNerve()));
	o.u8_(b->getHitPoints());
	o.u8_(b->unk350 ? 1 : 0);
	writeWhere(o, b);
}

void readKingBoo(In& in)
{
	TBossTelesa* b = static_cast<TBossTelesa*>(s_boss);
	const int nerve = in.u8_(), hp = in.u8_(), spicy = in.u8_();
	if (!in.ok)
		return;
	if (s_followed) {
		// fruit while spicy, here (the host's own hit plays Die here too, and
		// Die takes the hit point: that one is not news)
		if (b->getHitPoints() < s_setHp && nerve != kKingBooDie)
			mpg_boss_hit(0, HIT_FRUIT, s_setHp - b->getHitPoints());
		// a pepper here: spicy, where the host's last word was not
		if (b->unk350 && !s_setValue[0] && !spicy)
			mpg_boss_hit(0, HIT_SPICY, 1);
	}
	s_setValue[0] = spicy;
	followNerve(b, 0, nerve, kingBooNerve, kKingBooDie, kKingBooDie);
	b->mHitPoints = (u8)hp;
	b->unk350     = spicy != 0;
	readWhere(in, b);
	s_setHp = hp;
}

void hitKingBoo(int, int kind, int)
{
	TBossTelesa* b = static_cast<TBossTelesa*>(s_boss);
	if (kind == HIT_SPICY) {
		b->unk350 = true;
		b->unk36C = 0;
	} else if (kind == HIT_FRUIT) {
		b->unk350 = true; // it was spicy in the game it was hit in
		b->kill();        // TBossTelesa::kill: the hit; Die takes the hit point
	}
}

// ------------------------------------------------------------------ Mecha-Bowser

// His fight runs on each player's own coaster ride (his moves are timed off
// it), so only the damage is shared: how many plates are off and how many
// hits the current one has left.
void writeMecha(Out& o)
{
	TTinKoopa* b = static_cast<TTinKoopa*>(s_boss);
	o.u8_((u8)nerveIndex(mechaNerve, b->getLatestNerve()));
	o.u8_((u8)b->mDamageStage);
	o.u8_((u8)b->mPartsHitPoints);
}

void readMecha(In& in)
{
	TTinKoopa* b = static_cast<TTinKoopa*>(s_boss);
	const int nerve = in.u8_(), stage = in.u8_(), parts = in.u8_();
	if (!in.ok)
		return;
	// a rocket here: a hit off the current plate (a plate coming off, at the
	// end of the Break the host's game sends too, is not news)
	if (s_followed && b->mDamageStage == s_setValue[0] && b->mPartsHitPoints < s_setHp)
		mpg_boss_hit(0, HIT_ROCKET, s_setHp - b->mPartsHitPoints);
	followNerve(b, 0, nerve, mechaNerve, -1, -1);
	b->mDamageStage    = stage;
	b->mPartsHitPoints = parts;
	s_setValue[0]      = stage;
	s_setHp            = parts;
}

void hitMecha(int, int kind, int count)
{
	TTinKoopa* b = static_cast<TTinKoopa*>(s_boss);
	for (int i = 0; kind == HIT_ROCKET && i < count && b->mDamageStage < 4; i++)
		b->hitParts();
}

// ------------------------------------------------------------------ Bowser's hot tub

// Bowser falls when all five corners of his tub are broken by super ground
// pounds (TBathtubGrip::receiveMessage, message 3); the last starts the
// ending. A corner broken in any game breaks in every game.
bool gripDown(TBathtubGrip* g) { return g->unk248 || !g->unk249; }

void writeTub(Out& o)
{
	u8 down = 0;
	for (int i = 0; i < 5; i++)
		if (s_tub->unk168[i] && gripDown(s_tub->unk168[i]))
			down |= (u8)(1 << i);
	o.u8_(down);
}

void readTub(In& in)
{
	const int down = in.u8_();
	if (!in.ok)
		return;
	for (int i = 0; i < 5; i++) {
		TBathtubGrip* g = s_tub->unk168[i];
		if (!g)
			continue;
		const bool host = (down >> i) & 1, here = gripDown(g);
		if (here && !host && !s_reported[i]) {
			mpg_boss_hit(i, HIT_GRIP, 1); // broken here
			s_reported[i] = 1;
		} else if (host && !here) {
			s_reported[i] = 1;
			g->receiveMessage(gpMarioOriginal, 3); // broken in the host's game: here too
		}
	}
}

void hitTub(int unit, int kind, int)
{
	if (kind == HIT_GRIP && unit >= 0 && unit < 5 && s_tub->unk168[unit] && !gripDown(s_tub->unk168[unit]))
		s_tub->unk168[unit]->receiveMessage(gpMarioOriginal, 3);
}

// ------------------------------------------------------------------ Shadow Mario

// The host's Shadow Mario is the one everyone chases: where he is, what he is
// doing, his water meter, the stomps left once he is down and the node his
// vanished run heads for.
const int kShadowRecord = 2 + 2 + 2 + 12 + 2 + 1;
int s_shadowWater, s_shadowStomps; // client: what was last set from the host's Shadow Mario
int s_shadowHostDoing = -1; // client: the host's step last taken
bool s_mainDownSaid;

// Debug: this game's Shadow Mario, whenever his step, whether he is drawn or
// what he carries changes (and the host's step, on a client).
void sayShadow(TEnemyMario* e, int hostDoing)
{
	static int said = -1;
	const bool drawn = e->checkEMFlag(TEnemyMario::EM_FLAG_DISP_PENCIL) && e->checkUnk114(TMario::UNK114_FLAG_VISIBLE)
	                   && e->mEMDoing != TEnemyMario::EM_DOING_HIDE;
	const int now = e->mEMDoing | (drawn ? 0x100 : 0) | (e->getHeldObject() ? 0x200 : 0) | ((hostDoing & 0xFF) << 12);
	if (!debug() || now == said)
		return;
	said = now;
	fprintf(stderr, "[multiplayer] Shadow Mario: step 0x%x%s, %s, %s\n", e->mEMDoing,
	        hostDoing < 0 ? "" : (hostDoing == e->mEMDoing ? " (the host's too)" : " (not the host's)"),
	        drawn ? "drawn" : "not drawn", e->getHeldObject() ? "carrying" : "carrying nothing");
}

void writeShadow(Out& o)
{
	TEnemyMario* e = s_shadow;
	sayShadow(e, -1);
	o.bytes(&e->mEMDoing, 2);
	o.s16_(e->mWaterCounter);
	o.s16_(e->mTrampleCount);
	o.f32_(e->mPosition.x);
	o.f32_(e->mPosition.y);
	o.f32_(e->mPosition.z);
	o.s16_(e->mFaceAngle.y);
	o.u8_((u8)e->mRunAwayNodeIndex);
}

// How far into going down and getting away a step is (0: not part of it): in
// the plaza, down, vanished, back on a node, waiting, to the gate, at it,
// drawing the M, gone into it. A game whose own player has moved him further
// on (each game runs the plaza's script for its own player) is not sent back.
int shadowStage(u16 doing)
{
	switch (doing) {
	case TEnemyMario::EM_DOING_PRE_DOWN_ANIMATION: return 1;
	case TEnemyMario::EM_DOING_DOWN_ANIMATION: return 2;
	case TEnemyMario::EM_DOING_RUN_AWAY_TO_NEAREST_NODE: return 3;
	case TEnemyMario::EM_DOING_REPLAY_RUN_AWAY: return 4;
	case TEnemyMario::EM_DOING_WAITING_TO_INVITE_MARIO: return 5;
	case TEnemyMario::EM_DOING_REPLAY_RUN_AWAY_TO_GATE: return 6;
	case TEnemyMario::EM_DOING_UNK18: return 7;
	case TEnemyMario::EM_DOING_GATE_DRAWING: return 8;
	case TEnemyMario::EM_DOING_DISAPPEAR_TO_GATE: return 9;
	}
	return 0;
}

// Steps in which he is not drawn, or only for a moment.
bool shadowHidden(u16 doing)
{
	return doing == TEnemyMario::EM_DOING_HIDE || doing == TEnemyMario::EM_DOING_DISAPPEAR
	    || doing == TEnemyMario::EM_DOING_DISAPPEAR_TO_GATE
	    || doing == TEnemyMario::EM_DOING_RUN_AWAY_TO_NEAREST_NODE;
}

JGeometry::TVec3<f32> shadowNode(TEnemyMario* e, int node)
{
	JGeometry::TVec3<f32> p;
	e->mEMario->getTracer()->getGraph()->getGraphNode(node).getPoint(&p);
	return p;
}

// This game's copy takes the host's new step as if it had come to it on its
// own: what the step's way in does (the run-away replay from the host's node,
// the run to the gate, the M drawn) is done here too. A step only set would
// leave him as the old one left him: hidden for good if that was his vanished
// run, which hides him until its own end and shows him again only there.
void enterShadowStep(TEnemyMario* e, u16 doing, int node)
{
	const u16 was = e->mEMDoing;
	const bool vanished = was == TEnemyMario::EM_DOING_RUN_AWAY_TO_NEAREST_NODE;
	if (debug())
		fprintf(stderr, "[multiplayer] Shadow Mario takes the host's step 0x%x here (was 0x%x, %u ticks in)\n", doing, was,
		        (unsigned)e->mEMDoingTimer);
	switch (doing) {
	case TEnemyMario::EM_DOING_REPLAY_RUN_AWAY:
		if (e->mRunAwayInputReplays && node >= 0 && node < 8 && e->mRunAwayInputReplays[node]) {
			// as his vanished run ends in the plaza: on the node, facing
			// where he will wait, his replay from there
			const JGeometry::TVec3<f32> at = shadowNode(e, node), wait = shadowNode(e, 7);
			e->mFaceAngle.y    = matan(wait.z - at.z, wait.x - at.x);
			e->mModelFaceAngle = e->mFaceAngle.y;
			e->mPosition.set(at.x, at.y + 5.0f, at.z);
			e->changePlayerStatus(MARIO_STATUS_WAIT, 0, true);
			e->mRunAwayNodeIndex = node;
			e->mReplayIndex      = node;
			e->mInputReplays     = e->mRunAwayInputReplays;
			e->mInputReplays[node]->reset();
			e->mInputReplays[node]->start();
		}
		e->changeEMDoing(doing);
		break;
	case TEnemyMario::EM_DOING_REPLAY_RUN_AWAY_TO_GATE:
		if (e->mGateReplay) {
			const JGeometry::TVec3<f32> wait = shadowNode(e, 7), gate = shadowNode(e, 8);
			e->mPosition       = wait;
			e->mFaceAngle.y    = matan(gate.z - wait.z, gate.x - wait.x);
			e->mModelFaceAngle = e->mFaceAngle.y;
			e->changePlayerStatus(MARIO_STATUS_WAIT, 0, true);
			e->mReplayIndex = 0;
			e->mGateReplay->reset();
			e->mGateReplay->start();
		}
		e->changeEMDoing(doing);
		break;
	case TEnemyMario::EM_DOING_GATE_DRAWING: e->startGateDrawing(); break;
	case TEnemyMario::EM_DOING_DISAPPEAR:
	case TEnemyMario::EM_DOING_DISAPPEAR_TO_GATE: e->startDisappear(doing); break;
	default:
		if (vanished && node >= 0 && node < e->mEMario->getTracer()->getGraph()->getNodeNum()) {
			// as his vanished run ends elsewhere: back on the node, waiting
			const JGeometry::TVec3<f32> at = shadowNode(e, node);
			e->mPosition.set(at.x, at.y + 5.0f, at.z);
			e->changePlayerStatus(MARIO_STATUS_WAIT, 0, true);
		}
		e->changeEMDoing(doing);
		break;
	}
	if (!shadowHidden(doing)) {
		e->onEMFlag(TEnemyMario::EM_FLAG_DISP_PENCIL);
		e->onUnk114(TMario::UNK114_FLAG_VISIBLE);
		if (was == TEnemyMario::EM_DOING_HIDE || was == TEnemyMario::EM_DOING_DISAPPEAR) {
			e->offHitFlag(HIT_FLAG_NO_COLLISION);
			e->mEMario->offHitFlag(HIT_FLAG_NO_COLLISION);
		}
	}
}

void readShadow(In& in)
{
	TEnemyMario* e = s_shadow;
	u16 doing;
	in.bytes(&doing, 2);
	const s16 water = in.s16_(), stomps = in.s16_();
	const f32 x = in.f32_(), y = in.f32_(), z = in.f32_();
	const s16 face = in.s16_();
	if (!in.ok)
		return;
	const int node = in.at < in.n ? in.u8_() : -1;
	if (s_followed) {
		if (e->mWaterCounter < s_shadowWater) // sprayed here
			mpg_boss_hit(0, HIT_SHADOW_WATER, s_shadowWater - e->mWaterCounter);
		if (e->mTrampleCount < s_shadowStomps) // stomped here
			mpg_boss_hit(0, HIT_SHADOW_STOMP, s_shadowStomps - e->mTrampleCount);
	}
	// This game's Shadow Mario runs, animates and is hit as ever (so he
	// walks and sounds right, and water lands where it is seen to); the
	// host's says how much water he has left and when he goes down, and
	// gives each new step of his. He is kept near the host's: left alone
	// within a step or so, pulled harder the further off, at once when far.
	const int hostStage = shadowStage(doing);
	if (doing != s_shadowHostDoing) {
		s_shadowHostDoing = doing;
		const int here = shadowStage(e->mEMDoing);
		if (doing != e->mEMDoing && !(hostStage && here > hostStage))
			enterShadowStep(e, doing, node);
	}
	// Down on the host: whoever he carries (the plaza's Peach) is let go here
	// too, as his own fall would have (the host's fall came as a step).
	if (hostStage && e->getHeldObject()) {
		((TLiveActor*)e->getHeldObject())->offLiveFlag(LIVE_FLAG_UNK100000);
		e->dropObject();
	}
	e->mWaterCounter = water;
	e->mTrampleCount = stomps;
	// His vanished run: the host's node, set before the run starts toward it
	// (it heads from where he vanished, so it reaches the node from there
	// whatever pulled him since he went down). Neither game's place means
	// anything while he is vanished, and a game further on goes its own way.
	const bool vanishedHere = e->mEMDoing == TEnemyMario::EM_DOING_RUN_AWAY_TO_NEAREST_NODE;
	if (vanishedHere && e->mEMDoingTimer >= 1 && e->mEMDoingTimer < 100) {
		if (node >= 0 && node < e->mEMario->getTracer()->getGraph()->getNodeNum()
		    && (hostStage == 3 || hostStage == 4))
			e->mRunAwayNodeIndex = node;
		e->mReferencePosition.set(e->mDisappearPosition.x, e->mDisappearPosition.y - 80.0f,
		                          e->mDisappearPosition.z);
	}
	sayShadow(e, doing);
	if (vanishedHere || hostStage == 3 || (hostStage && shadowStage(e->mEMDoing) > hostStage)) {
		s_shadowWater  = water;
		s_shadowStomps = stomps;
		return;
	}
	const f32 dx = x - e->mPosition.x, dy = y - e->mPosition.y, dz = z - e->mPosition.z;
	const f32 far = dx * dx + dy * dy + dz * dz;
	if (far > 700.0f * 700.0f || s_restoring) {
		e->mPosition.set(x, y, z);
		e->mFaceAngle.y = face;
	} else if (far > 30.0f * 30.0f) {
		const f32 k = far > 200.0f * 200.0f ? 0.15f : 0.06f;
		e->mPosition.x += dx * k;
		e->mPosition.y += dy * k;
		e->mPosition.z += dz * k;
	}
	s_shadowWater  = water;
	s_shadowStomps = stomps;
}

void hitShadow(int, int kind, int count)
{
	TEnemyMario* e = s_shadow;
	for (int i = 0; i < count; i++) {
		if (kind == HIT_SHADOW_WATER) {
			// a spray on him (its splash where he is, not at this game's Mario)
			TWaterHitActor& water = TModelWaterManager::mStaticHitActor;
			water.mPosition       = e->mPosition;
			water.mParticleIndex  = 0;
			e->hitWater(&water);
		}
		else if (kind == HIT_SHADOW_STOMP && e->mEMDoing == TEnemyMario::EM_DOING_DOWN_ANIMATION)
			e->thinkTrample();
	}
}

// Host: the last record of each boss part it sent this session. Losing a
// life reloads the host's stage in the same session while the others fight on:
// its fresh boss would be everyone's again at full strength (corners of
// Bowser's tub mended, a Gatekeeper healed). Once the reloaded boss has set
// itself up, these are read into it, as a joining game reads the host's (no
// hits come of it: a host reports none), and the fight goes on where it was.
struct Kept {
	u8 kind, unit, len;
	u8 data[96];
};
const int kMaxKept = kMaxUnits + 32; // each of Phantamanta's pieces has its own, the Gatekeepers theirs
Kept s_kept[kMaxKept];
int s_keptCount;
uint32_t s_keptSession;
int s_restoreWait = -1; // host, after a stage setup: ticks the boss has been ready (-1: nothing to do)

bool restorable(int kind) { return kind >= KIND_PETEY && kind <= KIND_GATE; }

void keepRecords(const u8* buf, int n)
{
	In in(buf, n);
	while (in.ok && in.at + 3 <= n) {
		const int kind = in.u8_(), unit = in.u8_(), len = in.u8_();
		if (!in.ok || in.at + len > n)
			break;
		const u8* data = buf + in.at;
		in.at += len;
		if (!restorable(kind) || len > (int)sizeof s_kept[0].data)
			continue;
		int i = 0;
		while (i < s_keptCount && (s_kept[i].kind != kind || s_kept[i].unit != unit))
			i++;
		if (i == s_keptCount) {
			if (s_keptCount == kMaxKept)
				continue;
			s_keptCount++;
		}
		s_kept[i].kind = (u8)kind, s_kept[i].unit = (u8)unit, s_kept[i].len = (u8)len;
		memcpy(s_kept[i].data, data, len);
	}
}

void restoreKept()
{
	int restored = 0;
	s_restoring = true;
	for (int i = 0; i < s_keptCount; i++) {
		const Kept& k = s_kept[i];
		In rec(k.data, k.len);
		if (k.kind == KIND_GATE) {
			if (s_gates && gateAt(k.unit))
				readGate(rec, k.unit), restored++;
			continue;
		}
		if (k.kind == KIND_SHADOW) {
			if (s_shadow)
				readShadow(rec), restored++;
			continue;
		}
		if (k.kind == KIND_MANTA) {
			if (s_kind == KIND_MANTA)
				readManta(rec, k.unit), restored++;
			continue;
		}
		if (k.kind != s_kind || (s_boss && s_boss->checkLiveFlag(LIVE_FLAG_DEAD)))
			continue;
		switch (k.kind) {
		case KIND_PETEY: readPetey(rec); break;
		case KIND_GESSO: readGesso(rec); break;
		case KIND_EEL: readEel(rec); break;
		case KIND_WIGGLER: readWiggler(rec); break;
		case KIND_KINGBOO: readKingBoo(rec); break;
		case KIND_MECHA: readMecha(rec); break;
		case KIND_TUB:
			if (k.len && k.data[0] == 0x1F)
				continue; // all five down: the fight was won (the ending is the game's)
			readTub(rec);
			break;
		}
		restored++;
		if (debug())
			fprintf(stderr, "[multiplayer] boss: restored kind %d part %d (first byte %02x)\n", k.kind, k.unit, k.data[0]);
	}
	s_restoring = false;
	if (restored)
		fprintf(stderr, "[multiplayer] boss: back as the fight was before this game's player lost a life (%d part%s)\n",
		        restored, restored == 1 ? "" : "s");
}

} // namespace

// ------------------------------------------------------------------ the hooks

void MpBossStageSetup()
{
	s_kind     = 0;
	s_boss     = nullptr;
	s_mantas   = nullptr;
	s_gates    = nullptr;
	memset(s_sprayWait, 0, sizeof s_sprayWait);
	s_tub      = nullptr;
	s_shadow   = nullptr;
	s_shadowList = nullptr;
	s_shadowAlone = nullptr;
	s_shadowWater = s_shadowStomps = 0;
	s_shadowHostDoing = -1;
	s_mainDownSaid = false;
	s_next     = 0;
	s_followed = false;
	s_restoreWait = s_keptCount ? 0 : -1; // the host's: back in the same session, or not (MpBossHostFrame)
	s_setHp    = 0;
	memset(s_setValue, 0, sizeof s_setValue);
	memset(s_reported, 0, sizeof s_reported);
	s_gessoDying = 0;
	resetTentacles();
	for (int i = 0; i < kMaxUnits; i++)
		s_hostNerve[i] = s_localNerve[i] = -1;
	if (!gpConductor)
		return;
	// every boss here, not just the first found: a stage can have its boss
	// and Gatekeepers and Shadow Mario (the plaza has a sleeping Gatekeeper
	// beside Shadow Mario's chase)
	for (JGadget::TList<TEnemyManager*>::iterator it = gpConductor->unk20.begin(); it != gpConductor->unk20.end(); ++it) {
		TEnemyManager* mgr = *it;
		if (TEMarioManager* em = dynamic_cast<TEMarioManager*>(mgr)) {
			s_shadowList = em; // Shadow Mario himself comes later
			continue;
		}
		if (!mgr || !mgr->getObjNum())
			continue;
		TLiveActor* first = mgr->getObj(0);
		if (dynamic_cast<TBiancoGateKeeper*>(first)) {
			s_gates = mgr;
			continue;
		}
		if (s_kind)
			continue;
		if (TBossPakkunManager* pm = dynamic_cast<TBossPakkunManager*>(mgr)) {
			if (debug())
				fprintf(stderr, "[multiplayer] a Petey manager (%s)\n", pm->mIsLightVersion ? "light" : "full");
			if (!pm->mIsLightVersion && dynamic_cast<TBossPakkun*>(first)) // the light one does not fight
				s_kind = KIND_PETEY, s_boss = static_cast<TSpineEnemy*>(first);
		} else if (dynamic_cast<TBossGesso*>(first)) {
			s_kind = KIND_GESSO, s_boss = static_cast<TSpineEnemy*>(first);
		} else if (dynamic_cast<TBossEel*>(first)) {
			s_kind = KIND_EEL, s_boss = static_cast<TSpineEnemy*>(first);
		} else if (dynamic_cast<TBossManta*>(first)) {
			s_kind = KIND_MANTA, s_mantas = mgr;
		} else if (dynamic_cast<TBossHanachan*>(first)) {
			s_kind = KIND_WIGGLER, s_boss = static_cast<TSpineEnemy*>(first);
		} else if (dynamic_cast<TBossTelesa*>(first)) {
			s_kind = KIND_KINGBOO, s_boss = static_cast<TSpineEnemy*>(first);
		} else if (dynamic_cast<TTinKoopa*>(first)) {
			s_kind = KIND_MECHA, s_boss = static_cast<TSpineEnemy*>(first);
		}
	}
	// Shadow Mario's list may sit with the other object lists, not the enemies'
	if (!s_shadowList)
		for (JGadget::TList<TLiveManager*>::iterator it = gpConductor->unk10.begin(); it != gpConductor->unk10.end(); ++it)
			if (TEMarioManager* em = dynamic_cast<TEMarioManager*>(*it))
				s_shadowList = em;
	// or on his own (the conductor's lone actors: the plaza's Shadow Mario)
	if (!s_shadowList)
		for (JGadget::TList<TLiveActor*>::iterator it = gpConductor->unk30.begin(); it != gpConductor->unk30.end(); ++it)
			if (TEMario* em = dynamic_cast<TEMario*>(*it))
				// Il Piantissimo is one of these too: each player races their own
				if (em->mEnemyMario && em->mEnemyMario->mPlayerType != TMario::PLAYER_TYPE_MONTE_MAN)
					s_shadowAlone = em;
	if (!s_kind)
		if (TBathtub* tub = dynamic_cast<TBathtub*>(JDrama::TNameRefGen::search2("バスタブ")))
			if (tub->unk168)
				s_kind = KIND_TUB, s_tub = tub;
	static const char* const names[] = {"",        "Petey Piranha", "Gooper Blooper", "Eely-Mouth",  "Phantamanta",
	                                    "Wiggler", "King Boo",      "Mecha-Bowser",   "Bowser's tub", "Shadow Mario",
	                                    "The Gatekeeper"};
	if (s_kind)
		fprintf(stderr, "[multiplayer] %s is here: the host's is everyone's\n", names[s_kind]);
	if (s_gates)
		fprintf(stderr, "[multiplayer] %s is here: the host's is everyone's\n", names[KIND_GATE]);
	if (s_shadowList || s_shadowAlone)
		fprintf(stderr, "[multiplayer] %s is here: the host's is everyone's\n", names[KIND_SHADOW]);
	if (!s_kind && !s_gates && !s_shadowList && !s_shadowAlone && debug())
		for (JGadget::TList<TEnemyManager*>::iterator it = gpConductor->unk20.begin(); it != gpConductor->unk20.end(); ++it)
			if (*it && (*it)->getObjNum())
				fprintf(stderr, "[multiplayer] enemies here: %d of actor type %x\n", (*it)->getObjNum(),
				        (unsigned)(*it)->getObj(0)->getActorType());
}

// Shadow Mario is set up after the stage's objects: found when he is there.
bool bossReady()
{
	if (!s_shadow && s_shadowList)
		for (int i = 0; i < s_shadowList->getObjNum() && !s_shadow; i++)
			if (TEMario* em = dynamic_cast<TEMario*>(s_shadowList->getObj(i)))
				s_shadow = em->mEnemyMario;
	if (!s_shadow && s_shadowAlone)
		s_shadow = s_shadowAlone->mEnemyMario;
	return s_kind || s_gates || s_shadow;
}

// The record of the stage's main boss (one kind a stage), as [kind, unit, length, payload].
void writeMain(Out& o)
{
	if (s_kind == KIND_MANTA) {
		const int n = s_mantas->getObjNum() < kMaxUnits ? s_mantas->getObjNum() : kMaxUnits;
		for (int k = 0; k < kMantaPerUpdate && k < n && o.room(3 + kMantaRecord); k++) {
			const int slot = (s_next + k) % n;
			o.u8_(KIND_MANTA);
			o.u8_((u8)slot);
			o.u8_((u8)kMantaRecord);
			writeManta(o, slot);
		}
		if (n)
			s_next = (s_next + kMantaPerUpdate) % n;
		return;
	}
	o.u8_((u8)s_kind);
	o.u8_(0);
	const int lenAt = o.n;
	o.u8_(0);
	switch (s_kind) {
	case KIND_PETEY: writePetey(o); break;
	case KIND_GESSO: writeGesso(o); break;
	case KIND_EEL: writeEel(o); break;
	case KIND_WIGGLER: writeWiggler(o); break;
	case KIND_KINGBOO: writeKingBoo(o); break;
	case KIND_MECHA: writeMecha(o); break;
	case KIND_TUB: writeTub(o); break;
	}
	o.p[lenAt] = (u8)(o.n - lenAt - 1);
}

void MpBossHostFrame()
{
	if (!bossReady())
		return;
	if (s_restoreWait >= 0) {
		if (mpg_session() != s_keptSession) {
			s_keptCount   = 0; // a new session: a new fight
			s_restoreWait = -1;
		} else if (++s_restoreWait < 30) {
			return; // the reloaded boss sets itself up first; the others' hits wait, and nothing is sent
		} else {
			restoreKept();
			s_restoreWait = -1;
		}
	}
	if (s_kind == KIND_GESSO)
		beakOverTentacle();
	int unit, kind, count;
	while (mpg_next_boss_hit(&unit, &kind, &count)) {
		if (debug())
			fprintf(stderr, "[multiplayer] boss: %d hit(s) of kind %d on part %d from another game\n", count, kind, unit);
		// the kind of hit says whose it is
		if (kind == HIT_GATE_SPRAY || kind == HIT_GATE_MOUTH) {
			hitGate(unit, kind, count);
			continue;
		}
		if (kind == HIT_SHADOW_WATER || kind == HIT_SHADOW_STOMP) {
			if (s_shadow)
				hitShadow(unit, kind, count);
			continue;
		}
		switch (s_kind) {
		case KIND_PETEY: hitPetey(unit, kind, count); break;
		case KIND_GESSO: hitGesso(unit, kind, count); break;
		case KIND_EEL: hitEel(unit, kind, count); break;
		case KIND_MANTA: hitManta(unit, kind, count); break;
		case KIND_WIGGLER: hitWiggler(unit, kind, count); break;
		case KIND_KINGBOO: hitKingBoo(unit, kind, count); break;
		case KIND_MECHA: hitMecha(unit, kind, count); break;
		case KIND_TUB: hitTub(unit, kind, count); break;
		}
	}
	u8 buf[200];
	Out o(buf, sizeof buf);
	if (s_kind)
		writeMain(o);
	if (s_shadow && o.room(3 + kShadowRecord)) {
		o.u8_(KIND_SHADOW);
		o.u8_(0);
		o.u8_((u8)kShadowRecord);
		writeShadow(o);
	}
	if (s_gates) {
		const int n = s_gates->getObjNum() < kMaxGates ? s_gates->getObjNum() : kMaxGates;
		for (int g = 0; g < n && o.room(3 + kGateRecord); g++) {
			if (!gateAt(g))
				continue;
			o.u8_(KIND_GATE);
			o.u8_((u8)g);
			o.u8_((u8)kGateRecord);
			writeGate(o, g);
		}
	}
	mpg_set_boss(buf, o.n);
	keepRecords(buf, o.n);
	s_keptSession = mpg_session();
	static bool saidDown;
	if (s_boss && (s_boss->checkLiveFlag(LIVE_FLAG_DEAD) != 0) != saidDown) {
		saidDown = s_boss->checkLiveFlag(LIVE_FLAG_DEAD) != 0;
		if (debug())
			fprintf(stderr, "[multiplayer] the boss (host) is %s\n", saidDown ? "gone" : "here");
	}
	static int debugFrames;
	if (debug() && (s_boss || s_tub || s_shadow || s_mantas) && ++debugFrames % 300 == 0) {
		if (s_shadow)
			fprintf(stderr, "[multiplayer] boss (host): Shadow Mario's water %d, stomps %d\n", (int)s_shadow->mWaterCounter,
			        (int)s_shadow->mTrampleCount);
		if (s_mantas) {
			int alive = 0, hits = -1, gen = -1;
			for (int i = 0; i < s_mantas->getObjNum(); i++)
				if (!s_mantas->getObj(i)->checkLiveFlag(LIVE_FLAG_DEAD)) {
					TBossManta* m = static_cast<TBossManta*>(s_mantas->getObj(i));
					if (!alive++)
						hits = m->unk19C, gen = m->mGeneration;
				}
			fprintf(stderr, "[multiplayer] boss (host): %d Phantamanta pieces out (the first: size %d, %d hits)\n", alive, gen,
			        hits);
		}
		if (s_boss)
			fprintf(stderr, "[multiplayer] boss (host): kind %d, %d hit points\n", s_kind, s_boss->getHitPoints());
		else if (s_tub)
			fprintf(stderr, "[multiplayer] boss (host): the tub's corners down %02x\n", buf[3]);
	}
}

void MpBossClientTick()
{
	if (!bossReady())
		return;
	s_bossTick++;
	if (s_kind == KIND_GESSO)
		beakOverTentacle();
	u8 buf[200];
	float age     = 0.0f;
	const int got = (int)mpg_boss(buf, sizeof buf, &age);
	static uint32_t lastSeq;
	s_newWorld = mpg_world_seq() != lastSeq;
	lastSeq    = mpg_world_seq();
	if (!got || age > 1.0f || MpHostPaused()) {
		s_followed = false;
		return;
	}
	// the main boss down here too: it follows no more (the others still do)
	const bool mainDown = s_boss && s_boss->checkLiveFlag(LIVE_FLAG_DEAD);
	if (mainDown && s_followed && !s_mainDownSaid) {
		fprintf(stderr, "[multiplayer] the boss is down here too: its Shine is out\n");
		s_mainDownSaid = true;
	}
	In in(buf, got);
	while (in.ok && in.at + 3 <= got) {
		const int kind = in.u8_(), unit = in.u8_(), len = in.u8_();
		if (!in.ok || in.at + len > got)
			break;
		In rec(buf + in.at, len);
		in.at += len;
		if (kind == KIND_GATE) {
			if (s_gates)
				readGate(rec, unit);
			continue;
		}
		if (kind == KIND_SHADOW) {
			if (s_shadow)
				readShadow(rec);
			continue;
		}
		if (kind != s_kind || mainDown)
			continue;
		switch (kind) {
		case KIND_PETEY: readPetey(rec); break;
		case KIND_GESSO: readGesso(rec); break;
		case KIND_EEL: readEel(rec); break;
		case KIND_MANTA: readManta(rec, unit); break;
		case KIND_WIGGLER: readWiggler(rec); break;
		case KIND_KINGBOO: readKingBoo(rec); break;
		case KIND_MECHA: readMecha(rec); break;
		case KIND_TUB: readTub(rec); break;
		}
	}
	s_followed = true;
	if (s_kind == KIND_GESSO)
		finishGesso();
	static int debugTicks;
	if (debug() && s_boss && ++debugTicks % 300 == 0)
		fprintf(stderr, "[multiplayer] boss (here): kind %d, %d hit points\n", s_kind, s_boss->getHitPoints());
}

bool MpBossTestTubMiddle(float at[3])
{
	TBathtub* tub = s_tub ? s_tub : dynamic_cast<TBathtub*>(JDrama::TNameRefGen::search2("バスタブ"));
	if (!tub)
		return false;
	at[0] = tub->mPosition.x, at[1] = tub->mPosition.y, at[2] = tub->mPosition.z;
	return true;
}

bool MpBossBuildsAroundMario(const void* object)
{
	if (!s_tub || !object)
		return false;
	const TLiveActor* a = static_cast<const TLiveActor*>(object); // as objectMove passes it
	if (a == static_cast<const TLiveActor*>(s_tub))
		return true;
	for (int i = 0; i < 5 && s_tub->unk168; i++)
		if (s_tub->unk168[i] && a == static_cast<const TLiveActor*>(s_tub->unk168[i]))
			return true;
	return false;
}

// Test aid (SMS_MULTIPLAYER_TEST bosshit@N): this game's player lands a hit on
// its copy, as playing would (the report then reaches the host's boss).
int MpBossTestHit()
{
	switch (s_kind ? s_kind : s_gates ? KIND_GATE : s_shadow ? KIND_SHADOW : 0) {
	case KIND_PETEY:
		for (int i = 0; i < 3; i++)
			static_cast<TBossPakkun*>(s_boss)->gotWaterDamage();
		return s_kind;
	case KIND_GESSO: static_cast<TBossGesso*>(s_boss)->gotBeakDamage(); return s_kind;
	case KIND_EEL: {
		TBossEel* b = static_cast<TBossEel*>(s_boss);
		for (int i = 0; i < 5 && b->mTeeth[0]; i++) {
			b->mTeeth[0]->mDamageCooldown = 0;
			b->mTeeth[0]->receiveMessage(gpMarioOriginal, HIT_MESSAGE_SPRAYED_BY_WATER);
		}
		return s_kind;
	}
	case KIND_MANTA:
		for (int i = 0; s_mantas && i < s_mantas->getObjNum(); i++) {
			TBossManta* m = static_cast<TBossManta*>(s_mantas->getObj(i));
			if (m && !m->checkLiveFlag(LIVE_FLAG_DEAD)) {
				m->unk1A0 = 0;
				m->collidedWithWater();
				return s_kind;
			}
		}
		return 0;
	case KIND_WIGGLER: static_cast<TBossHanachan*>(s_boss)->execDamage(); return s_kind;
	case KIND_KINGBOO:
		static_cast<TBossTelesa*>(s_boss)->unk350 = true;
		static_cast<TBossTelesa*>(s_boss)->unk36C = 0;
		return s_kind;
	case KIND_MECHA: static_cast<TTinKoopa*>(s_boss)->hitParts(); return s_kind;
	case KIND_TUB:
		for (int i = 0; i < 5; i++)
			if (s_tub->unk168[i] && !gripDown(s_tub->unk168[i])) {
				s_tub->unk168[i]->receiveMessage(gpMarioOriginal, 3);
				return s_kind;
			}
		return 0;
	case KIND_SHADOW: s_shadow->hitWater(gpMarioOriginal); return KIND_SHADOW;
	case KIND_GATE:
		// water on it asleep, or in its open mouth, as a player aims it: its
		// own step decides what that does
		for (int i = 0; i < kMaxGates; i++)
			if (TBiancoGateKeeper* g = gateAt(i))
				if (!g->checkLiveFlag(LIVE_FLAG_DEAD)) {
					const int nerve = nerveIndex(gateNerve, g->getLatestNerve());
					if (nerve == 0 || nerve == 4 || g->isHeadHitActive())
						g->unk154 = 1;
					return KIND_GATE;
				}
		return 0;
	}
	return 0;
}

int MpBossTestDown()
{
	if (s_kind != KIND_GESSO || !s_boss || s_boss->checkLiveFlag(LIVE_FLAG_DEAD))
		return 0;
	TBossGesso* b = static_cast<TBossGesso*>(s_boss);
	fprintf(stderr, "[multiplayer] test: Gooper Blooper's beak pulled at his last hit point\n");
	b->mHitPoints = 1;
	b->mSpine->reset();
	b->gotBeakDamage();
	return 1;
}

int MpBossTestTear()
{
	if (s_kind != KIND_GESSO || !s_boss)
		return 0;
	TBGTentacle* t = static_cast<TBossGesso*>(s_boss)->mTentacles[0];
	if (!t)
		return 0;
	fprintf(stderr, "[multiplayer] test: tearing Gooper Blooper's tentacle 0 (attack mode %d)\n",
	        static_cast<TBossGesso*>(s_boss)->mAttackMode);
	t->mDamageCount = t->mParams->mDamageCountMax.get();
	return 1;
}

// Test aid (shadowspray@N:K): a spray on this game's Shadow Mario.
int MpBossTestShadow()
{
	if (!bossReady() || !s_shadow)
		return 0;
	TWaterHitActor& water = TModelWaterManager::mStaticHitActor;
	water.mPosition       = s_shadow->mPosition;
	water.mParticleIndex  = 0;
	s_shadow->hitWater(&water);
	return KIND_SHADOW;
}

// Test aid (gotogate@N:D): where this game's first Gatekeeper still up is.
bool MpBossTestMantaOut()
{
	if (s_kind != KIND_MANTA || !s_mantas || !s_mantas->getObjNum() || !gpMarioOriginal)
		return false;
	TBossManta* m = static_cast<TBossManta*>(s_mantas->getObj(0));
	m->initNthGeneration(0);
	m->mPosition.set(gpMarioOriginal->mPosition.x, gpMarioOriginal->mPosition.y, gpMarioOriginal->mPosition.z + 1500.0f);
	m->onLiveFlag(LIVE_FLAG_CLIPPED_OUT); // as readManta's (drawn once its view is worked out)
	fprintf(stderr, "[multiplayer] test: Phantamanta out\n");
	return true;
}

bool MpBossTestGatePos(float at[3])
{
	for (int i = 0; i < kMaxGates; i++)
		if (TBiancoGateKeeper* g = gateAt(i))
			if (!g->checkLiveFlag(LIVE_FLAG_DEAD)) {
				at[0] = g->mPosition.x;
				at[1] = g->mPosition.y;
				at[2] = g->mPosition.z;
				return true;
			}
	return false;
}

// Test aid (SMS_MULTIPLAYER_TEST shadowrun@N, the host): Shadow Mario goes
// down as when sprayed dry, letting go of whoever he carries (step 0), then
// gets up and vanishes (step 1), as the plaza's script has him do once a
// player comes to him; step 2 ends his vanished run at once.
int MpBossTestShadowRun(int step)
{
	if (!bossReady() || !s_shadow)
		return 0;
	TEnemyMario* e = s_shadow;
	if (step == 0) {
		if (e->getHeldObject()) {
			((TLiveActor*)e->getHeldObject())->offLiveFlag(LIVE_FLAG_UNK100000);
			e->dropObject();
		}
		e->changeEMDoing(TEnemyMario::EM_DOING_PRE_DOWN_ANIMATION);
	} else if (step == 1)
		e->startRunAway();
	else if (e->mEMDoing == TEnemyMario::EM_DOING_RUN_AWAY_TO_NEAREST_NODE)
		e->mEMDoingTimer = 200; // (step 2) back on his node at once: this game ahead of the others
	return KIND_SHADOW;
}

// Whether `actor` is one of the bosses shared here (their hits go through
// the boss's own reports, not as hits on shared objects).
bool MpBossIsBoss(const void* actor)
{
	const TLiveActor* a = static_cast<const TLiveActor*>(actor);
	if (!a)
		return false;
	TLiveActor* b = const_cast<TLiveActor*>(a);
	return dynamic_cast<TBossPakkun*>(b) || dynamic_cast<TBossGesso*>(b) || dynamic_cast<TBossEel*>(b)
	       || dynamic_cast<TBossManta*>(b) || dynamic_cast<TBossHanachan*>(b) || dynamic_cast<TBossTelesa*>(b)
	       || dynamic_cast<TTinKoopa*>(b) || dynamic_cast<TBiancoGateKeeper*>(b) || dynamic_cast<TEMario*>(b);
	// (Il Piantissimo is a TEMario too: left alone, not shared as an object)
}
