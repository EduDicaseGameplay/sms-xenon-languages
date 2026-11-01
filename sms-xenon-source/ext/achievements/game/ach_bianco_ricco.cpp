// Achievements (ext/achievements/README.md): Bianco Hills' and Ricco
// Harbor's challenges.
//
//  83 Wash and Wave: talk to the cleaned brown-hat Pianta in
//     Bianco Hills' episodes 3 to 8, in one session
//  84 Ahead of Schedule: take episode 2's Shine (Petey) from episode 1
//  85 No Windmill Shortcuts: reach Petey's arena in episode 1 or 2 without the
//     windmill's platforms or the Hover Nozzle
//  86 Feet on the Floor: the Hillside Cave Secret's Shine without jumping or
//     F.L.U.D.D.
//  87 Nap Time: Mario asleep beside a sleeping Petey in episode 5
//  88 Dry Run: into the Secret of the Dirty Lake in episode 6 without
//     swimming or F.L.U.D.D.
//  89 Steady Footing: the Dirty Lake Secret's Shine without more than a
//     second on its moving platforms or Rocket Storage
//  93 Purple Racer: Blooper Surfing Safari's Shine on the purple Blooper
//  94 Ground Floor: Ricco's episode 3 without the high metal
//     scaffolding or the Rocket Nozzle
//  95 No Water Needed: into the Secret of Ricco Tower in episode 4
//     without F.L.U.D.D.
//  96 Turbo All the Way: the Ricco Tower Secret's Shine on the
//     Turbo Nozzle, never let go nor out of water past the start
//  97 Catch and Release: Gooper Blooper beaten in episode 5 with no tentacle
//     ripped off
//  98 Juice Run: Ricco's episode 8 without Yoshi on grass or metal,
//     nor the Rocket Nozzle
//
// Each attempt starts as its stage loads and ends with it (a lost life loads
// the stage again), as the reference set's "never Loading" resets do; 84
// lasts from Bianco's episode 1 while Mario stays there or in Petey's arena,
// and 83 for the whole session. Movement ticks run at 120 a second, so "a second" is 120.
#include "achievements.h"

#include "port_ext.h"

#include <Enemy/BossGesso.hpp>
#include <Enemy/BossPakkun.hpp>
#include <Enemy/Conductor.hpp>
#include <Enemy/EnemyManager.hpp>
#include <JSystem/JDrama/JDRNameRef.hpp>
#include <Map/MapData.hpp>
#include <MoveBG/Item.hpp>
#include <NPC/NpcBase.hpp>
#include <Player/Mario.hpp>
#include <Player/MarioFlags.hpp>
#include <Player/MarioStatus.hpp>
#include <Player/WaterGun.hpp>
#include <System/Application.hpp>

