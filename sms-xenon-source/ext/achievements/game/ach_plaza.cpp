// Achievements (ext/achievements/README.md): Delfino Plaza and its secrets,
// the airstrip and the ending.
//
//  61 Postcard Ending   the ending's bonus picture (omakeB), shown after 120 Shines
//  62 Flag Day         the top of the airstrip's flagpole
//  65 Bird Watcher             17 green birds in one visit to the flooded or post-Corona plaza
//  66 Slam Dunk              a coconut into the orange Pianta's basket from a terminal ground pound
//  67 Three-Pointer          a pineapple into the yellow Pianta's basket from behind the third line
//  68 Durian Juggler           juggle a durian 10 times by jumping
//  69 Banana Backflip            bananas into the purple Pianta's basket, thrown in a backflip off the roof
//  70 Early Bird Gets the Coin              the mushroom Pianta's blue coin on the first visit
//  71 Hover to the Pins       into Pachinko from an unflooded plaza, no Rocket, Turbo, Yoshi or boat
//  72 Hover to the Village              into Pianta Village, no Rocket, Turbo or Yoshi
//  73 Hover to the Field      into the Red Coin Field from an unflooded plaza, no Rocket, Turbo or Yoshi
//  74 Bell Ringer, West                the West Bell Shine with only the spray
//  75 Bell Ringer, East      the East Bell Shine with only Yoshi
//  76 Yoshi's Allowance             the 100-coin Shine on Yoshi in an unflooded plaza, no boat after it appears
//  77 Golden Bird, Turbo Style   the Gold Bird Shine in an unflooded plaza, no Rocket, Hover or Yoshi
//  78 Never Touch the Ground               the Super Slide's Shine without touching the ground past the start
//  79 Track Without Turbo   the Turbo Track's Shine without the Rocket or Turbo
//
// Each challenge starts over with every visit (stageSetup) and fails for the
// rest of it once the player does what it forbids.
#include "achievements.h"

#include "port_ext.h"

#include <Animal/BirdNerve.hpp>
#include <Enemy/Conductor.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <MoveBG/Item.hpp>
#include <MoveBG/ItemManager.hpp>
#include <MoveBG/MapObjBall.hpp>
#include <MoveBG/MapObjGeneral.hpp>
#include <MoveBG/MapObjHide.hpp>
#include <Player/Mario.hpp>
#include <Player/MarioStatus.hpp>
#include <Player/WaterGun.hpp>
#include <System/Application.hpp>

