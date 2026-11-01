#ifndef SMS_EXT_ACHIEVEMENTS_H
#define SMS_EXT_ACHIEVEMENTS_H
// Achievements (ext/achievements/README.md): the game half's shared helpers.
// Each part (ach_flags.cpp, ach_oneups.cpp, the stages' challenge files)
// registers its own PortExtension and calls ach::unlock when the player has
// done what an achievement asks. Achievement ids are the Core list's numbers,
// 1 to 148 (ext/achievements/achievements.json has their names).

class TMario;

namespace ach {

enum { COUNT = 148, ECLIPSE_COUNT = 128 };

// The game's areas (TGameSequence's stage, gpApplication.mCurrArea), as the
// achievements name them.
enum Area {
	AIRSTRIP           = 0x00,
	PLAZA              = 0x01,
	BIANCO             = 0x02,
	RICCO              = 0x03,
	GELATO             = 0x04,
	PINNA_BEACH        = 0x05,
	SIRENA_BEACH       = 0x06,
	SIRENA_HOTEL       = 0x07,
	PIANTA             = 0x08,
	NOKI               = 0x09,
	PINNA_PARK         = 0x0D,
	CASINO             = 0x0E,
	TITLE              = 0x0F, // the title screen, and the ending
	NOKI_RED_COIN_FISH = 0x10,
	PLAZA_AIRSTRIP     = 0x14, // the airstrip as the plaza reaches it
	SUPER_SLIDE        = 0x15,
	PACHINKO           = 0x16,
	RED_COIN_FIELD     = 0x17,
	LILY_PAD_RIDE      = 0x18,
	TURBO_TRACK        = 0x1D,
	BLOOPER_SURFING    = 0x1E,
	SHELL_SECRET       = 0x1F, // Noki Bay
	SAND_CASTLE_SECRET = 0x20, // Gelato Beach
	SAND_BIRD          = 0x21,
	CASINO_SECRET      = 0x28,
	YOSHI_GO_ROUND     = 0x29, // Pinna Park's secret
	VILLAGE_UNDERSIDE  = 0x2A,
	RED_COINS_BOTTLE   = 0x2C, // Noki Bay
	DIRTY_LAKE_SECRET  = 0x2E,
	HILLSIDE_CAVE      = 0x2F,
	RICCO_TOWER_SECRET = 0x30,
	BEACH_CANNON       = 0x32, // Pinna Park's secret
	HOTEL_LOBBY_SECRET = 0x33,
	CORONA             = 0x34,
	PETEY_FIGHT        = 0x37,
	KING_BOO_FIGHT     = 0x38,
	EELY_MOUTH_FIGHT   = 0x39,
	ROLLER_COASTER     = 0x3A,
	GOOPER_FIGHT       = 0x3B,
	CORONA_BOSS        = 0x3C, // Family Feud
};

// Sunshine's achievements are on: the launcher passed SMS_ACHIEVEMENTS (the
// file they are kept in) and this isn't a Super Mario Eclipse build.
bool on();
// This is a Super Mario Eclipse build: its own achievements (ach_eclipse_*.cpp,
// eclipse.json), numbered from 1 as Sunshine's are.
bool eclipse();
// Eclipse's achievements are on (SMS_ACHIEVEMENTS in an Eclipse build).
bool eclipseOn();
// The player earned achievement `id` (of the build's set: Sunshine's, or Eclipse's in an Eclipse build). Kept and announced once; ignored while
// the play doesn't count (tainted()).
void unlock(int id);
// Shows an unlock's banner (ach_banner.cpp), after any already waiting.
void banner(int id);
bool unlocked(int id);
// The play since the file was chosen doesn't count: a save state was loaded
// or the warp menu used (until the next file select), or a cheat is on (until
// the next stage), or the stage or the game's code is modded (SMS_MOD,
// SMS_CODE_MODS, a native mod setting flags or warping).
bool tainted();

// What achievement `id` needs in this stage (an object, an enemy, a 1-Up's
// slot) was looked for at its start: `found` false means it can't be earned
// here, which is a bug, so it is always reported (stderr, once per stage),
// and the tour test (SMS_ACHIEVEMENTS_TEST=tour) counts it.
void require(int id, const char* what, bool found);
// The survey (a test aid, ach_survey.cpp): lists this stop's 1-Ups, Shines and star-named actors.
void survey();

// Where the player is: the current area and its scenario (episode 0-7 in the
// main courses; the plaza's and Pinna Park's own states elsewhere).
int area();
int episode();
// A stage is being played (not the title, file select or a movie).
bool playing();
// Counts stage loads: a part keeping per-visit state resets it when this
// changes (or in its own stageSetup).
unsigned visit();
// The player's Mario (gpMarioOriginal), or null.
TMario* mario();
// Debug output (SMS_ACHIEVEMENTS_DEBUG=1), printf-like.
bool debug();
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

} // namespace ach

#endif
