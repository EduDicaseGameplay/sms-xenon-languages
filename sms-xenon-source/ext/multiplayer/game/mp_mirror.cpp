// Mirrored objects in multiplayer: things whose state the game changes on
// its own, run by one game (the owner), which sends their state about ten
// times a second; the others take it after each movement step.
//
//   - Who owns one: the game whose player rides it (a boat: Mario's ground
//     is it) or used it lately (sprayed a slot machine, bumped the
//     watermelon); else the host. A game that hears from a client about an
//     object leaves it to that client for half a second.
//   - Objects are named as shared objects are (mp_game.cpp): their manager's
//     place in the stage's list and their place in it, items' managers
//     included, lone actors as manager 255; the sea's waves are manager 254.
//   - Each kind says what to send (at most 48 bytes) and how to take it:
//     movers are put where the owner's are (moved on by their speed since),
//     NPCs eased toward it, a slot machine's reels set to the owner's.
//
// What the owner's game alone may decide (a slot machine's draw, an egg's
// wish) is never decided in the others: their copies are kept from it.
#include "mp_mirror.hpp"
#include "mp_boss.hpp"
#include "../mp_game_api.h"

#include <Enemy/BossWanwan.hpp>
#include <Enemy/Conductor.hpp>
#include <Enemy/FruitsBoat.hpp>
#include <Enemy/Koopa.hpp>
#include <Enemy/KoopaJr.hpp>
#include <Enemy/KoopaNerve.hpp>
#include <Enemy/LimitKoopaJr.hpp>
#include <Enemy/RiccoHook.hpp>
#include <Enemy/WireTrap.hpp>
#include <Enemy/SmallEnemy.hpp>
#include <Enemy/SleepBossHanachan.hpp>
#include <Animal/AnimalBase.hpp>
#include <Animal/BirdNerve.hpp>
#include <Animal/boid.hpp>
#include <Animal/fishoid.hpp>
#include <M3DUtil/MActor.hpp>
#include <M3DUtil/M3UModelMario.hpp>
#include <M3DUtil/MActorUtil.hpp>
#include <M3DUtil/SDLModel.hpp>
#include <MSound/MSound.hpp>
#include <MSound/SoundEffects.hpp>
#include <Map/Map.hpp>
#include <Map/MapData.hpp>
#include <MoveBG/Item.hpp>
#include <MoveBG/ItemManager.hpp>
#include <MoveBG/MapObjBall.hpp>
#include <MoveBG/MapObjBianco.hpp>
#include <MoveBG/MapObjBlock.hpp>
#include <MoveBG/MapObjCorona.hpp>
#include <MoveBG/MapObjDolpic.hpp>
#include <MoveBG/MapObjEx.hpp>
#include <MoveBG/MapObjMare.hpp>
#include <MoveBG/MapObjMonte.hpp>
#include <MoveBG/MapObjMamma.hpp>
#include <MoveBG/MapObjPinna.hpp>
#include <MoveBG/MapObjRailBlock.hpp>
#include <MoveBG/MapObjRicco.hpp>
#include <MoveBG/MapObjTurn.hpp>
#include <MoveBG/MapObjSirena.hpp>
#include <MoveBG/MapObjTown.hpp>
#include <MoveBG/MapObjWave.hpp>
#include <NPC/NpcBase.hpp>
#include <Player/Mario.hpp>
#include <Player/MarioAccess.hpp>
#include <Player/Yoshi.hpp>
#include <System/Application.hpp>
#include <System/MarDirector.hpp>
#include <stdio.h>
#include <typeinfo>
#include <stdlib.h>
#include <string.h>

