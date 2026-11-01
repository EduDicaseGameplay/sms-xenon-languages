#ifndef MP_MIRROR_HPP
#define MP_MIRROR_HPP

// Mirrored objects (mp_mirror.cpp): things whose state the game changes on
// its own (a boat's drift, a wheel's turn, a slot's reels, an egg's wish,
// where an NPC has walked) are run by one game, which sends their state; the
// others take it. The owner is whoever rides or last used the object, else
// the host.

// Client: the host's game is paused (mp_game.cpp, from its pose): its world
// stands still, so this game's runs on its own until the host plays again.
bool MpHostPaused();
// This game is paused: what it runs stands still, and is not sent.
bool MpPausedHere();

// Host: a client's report of an enemy its game runs (mp_game.cpp; it comes
// on the mirror channel as manager 253, index manager << 8 | index).
void MpEnemyReport(int slot, int key, const void* data, int size);

// Debug (SMS_MULTIPLAYER_MOVERS=1): `stageFrame` frames into play, the
// stage's objects that moved by themselves, and how each kind is shared.
void MpMirrorSurvey(int stageFrame);

// Boats every player has their own of (Noki Bay's mud boats, Bianco Hills'
// leaf boats): as the stage loads (its memory current), a copy of each kind
// for each other player, drawn under them as they ride theirs.
void MpMirrorPrepareRides();
// The boat `slot` rides in their game, placed under their puppet's root at
// `root` (where it is drawn, so the two move as one); null when they ride none
// (or ride where this game's own of that boat is: that one is under them).
class MActor;
MActor* MpMirrorRide(int slot, const float root[3]);

// A stage is in play: find its mirrored objects.
void MpMirrorCollect();
// A stage is loading: the last stage's objects are gone (none is touched again).
void MpMirrorForget();
// Every tick after movement: send what this game owns, take the rest.
void MpMirrorTick();
// This game's player used `object` (sprayed it, say): this game runs it for a while.
void MpMirrorUsed(const void* object);
// Whether the others' hits must not land on `object` here (its owner runs
// it, and sends the outcome: a slot machine's draw, for one).
bool MpMirrorKeepsHits(const void* object);
// Whether this game's Mario stands on Bowser's hot tub (the host tips it under him).
bool MpMirrorOnTub();
// Test aid (turbodoor@N): breaks down this game's first Turbo nozzle door, as a dash into it would.
bool MpMirrorTestTurboDoor();
// Test aid (rideboat@N[:S]): puts this game's Mario on the nearest mud or
// leaf boat and drives it on for S seconds (10; 0: stands on it), as his water would.
bool MpMirrorTestRide(int seconds);
// Test aid: feeds this game's Yoshi's egg the fruit it wants (it hatches).
bool MpMirrorTestHatch();
// Test aid: puts this game's Mario by the edge of Bowser's tub (his weight tips it).
bool MpMirrorTestTubEdge();
// Test aid: claims the first NPC (as talking to it does) and walks it off.
bool MpMirrorTestClaimNpc();
// Test aid: rolls the big watermelon along (this game's Mario against it); `frame` counts from 0.
bool MpMirrorTestRollMelon(int frame);
// Test aid: drops this game's Mario onto the first sand block.
bool MpMirrorTestSandBlock();
// Test aid (manhole@N[:1]): this game's Mario ground pounds into the nearest
// manhole lid (:1 stands beside it); `frame` counts from 0, true once done.
bool MpMirrorTestManhole(int frame, bool watch);
// Test aid: stands this game's Mario before the first NPC, facing it; true once a talk is on.
bool MpMirrorTestNearNpc();

#endif