namespace {

using namespace ach;

// The reference set's ids: Shines (TShine's event id), actor types of what
// Mario rides, NPC key codes, the ground's sound kind (TBGCheckData::unk6).
const unsigned SHINE_BIANCO_2 = 0x01, SHINE_BIANCO_3 = 0x02, SHINE_BIANCO_6 = 0x05;
const unsigned SHINE_RICCO_3 = 0x0c, SHINE_RICCO_8 = 0x11;
const int ACTOR_BIANCO_WINDMILL  = 0x400000a8;
const int ACTOR_MOVING_PLATFORM  = 0x400002bd;
const u16 KEY_DIRTY_PIANTA       = 0x8f7b;
const int GROUND_METAL           = 0x0c;
const int GROUND_GRASS           = 0x10;
const int GESSO_BCK_TENTACLE_HIT = 3; // TNerveBGTentacleDamage's animation

const float SCAFFOLD_Y        = 3000.0f;   // 94: the metal above this is the scaffolding
const float TOWER_START_X     = -6500.0f;  // 96: past the starting area
const float ROCKET_STORAGE_VY = 170.0f;    // 89: faster up than any plain launch
const float PETEY_NEAR_SQ     = 100000.0f; // 87: Petey's squared distance to Mario
const int TICKS_PER_SECOND    = 120;

// 83, 84: across stage loads
unsigned s_showerEpisodes; // Bianco episodes (bit per scenario) the Pianta was talked to in
bool s_biancoOne;          // in Bianco's episode 1 since the plaza or the title

// this visit's attempts, each true while it still counts
bool s_spinCycle, s_cubeShuffle, s_dryClean, s_upUpAway, s_rodeBlooper, s_fromBottom, s_anyWay, s_humane,
	s_powerJuice;
int s_movingTicks;   // 89: ticks on the moving platforms
bool s_turboRun;     // 96: past the start on the Turbo Nozzle
int s_letGoTicks;    // 96: ticks in a row without F.L.U.D.D.
TBossPakkun* s_petey; // 87
TBossGesso* s_gooper; // 97
int s_gooperHp;
bool s_havePrev; // last tick's Mario, for 86's jumps
float s_prevVy;
u16 s_prevAnim;

bool in(int a, int scenario)
{
	return area() == a && episode() == scenario;
}

// F.L.U.D.D. pumped (R held, the reference's F.L.U.D.D. state 0).
bool usingFludd(const TMario* m)
{
	return m->mUpperState == TMario::UPPER_STATE_PUMPING;
}

// `nozzle` (TWaterGun::TNozzleType) is on and spraying.
bool usingNozzle(const TMario* m, int nozzle)
{
	const TWaterGun* g = m->mWaterGun;
	return g && g->mCurrentNozzle == nozzle && g->mIsEmitWater;
}

// Standing on ground of sound kind `kind` (on it, not just above it).
bool onGround(const TMario* m, int kind)
{
	return m->mGroundPlane && m->mGroundPlane->unk6 == kind && m->mPosition.y == m->mFloorPosition.y;
}

// The actor Mario rides is of type `type`.
bool riding(const TMario* m, int type)
{
	return m->unk2C0 && m->unk2C0->getActorTypeID() == type;
}

bool swimming(const TMario* m)
{
	return m->mStatus >= MARIO_STATUS_SWIM_START && m->mStatus <= MARIO_STATUS_SWIM_DIVE;
}

void fail(bool* attempt, int id, const char* why)
{
	if (*attempt && debug())
		log("%d: attempt over (%s)", id, why);
	*attempt = false;
}

// The stage's Petey and Gooper Blooper, if it has them.
void findBosses()
{
	if (!gpConductor)
		return;
	for (JGadget::TList<TEnemyManager*>::iterator it = gpConductor->unk20.begin(); it != gpConductor->unk20.end();
	     ++it) {
		TEnemyManager* mgr = *it;
		if (!mgr || !mgr->getObjNum())
			continue;
		TLiveActor* first = mgr->getObj(0);
		if (TBossPakkun* p = dynamic_cast<TBossPakkun*>(first)) {
			TBossPakkunManager* pm = dynamic_cast<TBossPakkunManager*>(mgr);
			if (!s_petey || (pm && !pm->mIsLightVersion))
				s_petey = p;
		} else if (TBossGesso* g = dynamic_cast<TBossGesso*>(first)) {
			if (!s_gooper)
				s_gooper = g;
		}
	}
}

void stageSetup()
{
	if (!on())
		return;
	const int a = area(), e = episode();
	// Petey's arena is the one place episode 1 leads to (the reference ends
	// the attempt at the plaza or the title; nowhere else is reachable)
	s_biancoOne = (a == BIANCO && e == 0) || (a == PETEY_FIGHT && s_biancoOne);

	s_spinCycle   = a == BIANCO && (e == 0 || e == 1);
	s_cubeShuffle = a == HILLSIDE_CAVE;
	s_dryClean    = in(BIANCO, 5);
	s_upUpAway    = a == DIRTY_LAKE_SECRET;
	s_movingTicks = 0;
	s_rodeBlooper = false;
	s_fromBottom  = in(RICCO, 2);
	s_anyWay      = in(RICCO, 3);
	s_turboRun    = false;
	s_letGoTicks  = 0;
	s_humane      = in(RICCO, 4);
	s_powerJuice  = in(RICCO, 7);
	s_havePrev    = false;
	s_petey       = nullptr;
	s_gooper      = nullptr;
	if (in(BIANCO, 4) || in(RICCO, 4))
		findBosses();
	s_gooperHp = s_gooper ? s_gooper->mHitPoints : 0;
	if (in(BIANCO, 4))
		require(87, "Petey Piranha", s_petey != nullptr);
	if (in(RICCO, 4))
		require(97, "Gooper Blooper", s_gooper != nullptr);
}

void afterMovement()
{
	if (!on() || !playing())
		return;
	TMario* m = mario();
	const int a = area();

	// 85: no windmill platform, no Hover Nozzle
	if (s_spinCycle && (riding(m, ACTOR_BIANCO_WINDMILL) || usingNozzle(m, TWaterGun::Hover)))
		fail(&s_spinCycle, 85, "windmill platform or Hover Nozzle");

	// 86: no jump (up from a standstill, a slide's hop aside), no F.L.U.D.D.
	if (s_cubeShuffle) {
		const bool jumped = s_havePrev && s_prevVy == 0.0f && m->mVel.y > 0.0f && s_prevAnim != TMario::ANIM_SLDCT
		                 && m->mAnimationId != TMario::ANIM_SLDCT;
		if (jumped || usingFludd(m))
			fail(&s_cubeShuffle, 86, jumped ? "jumped" : "F.L.U.D.D.");
	}

	// 87: both asleep, Mario close by
	if (s_petey && in(BIANCO, 4) && m->mStatus == MARIO_STATUS_SLEEP && s_petey->mMActor
	    && s_petey->mMActor->checkCurBckFromIndex(BOSSPAKU_BCK_SLEEP)) {
		const float dx = s_petey->mPosition.x - m->mPosition.x, dy = s_petey->mPosition.y - m->mPosition.y,
		            dz = s_petey->mPosition.z - m->mPosition.z;
		if (dx * dx + dy * dy + dz * dz < PETEY_NEAR_SQ)
			unlock(87);
	}

	// 88: no swimming, no F.L.U.D.D.
	if (s_dryClean && (swimming(m) || usingFludd(m)))
		fail(&s_dryClean, 88, "swam or F.L.U.D.D.");

	// 89: a second on moving platforms all told, or a Rocket Storage launch
	if (s_upUpAway) {
		if (riding(m, ACTOR_MOVING_PLATFORM))
			++s_movingTicks;
		if (s_movingTicks > TICKS_PER_SECOND || m->mVel.y >= ROCKET_STORAGE_VY)
			fail(&s_upUpAway, 89, s_movingTicks > TICKS_PER_SECOND ? "a second on moving platforms" : "Rocket Storage");
	}

	// 94: no metal up on the scaffolding, no Rocket Nozzle
	if (s_fromBottom
	    && ((m->mPosition.y >= SCAFFOLD_Y && onGround(m, GROUND_METAL)) || usingNozzle(m, TWaterGun::Rocket)))
		fail(&s_fromBottom, 94, "scaffolding or Rocket Nozzle");

	// 95: no F.L.U.D.D.
	if (s_anyWay && usingFludd(m))
		fail(&s_anyWay, 95, "F.L.U.D.D.");

	// 96: past the start on the Turbo Nozzle, then never let go (a blip of
	// one tick is allowed) nor run dry
	if (a == RICCO_TOWER_SECRET) {
		if (s_turboRun) {
			const TWaterGun* g = m->mWaterGun;
			s_letGoTicks = usingFludd(m) && g && g->mCurrentNozzle == TWaterGun::Turbo ? 0 : s_letGoTicks + 1;
			if (s_letGoTicks > 1 || !g || !g->hasWater())
				fail(&s_turboRun, 96, s_letGoTicks > 1 ? "let go of the Turbo Nozzle" : "out of water");
		} else if (m->mPosition.x < TOWER_START_X && usingNozzle(m, TWaterGun::Turbo)) {
			s_turboRun   = true;
			s_letGoTicks = 0;
			if (debug())
				log("96: past the start on the Turbo Nozzle");
		}
	}

	// 97: no tentacle ripped off; he is beaten as his health reaches 0
	if (s_gooper && in(RICCO, 4)) {
		if (s_gooper->getLatestNerve() == &TNerveBGTentacleDamage::theNerve()
		    || (s_gooper->mMActor && s_gooper->mMActor->checkCurBckFromIndex(GESSO_BCK_TENTACLE_HIT)))
			fail(&s_humane, 97, "a tentacle ripped off");
		const int hp = s_gooper->mHitPoints;
		if (s_gooperHp > 0 && hp == 0 && s_humane)
			unlock(97);
		s_gooperHp = hp;
	}

	// 98: no grass or metal under Yoshi, no Rocket Nozzle
	if (s_powerJuice
	    && ((m->onYoshi() && (onGround(m, GROUND_METAL) || onGround(m, GROUND_GRASS)))
	        || usingNozzle(m, TWaterGun::Rocket)))
		fail(&s_powerJuice, 98, "Yoshi on grass or metal, or Rocket Nozzle");

	s_havePrev = true;
	s_prevVy   = m->mVel.y;
	s_prevAnim = m->mAnimationId;
}

// 93: on a Blooper at all this visit (his colour is read as the Shine appears)
void playerStatusChanged(void* who, unsigned from, unsigned to, unsigned arg)
{
	(void)from, (void)arg;
	if (on() && who == mario() && to == MARIO_STATUS_SURF && area() == BLOOPER_SURFING)
		s_rodeBlooper = true;
}

void shine(void* actor, int what)
{
	if (!on() || !playing())
		return;
	TMario* m = mario();
	const int a = area();
	if (what == PORT_SHINE_APPEAR) {
		// 93: the race's Shine, the purple Blooper ridden
		if (a == BLOOPER_SURFING && s_rodeBlooper && m->mSurfGessoType == TMario::SURF_GESSO_TYPE_RED)
			unlock(93);
		return;
	}
	if (what != PORT_SHINE_GET)
		return;
	const unsigned id = static_cast<TShine*>(static_cast<THitActor*>(actor))->mEventId;
	if (id == SHINE_BIANCO_2 && s_biancoOne)
		unlock(84);
	if (a == HILLSIDE_CAVE && id == SHINE_BIANCO_3 && s_cubeShuffle)
		unlock(86);
	if (a == DIRTY_LAKE_SECRET && id == SHINE_BIANCO_6 && s_upUpAway && m->checkFlag(MARIO_FLAG_HAS_FLUDD))
		unlock(89);
	if (in(RICCO, 2) && id == SHINE_RICCO_3 && s_fromBottom)
		unlock(94);
	// 96: the secret has the one Shine
	if (a == RICCO_TOWER_SECRET && s_turboRun)
		unlock(96);
	if (in(RICCO, 7) && id == SHINE_RICCO_8 && s_powerJuice)
		unlock(98);
}

// 85, 88, 95: the warp into the secret or the arena
void moveStage()
{
	if (!on() || !playing())
		return;
	const int next = SMSGetApplication()->mNextArea.getStage();
	if (next == PETEY_FIGHT && s_spinCycle)
		unlock(85);
	if (next == DIRTY_LAKE_SECRET && s_dryClean)
		unlock(88);
	if (next == RICCO_TOWER_SECRET && s_anyWay)
		unlock(95);
}

// 83: a talk with the brown-hat Pianta, cleaned, in Bianco's episodes 3-8
void talk(int what, void* npc, unsigned* message, unsigned flags)
{
	(void)message, (void)flags;
	if (what != PORT_TALK_START || !npc || !on() || tainted() || area() != BIANCO)
		return;
	const int e = episode();
	TBaseNPC* p = static_cast<TBaseNPC*>(npc);
	if (e < 2 || e > 7 || !p->getName() || JDrama::TNameRef::calcKeyCode(p->getName()) != KEY_DIRTY_PIANTA
	    || !p->isClean())
		return;
	s_showerEpisodes |= 1u << e;
	if (debug())
		log("83: the Pianta in episode %d (%#x of 0xfc)", e + 1, s_showerEpisodes);
	if (s_showerEpisodes == 0xfcu)
		unlock(83);
}

// 84: a file chosen starts over
void afterFileSelect()
{
	s_biancoOne = false;
}

PortExtension s_extension;

struct Register {
	Register()
	{
		s_extension.name                = "achievements-bianco-ricco";
		s_extension.stageSetup          = stageSetup;
		s_extension.afterFileSelect     = afterFileSelect;
		s_extension.afterMovement       = afterMovement;
		s_extension.playerStatusChanged = playerStatusChanged;
		s_extension.shine               = shine;
		s_extension.moveStage           = moveStage;
		s_extension.talk                = talk;
		port_ext_register(&s_extension);
	}
} s_register;

} // namespace