namespace {

const int kAlone = 255, kWaves = 254;

int fps(); // ticks a second (below)
const int kMaxMirrors = 512; // (a sand course has some 200 blocks)

enum Owner {
	OWN_HOST,  // the host's (no player steers it)
	OWN_RIDER, // whoever stands on it
	OWN_USER,  // whoever used it last (sprayed it, bumped it)
	OWN_EACH,  // every player's own; the others see a rider's under them (MpMirrorRide)
};

struct Kind {
	const char* name;
	bool (*match)(TLiveActor*);
	int (*save)(TLiveActor*, u8* out);                     // bytes written
	void (*take)(TLiveActor*, const u8* in, int size, int age, int* scratch); // age: ticks since it came
	Owner owner;
	int every;       // ticks between sends
	bool keepsHits;  // the others' hits do not land here (the owner sends the outcome)
	// drawn smoothly a moment in the past (smooth > 0): its state starts with
	// `smooth` floats (those in angleMask are degrees, turned the short way
	// round) blended between the two states either side of that moment, by
	// the owner's clock, which each state carries at its end; `take` gets the
	// blend and the newest state's other bytes, and sets them as they are
	int smooth;
	unsigned angleMask;
};

// A smooth kind's states as they came: the owner's clock (ms) and the floats.
const int kSmoothMax = 8, kHistory = 6;
// A boat's state from its rider (saveRide): from them, its turn, its scale.
const int kRideState = 12 + 8 + 12;
struct Sample {
	u32 at;
	f32 v[kSmoothMax];
};

struct Mirrored {
	TLiveActor* actor; // null for the waves
	int manager, index;
	const Kind* kind;
	int usedAt;  // this game's player used it (tick)
	int heardAt; // a state came from another game (tick)
	int heardSlot;
	u8 state[48];
	int stateSize, stateAt;
	u8 sent[48];
	int sentSize, sentAt;
	int scratch; // the kind's own (a follower: the host's step last taken)
	int claimedAt; // an NPC this game's player talked to (tick; 0: none): run here since
	Sample history[kHistory]; // a smooth kind's states, oldest first
	int historyCount;
	double arrivedAt; // a smooth kind: when its newest state came (s_ms), and the usual time between
	f32 arrivalGap;
};

Mirrored s_mirrors[kMaxMirrors];
int s_count;

// Each other player's boat as their game last said (OWN_EACH), and a copy of
// each kind of boat here for each of them to stand on, made as the stage loads.
struct Ride {
	const Kind* kind;
	TLiveActor* here; // this game's own of the same boat
	u8 data[kRideState];
	int at; // s_clock; 0: none
};
Ride s_rides[MPG_MAX_PLAYERS];
const int kRideKinds = 2; // leaf boat, mud boat
MActor* s_rideCopies[MPG_MAX_PLAYERS][kRideKinds];
int s_clock;
TLiveActor* s_testWalked; // test aid claim@: an NPC walked off here
int s_testWalkFrames;
TLiveActor* s_testBoat; // test aid rideboat@: the boat this game's Mario rides, driven on
int s_testBoatFrames;

// ------------------------------------------------------------------ bytes

struct Put {
	u8* p;
	int n;
	explicit Put(u8* out) : p(out), n(0) {}
	void raw(const void* d, int k)
	{
		if (n + k <= 48) {
			memcpy(p + n, d, k);
			n += k;
		}
	}
	void f(f32 v) { raw(&v, 4); }
	void i(s32 v) { raw(&v, 4); }
	void h(s16 v) { raw(&v, 2); }
	void b(u8 v) { raw(&v, 1); }
	void v3(const JGeometry::TVec3<f32>& v)
	{
		f(v.x);
		f(v.y);
		f(v.z);
	}
};

struct Get {
	const u8* p;
	int n, at;
	bool ok;
	Get(const u8* in, int size) : p(in), n(size), at(0), ok(true) {}
	void raw(void* d, int k)
	{
		if (at + k > n) {
			ok = false;
			memset(d, 0, k);
			return;
		}
		memcpy(d, p + at, k);
		at += k;
	}
	f32 f()
	{
		f32 v;
		raw(&v, 4);
		return v;
	}
	s32 i()
	{
		s32 v;
		raw(&v, 4);
		return v;
	}
	s16 h()
	{
		s16 v;
		raw(&v, 2);
		return v;
	}
	u8 b()
	{
		u8 v;
		raw(&v, 1);
		return v;
	}
	JGeometry::TVec3<f32> v3()
	{
		JGeometry::TVec3<f32> v;
		v.x = f();
		v.y = f();
		v.z = f();
		return v;
	}
};

f32 frameOf(MActor* a)
{
	J3DFrameCtrl* c = a ? a->getFrameCtrl(ANM_TYPE_BCK) : nullptr;
	return c ? c->getFrame() : 0.0f;
}

void setFrameOf(MActor* a, f32 frame)
{
	if (J3DFrameCtrl* c = a ? a->getFrameCtrl(ANM_TYPE_BCK) : nullptr)
		c->setFrame(frame);
}

// Eases `have` toward `want`: at once when far, a little each tick when near.
void easeTo(JGeometry::TVec3<f32>& have, const JGeometry::TVec3<f32>& want, f32 snap, f32 k)
{
	const f32 dx = want.x - have.x, dy = want.y - have.y, dz = want.z - have.z;
	if (dx * dx + dy * dy + dz * dz > snap * snap) {
		have = want;
		return;
	}
	have.x += dx * k;
	have.y += dy * k;
	have.z += dz * k;
}

// Where a sent state's object has got to, `age` ticks on at its speed: a
// third of a second on at most (a state unchanged is sent once a second, and
// its speed then is no guide to where it is a second later).
JGeometry::TVec3<f32> ahead(const JGeometry::TVec3<f32>& pos, const JGeometry::TVec3<f32>& vel, int age)
{
	const f32 t = (f32)(age < 10 ? age : 10);
	return JGeometry::TVec3<f32>(pos.x + vel.x * t, pos.y + vel.y * t, pos.z + vel.z * t);
}

// ------------------------------------------------------------------ kinds

// NPCs: where the host's have walked (each game walks its own; they are
// eased toward the host's, so they stand where everyone sees them). Where
// one is headed is sent too, so one walking between states walks on, and
// faces the way the host's does. The first state places it outright.
bool isNpc(TLiveActor* a) { return dynamic_cast<TBaseNPC*>(a) != nullptr; }
int saveNpc(TLiveActor* a, u8* out)
{
	Put o(out);
	o.v3(a->mPosition);
	JGeometry::TVec3<f32> vel = a->mVelocity;
	vel.add(a->mLinearVelocity);
	o.v3(vel);
	o.f(a->mRotation.y);
	return o.n;
}
void takeNpc(TLiveActor* a, const u8* in, int size, int age, int* placed)
{
	Get g(in, size);
	const JGeometry::TVec3<f32> pos = g.v3(), vel = g.v3();
	const f32 yaw                   = g.f();
	if (!g.ok || a->mHolder) // one being carried goes with its carrier
		return;
	// this game's player talking (or a talk's camera): every NPC here stands
	// still in its waiting pose (TBaseNPC::execWalk), so the host's, walking
	// on, would slide it along standing; it catches up once the talk is over
	if (*placed && gpMarDirector && gpMarDirector->isThing())
		return;
	const f32 ahead = (f32)(age < 10 ? age : 10); // ticks it has walked on since
	const JGeometry::TVec3<f32> want(pos.x + vel.x * ahead, pos.y + vel.y * ahead, pos.z + vel.z * ahead);
	if (!*placed) {
		a->mPosition   = want;
		a->mRotation.y = yaw;
		*placed        = 1;
		return;
	}
	easeTo(a->mPosition, want, 500.0f, 0.2f);
	f32 d = yaw - a->mRotation.y;
	while (d > 180.0f)
		d -= 360.0f;
	while (d < -180.0f)
		d += 360.0f;
	a->mRotation.y += d * 0.2f;
}

// Bianco's leaf boats.
bool isLeafBoat(TLiveActor* a) { return dynamic_cast<TLeafBoat*>(a) != nullptr; }
// Noki's mud boat: steered by its rider's water.
bool isMuddyBoat(TLiveActor* a) { return dynamic_cast<TMuddyBoat*>(a) != nullptr; }
// A boat every player has their own of (one boat for all left the others
// stranded once its rider took it away): its rider's game sends where it is
// from them and how it is turned and scaled, and the others draw a copy under
// that rider's puppet (MpMirrorRide), so it moves as smoothly as they do.
int saveRide(TLiveActor* a, u8* out)
{
	TMario* me = gpMarioOriginal;
	J3DModel* model = a->getModel();
	if (!me || !model || !me->getM3UModel())
		return 0;
	MtxPtr m    = model->getAnmMtx(0);
	MtxPtr root = me->getM3UModel()->getModel()->getBaseTRMtx(); // as the pose has it
	f32 scale[3], r[3][3];
	for (int c = 0; c < 3; c++) {
		scale[c] = sqrtf(m[0][c] * m[0][c] + m[1][c] * m[1][c] + m[2][c] * m[2][c]);
		for (int row = 0; row < 3; row++)
			r[row][c] = scale[c] > 0.0001f ? m[row][c] / scale[c] : 0.0f;
	}
	// its turn as a quaternion (x, y, z, w)
	f32 q[4];
	const f32 tr = r[0][0] + r[1][1] + r[2][2];
	if (tr > 0.0f) {
		const f32 s = sqrtf(tr + 1.0f) * 2.0f;
		q[3] = 0.25f * s, q[0] = (r[2][1] - r[1][2]) / s, q[1] = (r[0][2] - r[2][0]) / s, q[2] = (r[1][0] - r[0][1]) / s;
	} else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
		const f32 s = sqrtf(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2.0f;
		q[3] = (r[2][1] - r[1][2]) / s, q[0] = 0.25f * s, q[1] = (r[0][1] + r[1][0]) / s, q[2] = (r[0][2] + r[2][0]) / s;
	} else if (r[1][1] > r[2][2]) {
		const f32 s = sqrtf(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2.0f;
		q[3] = (r[0][2] - r[2][0]) / s, q[0] = (r[0][1] + r[1][0]) / s, q[1] = 0.25f * s, q[2] = (r[1][2] + r[2][1]) / s;
	} else {
		const f32 s = sqrtf(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2.0f;
		q[3] = (r[1][0] - r[0][1]) / s, q[0] = (r[0][2] + r[2][0]) / s, q[1] = (r[1][2] + r[2][1]) / s, q[2] = 0.25f * s;
	}
	Put o(out);
	for (int i = 0; i < 3; i++)
		o.f(m[i][3] - root[i][3]);
	for (int i = 0; i < 4; i++)
		o.h((s16)(q[i] * 32767.0f));
	for (int i = 0; i < 3; i++)
		o.f(scale[i]);
	return o.n;
}

// The Turbo nozzle's doors (the plaza's, the airport's): broken down by a
// player dashing into them, in their game only (TTurboNozzleDoor::touchPlayer).
// Broken down in one game, in all: the others' break as theirs would. Open is
// gone at the airport (both halves) and, in the plaza, its opening animation
// on; one open as the stage loaded (`scratch` 1: not there in this episode)
// is no news.
bool isTurboDoor(TLiveActor* a) { return dynamic_cast<TTurboNozzleDoor*>(a) != nullptr; }
bool doorOpen(TLiveActor* a)
{
	if (a->checkLiveFlag(LIVE_FLAG_DEAD))
		return true;
	// the plaza's stands at its opening's first frame: open once it has moved on
	MActor* m = a->getMActor();
	if (!m || !m->checkCurAnm("nozzledoor", ANM_TYPE_BCK))
		return false;
	J3DFrameCtrl* fc = m->getFrameCtrl(ANM_TYPE_BCK);
	return fc && fc->getFrame() > 0.5f;
}
int saveTurboDoor(TLiveActor*, u8* out)
{
	out[0] = 1; // sent only once open (MpMirrorTick)
	return 1;
}
void takeTurboDoor(TLiveActor* a, const u8* in, int size, int, int* scratch)
{
	TTurboNozzleDoor* d = static_cast<TTurboNozzleDoor*>(a);
	(void)scratch;
	if (size < 1 || !in[0] || doorOpen(a))
		return;
	if (gpMarDirector && gpMarDirector->getCurrentMap() == 1) {
		d->startBck("nozzledoor");
		if (J3DFrameCtrl* fc = d->getMActor() ? d->getMActor()->getFrameCtrl(ANM_TYPE_BCK) : nullptr)
			fc->setFrame(1.0f); // on its way (as the next frame's step would have it)
	} else {
		d->makeObjDead();
		if (d->unk144)
			d->unk144->makeObjDead();
	}
	if (getenv("SMS_MULTIPLAYER_DEBUG"))
		fprintf(stderr, "[multiplayer] the Turbo nozzle door at %.0f %.0f %.0f was broken down in another game: here too\n",
		        a->mPosition.x, a->mPosition.y, a->mPosition.z);
}

// The plaza's manhole lids (to the tunnels under it): a player who ground
// pounds into one (or jumps up into it from below) flips it open, in their
// game only (TManhole::touchPlayer). Each game counts the flips it knows of
// (`scratch`: the count, and whether the lid moved last tick and whether a
// flip from another game moved it); one its own player starts counts one more,
// and is sent. A count ahead of this game's flips the lid here as a ground
// pound would, its sound included; a player's first count from the host is
// taken as it is (flips from before they came are no news).
const int kLidCount = 0xFF, kLidMoving = 0x100, kLidTaken = 0x200, kLidKnown = 0x400;
bool isManhole(TLiveActor* a) { return dynamic_cast<TManhole*>(a) != nullptr; }
J3DFrameCtrl* lidAnim(TLiveActor* a)
{
	MActor* m = a->getMActor();
	return m ? m->getFrameCtrl(ANM_TYPE_BCK) : nullptr;
}
// Once a tick: a flip begun here by this game's player counts one more.
// True when it did.
bool lidTick(TLiveActor* a, int* scratch)
{
	const J3DFrameCtrl* fc = lidAnim(a);
	const bool moving = fc && fc->getRate() != 0.0f;
	bool flipped = false;
	if (moving && !(*scratch & (kLidMoving | kLidTaken))) {
		*scratch = (*scratch & ~kLidCount) | ((*scratch + 1) & kLidCount);
		flipped  = true;
	}
	*scratch = moving ? *scratch | kLidMoving : *scratch & ~(kLidMoving | kLidTaken);
	return flipped;
}
int saveManhole(TLiveActor*, u8* out)
{
	out[0] = 0; // (MpMirrorTick fills in the count, which `scratch` holds)
	return 1;
}
void takeManhole(TLiveActor* a, const u8* in, int size, int, int* scratch)
{
	if (size < 1)
		return;
	const int count = in[0];
	// (the host's count is everyone's from the start)
	const bool known = (*scratch & kLidKnown) != 0 || mpg_role() == MPG_ROLE_HOST;
	const int ahead  = (int)(s8)(u8)(count - (*scratch & kLidCount));
	*scratch         = (*scratch & ~kLidCount) | count | kLidKnown;
	if (!known || ahead <= 0)
		return;
	TManhole* h      = static_cast<TManhole*>(a);
	J3DFrameCtrl* fc = lidAnim(a);
	if (!fc || fc->getRate() != 0.0f)
		return; // flipping already (this game's player is at it too)
	// as TManhole::touchPlayer starts it for a ground pound
	fc->setRate(SMSGetAnmFrameRate());
	fc->setFrame(fc->getFrame() + SMSGetAnmFrameRate());
	h->offMapObjFlag(TMapObjBase::MAP_OBJ_FLAG_UNK100);
	SMSGetMSound()->startSoundActor(MSD_SE_OBJ_MANHOLE_OPEN, &h->mPosition);
	*scratch |= kLidTaken | kLidMoving;
	if (getenv("SMS_MULTIPLAYER_DEBUG"))
		fprintf(stderr, "[multiplayer] the manhole lid at %.0f %.0f %.0f flipped open in another game: here too\n",
		        a->mPosition.x, a->mPosition.y, a->mPosition.z);
}

// The plaza's fruit boats: rowed along their routes, stopped and set off by riders.
bool isFruitsBoat(TLiveActor* a) { return dynamic_cast<TFruitsBoat*>(a) != nullptr; }
int saveFruitsBoat(TLiveActor* a, u8* out)
{
	TFruitsBoat* b = static_cast<TFruitsBoat*>(a);
	Put o(out);
	o.v3(b->mPosition);
	o.f(b->mRotation.y);
	o.v3(b->mRollAxis);
	o.f(b->mRollAngle);
	o.f(b->mRollSpeed);
	o.h(b->mReversed);
	o.i((s32)(b->mLiveFlag & TFruitsBoat::LIVE_FLAG_UNK10000));
	return o.n;
}

// A boat's roll is about mRollAxis (TFruitsBoat::calcRootMatrix), which its
// rider steers by easing it, not normalising it, from where they stand: one
// that eases through nothing (a rider boarding with the boat's "aboard"
// step skipped) makes the boat's matrix nothing, and it vanishes.
void keepRollAxis(TFruitsBoat* b)
{
	JGeometry::TVec3<f32>& a = b->mRollAxis;
	const f32 len            = sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
	if (len != len || len < 0.2f)
		a.set(1.0f, 0.0f, 0.0f);
	else if (len < 0.9f)
		a.scale(1.0f / len);
}
void takeFruitsBoat(TLiveActor* a, const u8* in, int size, int, int*)
{
	TFruitsBoat* b = static_cast<TFruitsBoat*>(a);
	Get g(in, size);
	const JGeometry::TVec3<f32> pos = g.v3();
	const f32 yaw                   = g.f();
	const JGeometry::TVec3<f32> axis = g.v3();
	const f32 roll = g.f(), rollSpeed = g.f();
	const s16 reversed = g.h();
	const s32 flags    = g.i();
	if (!g.ok)
		return;
	b->mPosition   = pos;
	b->mRotation.y = yaw;
	b->mRollAxis   = axis;
	b->mRollAngle  = roll;
	b->mRollSpeed  = rollSpeed;
	b->mReversed   = reversed;
	// whether it may row, not whether the owner's player is aboard (this
	// game's is not: when they board, the boat's own "came aboard" step runs)
	b->mLiveFlag = (b->mLiveFlag & ~(u32)TFruitsBoat::LIVE_FLAG_UNK10000) | ((u32)flags & TFruitsBoat::LIVE_FLAG_UNK10000);
	keepRollAxis(b);
}

// Pinna Park's Ferris wheel: its turn, speed and calming down.
bool isFerrisWheel(TLiveActor* a) { return dynamic_cast<TFerrisWheel*>(a) != nullptr; }
int saveFerrisWheel(TLiveActor* a, u8* out)
{
	TFerrisWheel* w = static_cast<TFerrisWheel*>(a);
	Put o(out);
	o.f(frameOf(w->getMActor()));
	o.f(w->mAnmRate);
	o.h((s16)w->mState);
	o.i(w->mStateTimer);
	return o.n;
}
void takeFerrisWheel(TLiveActor* a, const u8* in, int size, int age, int*)
{
	TFerrisWheel* w = static_cast<TFerrisWheel*>(a);
	Get g(in, size);
	const f32 frame = g.f(), rate = g.f();
	const s16 state = g.h();
	const s32 timer = g.i();
	if (!g.ok)
		return;
	// on at its rate since (a third of a second at most), round its loop
	f32 at = frame + rate * (f32)(age < 10 ? age : 10);
	if (J3DFrameCtrl* c = w->getMActor() ? w->getMActor()->getFrameCtrl(ANM_TYPE_BCK) : nullptr)
		if (c->getEnd() > 0 && at >= c->getEnd())
			at = fmodf(at, (f32)c->getEnd());
	setFrameOf(w->getMActor(), at);
	w->mAnmRate    = rate;
	w->mState      = (u16)state;
	w->mStateTimer = timer;
}

// Pinna Park's merry-go-round: how far round it is.
bool isMerrygoround(TLiveActor* a) { return dynamic_cast<TMerrygoround*>(a) != nullptr; }
int saveTurner(TLiveActor* a, u8* out)
{
	Put o(out);
	o.f(a->mRotation.y);
	o.f(frameOf(a->getMActor()));
	return o.n;
}
void takeTurner(TLiveActor* a, const u8* in, int size, int, int*)
{
	Get g(in, size);
	const f32 yaw = g.f(), frame = g.f();
	if (!g.ok)
		return;
	a->mRotation.y = yaw;
	setFrameOf(a->getMActor(), frame);
}

// Pinna Park's coaster car going round its rail (scenery: riding one is
// each player's own).
bool isPinnaCoaster(TLiveActor* a) { return dynamic_cast<TPinnaCoaster*>(a) != nullptr; }
int savePinnaCoaster(TLiveActor* a, u8* out)
{
	TPinnaCoaster* c = static_cast<TPinnaCoaster*>(a);
	Put o(out);
	o.f(frameOf(c->mRail));
	o.f(frameOf(c->getMActor()));
	return o.n;
}
void takePinnaCoaster(TLiveActor* a, const u8* in, int size, int, int*)
{
	TPinnaCoaster* c = static_cast<TPinnaCoaster*>(a);
	Get g(in, size);
	const f32 rail = g.f(), car = g.f();
	if (!g.ok)
		return;
	setFrameOf(c->mRail, rail);
	setFrameOf(c->getMActor(), car);
}

// The casino's roulette (King Boo spins it): its turn and speed. Its
// sinking switch is left to each game (it holds the player still).
bool isRoulette(TLiveActor* a) { return dynamic_cast<TRoulette*>(a) != nullptr; }
int saveRoulette(TLiveActor* a, u8* out)
{
	TRoulette* r = static_cast<TRoulette*>(a);
	Put o(out);
	o.f(r->mRotation.y);
	o.f(r->unk13C);
	return o.n;
}
void takeRoulette(TLiveActor* a, const u8* in, int size, int, int*)
{
	TRoulette* r = static_cast<TRoulette*>(a);
	Get g(in, size);
	const f32 yaw = g.f(), speed = g.f();
	if (!g.ok)
		return;
	r->mRotation.y = yaw;
	r->unk13C      = speed;
}

// Slot machines: the reels as the player spraying them sees them. The big
// one's lights and win too; the small ones' draws (and the coins and Boos
// they pay out) happen in that player's game only, so the copies' reels
// are only turned, never left running.
int reelCount(TSirenaRollMapObj* s) { return s->unk164 > 0 && s->unk164 <= 3 ? s->unk164 : 3; }
bool isSlotDrum(TLiveActor* a)
{
	return dynamic_cast<TSlotDrum*>(a) != nullptr && dynamic_cast<TItemSlotDrum*>(a) == nullptr;
}
bool isItemSlotDrum(TLiveActor* a) { return dynamic_cast<TItemSlotDrum*>(a) != nullptr; }
int saveSlot(TLiveActor* a, u8* out)
{
	TSlotDrum* s = static_cast<TSlotDrum*>(a);
	Put o(out);
	const int n = reelCount(s);
	o.b((u8)n);
	for (int i = 0; i < n; i++)
		o.f(s->unk13C ? s->unk13C[i] : 0.0f);
	for (int i = 0; i < n; i++)
		o.f(s->unk138 ? s->unk138[i] : 0.0f);
	for (int i = 0; i < n; i++)
		o.raw(&s->unk170[i], sizeof s->unk170[i]);
	o.b(s->unk194 ? 1 : 0);
	return o.n;
}
bool reelsTurning(TSlotDrum* s)
{
	for (int i = 0; s->unk138 && i < reelCount(s); i++)
		if (s->unk138[i] != 0.0f)
			return true;
	return false;
}

void takeSlotWith(TLiveActor* a, const u8* in, int size, bool turnOnly)
{
	TSlotDrum* s = static_cast<TSlotDrum*>(a);
	Get g(in, size);
	const int n = g.b();
	if (!g.ok || n != reelCount(s) || !s->unk13C || !s->unk138)
		return;
	f32 angles[3], speeds[3];
	GXColorS10 lights[3];
	for (int i = 0; i < n; i++)
		angles[i] = g.f();
	for (int i = 0; i < n; i++)
		speeds[i] = g.f();
	for (int i = 0; i < n; i++)
		g.raw(&lights[i], sizeof lights[i]);
	const bool won = g.b() != 0;
	if (!g.ok)
		return;
	for (int i = 0; i < n; i++) {
		s->unk13C[i] = angles[i];
		s->unk138[i] = turnOnly ? 0.0f : speeds[i];
		s->unk170[i] = lights[i];
	}
	s->unk194 = won;
}
void takeSlotDrum(TLiveActor* a, const u8* in, int size, int, int*) { takeSlotWith(a, in, size, false); }
void takeItemSlotDrum(TLiveActor* a, const u8* in, int size, int, int*) { takeSlotWith(a, in, size, true); }

// Yoshi's egg: the fruit it wants (the host's draw, or the last wish of the
// game whose egg hatched). Every game has its own Yoshi, so an egg hatched by
// one player stays in the others' games, for each of them to hatch their own
// (the others see a player's Yoshi under them).
bool isEgg(TLiveActor* a) { return dynamic_cast<TEggYoshi*>(a) != nullptr; }
const u16 kEggHatching = 0xB, kEggHatched = 0xC;
// Hatching or hatched here (seen in this game's own run, not an egg still
// waiting to appear at a stage's start).
bool eggHatching(TEggYoshi* e) { return e->mState == kEggHatching || e->mState == kEggHatched; }
bool s_eggHatchedHere;
int saveEgg(TLiveActor* a, u8* out)
{
	TEggYoshi* e = static_cast<TEggYoshi*>(a);
	Put o(out);
	o.i((s32)e->unk14C);
	o.b(s_eggHatchedHere ? 1 : 0);
	return o.n;
}
void takeEgg(TLiveActor* a, const u8* in, int size, int, int*)
{
	TEggYoshi* e = static_cast<TEggYoshi*>(a);
	Get g(in, size);
	const u32 wants = (u32)g.i();
	g.b(); // hatched in the sender's game (theirs; this one stays)
	if (!g.ok || s_eggHatchedHere || eggHatching(e) || e->checkLiveFlag(LIVE_FLAG_DEAD))
		return;
	if (wants && wants != e->unk14C && e->mState == 1) {
		e->unk14C = wants;
		e->startBalloonAnim();
	}
}

// Gelato Beach's big watermelon: rolled by whoever bumped it last.
bool isWatermelon(TLiveActor* a) { return dynamic_cast<TBigWatermelon*>(a) != nullptr; }
int saveWatermelon(TLiveActor* a, u8* out)
{
	TBigWatermelon* w = static_cast<TBigWatermelon*>(a);
	Put o(out);
	o.v3(w->mPosition);
	o.v3(w->mVelocity);
	o.h((s16)w->mState);
	o.i(w->mStateTimer);
	o.f(w->mScaling.x);
	o.i(w->unk19C);
	return o.n;
}
void takeWatermelon(TLiveActor* a, const u8* in, int size, int age, int*)
{
	TBigWatermelon* w = static_cast<TBigWatermelon*>(a);
	Get g(in, size);
	const JGeometry::TVec3<f32> pos = g.v3(), vel = g.v3();
	const s16 state = g.h();
	const s32 timer = g.i();
	const f32 scale = g.f();
	const s32 coins = g.i();
	if (!g.ok || w->mHolder)
		return;
	easeTo(w->mPosition, ahead(pos, vel, age), 400.0f, 0.5f);
	w->mVelocity   = vel;
	w->mState      = (u16)state;
	w->mStateTimer = timer;
	w->mScaling.set(scale, scale, scale);
	w->unk19C = coins;
}


// Bosses that follow the host's steps (as Shadow Mario does, mp_boss.cpp):
// each game's copy runs, animates and is hit as ever; it takes each new step
// of the host's (its nerve, by its place in the boss's own table), its hit
// points, and is eased toward where the host's is. Hits a player lands on a
// copy reach the host's as hits on shared objects (mp_game.cpp).
typedef const TNerveBase<TLiveActor>* Nerve;
typedef Nerve (*Steps)(int);

int stepOf(Steps steps, Nerve n)
{
	for (int i = 0; i < 64; i++) {
		Nerve t = steps(i);
		if (!t)
			break;
		if (t == n)
			return i;
	}
	return 0xFF;
}

int saveFollower(TLiveActor* a, u8* out, Steps steps)
{
	TSpineEnemy* e = static_cast<TSpineEnemy*>(a);
	Put o(out);
	o.b((u8)stepOf(steps, e->getLatestNerve()));
	o.v3(e->mPosition);
	o.f(e->mRotation.y);
	o.b((u8)e->getHitPoints());
	return o.n;
}

void takeFollower(TLiveActor* a, const u8* in, int size, int* last, Steps steps)
{
	TSpineEnemy* e = static_cast<TSpineEnemy*>(a);
	Get g(in, size);
	const int step                  = g.b();
	const JGeometry::TVec3<f32> pos = g.v3();
	const f32 yaw                   = g.f();
	const int hp                    = g.b();
	if (!g.ok || e->checkLiveFlag(LIVE_FLAG_DEAD))
		return;
	// *last: the host's step last taken, plus one (low byte); ticks this copy
	// has been on another step since (above it)
	if (step != 0xFF && steps(step)) {
		const bool newStep = step + 1 != (*last & 0xFF);
		int apart          = *last >> 8;
		apart              = stepOf(steps, e->getLatestNerve()) != step ? apart + 1 : 0;
		// a new step of the host's: taken (only then, so animations are not
		// restarted); a copy gone its own way for a second is put back on it
		if (newStep || apart > fps()) {
			if (stepOf(steps, e->getLatestNerve()) != step) {
				e->mSpine->reset();
				e->mSpine->setNext(steps(step));
			}
			apart = 0;
		}
		*last = (step + 1) | apart << 8;
	}
	easeTo(e->mPosition, pos, 700.0f, 0.2f);
	f32 d = yaw - e->mRotation.y;
	while (d > 180.0f)
		d -= 360.0f;
	while (d < -180.0f)
		d += 360.0f;
	e->mRotation.y += d * 0.2f;
	e->mHitPoints = (u8)hp;
}

Nerve chompStep(int i)
{
	switch (i) {
	case 0: return &TNerveBWGraphWander::theNerve();
	case 1: return &TNerveBWRoll::theNerve();
	case 2: return &TNerveBWBark::theNerve();
	case 3: return &TNerveBWJump::theNerve();
	case 4: return &TNerveBWStun::theNerve();
	case 5: return &TNerveBWWakeup::theNerve();
	case 6: return &TNerveBWJumpToBath::theNerve();
	case 7: return &TNerveBWDie::theNerve();
	case 8: return &TNerveBWJumpAway::theNerve();
	case 9: return &TNerveBWShake::theNerve();
	case 10: return &TNerveBWFall::theNerve();
	default: return nullptr;
	}
}
Nerve bowserStep(int i)
{
	switch (i) {
	case 0: return &TNerveKoopaWait::theNerve();
	case 1: return &TNerveKoopaTurnL::theNerve();
	case 2: return &TNerveKoopaTurnR::theNerve();
	case 3: return &TNerveKoopaTumble::theNerve();
	case 4: return &TNerveKoopaProvoke::theNerve();
	case 5: return &TNerveKoopaFlame::theNerve();
	case 6: return &TNerveKoopaGetDown::theNerve();
	case 7: return &TNerveKoopaStagger::theNerve();
	case 8: return &TNerveKoopaFall::theNerve();
	case 9: return &TNerveKoopaGetShowered::theNerve();
	default: return nullptr;
	}
}
Nerve juniorStep(int i)
{
	switch (i) {
	case 0: return &TNerveKoopaJrWait::theNerve();
	case 1: return &TNerveKoopaJrDamage::theNerve();
	case 2: return &TNerveKoopaJrDemo::theNerve();
	case 3: return &TNerveKoopaJrLaunch::theNerve();
	case 4: return &TNerveKoopaJrYahoo::theNerve();
	case 5: return &TNerveKoopaJrSubmarineWait::theNerve();
	case 6: return &TNerveKoopaJrSubmarineCannonOpenClose::theNerve();
	case 7: return &TNerveKoopaJrSubmarineLaunchKiller::theNerve();
	case 8: return &TNerveLimitKoopaJrWait::theNerve();
	case 9: return &TNerveLimitKoopaJrRun::theNerve();
	case 10: return &TNerveLimitKoopaJrLaunch::theNerve();
	case 11: return &TNerveLimitKoopaJrYahoo::theNerve();
	default: return nullptr;
	}
}
Nerve sleepyWigglerStep(int i)
{
	switch (i) {
	case 0: return &TNerveSBH_SleepContinue::theNerve();
	case 1: return &TNerveSBH_Fall::theNerve();
	default: return nullptr;
	}
}

// Chain Chomp's bath (Pianta Village): run by the player dragging him by his picket, else the host.
bool isChomp(TLiveActor* a) { return dynamic_cast<TBossWanwan*>(a) != nullptr; }
int saveChomp(TLiveActor* a, u8* out)
{
	TBossWanwan* c = static_cast<TBossWanwan*>(a);
	const int n    = saveFollower(a, out, chompStep);
	Put o(out + n);
	o.v3(c->mPullVelocity);
	o.f(c->mRollAngle);
	o.b((u8)c->mIsRolling);
	o.b((u8)c->mIsInBath);
	o.b((u8)c->mShineAppeared);
	return n + o.n;
}
void takeChomp(TLiveActor* a, const u8* in, int size, int, int* last)
{
	TBossWanwan* c = static_cast<TBossWanwan*>(a);
	const int n    = 1 + 12 + 4 + 1;
	takeFollower(a, in, size, last, chompStep);
	Get g(in + n, size - n);
	const JGeometry::TVec3<f32> pull = g.v3();
	const f32 roll                   = g.f();
	const u8 rolling = g.b(), inBath = g.b(), shine = g.b();
	if (!g.ok)
		return;
	c->mPullVelocity = pull;
	c->mRollAngle    = roll;
	c->mIsRolling    = rolling;
	c->mIsInBath     = (s8)inBath;
	if (shine)
		c->mShineAppeared = 1;
}
// Bowser in the final bath, Bowser Jr. (in it, and his sub and chases), Gelato's sleeping Wiggler: the host's.
bool isBowser(TLiveActor* a) { return dynamic_cast<TKoopa*>(a) != nullptr; }
int saveBowser(TLiveActor* a, u8* out)
{
	static int said = -1;
	const int step  = stepOf(bowserStep, static_cast<TSpineEnemy*>(a)->getLatestNerve());
	if (step != said && getenv("SMS_MULTIPLAYER_DEBUG")) {
		said = step;
		fprintf(stderr, "[multiplayer] Bowser: step %d%s\n", step, step == 7 ? " (staggering)" : "");
	}
	return saveFollower(a, out, bowserStep);
}
void takeBowser(TLiveActor* a, const u8* in, int size, int, int* last) { takeFollower(a, in, size, last, bowserStep); }
bool isJunior(TLiveActor* a)
{
	return dynamic_cast<TKoopaJr*>(a) || dynamic_cast<TKoopaJrSubmarine*>(a) || dynamic_cast<TLimitKoopaJr*>(a);
}
int saveJunior(TLiveActor* a, u8* out) { return saveFollower(a, out, juniorStep); }
void takeJunior(TLiveActor* a, const u8* in, int size, int, int* last) { takeFollower(a, in, size, last, juniorStep); }
bool isSleepyWiggler(TLiveActor* a) { return dynamic_cast<TSleepBossHanachan*>(a) != nullptr; }
int saveSleepyWiggler(TLiveActor* a, u8* out) { return saveFollower(a, out, sleepyWigglerStep); }
void takeSleepyWiggler(TLiveActor* a, const u8* in, int size, int, int* last)
{
	takeFollower(a, in, size, last, sleepyWigglerStep);
}

// Bowser's hot tub: how it is tipped, the host's, which tips under every
// player standing on it (each game tips its own under its own player only).
bool isTub(TLiveActor* a) { return dynamic_cast<TBathtub*>(a) != nullptr; }
int saveTub(TLiveActor* a, u8* out)
{
	TBathtub* t = static_cast<TBathtub*>(a);
	Put o(out);
	o.f(t->mQuat.x);
	o.f(t->mQuat.y);
	o.f(t->mQuat.z);
	o.f(t->mQuat.w);
	o.v3(t->mAngleVel);
	return o.n;
}
void takeTub(TLiveActor* a, const u8* in, int size, int, int*)
{
	TBathtub* t = static_cast<TBathtub*>(a);
	Get g(in, size);
	f32 q[4];
	for (int i = 0; i < 4; i++)
		q[i] = g.f();
	const JGeometry::TVec3<f32> spin = g.v3();
	if (!g.ok || t->unk29A) // overturned: the ending, played in each game
		return;
	// eased toward the host's (the shorter way round), then on as it spins
	f32 dot = q[0] * t->mQuat.x + q[1] * t->mQuat.y + q[2] * t->mQuat.z + q[3] * t->mQuat.w;
	// a little at a time: at the rim (some 4500 from the middle) a degree
	// moves the floor some 80 units, past what a player standing there
	// keeps their feet on (the game finds a floor up to 78 above them)
	const f32 apartRad = 2.0f * acosf(fminf(1.0f, fabsf(dot)));
	const f32 maxStep  = 0.25f * 0.0174533f;
	const f32 sign = dot < 0.0f ? -1.0f : 1.0f, k = apartRad * 0.3f > maxStep ? maxStep / apartRad : 0.3f;
	t->mQuat.x += (sign * q[0] - t->mQuat.x) * k;
	t->mQuat.y += (sign * q[1] - t->mQuat.y) * k;
	t->mQuat.z += (sign * q[2] - t->mQuat.z) * k;
	t->mQuat.w += (sign * q[3] - t->mQuat.w) * k;
	t->mQuat.normalize();
	t->mAngleVel = spin;
	static int taken;
	if (getenv("SMS_MULTIPLAYER_DEBUG") && taken++ % 150 == 0) {
		// tilt: the angle between the tub's up and the world's (a turn's w is cos(half the angle))
		const f32 hostTilt = 2.0f * acosf(fminf(1.0f, sqrtf(q[3] * q[3] + q[1] * q[1]))) * 57.29578f;
		const f32 hereTilt = 2.0f * acosf(fminf(1.0f, sqrtf(t->mQuat.w * t->mQuat.w + t->mQuat.y * t->mQuat.y))) * 57.29578f;
		const f32 apart = 2.0f * acosf(fminf(1.0f, fabsf(dot))) * 57.29578f;
		fprintf(stderr, "[multiplayer] tub: the host's tipped %.2f degrees, this one %.2f, %.2f apart\n", hostTilt, hereTilt,
		        apart);
	}
}

// The host: the others standing on the tub weigh on it too, as this game's
// Mario does (TBathtub::control's liftMario).
void weighTub(TBathtub* t)
{
	if (t->unk29A || t->mHeldObject)
		return;
	for (int slot = 0; slot < MPG_MAX_PLAYERS; slot++) {
		MpgPose pose;
		float age;
		if (slot == mpg_slot() || !mpg_player_present(slot) || !mpg_pose(slot, &pose, &age) || age > 0.5f
		    || !(pose.more & MPG_POSE2_ON_TUB))
			continue;
		const JGeometry::TVec3<f32> at(pose.mtx[0][3], pose.mtx[1][3], pose.mtx[2][3]);
		t->liftMario(at);
		static int weighed;
		if (getenv("SMS_MULTIPLAYER_DEBUG") && weighed++ % 300 == 0)
			fprintf(stderr, "[multiplayer] tub: %s weighs on it (%.0f %.0f %.0f)\n", mpg_character_name(slot), at.x, at.y, at.z);
	}
}

// Critters and things that wander on their own (seagulls, pigeons and what
// they carry, butterflies, Ricco's crane hooks, Pianta Village's wire traps): where the host's are, how fast they go and
// which way they face. Each game's copy runs and animates; it is eased toward
// the host's, carried on at its speed between states. A hook's speed is the
// host's too, so a player hanging from it is carried along smoothly.
// A pigeon's steps (on the ground, taking off, flying round, landing):
// the host's, or a copy stays on its perch while the host's flies.
Nerve birdStep(int i)
{
	switch (i) {
	case 0: return &TNerveAnimalBirdWaitOnGround::theNerve();
	case 1: return &TNerveAnimalBirdActionOnGround::theNerve();
	case 2: return &TNerveAnimalBirdWalkOnGround::theNerve();
	case 3: return &TNerveAnimalBirdTakeoff::theNerve();
	case 4: return &TNerveAnimalBirdGraphWander::theNerve();
	case 5: return &TNerveAnimalBirdChangeToCoin::theNerve();
	case 6: return &TNerveAnimalBirdComeback::theNerve();
	case 7: return &TNerveAnimalBirdPreLanding::theNerve();
	case 8: return &TNerveAnimalBirdLanding::theNerve();
	default: return nullptr;
	}
}

bool isCritter(TLiveActor* a)
{
	return dynamic_cast<TAnimalBase*>(a) || dynamic_cast<TAnimalBird*>(a) || dynamic_cast<TRiccoHook*>(a)
	       || dynamic_cast<TWireTrap*>(a);
}
int saveCritter(TLiveActor* a, u8* out)
{
	Put o(out);
	o.v3(a->mPosition);
	JGeometry::TVec3<f32> vel = a->mVelocity;
	vel.add(a->mLinearVelocity);
	o.v3(vel);
	o.f(a->mRotation.y);
	TAnimalBird* bird = dynamic_cast<TAnimalBird*>(a);
	o.b(bird ? (u8)stepOf(birdStep, bird->getLatestNerve()) : 0xFF);
	return o.n;
}
void takeCritter(TLiveActor* a, const u8* in, int size, int age, int* placed)
{
	Get g(in, size);
	const JGeometry::TVec3<f32> pos = g.v3(), vel = g.v3();
	const f32 yaw                   = g.f();
	const int step                  = g.b(); // a pigeon's (else 0xFF)
	if (!g.ok || a->mHolder)
		return;
	// a new step of the host's pigeon: taken (not its turning to a coin, which
	// a spray brings about here too); scratch: bit 0 placed, bits 8-15 the step
	if (TAnimalBird* bird = dynamic_cast<TAnimalBird*>(a))
		if (step != 0xFF && step != 5 && birdStep(step) && step + 1 != ((*placed >> 8) & 0xFF)) {
			*placed = (*placed & 0xFF) | ((step + 1) << 8);
			if (stepOf(birdStep, bird->getLatestNerve()) != step) {
				bird->getSpine()->reset();
				bird->getSpine()->setNext(birdStep(step));
			}
		}
	const JGeometry::TVec3<f32> want = ahead(pos, vel, age);
	if (!(*placed & 1)) {
		a->mPosition   = want;
		a->mRotation.y = yaw;
		*placed |= 1;
		return;
	}
	easeTo(a->mPosition, want, 600.0f, 0.3f);
	f32 d = yaw - a->mRotation.y;
	while (d > 180.0f)
		d -= 360.0f;
	while (d < -180.0f)
		d += 360.0f;
	a->mRotation.y += d * 0.3f;
	if (dynamic_cast<TRiccoHook*>(a))
		a->mLinearVelocity = vel; // what a hanging player is moved by
}

// Machines that run on their own clocks (cranes that tip and turn, windmills
// and watermills, cogwheels, lifts and blocks on rails, Gelato's yacht): each
// starts when its stage does, so each game's is somewhere else in its round.
// Their state, its timer, how they are turned and where they are: the
// host's, or a rider's (a lift or block some move only while ridden), drawn
// smoothly a moment behind the owner's (as other players are), not eased
// toward its last word while running on by itself (which tugged it back and
// forth, worst where it turns back).
bool isMachine(TLiveActor* a)
{
	return dynamic_cast<TCraneRotY*>(a) || dynamic_cast<TCraneUpDown*>(a) || dynamic_cast<TBigWindmill*>(a)
	       || dynamic_cast<TRiccoWatermill*>(a) || dynamic_cast<TCogwheel*>(a) || dynamic_cast<TBellWatermill*>(a)
	       || dynamic_cast<TMammaYacht*>(a) || dynamic_cast<TMapObjTurn*>(a) || dynamic_cast<TBiancoWatermill*>(a)
	       || dynamic_cast<TBiancoWatermillVertical*>(a);
}
// Lifts, blocks and clouds that go along rails (a secret course's are many):
// sent more often, they turn back on their rails where they should.
bool isRailMover(TLiveActor* a) { return dynamic_cast<TRailMapObj*>(a) != nullptr; }
// where it is and how it is turned (blended), then its state and timer
int saveMachine(TLiveActor* a, u8* out)
{
	TMapObjBase* m = static_cast<TMapObjBase*>(a);
	Put o(out);
	o.v3(m->mPosition);
	o.v3(m->mRotation);
	o.i((s32)m->mState);
	o.i(m->mStateTimer);
	return o.n;
}
void takeMachine(TLiveActor* a, const u8* in, int size, int, int*)
{
	TMapObjBase* m = static_cast<TMapObjBase*>(a);
	Get g(in, size);
	const JGeometry::TVec3<f32> pos = g.v3(), rot = g.v3();
	const s32 state = g.i(), timer = g.i();
	if (!g.ok)
		return;
	m->mPosition   = pos;
	m->mRotation   = rot;
	m->mState      = (u32)state;
	m->mStateTimer = timer;
}

// Things carried round a path by an animation (a secret course's sand bird
// and the blocks and coins on it, TJointCoin): where both its animations
// are, the host's. Each game's plays on smoothly at its own rate; it is put
// on the host's frame when far from it, and nudged toward it when near.
bool isJointMover(TLiveActor* a) { return dynamic_cast<TJointCoin*>(a) != nullptr; }
int saveJointMover(TLiveActor* a, u8* out)
{
	TJointCoin* j = static_cast<TJointCoin*>(a);
	Put o(out);
	o.f(frameOf(j->unk138));
	o.f(frameOf(j->getMActor()));
	return o.n;
}
void stepToward(MActor* a, f32 frame, int age)
{
	J3DFrameCtrl* c = a ? a->getFrameCtrl(ANM_TYPE_BCK) : nullptr;
	if (!c)
		return;
	const f32 end = c->getEnd() > 0 ? (f32)c->getEnd() : 0.0f;
	f32 want      = frame + c->getRate() * (f32)(age < 30 ? age : 30); // on at this game's rate since
	if (end > 0.0f)
		want = fmodf(want, end);
	f32 d = want - c->getFrame();
	if (end > 0.0f && d > end * 0.5f) // the short way round its loop
		d -= end;
	else if (end > 0.0f && d < -end * 0.5f)
		d += end;
	f32 at = d > 10.0f || d < -10.0f ? want : c->getFrame() + d * 0.1f;
	if (end > 0.0f && at < 0.0f)
		at += end;
	else if (end > 0.0f && at >= end)
		at -= end;
	c->setFrame(at);
}
void takeJointMover(TLiveActor* a, const u8* in, int size, int age, int*)
{
	TJointCoin* j = static_cast<TJointCoin*>(a);
	Get g(in, size);
	const f32 path = g.f(), body = g.f();
	if (!g.ok)
		return;
	static int said;
	if (getenv("SMS_MULTIPLAYER_DEBUG") && said++ % 150 == 0)
		fprintf(stderr, "[multiplayer] path animation: here at frame %.1f, the owner's %.1f (%d ticks ago)\n", frameOf(j->unk138),
		        path, age);
	stepToward(j->unk138, path, age);
	stepToward(j->getMActor(), body, age);
}

// Sand blocks that crumble under a player (secret courses): one crumbling in
// any game crumbles in all, as if each game's player had stood on it (its
// fall and the time it is gone run in each game; it comes back in each when
// nobody there stands where it was).
bool isSandBlock(TLiveActor* a) { return dynamic_cast<TSandBlock*>(a) != nullptr; }
int saveSandBlock(TLiveActor* a, u8* out)
{
	TSandBlock* b = static_cast<TSandBlock*>(a);
	Put o(out);
	o.b((u8)b->mState);
	o.i(b->mStateTimer);
	return o.n;
}
void takeSandBlock(TLiveActor* a, const u8* in, int size, int, int*)
{
	TSandBlock* b = static_cast<TSandBlock*>(a);
	Get g(in, size);
	const int state = g.b();
	const s32 timer = g.i();
	if (!g.ok)
		return;
	if ((state == TSandBlock::STATE_TOUCHED || state == TSandBlock::STATE_FALLING)
	    && (b->mState == TMapObjBase::STATE_NORMAL || b->mState == TSandBlock::STATE_RESTORING)) {
		b->startStateTimer(state == TSandBlock::STATE_TOUCHED && timer > 0 ? timer : 1);
		b->setState(TSandBlock::STATE_TOUCHED);
		if (getenv("SMS_MULTIPLAYER_DEBUG"))
			fprintf(stderr, "[multiplayer] a sand block at %.0f %.0f %.0f crumbles in another game: here too\n", b->mPosition.x,
			        b->mPosition.y, b->mPosition.z);
	}
}

// Blocks that spin round on their own (a secret course's): how far round.
bool isRollBlock(TLiveActor* a) { return dynamic_cast<TRollBlock*>(a) != nullptr; }
int saveRollBlock(TLiveActor* a, u8* out)
{
	Put o(out);
	o.f(static_cast<TRollBlock*>(a)->unk138);
	return o.n;
}
void takeRollBlock(TLiveActor* a, const u8* in, int size, int, int*)
{
	Get g(in, size);
	const f32 angle = g.f();
	if (g.ok)
		static_cast<TRollBlock*>(a)->unk138 = angle;
}

// A school of fish carrying something (a blue coin swims with Gelato's): its
// last fish, which the coin follows, the host's. The school is moved
// along as one toward it, so it keeps its shape. Schools carrying nothing
// swim their own way in each game (only scenery).
bool isCarryingSchool(TLiveActor* a)
{
	TFishoid* f = dynamic_cast<TFishoid*>(a);
	return f && f->unk15C && f->getBoidLeader() && f->getBoidNum() > 0;
}
int saveSchool(TLiveActor* a, u8* out)
{
	TFishoid* f = static_cast<TFishoid*>(a);
	TBoid* last = f->getBoid(f->getBoidNum() - 1);
	Put o(out);
	o.v3(last->mPosition);
	o.v3(last->mHeading);
	o.f(last->mSpeed);
	return o.n;
}
void takeSchool(TLiveActor* a, const u8* in, int size, int age, int*)
{
	TFishoid* f = static_cast<TFishoid*>(a);
	Get g(in, size);
	const JGeometry::TVec3<f32> pos = g.v3(), heading = g.v3();
	const f32 speed                 = g.f();
	if (!g.ok)
		return;
	const f32 t = speed * (f32)(age < 10 ? age : 10);
	const JGeometry::TVec3<f32> want(pos.x + heading.x * t, pos.y + heading.y * t, pos.z + heading.z * t);
	TBoid* last = f->getBoid(f->getBoidNum() - 1);
	JGeometry::TVec3<f32> by(want.x - last->mPosition.x, want.y - last->mPosition.y, want.z - last->mPosition.z);
	const f32 k = by.x * by.x + by.y * by.y + by.z * by.z > 1500.0f * 1500.0f ? 1.0f : 0.2f;
	by.scale(k);
	for (int i = 0; i < f->getBoidNum(); i++)
		f->getBoid(i)->mPosition.add(by);
}

// Swinging boards (Noki Bay, Pianta Village): their swing, run by whoever
// stands on one (it swings under them), else the host.
bool isSwingBoard(TLiveActor* a) { return dynamic_cast<TSwingBoard*>(a) != nullptr; }
int saveSwingBoard(TLiveActor* a, u8* out)
{
	TSwingBoard* b = static_cast<TSwingBoard*>(a);
	Put o(out);
	o.f(b->mAngle);
	o.f(b->mAngleSpeed);
	return o.n;
}
void takeSwingBoard(TLiveActor* a, const u8* in, int size, int, int*)
{
	TSwingBoard* b = static_cast<TSwingBoard*>(a);
	Get g(in, size);
	const f32 angle = g.f(), speed = g.f();
	if (!g.ok)
		return;
	b->mAngle += (angle - b->mAngle) * 0.3f;
	b->mAngleSpeed = speed;
}

// Pinna Park's pirate ship: its swing (or loop), the host's.
bool isViking(TLiveActor* a) { return dynamic_cast<THorizontalViking*>(a) != nullptr; }
int saveViking(TLiveActor* a, u8* out)
{
	THorizontalViking* v = static_cast<THorizontalViking*>(a);
	Put o(out);
	o.i(v->mState);
	o.f(v->mSwingSpeed);
	o.f(v->mSwingAngle);
	return o.n;
}
void takeViking(TLiveActor* a, const u8* in, int size, int age, int*)
{
	THorizontalViking* v = static_cast<THorizontalViking*>(a);
	Get g(in, size);
	const s32 state = g.i();
	const f32 speed = g.f(), angle = g.f();
	if (!g.ok)
		return;
	const f32 want = angle + speed * (f32)(age < 10 ? age : 10);
	f32 d          = want - v->mSwingAngle;
	while (d > 180.0f)
		d -= 360.0f;
	while (d < -180.0f)
		d += 360.0f;
	v->mState = state;
	v->mSwingSpeed = speed;
	v->mSwingAngle += fabsf(d) > 60.0f ? d : d * 0.3f;
}

const Kind kKinds[] = {
	{"NPC", isNpc, saveNpc, takeNpc, OWN_HOST, 6, false, 0, 0},
	{"leaf boat", isLeafBoat, saveRide, nullptr, OWN_EACH, 2, false, 0, 0},
	{"mud boat", isMuddyBoat, saveRide, nullptr, OWN_EACH, 2, false, 0, 0},
	{"fruit boat", isFruitsBoat, saveFruitsBoat, takeFruitsBoat, OWN_RIDER, 2, false, 0, 0},
	{"Ferris wheel", isFerrisWheel, saveFerrisWheel, takeFerrisWheel, OWN_HOST, 15, false, 0, 0},
	{"merry-go-round", isMerrygoround, saveTurner, takeTurner, OWN_HOST, 6, false, 0, 0},
	{"coaster car", isPinnaCoaster, savePinnaCoaster, takePinnaCoaster, OWN_HOST, 6, false, 0, 0},
	{"roulette", isRoulette, saveRoulette, takeRoulette, OWN_HOST, 3, false, 0, 0},
	{"slot machine", isSlotDrum, saveSlot, takeSlotDrum, OWN_USER, 2, true, 0, 0},
	{"item slot", isItemSlotDrum, saveSlot, takeItemSlotDrum, OWN_USER, 2, true, 0, 0},
	{"Yoshi's egg", isEgg, saveEgg, takeEgg, OWN_USER, 10, false, 0, 0},
	{"Chain Chomp", isChomp, saveChomp, takeChomp, OWN_USER, 2, false, 0, 0},
	{"Bowser", isBowser, saveBowser, takeBowser, OWN_HOST, 2, false, 0, 0},
	{"Bowser Jr.", isJunior, saveJunior, takeJunior, OWN_HOST, 2, false, 0, 0},
	{"sleeping Wiggler", isSleepyWiggler, saveSleepyWiggler, takeSleepyWiggler, OWN_HOST, 3, false, 0, 0},
	{"Bowser's tub", isTub, saveTub, takeTub, OWN_HOST, 2, false, 0, 0},
	{"critter", isCritter, saveCritter, takeCritter, OWN_HOST, 3, false, 0, 0},
	{"pirate ship", isViking, saveViking, takeViking, OWN_HOST, 4, false, 0, 0},
	{"swinging board", isSwingBoard, saveSwingBoard, takeSwingBoard, OWN_RIDER, 3, false, 0, 0},
	{"machine", isMachine, saveMachine, takeMachine, OWN_RIDER, 4, false, 6, 0x38},
	{"rail mover", isRailMover, saveMachine, takeMachine, OWN_RIDER, 2, false, 6, 0x38},
	{"roll block", isRollBlock, saveRollBlock, takeRollBlock, OWN_HOST, 3, false, 1, 0x1},
	{"path animation", isJointMover, saveJointMover, takeJointMover, OWN_HOST, 6, false, 0, 0},
	{"sand block", isSandBlock, saveSandBlock, takeSandBlock, OWN_RIDER, 3, false, 0, 0},
	{"fish with a coin", isCarryingSchool, saveSchool, takeSchool, OWN_HOST, 3, false, 0, 0},
	{"watermelon", isWatermelon, saveWatermelon, takeWatermelon, OWN_USER, 2, false, 0, 0},
	{"Turbo nozzle door", isTurboDoor, saveTurboDoor, takeTurboDoor, OWN_USER, 3, false, 0, 0},
	{"manhole lid", isManhole, saveManhole, takeManhole, OWN_USER, 3, false, 0, 0},
};
const int kKindCount = sizeof kKinds / sizeof kKinds[0];

// The sea's waves (their phases start at random): the host's.
const Kind kWaveKind = {"waves", nullptr, nullptr, nullptr, OWN_HOST, 30, false, 0, 0};

// ------------------------------------------------------------------ ownership

int fps() { return port_active_frame_rate > 0 ? port_active_frame_rate : 30; }

// ------------------------------------------------------------------ smooth kinds

// This game's clock (ms of play), and each owner's as seen here: how far
// behind ours their stamps arrive, at least (their clock's offset plus the
// quickest trip): down at once to a quicker one, up slowly (the two games'
// clocks drift apart as their frame rates differ); and the offset used,
// which leans toward it gently so the moment drawn does not jump.
double s_ms;
int s_traceIndex = -1; // debug: the smooth mirror traced (the first that moves)
struct Lag {
	double min, use;
	bool known;
};
Lag s_lag[MPG_MAX_PLAYERS];

void noteLag(int slot, u32 at)
{
	Lag& l         = s_lag[(unsigned)slot % MPG_MAX_PLAYERS];
	const double d = s_ms - (double)at;
	if (!l.known || d < l.min - 1000.0 || d > l.min + 1000.0) {
		l.min = l.use = d; // the first, or a clock that jumped (a new stage, a reload)
		l.known       = true;
		return;
	}
	if (d < l.min)
		l.min = d;
	else
		l.min += (d - l.min) * 0.02;
}

void tickLags()
{
	for (int i = 0; i < MPG_MAX_PLAYERS; i++) {
		Lag& l = s_lag[i];
		if (!l.known)
			continue;
		const double d = l.min - l.use;
		l.use += d > 300.0 || d < -300.0 ? d : d * 0.05;
	}
}

f32 turnBetween(f32 a, f32 b, f32 t)
{
	f32 d = b - a;
	while (d > 180.0f)
		d -= 360.0f;
	while (d < -180.0f)
		d += 360.0f;
	return a + d * t;
}

// A smooth kind's new state: into its history (oldest first), unless it
// came late. One after a long quiet (the owner sends an unchanged state
// seldom) is taken to have started from the last a send's time before.
void addSample(Mirrored& m, int slot, const u8* data, int size)
{
	const Kind& k = *m.kind;
	if (size < k.smooth * 4 + 4)
		return;
	Sample n;
	memcpy(&n.at, data + size - 4, 4);
	memcpy(n.v, data, k.smooth * 4);
	noteLag(slot, n.at);
	// the time between the owner's states as they have come (or as it sends them)
	u32 period = (u32)(k.every * 1000 / fps());
	if (m.arrivalGap > (f32)period && m.arrivalGap < 500.0f)
		period = (u32)m.arrivalGap;
	if (m.historyCount) {
		const Sample& last = m.history[m.historyCount - 1];
		if ((s32)(n.at - last.at) <= 0)
			return;
		if (n.at - last.at > 3 * period + 50) {
			Sample still = last;
			still.at     = n.at - period;
			if (m.historyCount == kHistory)
				memmove(m.history, m.history + 1, sizeof(Sample) * (kHistory - 1)), m.historyCount--;
			m.history[m.historyCount++] = still;
		}
	}
	if (m.historyCount == kHistory)
		memmove(m.history, m.history + 1, sizeof(Sample) * (kHistory - 1)), m.historyCount--;
	m.history[m.historyCount++] = n;
	// states come in bursts (a few a packet): the time between them, as they come
	const f32 gap = (f32)(s_ms - m.arrivedAt);
	if (m.arrivedAt > 0.0 && gap > 0.0f && gap < 500.0f)
		m.arrivalGap += (gap - m.arrivalGap) * 0.1f;
	m.arrivedAt = s_ms;
}

// A smooth kind's floats a moment in the past (about two of the owner's
// sends behind its newest, as they have come lately): between the two states
// either side of it; past the newest, the newest (not on as it went: where
// it stopped or turned back it would go too far, and come back).
void blendSample(const Mirrored& m, f32* out)
{
	const Kind& k   = *m.kind;
	const Sample* h = m.history;
	const int n     = m.historyCount;
	const Lag& l    = s_lag[(unsigned)m.heardSlot % MPG_MAX_PLAYERS];
	f32 gaps = 0.0f;
	int gapCount = 0;
	for (int i = 0; i + 1 < n; i++) {
		const f32 gap = (f32)(s32)(h[i + 1].at - h[i].at);
		if (gap > 0.0f && gap < 500.0f)
			gaps += gap, gapCount++;
	}
	// behind the newest by about two of the gaps between states, or between
	// their arrivals (the larger), so a late one seldom leaves it waiting
	f32 every = gapCount ? gaps / gapCount : k.every * 1000.0f / fps();
	if (m.arrivalGap > every)
		every = m.arrivalGap;
	double delay = every * 2.2 + 40.0;
	delay        = delay < 120.0 ? 120.0 : delay > 400.0 ? 400.0 : delay;
	const double at    = s_ms - l.use - delay;
	const Sample* a = &h[n - 1];
	const Sample* b = &h[n - 1];
	f32 t           = 0.0f;
	if (at < (double)h[0].at) {
		a = b = &h[0];
	} else {
		for (int i = 0; i + 1 < n; i++)
			if ((double)h[i].at <= at && at < (double)h[i + 1].at) {
				a = &h[i];
				b = &h[i + 1];
				t = (f32)((at - (double)a->at) / (double)(s32)(b->at - a->at));
				break;
			}
	}
	for (int i = 0; i < k.smooth; i++)
		out[i] = (k.angleMask >> i) & 1 ? turnBetween(a->v[i], b->v[i], t) : a->v[i] + (b->v[i] - a->v[i]) * t;
	static const bool trace = getenv("SMS_MULTIPLAYER_TRACE") != nullptr;
	if (trace && &m == &s_mirrors[0] + (s_traceIndex >= 0 ? s_traceIndex : 0) && s_traceIndex >= 0)
		fprintf(stderr, "[mptrace] mirror %d.%d: at %.0f, newest %u (%d states), lag %.0f, t %.2f -> %.1f %.1f %.1f\n", m.manager,
		        m.index, at, h[n - 1].at, n, l.use, t, out[0], out[1], out[2]);
}

void takeSmooth(Mirrored& m)
{
	if (!m.historyCount || m.stateSize < m.kind->smooth * 4)
		return;
	u8 now[48];
	f32 v[kSmoothMax];
	blendSample(m, v);
	memcpy(now, v, m.kind->smooth * 4);
	memcpy(now + m.kind->smooth * 4, m.state + m.kind->smooth * 4, m.stateSize - m.kind->smooth * 4);
	m.kind->take(m.actor, now, m.stateSize, 0, &m.scratch);
}

bool rides(TLiveActor* a)
{
	TMario* m = gpMarioOriginal;
	return m && m->mGroundPlane && m->mGroundPlane->getActor() == a && m->isTouchGround4cm();
}

// This game's player is talking to `a` (an NPC): it stands and faces them here.
bool talkingHere(TLiveActor* a)
{
	TMarDirector* d = gpMarDirector;
	return d && d->isTalkModeNow() && (TLiveActor*)d->getTalkingNPC() == a;
}

bool ownedHere(Mirrored& m)
{
	// an NPC this game's player is talking to: run here (it stops and turns
	// to them, as it does for everyone else too, since this game sends it),
	// and after, until another player talks to it (one the talk sent off
	// somewhere, a lost child following its finder, goes where it goes here)
	if (m.actor && m.kind->match == isNpc && talkingHere(m.actor)) {
		if (!m.claimedAt) {
			m.claimedAt = s_clock;
			if (getenv("SMS_MULTIPLAYER_DEBUG"))
				fprintf(stderr, "[multiplayer] NPC %d.%d: talked to here, run here now\n", m.manager, m.index);
		}
		return true;
	}
	if (m.claimedAt)
		return true;
	if (m.actor) {
		if ((m.kind->owner == OWN_RIDER || m.kind->owner == OWN_EACH) && rides(m.actor))
			m.usedAt = s_clock;
		// every player's own: "owned" (sent, for the others to draw) while
		// this game's player rides it (a hop on it included)
		if (m.kind->owner == OWN_EACH)
			return s_clock - m.usedAt < fps() / 2;
		// a big melon this game's Mario is up against: bumped here
		if (m.kind->match == isWatermelon && gpMarioOriginal) {
			const f32 dx = gpMarioOriginal->mPosition.x - m.actor->mPosition.x;
			const f32 dz = gpMarioOriginal->mPosition.z - m.actor->mPosition.z;
			const f32 r  = m.actor->mDamageRadius + 80.0f;
			if (dx * dx + dz * dz < r * r)
				m.usedAt = s_clock;
		}
	}
	// Chain Chomp dragged by this game's Mario (his picket in hand): ours
	if (m.actor && m.kind->match == isChomp && gpMarioOriginal
	    && gpMarioOriginal->mHeldObject == static_cast<TBossWanwan*>(m.actor)->mPicket)
		m.usedAt = s_clock;
		// an egg that hatched here: ours to say so (the others' go)
	if (m.actor && m.kind->match == isEgg) {
		if (eggHatching(static_cast<TEggYoshi*>(m.actor)))
			s_eggHatchedHere = true;
		if (s_eggHatchedHere)
			m.usedAt = s_clock;
	}
	// a slot machine this game's player set spinning: ours until its reels
	// stop (they land here, and pay out here; the others are sent where)
	// (the host's states do not count against it: the host gives way once it
	// hears from us; another player's, sent since, do: they spun it after)
	if (m.actor && (m.kind->match == isSlotDrum || m.kind->match == isItemSlotDrum)
	    && (m.heardSlot == 0 || m.usedAt > m.heardAt) && s_clock - m.usedAt < 2 * fps()) {
		if (reelsTurning(static_cast<TSlotDrum*>(m.actor)))
			m.usedAt = s_clock;
		else if (s_clock - m.usedAt == 1 && getenv("SMS_MULTIPLAYER_DEBUG"))
			fprintf(stderr, "[multiplayer] %s %d.%d: its reels stopped here (won %d)\n", m.kind->name, m.manager, m.index,
			        (int)static_cast<TSlotDrum*>(m.actor)->unk194);
	}
	// a Turbo door broken down here: ours to say so (closed, nothing to say)
	if (m.actor && m.kind->match == isTurboDoor) {
		const bool open = doorOpen(m.actor) && !m.scratch;
		if (open && !m.sentSize)
			m.usedAt = s_clock;
		if (!open)
			return false;
	}
	// a manhole lid this game's player flipped: ours to say so
	if (m.actor && m.kind->match == isManhole && lidTick(m.actor, &m.scratch))
		m.usedAt = s_clock;
	// used here lately: ours (two seconds after; riding, while on it)
	if (m.kind->owner != OWN_HOST && s_clock - m.usedAt < 2 * fps())
		return true;
	if (mpg_role() != MPG_ROLE_HOST)
		return false;
	// the host runs it, unless a player is using it now
	return !(m.heardSlot != 0 && s_clock - m.heardAt < fps());
}

Mirrored* find(int manager, int index)
{
	for (int i = 0; i < s_count; i++)
		if (s_mirrors[i].manager == manager && s_mirrors[i].index == index)
			return &s_mirrors[i];
	return nullptr;
}

Mirrored* findActor(const void* a)
{
	for (int i = 0; i < s_count; i++)
		if (s_mirrors[i].actor == a)
			return &s_mirrors[i];
	return nullptr;
}

void add(TLiveActor* a, int manager, int index)
{
	if (!a || s_count >= kMaxMirrors || MpBossIsBoss(a))
		return;
	for (int k = 0; k < kKindCount; k++)
		if (kKinds[k].match(a)) {
			Mirrored& m = s_mirrors[s_count++];
			memset(&m, 0, sizeof m);
			m.actor   = a;
			m.manager = manager;
			m.index   = index;
			m.kind    = &kKinds[k];
			m.usedAt  = m.heardAt = -100000;
			if (isTurboDoor(a))
				m.scratch = doorOpen(a) ? 1 : 0; // open as it loaded
			return;
		}
}

int saveWaves(u8* out)
{
	Put o(out);
	o.f(gpMapObjWave->mWavePhaseX);
	o.f(gpMapObjWave->mWavePhaseZ);
	return o.n;
}

void takeWaves(const u8* in, int size)
{
	Get g(in, size);
	const f32 x = g.f(), z = g.f();
	if (g.ok && gpMapObjWave) {
		gpMapObjWave->mWavePhaseX = x;
		gpMapObjWave->mWavePhaseZ = z;
	}
}

} // namespace

void debugMirrors();

void MpMirrorForget()
{
	s_count          = 0;
	s_eggHatchedHere = false;
	s_testWalked     = nullptr;
	memset(s_rides, 0, sizeof s_rides);
	s_testBoat = nullptr;
	memset(s_rideCopies, 0, sizeof s_rideCopies); // the last stage's, gone with its memory
}

void MpMirrorPrepareRides()
{
	if (!gpConductor)
		return;
	TLiveActor* first[kRideKinds] = {};
	for (JGadget::TList<TLiveManager*>::iterator it = gpConductor->unk10.begin(); it != gpConductor->unk10.end(); ++it)
		for (int i = 0; *it && i < (*it)->getObjNum(); i++) {
			TLiveActor* a = (*it)->getObj(i);
			if (!first[0] && isLeafBoat(a))
				first[0] = a;
			if (!first[1] && isMuddyBoat(a))
				first[1] = a;
		}
	for (int k = 0; k < kRideKinds; k++) {
		MActor* own = first[k] ? first[k]->getMActor() : nullptr;
		if (!own || !own->mModel)
			continue;
		SDLModelData* data = static_cast<SDLModel*>(own->mModel)->getSDLModelData();
		if (!data)
			continue;
		const u32 flags = static_cast<TMapObjBase*>(first[k])->getSDLModelFlag();
		int made = 0;
		for (int slot = 0; slot < MPG_MAX_PLAYERS; slot++)
			if ((slot != mpg_slot() || getenv("SMS_MULTIPLAYER_RIDE_ECHO"))
			    && (s_rideCopies[slot][k] = SMS_MakeMActorFromSDLModelData(data, own->mAnmData, flags)))
				made++;
		fprintf(stderr, "[multiplayer] %s: a copy for each of %d other players to ride\n", k ? "mud boats" : "leaf boats", made);
	}
}

bool MpMirrorTestTurboDoor()
{
	for (int i = 0; i < s_count; i++) {
		Mirrored& m = s_mirrors[i];
		if (!m.actor || m.kind->match != isTurboDoor || doorOpen(m.actor))
			continue;
		const u8 open = 1;
		takeTurboDoor(m.actor, &open, 1, 0, &m.scratch); // as a dash into it would
		fprintf(stderr, "[multiplayer] test: broke down the Turbo nozzle door at %.0f %.0f %.0f\n", m.actor->mPosition.x,
		        m.actor->mPosition.y, m.actor->mPosition.z);
		return true;
	}
	return false;
}

bool MpMirrorTestRide(int seconds)
{
	TLiveActor* best = nullptr;
	f32 nearest      = 1e30f;
	for (int i = 0; i < s_count && gpMarioOriginal; i++) {
		TLiveActor* a = s_mirrors[i].actor;
		if (!a || s_mirrors[i].kind->owner != OWN_EACH)
			continue;
		const f32 dx = a->mPosition.x - gpMarioOriginal->mPosition.x, dz = a->mPosition.z - gpMarioOriginal->mPosition.z;
		if (dx * dx + dz * dz < nearest)
			best = a, nearest = dx * dx + dz * dz;
	}
	if (!best)
		return false;
	gpMarioOriginal->mPosition.set(best->mPosition.x, best->mPosition.y + 400.0f, best->mPosition.z);
	s_testBoat       = best;
	s_testBoatFrames = seconds * fps();
	fprintf(stderr, "[multiplayer] test: on the %s at %.0f %.0f %.0f\n", isMuddyBoat(best) ? "mud boat" : "leaf boat",
	        best->mPosition.x, best->mPosition.y, best->mPosition.z);
	return true;
}

MActor* MpMirrorRide(int slot, const f32 root[3])
{
	if (slot < 0 || slot >= MPG_MAX_PLAYERS)
		return nullptr;
	const Ride& r = s_rides[slot];
	if (!r.at || s_clock - r.at > fps() / 2 || !r.kind)
		return nullptr; // not riding one
	MActor* copy = s_rideCopies[slot][r.kind->match == isMuddyBoat ? 1 : 0];
	if (!copy)
		return nullptr;
	Get g(r.data, kRideState);
	f32 at[3], q[4], scale[3];
	for (int i = 0; i < 3; i++)
		at[i] = root[i] + g.f();
	if (slot == mpg_slot())
		at[0] += 400.0f; // the debug echo of our own (beside it)
	for (int i = 0; i < 4; i++)
		q[i] = g.h() / 32767.0f;
	for (int i = 0; i < 3; i++)
		scale[i] = g.f();
	if (!g.ok)
		return nullptr;
	// where this game's own of that boat is too (they boarded it where it
	// waits here): that one is under them, not a copy on top of it
	if (r.here) {
		const f32 dx = r.here->mPosition.x - at[0], dz = r.here->mPosition.z - at[2];
		if (dx * dx + dz * dz < 250.0f * 250.0f)
			return nullptr;
	}
	const f32 n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	if (n < 0.5f)
		return nullptr;
	const f32 x = q[0] / n, y = q[1] / n, z = q[2] / n, w = q[3] / n;
	const f32 rot[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
	                       {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
	                       {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
	Mtx m;
	for (int row = 0; row < 3; row++) {
		for (int c = 0; c < 3; c++)
			m[row][c] = rot[row][c] * scale[c];
		m[row][3] = at[row];
	}
	copy->getModel()->setBaseTRMtx(m);
	static int said;
	if (getenv("SMS_MULTIPLAYER_DEBUG") && ++said % 60 == 1)
		fprintf(stderr, "[multiplayer] %s rides their %s at %.0f %.0f %.0f (this game's is at %.0f %.0f %.0f)\n",
		        mpg_character_name(slot), r.kind->name, at[0], at[1], at[2], r.here ? r.here->mPosition.x : 0.0f,
		        r.here ? r.here->mPosition.y : 0.0f, r.here ? r.here->mPosition.z : 0.0f);
	return copy;
}

void MpMirrorCollect()
{
	s_count          = 0;
	s_eggHatchedHere = false;
	if (!gpConductor)
		return;
	int m = 0;
	for (JGadget::TList<TLiveManager*>::iterator it = gpConductor->unk10.begin(); it != gpConductor->unk10.end() && m < kWaves;
	     ++it, ++m) {
		TLiveManager* mgr = *it;
		for (int i = 0; mgr && i < mgr->getObjNum() && i < 0x10000; i++)
			add(mgr->getObj(i), m, i);
	}
	int i = 0;
	for (JGadget::TList<TLiveActor*>::iterator it = gpConductor->unk30.begin(); it != gpConductor->unk30.end() && i < 512;
	     ++it, ++i)
		add(*it, kAlone, i);
	if (gpMapObjWave && s_count < kMaxMirrors) {
		Mirrored& w = s_mirrors[s_count++];
		memset(&w, 0, sizeof w);
		w.manager = kWaves;
		w.kind    = &kWaveKind;
		w.usedAt = w.heardAt = -100000;
	}
	if (getenv("SMS_MULTIPLAYER_DEBUG")) {
		int counts[kKindCount] = {0};
		for (int k = 0; k < s_count; k++)
			if (s_mirrors[k].kind != &kWaveKind)
				counts[s_mirrors[k].kind - kKinds]++;
		for (int k = 0; k < kKindCount; k++)
			if (counts[k]) {
				// and of which classes
				const char* seen[8] = {nullptr};
				int seenCount[8] = {0};
				for (int i = 0; i < s_count; i++) {
					if (s_mirrors[i].kind != &kKinds[k] || !s_mirrors[i].actor)
						continue;
					const char* name = typeid(*s_mirrors[i].actor).name();
					for (int j = 0; j < 8; j++)
						if (!seen[j] || !strcmp(seen[j], name)) {
							seen[j] = name;
							seenCount[j]++;
							break;
						}
				}
				char classes[256] = "";
				for (int j = 0; j < 8 && seen[j]; j++)
					snprintf(classes + strlen(classes), sizeof classes - strlen(classes), " %d %s", seenCount[j], seen[j]);
				fprintf(stderr, "[multiplayer] mirrored here: %d %s (%s )\n", counts[k], kKinds[k].name, classes);
			}
	}
}

void MpMirrorTick()
{
	s_clock++;
	s_ms += 1000.0 / fps();
	tickLags();
	if (s_testBoat && s_testBoatFrames > 0) {
		// on, as the rider's water would drive it
		s_testBoatFrames--;
		if (TMuddyBoat* b = dynamic_cast<TMuddyBoat*>(s_testBoat)) {
			b->mSpeed = 4.0f;
			b->offLiveFlag(LIVE_FLAG_UNK10); // awake, as its rider's water wakes it (moveByWater)
		}
		else
			s_testBoat->mVelocity.x = 3.0f;
		if (s_testBoatFrames % 60 == 0)
			fprintf(stderr, "[multiplayer] test: riding the boat at %.0f %.0f %.0f (on it: %d)\n", s_testBoat->mPosition.x,
			        s_testBoat->mPosition.y, s_testBoat->mPosition.z, rides(s_testBoat));
	}
	if (s_testWalked && s_testWalkFrames > 0) {
		s_testWalkFrames--;
		s_testWalked->mPosition.x += 3.0f;
		if (!s_testWalkFrames)
			fprintf(stderr, "[multiplayer] test: NPC walked to %.0f %.0f %.0f\n", s_testWalked->mPosition.x,
			        s_testWalked->mPosition.y, s_testWalked->mPosition.z);
	}
	// the others' states, kept: taken every tick (a mover between theirs moves on)
	int slot, manager, index, size;
	u8 data[48];
	while (mpg_next_mirror(&slot, &manager, &index, data, &size)) {
		if (manager == 253) {
			MpEnemyReport(slot, index, data, size); // an enemy a client's game runs
			continue;
		}
		Mirrored* m = find(manager, index);
		if (!m || size > 48)
			continue;
		if (m->kind->owner == OWN_EACH) {
			// their own boat, under them: drawn with them, this game's stays as it is
			if (slot >= 0 && slot < MPG_MAX_PLAYERS && size == kRideState) {
				Ride& r = s_rides[slot];
				r.kind  = m->kind;
				r.here  = m->actor;
				memcpy(r.data, data, size);
				r.at = s_clock;
			}
			continue;
		}
		// another player's game runs it now (they talked to it since)
		// (not while this game's player is still talking to it: two players
		// talking to one NPC each have it face them, in their own game)
		if (m->claimedAt && slot != mpg_slot() && s_clock - m->claimedAt > fps() && !(m->actor && talkingHere(m->actor)))
			m->claimedAt = 0;
		m->heardAt   = s_clock;
		m->heardSlot = slot;
		if (m->kind->smooth) {
			addSample(*m, slot, data, size);
			size -= 4; // the owner's clock
			if (size < 0)
				continue;
		}
		memcpy(m->state, data, size);
		m->stateSize = size;
		m->stateAt   = s_clock;
	}
	// paused here: what this game runs stands still, and the others' run on
	// their own meanwhile (an unchanged state sent on would hold them still);
	// the host paused: its states are of a world that stands still
	const bool pausedHere = MpPausedHere(), hostPaused = MpHostPaused();
	static bool wasHostPaused;
	if (hostPaused != wasHostPaused && getenv("SMS_MULTIPLAYER_DEBUG"))
		fprintf(stderr, "[multiplayer] the host %s\n",
		        hostPaused ? "paused: this game's world runs on its own" : "plays again: its world is everyone's again");
	wasHostPaused = hostPaused;
	for (int i = 0; i < s_count; i++) {
		Mirrored& m = s_mirrors[i];
		if (m.actor && m.kind->match == isFruitsBoat)
			keepRollAxis(static_cast<TFruitsBoat*>(m.actor));
		if (ownedHere(m)) {
			if (pausedHere)
				continue;
			if (m.actor && m.kind->match == isTub && mpg_role() == MPG_ROLE_HOST)
				weighTub(static_cast<TBathtub*>(m.actor));
			// each on a tick of its own (by its place), so a stage's many
			// critters are not all sent in one tick
			if ((s_clock + i) % m.kind->every)
				continue;
			u8 out[48];
			const int n = m.actor ? m.kind->save(m.actor, out) : (gpMapObjWave ? saveWaves(out) : 0);
			if (m.actor && m.kind->match == isManhole)
				out[0] = (u8)(m.scratch & kLidCount); // the flips known here
			// unchanged: once a second from the host, so a newcomer has it; a
			// few times a second from a player using it, so the host (which
			// takes it back once it hears nothing for a second) keeps letting them
			const int keep = mpg_role() == MPG_ROLE_HOST ? fps() : fps() / 3;
			if (n <= 0 || (n == m.sentSize && !memcmp(out, m.sent, n) && s_clock - m.sentAt < keep))
				continue;
			m.historyCount = 0; // ours now: what came from another owner is old news
			if (m.kind->smooth) {
				// stamped with this game's clock, for the others to draw it smoothly
				const u32 now = (u32)s_ms;
				if (n + 4 > 48)
					continue;
				u8 stamped[48];
				memcpy(stamped, out, n);
				memcpy(stamped + n, &now, 4);
				mpg_mirror(m.manager, m.index, stamped, n + 4);
			} else
				mpg_mirror(m.manager, m.index, out, n);
			// debug (SMS_MULTIPLAYER_RIDE_ECHO=1): our own boat's state for our
			// own puppet too, drawn beside us, to see the copy others see
			static const bool echo = getenv("SMS_MULTIPLAYER_RIDE_ECHO") != nullptr;
			if (echo && m.kind->owner == OWN_EACH && n == kRideState && mpg_slot() >= 0) {
				Ride& r = s_rides[mpg_slot()];
				r.kind  = m.kind;
				r.here  = nullptr;
				memcpy(r.data, out, n);
				r.at = s_clock;
			}
			memcpy(m.sent, out, n);
			m.sentSize = n;
			m.sentAt   = s_clock;
			continue;
		}
		if (m.kind->owner == OWN_EACH)
			continue; // this game's own, whoever rides theirs
		if (!m.stateSize || s_clock - m.stateAt > 2 * fps() || (hostPaused && m.heardSlot == 0))
			continue; // nothing lately: it runs as it would
		if (m.actor && m.kind->smooth)
			takeSmooth(m);
		else if (m.actor)
			m.kind->take(m.actor, m.state, m.stateSize, s_clock - m.stateAt, &m.scratch);
		else if (m.stateAt == s_clock)
			takeWaves(m.state, m.stateSize);
	}
	debugMirrors();
}

// Debug: how smoothly the smooth kinds taken here move: each tick's step
// against the last (a hitch, a tug back, shows as a large change of step).
void measureSmooth()
{
	struct Track {
		JGeometry::TVec3<f32> last, step;
		f32 rough, moved;
		int ticks;
	};
	static Track tracks[kMaxMirrors];
	static int frames;
	f32 rough = 0.0f, moved = 0.0f;
	int movers = 0;
	for (int i = 0; i < s_count; i++) {
		Mirrored& m = s_mirrors[i];
		Track& k    = tracks[i];
		if (!m.actor || !m.kind->smooth || ownedHere(m) || !m.historyCount) {
			k.ticks = 0;
			continue;
		}
		const JGeometry::TVec3<f32> p = m.actor->mPosition;
		const JGeometry::TVec3<f32> step(p.x - k.last.x, p.y - k.last.y, p.z - k.last.z);
		if (k.ticks >= 2) {
			const f32 dx = step.x - k.step.x, dy = step.y - k.step.y, dz = step.z - k.step.z;
			k.rough += sqrtf(dx * dx + dy * dy + dz * dz);
			k.moved += sqrtf(step.x * step.x + step.y * step.y + step.z * step.z);
		}
		k.last = p;
		k.step = step;
		k.ticks++;
		if (k.moved > 0.0f && s_traceIndex < 0)
			s_traceIndex = i;
		if (k.moved > 0.0f) {
			rough += k.rough;
			moved += k.moved;
			movers++;
		}
	}
	if (++frames % (10 * fps()) == 0 && movers) {
		fprintf(stderr, "[multiplayer] smooth movers here: %d moving, change of step %.1f%% of the distance moved\n", movers,
		        moved > 0.0f ? 100.0f * rough / moved : 0.0f);
		for (int i = 0; i < s_count; i++)
			tracks[i].rough = tracks[i].moved = 0.0f;
	}
}

// Debug: the first of each kind, where it is here and who runs it.
void debugMirrors()
{
	if (getenv("SMS_MULTIPLAYER_DEBUG"))
		measureSmooth();
	if (!getenv("SMS_MULTIPLAYER_DEBUG") || s_clock % (10 * fps()))
		return;
	bool said[kKindCount] = {false};
	if (TMario* me = gpMarioOriginal)
		if (me->mGroundPlane && me->mGroundPlane->getActor())
			fprintf(stderr, "[multiplayer] standing on %s%s\n", typeid(*me->mGroundPlane->getActor()).name(),
			        MpMirrorOnTub() ? " (the tub)" : "");
	for (int i = 0; i < s_count; i++) {
		Mirrored& m = s_mirrors[i];
		if (!m.actor || said[m.kind - kKinds])
			continue;
		said[m.kind - kKinds] = true;
		char extra[64] = "";
		if (m.kind->match == isNpc)
			snprintf(extra, sizeof extra, " (flags %x%s)", (unsigned)m.actor->mLiveFlag, m.claimedAt ? ", claimed here" : "");
		if (m.kind->match == isSlotDrum || m.kind->match == isItemSlotDrum)
			snprintf(extra, sizeof extra, " (reels turning %d, won %d)", (int)reelsTurning(static_cast<TSlotDrum*>(m.actor)),
			         (int)static_cast<TSlotDrum*>(m.actor)->unk194);
		if (m.kind->match == isEgg)
			snprintf(extra, sizeof extra, " (egg state %d%s; Yoshi here %s)", (int)static_cast<TEggYoshi*>(m.actor)->mState,
			         m.actor->checkLiveFlag(LIVE_FLAG_DEAD) ? ", gone" : "",
			         SMS_GetYoshi() && SMS_GetYoshi()->isHatched() ? "hatched" : "not hatched");
		fprintf(stderr, "[multiplayer] mirror: %s %d.%d at %.0f %.0f %.0f, %s, last heard %d ticks ago%s\n", m.kind->name,
		        m.manager, m.index, m.actor->mPosition.x, m.actor->mPosition.y, m.actor->mPosition.z,
		        ownedHere(m) ? "run here" : "taken", s_clock - m.heardAt, extra);
	}
}

void MpMirrorUsed(const void* object)
{
	if (Mirrored* m = findActor(object))
		m->usedAt = s_clock;
}

bool MpMirrorTestHatch()
{
	for (int k = 0; k < s_count; k++) {
		Mirrored& m = s_mirrors[k];
		if (!m.actor || m.kind->match != isEgg)
			continue;
		TEggYoshi* e = static_cast<TEggYoshi*>(m.actor);
		if (e->checkLiveFlag(LIVE_FLAG_DEAD) || eggHatching(e) || !gpConductor)
			continue;
		if (e->mState == 0xE) { // asleep until the plaza's story allows it (a new save): woken, for the test
			e->mState = 1;
			if (!e->unk14C)
				e->decideRandomLoveFruit();
		}
		// a fruit of the stage of the kind it wants, fed as the game feeds it
		for (JGadget::TList<TLiveManager*>::iterator it = gpConductor->unk10.begin(); it != gpConductor->unk10.end(); ++it)
			for (int i = 0; *it && i < (*it)->getObjNum(); i++) {
				TLiveActor* fruit = (*it)->getObj(i);
				if (fruit && (u32)fruit->mActorType == e->unk14C) {
					fprintf(stderr, "[multiplayer] test: feeding Yoshi's egg %d.%d its fruit (%08x)\n", m.manager, m.index,
					        (unsigned)e->unk14C);
					e->touchFruit(fruit);
					return true;
				}
			}
		static bool said;
		if (!said) {
			said = true;
			fprintf(stderr, "[multiplayer] test: Yoshi's egg wants %08x (state %d); fruit here:", (unsigned)e->unk14C,
			        (int)e->mState);
			for (JGadget::TList<TLiveManager*>::iterator it = gpConductor->unk10.begin(); it != gpConductor->unk10.end(); ++it)
				for (int i = 0; *it && i < (*it)->getObjNum(); i++)
					if (TLiveActor* f = (*it)->getObj(i))
						if ((f->mActorType & 0xFFFFFF00) == 0x40000300)
							fprintf(stderr, " %08x", (unsigned)f->mActorType);
			fprintf(stderr, "\n");
		}
	}
	return false;
}

// Test aid: the first NPC is claimed as a talk to it would claim it, and
// walked off (as an event sends one): the others' games should follow it.
// Test aid: this game's Mario stood before the first NPC, facing it (for a
// press of A to talk to it), until a talk starts or `frames` run out.
bool talkable(TLiveActor* a)
{
	TMarDirector* d = gpMarDirector;
	if (!d || a->checkLiveFlag(LIVE_FLAG_DEAD) || a->checkLiveFlag(LIVE_FLAG_HIDDEN) || a->checkLiveFlag(LIVE_FLAG_UNK100000))
		return false;
	for (JGadget::TVector_pointer<TBaseNPC*>::iterator it = d->unk88.begin(); it != d->unk88.end(); ++it)
		if ((TLiveActor*)*it == a)
			return true;
	return false;
}

bool MpMirrorTestNearNpc()
{
	TMario* me = gpMarioOriginal;
	static int said = -1;
	for (int k = 0; me && k < s_count; k++)
		if (s_mirrors[k].actor && s_mirrors[k].kind->match == isNpc && talkable(s_mirrors[k].actor)) {
			if (said != k) {
				said = k;
				fprintf(stderr, "[multiplayer] test: standing before NPC %d.%d\n", s_mirrors[k].manager, s_mirrors[k].index);
			}
			const JGeometry::TVec3<f32>& n = s_mirrors[k].actor->mPosition;
			const f32 yaw = s_mirrors[k].actor->mRotation.y * (3.14159265f / 180.0f);
			me->mPosition.set(n.x + sinf(yaw) * 90.0f, n.y, n.z + cosf(yaw) * 90.0f);
			me->mFaceAngle.y = (s16)(atan2f(n.x - me->mPosition.x, n.z - me->mPosition.z) * (32768.0f / 3.14159265f));
			return gpMarDirector && gpMarDirector->isTalkModeNow();
		}
	return false;
}

// Test aid: this game's Mario against the big watermelon, rolling it along
// (as bumping it does) for `frames` ticks.
bool MpMirrorTestSandBlock()
{
	TMario* me = gpMarioOriginal;
	TLiveActor* best = nullptr;
	f32 nearest      = 0.0f;
	for (int k = 0; me && k < s_count; k++)
		if (s_mirrors[k].actor && s_mirrors[k].kind->match == isSandBlock) {
			const JGeometry::TVec3<f32>& b = s_mirrors[k].actor->mPosition;
			const f32 d = (b.x - me->mPosition.x) * (b.x - me->mPosition.x) + (b.z - me->mPosition.z) * (b.z - me->mPosition.z)
			              + (b.y - me->mPosition.y) * (b.y - me->mPosition.y);
			if (!best || d < nearest)
				best = s_mirrors[k].actor, nearest = d;
		}
	if (!best)
		return false;
	const JGeometry::TVec3<f32>& b = best->mPosition;
	const TBGCheckData* floor = nullptr;
	const f32 top = gpMap->checkGround(b.x, b.y + 1000.0f, b.z, &floor);
	me->warpRequest(JGeometry::TVec3<f32>(b.x, top + 10.0f, b.z), 0.0f);
	fprintf(stderr, "[multiplayer] test: onto the nearest sand block, at %.0f %.0f %.0f (%.0f away; state %d, scale %.2f, floor %.0f of %s)\n", b.x,
	        b.y, b.z, sqrtf(nearest), (int)static_cast<TMapObjBase*>(best)->mState, best->mScaling.y, top,
	        floor && floor->getActor() ? typeid(*floor->getActor()).name() : "the map");
	return true;
}

// Test aid: this game's Mario over the nearest manhole lid, ground pounding
// into it (`watch`: standing 6 m from it instead); `frame` counts from 0. True
// once done.
bool MpMirrorTestManhole(int frame, bool watch)
{
	TMario* me = gpMarioOriginal;
	TLiveActor* best = nullptr;
	f32 nearest      = 0.0f;
	for (int k = 0; me && k < s_count; k++)
		if (s_mirrors[k].actor && s_mirrors[k].kind->match == isManhole) {
			const JGeometry::TVec3<f32>& b = s_mirrors[k].actor->mPosition;
			const f32 d = (b.x - me->mPosition.x) * (b.x - me->mPosition.x) + (b.z - me->mPosition.z) * (b.z - me->mPosition.z);
			if (!best || d < nearest)
				best = s_mirrors[k].actor, nearest = d;
		}
	if (!best)
		return true;
	const JGeometry::TVec3<f32>& b = best->mPosition;
	if (frame == 0) {
		if (watch)
			me->warpRequest(JGeometry::TVec3<f32>(b.x, b.y + 50.0f, b.z - 600.0f), 0.0f);
		else
			me->warpRequest(JGeometry::TVec3<f32>(b.x, b.y + 500.0f, b.z), 0.0f);
		fprintf(stderr, "[multiplayer] test: %s the manhole lid at %.0f %.0f %.0f\n", watch ? "beside" : "over", b.x, b.y, b.z);
		return watch;
	}
	if (frame < 8)
		return false;
	me->changePlayerStatus(MARIO_STATUS_HIP_DROP, 0, false);
	fprintf(stderr, "[multiplayer] test: ground pound at %.0f %.0f %.0f\n", me->mPosition.x, me->mPosition.y, me->mPosition.z);
	return true;
}

bool MpMirrorTestRollMelon(int frame)
{
	for (int k = 0; k < s_count; k++)
		if (s_mirrors[k].actor && s_mirrors[k].kind->match == isWatermelon && gpMarioOriginal) {
			TLiveActor* w = s_mirrors[k].actor;
			if (frame == 0)
				fprintf(stderr, "[multiplayer] test: rolling the watermelon %d.%d from %.0f %.0f %.0f\n", s_mirrors[k].manager,
				        s_mirrors[k].index, w->mPosition.x, w->mPosition.y, w->mPosition.z);
			gpMarioOriginal->mPosition.set(w->mPosition.x - w->mDamageRadius - 40.0f, w->mPosition.y, w->mPosition.z);
			w->mPosition.x += 4.0f;
			return true;
		}
	return false;
}

bool MpMirrorTestClaimNpc()
{
	for (int k = 0; k < s_count; k++)
		if (s_mirrors[k].actor && s_mirrors[k].kind->match == isNpc) {
			s_mirrors[k].claimedAt = s_clock;
			s_testWalked           = s_mirrors[k].actor;
			s_testWalkFrames       = 300;
			fprintf(stderr, "[multiplayer] test: NPC %d.%d claimed here, walking it off\n", s_mirrors[k].manager,
			        s_mirrors[k].index);
			return true;
		}
	return false;
}

bool MpMirrorTestTubEdge()
{
	for (int k = 0; k < s_count; k++)
		if (s_mirrors[k].actor && s_mirrors[k].kind->match == isTub && gpMarioOriginal) {
			const JGeometry::TVec3<f32>& c = s_mirrors[k].actor->mPosition;
			gpMarioOriginal->mPosition.set(c.x + 900.0f, c.y + 300.0f, c.z);
			fprintf(stderr, "[multiplayer] test: to the tub's edge (%.0f %.0f %.0f)\n", c.x + 900.0f, c.y + 300.0f, c.z);
			return true;
		}
	return false;
}

bool MpMirrorOnTub()
{
	TMario* m = gpMarioOriginal;
	return m && m->mGroundPlane && m->mGroundPlane->getActor() && m->isTouchGround4cm()
	       && dynamic_cast<TBathtub*>((TLiveActor*)m->mGroundPlane->getActor()) != nullptr;
}

bool MpMirrorKeepsHits(const void* object)
{
	Mirrored* m = findActor(object);
	return m && m->kind->keepsHits;
}

// Debug (SMS_MULTIPLAYER_MOVERS=1): which of the stage's objects move by
// themselves, and how each kind is shared: every object's model placement
// a second into play and again ten seconds on, the classes of those that
// moved, and their mirror kind (or none: each game runs its own).
void MpMirrorSurvey(int stageFrame)
{
	static const bool on = getenv("SMS_MULTIPLAYER_MOVERS") != nullptr;
	if (!on || !gpConductor)
		return;
	const int first = fps(), second = 11 * fps();
	if (stageFrame != first && stageFrame != second)
		return;
	struct Seen {
		TLiveActor* actor;
		f32 mtx[12];
	};
	static Seen seen[4096];
	static int count;
	TLiveActor* all[4096];
	int n = 0;
	for (JGadget::TList<TLiveManager*>::iterator it = gpConductor->unk10.begin(); it != gpConductor->unk10.end(); ++it)
		for (int i = 0; *it && i < (*it)->getObjNum() && n < 4096; i++)
			if (TLiveActor* a = (*it)->getObj(i))
				all[n++] = a;
	for (JGadget::TList<TLiveActor*>::iterator it = gpConductor->unk30.begin(); it != gpConductor->unk30.end() && n < 4096; ++it)
		if (*it)
			all[n++] = *it;
	if (stageFrame == first) {
		count = 0;
		for (int i = 0; i < n; i++) {
			if (!all[i]->getMActor() || !all[i]->getMActor()->getModel())
				continue;
			seen[count].actor = all[i];
			memcpy(seen[count].mtx, all[i]->getMActor()->getModel()->getBaseTRMtx(), sizeof seen[count].mtx);
			count++;
		}
		fprintf(stderr, "[movers] stage %d: %d objects with models\n", (int)gpApplication.mCurrArea.getStage(), count);
		return;
	}
	const char* names[256];
	int moved[256], total[256];
	int classes = 0;
	for (int k = 0; k < count; k++) {
		TLiveActor* a = seen[k].actor;
		bool still = false;
		for (int i = 0; i < n; i++)
			if (all[i] == a)
				still = true;
		if (!still || !a->getMActor() || !a->getMActor()->getModel())
			continue;
		const f32* now = &a->getMActor()->getModel()->getBaseTRMtx()[0][0];
		bool move = false;
		for (int j = 0; j < 12; j++) {
			const f32 d = now[j] - seen[k].mtx[j];
			if ((j % 4 == 3 && (d > 1.0f || d < -1.0f)) || (j % 4 != 3 && (d > 0.01f || d < -0.01f)))
				move = true;
		}
		const char* name = typeid(*a).name();
		int c = 0;
		while (c < classes && strcmp(names[c], name))
			c++;
		if (c == classes) {
			if (classes == 256)
				continue;
			names[classes] = name;
			moved[classes] = total[classes] = 0;
			classes++;
		}
		total[c]++;
		moved[c] += move;
	}
	for (int c = 0; c < classes; c++) {
		if (!moved[c])
			continue;
		const char* kind = "not shared (each game runs its own)";
		for (int k = 0; k < count; k++)
			if (!strcmp(typeid(*seen[k].actor).name(), names[c])) {
				if (Mirrored* m = findActor(seen[k].actor))
					kind = m->kind->smooth ? m->kind->name : "mirrored, eased";
				else if (dynamic_cast<TSmallEnemy*>(seen[k].actor))
					kind = "enemy (the host's or nearest player's)";
				else if (MpBossIsBoss(seen[k].actor))
					kind = "boss";
				break;
			}
		fprintf(stderr, "[movers] stage %d: %d of %d %s moved: %s\n", (int)gpApplication.mCurrArea.getStage(), moved[c], total[c],
		        names[c], kind);
	}
	// and what stood still and is each game's own (some move only when ridden or hit)
	char line[1024] = "";
	for (int c = 0; c < classes; c++) {
		if (moved[c])
			continue;
		for (int k = 0; k < count; k++)
			if (!strcmp(typeid(*seen[k].actor).name(), names[c])) {
				TLiveActor* a = seen[k].actor;
				if (!findActor(a) && !dynamic_cast<TSmallEnemy*>(a) && !MpBossIsBoss(a) && !dynamic_cast<TItem*>(a))
					snprintf(line + strlen(line), sizeof line - strlen(line), " %s x%d", names[c], total[c]);
				break;
			}
	}
	fprintf(stderr, "[movers] stage %d: still, each game's own:%s\n", (int)gpApplication.mCurrArea.getStage(), line);
	fprintf(stderr, "[movers] stage %d: done\n", (int)gpApplication.mCurrArea.getStage());
}
