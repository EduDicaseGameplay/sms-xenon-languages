#ifndef MP_BOSS_HPP
#define MP_BOSS_HPP

// Bosses shared in multiplayer (mp_boss.cpp): the host's boss is the real
// one; the others' copies follow it, and the hits they land count on it.

// A stage's objects are set up: find its boss, if multiplayer knows it.
void MpBossStageSetup();
// Host, every frame in play: send the boss's state, take the others' hits.
void MpBossHostFrame();
// Client, every tick after movement: follow the host's boss, report hits.
void MpBossClientTick();
// Test aid: a hit by this game's player on its copy; the boss's kind, 0 if none.
int MpBossTestHit();
// Whether `object` (a TLiveActor) sets up its collision around Mario alone (Bowser's hot tub
// and its corners: TBathtub::setupCollisions_), so must move by this game's
// own player, not another player near it (mp_game.cpp's objectMove).
bool MpBossBuildsAroundMario(const void* object);
// Test aid (tubmiddle@N): where the middle of Bowser's tub is; false if it is not here.
bool MpBossTestTubMiddle(float at[3]);
// Whether `actor` (a TLiveActor) is a boss shared here.
bool MpBossIsBoss(const void* actor);
// Test aid: a spray on this game's Shadow Mario; his kind, 0 if he is not here.
int MpBossTestShadow();
// Test aid: where this game's first Gatekeeper still up is; false if none.
bool MpBossTestGatePos(float at[3]);
// Test aid (mantaout@N): Phantamanta comes out near this game's player (its fight begun).
bool MpBossTestMantaOut();
// Test aid: Gooper Blooper's first tentacle torn off this game's own way
// (TBGTentacle::checkDamage: in his first fight, its movie takes the stage
// on); 1 if he is here.
int MpBossTestTear();
// Test aid: this game's Gooper Blooper down (his beak pulled at his last
// hit point); 1 if he is here.
int MpBossTestDown();
// Test aid: Shadow Mario down (step 0), vanishing to run away (1), back on his node (2).
int MpBossTestShadowRun(int step);

#endif