namespace {

using namespace ach;

// Actor types (THitActor::mActorType)
const u32 POLE       = 0x4000002F;
const u32 PLAZA_BOAT = 0x4000007B;
const u32 COCONUT    = 0x40000390;
const u32 PINEAPPLE  = 0x40000392;
const u32 DURIAN     = 0x40000393;
const u32 BANANAS    = 0x40000394;

// Shines (their flag is 0x10000 + id; a TShine's event id)
const int SHINE_TURBO_TRACK = 0x57;
const int SHINE_SUPER_SLIDE = 0x5A;
const int SHINE_WEST_BELL   = 0x60;
const int SHINE_EAST_BELL   = 0x61;
const int SHINE_PLAZA_100   = 0x6B;
const int SHINE_GOLD_BIRD   = 0x76;

// The plaza's blue coin on the mushroom Pianta's back (0x10078 + its number)
const unsigned FLAG_MUSHROOM_PIANTA_COIN = 0x10078 + 4;

// The plaza's states (its scenario) that the challenges name
const int PLAZA_FIRST   = 0; // dolpic0, the first visit
const int PLAZA_CORONA  = 2; // dolpic10, after Corona Mountain
const int PLAZA_FLOODED = 9; // dolpic9

// TMovieDirector's movies: the staff roll, then the bonus picture of 120 Shines
const u32 MOVIE_STAFFROLL = 15;
const u32 MOVIE_OMAKE_B   = 17;

// Nozzles as bits, by TWaterGun::TNozzleType
const int SPRAY  = 1 << TWaterGun::Spray;
const int ROCKET = 1 << TWaterGun::Rocket;
const int HOVER  = 1 << TWaterGun::Hover;
const int TURBO  = 1 << TWaterGun::Turbo;

TMario* player(void* m)
{
	return m && m == (void*)ach::mario() ? (TMario*)m : nullptr;
}

// FLUDD is spraying with one of `nozzles` (the reference's "using" a nozzle).
bool usingNozzle(TMario* m, int nozzles)
{
	const TWaterGun* gun = m->mWaterGun;
	return gun && gun->mIsEmitWater && (nozzles & (1 << gun->mCurrentNozzle)) != 0;
}

// Mario stands on the ground (the reference: his height is the floor's).
bool grounded(TMario* m)
{
	return m->mPosition.y - m->mFloorPosition.y <= 0.5f;
}

bool onPlazaBoat(TMario* m)
{
	return m->unk2C0 && m->unk2C0->getActorType() == PLAZA_BOAT;
}

int actorType(const THitActor* a)
{
	return a ? (int)a->getActorType() : 0;
}

// --- 61: the ending ---

u32 s_movie;

void frame()
{
	if (!on())
		return;
	const u32 movie = SMSGetApplication()->getMovie();
	if (movie != s_movie && movie == MOVIE_OMAKE_B && s_movie == MOVIE_STAFFROLL)
		unlock(61);
	s_movie = movie;
}

// --- 65: the plaza's green birds ---

enum { MAX_BIRDS = 32, GREEN = 1, BIRDS_NEEDED = 17 };

TAnimalBird* s_birds[MAX_BIRDS];
u8 s_birdHp[MAX_BIRDS];
bool s_birdDown[MAX_BIRDS];
int s_birdCount, s_birdsDown;

void findBirds()
{
	s_birdCount = s_birdsDown = 0;
	if (!gpConductor)
		return;
	for (JGadget::TList<TLiveManager*>::iterator it = gpConductor->unk10.begin(); it != gpConductor->unk10.end(); ++it) {
		if (!dynamic_cast<TAnimalBirdManager*>(*it))
			continue;
		for (int i = 0; i < (*it)->getObjNum() && s_birdCount < MAX_BIRDS; ++i) {
			TAnimalBird* b = dynamic_cast<TAnimalBird*>((*it)->getObj(i));
			if (!b || b->mColorIndex != GREEN)
				continue;
			s_birdDown[s_birdCount] = false;
			s_birdHp[s_birdCount]   = b->mHitPoints;
			s_birds[s_birdCount++]  = b;
		}
	}
	if (debug())
		log("plaza: %d green birds", s_birdCount);
}

void watchBirds()
{
	if (episode() != PLAZA_FLOODED && episode() != PLAZA_CORONA)
		return;
	for (int i = 0; i < s_birdCount; ++i) {
		const u8 hp = s_birds[i]->mHitPoints;
		if (!s_birdDown[i] && s_birdHp[i] > 0 && hp == 0) {
			s_birdDown[i] = true;
			if (++s_birdsDown >= BIRDS_NEEDED)
				unlock(65);
		}
		s_birdHp[i] = hp;
	}
}

// --- 66, 67, 69: fruit thrown into a Pianta's basket ---

// The baskets the plaza's fruit Piantas count (TTalk2D2's names), with the
// fruit each counts (TFruitBasketEvent::getFruitNum's kind).
enum { BASKET_PURPLE, BASKET_ORANGE, BASKET_YELLOW, BASKETS };
const char* const kBasketNames[BASKETS] = { "フルーツかごＡ", "フルーツかごＢ", "フルーツかごＣ" };
const int kBasketFruit[BASKETS]         = { 0, 4, 3 }; // bananas, coconuts, pineapples

TFruitBasketEvent* s_baskets[BASKETS];
int s_basketCount[BASKETS]; // its fruit at the last tick

// One challenge's throw: the fruit it counts on, once thrown the way it asks.
struct Throw {
	TMapObjBase* fruit;
	int halted; // ticks the fruit hasn't moved up or down (it landed)
};

Throw s_coconut, s_pineapple, s_bananas;
bool s_terminal;    // 66: a ground pound reached full speed since Mario last left the ground
TTakeActor* s_held; // what Mario held at the last tick
u16 s_statusState;  // his mStatusState at the last tick
bool s_wasGrounded;

void findBaskets()
{
	for (int i = 0; i < BASKETS; ++i) {
		s_baskets[i]     = dynamic_cast<TFruitBasketEvent*>(JDrama::TNameRefGen::search2(kBasketNames[i]));
		s_basketCount[i] = s_baskets[i] ? s_baskets[i]->getFruitNum(kBasketFruit[i]) : 0;
	}
	if (debug())
		log("plaza: fruit baskets %s %s %s", s_baskets[0] ? "A" : "-", s_baskets[1] ? "B" : "-",
		    s_baskets[2] ? "C" : "-");
}

// The basket counted another of its fruit since the last tick (read before
// the counts are brought up to date).
bool basketScored(int basket)
{
	return s_baskets[basket] && s_baskets[basket]->getFruitNum(kBasketFruit[basket]) > s_basketCount[basket];
}

// The thrown fruit went into `basket` this tick: it is gone (waiting to
// appear again) as the basket counts it.
bool scored(const Throw& t, int basket)
{
	return t.fruit && t.fruit->mState == TMapObjGeneral::STATE_WAITING_TO_APPEAR && basketScored(basket);
}

// The fruit came to rest (on the ground, not in a basket): the throw missed.
bool landed(Throw& t)
{
	if (!t.fruit || t.fruit->mState == TMapObjGeneral::STATE_WAITING_TO_APPEAR)
		return false;
	t.halted = t.fruit->mLinearVelocity.y == 0.0f ? t.halted + 1 : 0;
	return t.halted >= 2;
}

void startThrow(Throw& t, TTakeActor* fruit)
{
	t.fruit  = static_cast<TMapObjBase*>(fruit);
	t.halted = 0;
}

void watchThrows(TMario* m)
{
	TTakeActor* held       = m->mHeldObject;
	TTakeActor* released   = s_held && !held ? s_held : nullptr;
	const bool air         = !grounded(m);
	const bool leftGround  = s_wasGrounded && air;
	const bool grabbing    = m->unk384 != nullptr;
	const int releasedType = actorType(released);

	// what scored this tick counts before what failed it
	if (scored(s_coconut, BASKET_ORANGE))
		unlock(66);
	if (scored(s_pineapple, BASKET_YELLOW))
		unlock(67);
	if (scored(s_bananas, BASKET_PURPLE))
		unlock(69);

	// 66: a ground pound at full speed, then the coconut let go in the air
	if (leftGround || grabbing) {
		s_terminal      = false;
		s_coconut.fruit = nullptr;
	}
	if (m->mStatus == MARIO_STATUS_HIP_DROP && s_statusState == 2 && m->mStatusState == 3)
		s_terminal = true;
	if (s_coconut.fruit && !held && landed(s_coconut))
		s_coconut.fruit = nullptr;
	if (!s_coconut.fruit && s_terminal && air && releasedType == (int)COCONUT)
		startThrow(s_coconut, released);

	// 67: the pineapple let go from behind the third line
	if (grabbing)
		s_pineapple.fruit = nullptr;
	if (s_pineapple.fruit && landed(s_pineapple))
		s_pineapple.fruit = nullptr;
	if (!s_pineapple.fruit && releasedType == (int)PINEAPPLE && m->mPosition.z >= -5175.0f)
		startThrow(s_pineapple, released);

	// 69: the bananas (picked as the backflip's throw starts, playerStatusChanged)
	if (grabbing)
		s_bananas.fruit = nullptr;
	if (s_bananas.fruit && !held && landed(s_bananas))
		s_bananas.fruit = nullptr;

	for (int i = 0; i < BASKETS; ++i)
		if (s_baskets[i])
			s_basketCount[i] = s_baskets[i]->getFruitNum(kBasketFruit[i]);
	s_held        = held;
	s_statusState = m->mStatusState;
	s_wasGrounded = !air;
}

void playerStatusChanged(void* mario, unsigned from, unsigned to, unsigned)
{
	TMario* m = player(mario);
	if (!on() || !m || !playing() || area() != PLAZA)
		return;
	// 69: a throw out of a backflip, up on the roof, with bananas in hand
	if (from == MARIO_STATUS_BACK_JUMP && to == MARIO_STATUS_JUMP_THROW && m->mPosition.y > 1800.0f
	    && actorType(m->mHeldObject) == (int)BANANAS && !s_bananas.fruit)
		startThrow(s_bananas, m->mHeldObject);
}

// --- 68: juggling a durian ---

enum { MAX_DURIANS = 8, JUGGLES = 10 };

TMapObjBase* s_durians[MAX_DURIANS];
f32 s_durianRise[MAX_DURIANS]; // its vertical speed at the last tick
bool s_durianHit[MAX_DURIANS]; // Mario was knocking it up at the last tick
int s_juggles[MAX_DURIANS];
int s_durianCount;

void findDurians()
{
	s_durianCount = 0;
	if (!gpItemManager)
		return;
	for (int i = 0; i < gpItemManager->getObjNum() && s_durianCount < MAX_DURIANS; ++i) {
		TLiveActor* a = gpItemManager->getObj(i);
		if (actorType(a) != (int)DURIAN)
			continue;
		TMapObjBase* d              = static_cast<TMapObjBase*>(a);
		s_durianRise[s_durianCount] = d->mLinearVelocity.y;
		s_durianHit[s_durianCount]  = false;
		s_juggles[s_durianCount]    = 0;
		s_durians[s_durianCount++]  = d;
	}
	if (debug())
		log("plaza: %d durians", s_durianCount);
}

// Each time Mario, jumping, knocks a durian up counts; its count starts over
// when it touches the ground.
void watchDurians(TMario* m)
{
	const bool bumped = !grounded(m) && m->mFreezeTimer != 0;
	for (int i = 0; i < s_durianCount; ++i) {
		TMapObjBase* d = s_durians[i];
		const f32 rise = d->mLinearVelocity.y;
		const bool hit = bumped && rise > s_durianRise[i];
		if (d->mPosition.y - d->mGroundHeight <= 5.0f)
			s_juggles[i] = 0;
		else if (hit && !s_durianHit[i] && ++s_juggles[i] >= JUGGLES)
			unlock(68);
		s_durianHit[i]  = hit;
		s_durianRise[i] = rise;
	}
}

// --- 71 to 79: the plaza's and its secrets' nozzle challenges ---

bool s_pachinko, s_village, s_coinField; // 71, 72, 73: still on
bool s_westBell, s_eastBell;             // 74, 75
bool s_plaza100, s_goldBird;             // 76, 77: their Shine appeared, still on
bool s_slide, s_turboTrack;              // 78, 79

void watchPlazaNozzles(TMario* m)
{
	const bool yoshi = m->onYoshi();
	if (usingNozzle(m, ROCKET | TURBO) || yoshi)
		s_pachinko = s_village = s_coinField = false;
	if (onPlazaBoat(m))
		s_pachinko = s_plaza100 = false;
	if (usingNozzle(m, ROCKET | HOVER | TURBO) || yoshi)
		s_westBell = false;
	// only Yoshi: no nozzle of FLUDD's own, his spray included (on Yoshi it is Yoshi's juice)
	if (usingNozzle(m, SPRAY | ROCKET | HOVER | TURBO))
		s_eastBell = false;
	if (usingNozzle(m, ROCKET | HOVER) || yoshi)
		s_goldBird = false;
}

// --- the hooks ---

bool s_flagpoleDone;

void stageSetup()
{
	if (!on())
		return;
	const int here   = area();
	const int prev   = SMSGetApplication()->mPrevArea.getStage();
	const bool plaza = here == PLAZA;

	s_birdCount = s_birdsDown = 0;
	s_durianCount             = 0;
	for (int i = 0; i < BASKETS; ++i)
		s_baskets[i] = nullptr;
	s_coconut.fruit = s_pineapple.fruit = s_bananas.fruit = nullptr;
	s_terminal    = false;
	s_held        = nullptr;
	s_statusState = 0;
	s_wasGrounded = false;
	if (plaza) {
		findBirds();
		findBaskets();
		findDurians();
		const int e = episode();
		if (e == PLAZA_FLOODED || e == PLAZA_CORONA)
			require(65, "17 green birds", s_birdCount >= BIRDS_NEEDED);
		if (e == 5 || e == 6 || e == 7 || e == 8 || e == PLAZA_CORONA) {
			require(66, "the coconut basket", s_baskets[BASKET_ORANGE] != nullptr);
			require(67, "the pineapple basket", s_baskets[BASKET_YELLOW] != nullptr);
			require(69, "the banana basket", s_baskets[BASKET_PURPLE] != nullptr);
			require(68, "a durian", s_durianCount > 0);
		}
	}

	s_pachinko  = plaza && episode() != PLAZA_FLOODED;
	s_coinField = s_pachinko;
	s_village   = plaza && prev != PIANTA && prev != VILLAGE_UNDERSIDE;
	s_westBell = s_eastBell = plaza;
	s_plaza100 = s_goldBird = false;
	s_slide                 = here == SUPER_SLIDE;
	s_turboTrack            = here == TURBO_TRACK;
	s_flagpoleDone          = false;
}

void afterMovement()
{
	TMario* m = ach::mario();
	if (!on() || !playing() || !m)
		return;
	switch (area()) {
	case AIRSTRIP:
	case PLAZA_AIRSTRIP:
		// 62: on the flagpole, at its top
		if (!s_flagpoleDone && actorType(m->mHolder) == (int)POLE && m->mPosition.y >= 3900.0f) {
			s_flagpoleDone = true;
			unlock(62);
		}
		break;
	case PLAZA:
		watchBirds();
		watchThrows(m);
		watchDurians(m);
		watchPlazaNozzles(m);
		break;
	case SUPER_SLIDE:
		// 78: on the ground past the start (the reference: the last tick's footing, this tick's place)
		if (s_wasGrounded && m->mPosition.z >= -16500.0f)
			s_slide = false;
		s_wasGrounded = grounded(m);
		break;
	case TURBO_TRACK:
		if (usingNozzle(m, ROCKET | TURBO))
			s_turboTrack = false;
		break;
	}
}

// 71 to 73: the plaza is left for the course the challenge asks for.
void moveStage()
{
	if (!on() || !playing() || area() != PLAZA)
		return;
	switch (SMSGetApplication()->mNextArea.getStage()) {
	case PACHINKO:
		if (s_pachinko)
			unlock(71);
		break;
	case PIANTA:
		if (s_village)
			unlock(72);
		break;
	case RED_COIN_FIELD:
		if (s_coinField)
			unlock(73);
		break;
	}
}

void shine(void* p, int what)
{
	if (!on() || !p || !playing())
		return;
	const int id = (int)static_cast<TShine*>((THitActor*)p)->getEventId();
	TMario* m    = ach::mario();
	if (what == PORT_SHINE_APPEAR) {
		// 76, 77 time the Shine from its appearing to its taking
		if (area() == PLAZA && episode() != PLAZA_FLOODED) {
			if (id == SHINE_PLAZA_100)
				s_plaza100 = true;
			if (id == SHINE_GOLD_BIRD)
				s_goldBird = true;
		}
		return;
	}
	if (id == SHINE_PLAZA_100 && s_plaza100 && area() == PLAZA && m && m->onYoshi())
		unlock(76);
	if (id == SHINE_GOLD_BIRD && s_goldBird && area() == PLAZA)
		unlock(77);
	if (id == SHINE_SUPER_SLIDE && s_slide && area() == SUPER_SLIDE)
		unlock(78);
	if (id == SHINE_TURBO_TRACK && s_turboTrack && area() == TURBO_TRACK)
		unlock(79);
}

// 74, 75: the bells' Shines, the first time (their flag is set as they are taken).
void shineCollected(int shine, int)
{
	if (!on() || !playing() || area() != PLAZA)
		return;
	if (shine == SHINE_WEST_BELL && s_westBell)
		unlock(74);
	if (shine == SHINE_EAST_BELL && s_eastBell)
		unlock(75);
}

// 70: the mushroom Pianta's blue coin, taken on the plaza's first visit.
int flagChange(unsigned flag, int old, int* value)
{
	if (on() && flag == FLAG_MUSHROOM_PIANTA_COIN && old == 0 && *value != 0 && playing() && area() == PLAZA
	    && episode() == PLAZA_FIRST)
		unlock(70);
	return 0;
}

PortExtension s_extension;

struct Register {
	Register()
	{
		s_extension.name                = "achievements-plaza";
		s_extension.frame               = frame;
		s_extension.stageSetup          = stageSetup;
		s_extension.afterMovement       = afterMovement;
		s_extension.moveStage           = moveStage;
		s_extension.shine               = shine;
		s_extension.shineCollected      = shineCollected;
		s_extension.flagChange          = flagChange;
		s_extension.playerStatusChanged = playerStatusChanged;
		port_ext_register(&s_extension);
	}
} s_register;

} // namespace
