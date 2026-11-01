#ifndef SMS_MP_FLAGS_H
#define SMS_MP_FLAGS_H

// The shared save file (ext/multiplayer): which of the game's flags (the
// TFlagManager numbers) every player shares, and how changes from several
// players merge. Pure data, no game or socket code: the game half copies the
// game's flags in and out, the session layer carries the messages.
//
// The host's values are the truth. A client reports what changed in its own
// game; the host merges that into its game and sends everyone the new values.
// Until the host has answered, a client keeps its own pickups on top of the
// host's values, so a coin never visibly comes and goes.
//
// How each shared flag merges:
//   ONCE      Shines, blue coins, nozzle unlocks, story progress (card bools):
//             once anyone has it, everyone has it; never taken back.
//   COUNT     lives: every player's gains and losses add up.
//   COUNT_UP  coins, red coins: every player's pickups add up; a client's
//             own reset (losing a life reloads its stage) is not passed on.
//   MAX       best coin count per world: the highest anyone has.
// The Shine and blue coin totals are counted from the ONCE flags, as the game
// itself counts them.

#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace mp {

enum FlagRule : uint8_t {
	FLAG_ONCE,
	FLAG_COUNT,
	FLAG_COUNT_UP,
	FLAG_MAX,
};

// The game's flag numbers this file shares.
enum : uint32_t {
	FLAG_CARD_BOOLS   = 0x10000,
	FLAG_CARD_BOOLS_N = 0x3B4,
	FLAG_SHINES_FIRST = 0x10000, // a Shine each, 120
	FLAG_SHINES_N     = 0x78,
	FLAG_BLUE_FIRST   = 0x10078, // a blue coin each, 50 per world
	FLAG_BLUE_N       = 0x2EE,
	FLAG_LIVES        = 0x20001,
	FLAG_BEST_COINS   = 0x20005, // + world, 0..14
	FLAG_BEST_COINS_N = 0xF,
	FLAG_SHINE_TOTAL  = 0x40000,
	FLAG_BLUE_TOTAL   = 0x40001,
	FLAG_COINS        = 0x40002,
	FLAG_RED_COINS    = 0x60000,
	FLAG_BALLOONS     = 0x60001, // this stage: Bowser Jr. balloons popped (Pinna Park's roller coaster)
	FLAG_ROCKETS      = 0x60002, // this stage: rockets on Mecha-Bowser
};

struct FlagSpec {
	uint32_t id;
	FlagRule rule;
};

// Every shared flag, in a fixed order (the index is the wire number).
const std::vector<FlagSpec>& sharedFlags();
// The index of `id` in sharedFlags(), or -1.
int sharedFlagIndex(uint32_t id);

// One value per shared flag, in sharedFlags() order.
typedef std::vector<int32_t> FlagValues;

// Recounts the Shine and blue coin totals from their flags (as
// TFlagManager::correctFlag does). `get` reads one game flag.
template <class Get> void flagTotals(Get get, int32_t* shines, int32_t* blue)
{
	int32_t s = 0, b = 0;
	for (uint32_t i = 0; i < FLAG_SHINES_N; i++)
		s += get(FLAG_SHINES_FIRST + i) != 0;
	for (uint32_t i = 0; i < FLAG_BLUE_N; i++)
		b += get(FLAG_BLUE_FIRST + i) != 0;
	*shines = s;
	*blue   = b;
}

// What a client tells the host: changes since its last report.
struct FlagReport {
	uint32_t seq; // counts up from 1 per client
	struct Change {
		uint16_t index;
		int32_t value; // ONCE/MAX: the new value; COUNT/COUNT_UP: the change
	};
	std::vector<Change> changes;
};

// What the host tells a client: the host's values that changed (or all of
// them: every flag not listed is 0), and the newest report from that client
// it has merged.
struct FlagUpdate {
	uint32_t applied = 0;
	bool full        = false;
	std::vector<std::pair<uint16_t, int32_t>> values;
};

// A report or update as messages of at most `limit` bytes each: a big one
// (a full update late in the game) goes as several, which the reader puts
// back together (FlagUpdateReader).
void writeReport(const FlagReport& r, size_t limit, std::vector<std::vector<uint8_t>>& out);
bool readReport(const uint8_t* data, size_t size, FlagReport& r, bool& last);
void writeUpdate(const FlagUpdate& u, size_t limit, std::vector<std::vector<uint8_t>>& out);
bool readUpdate(const uint8_t* data, size_t size, FlagUpdate& u, bool& last);

// Joins an update's messages: add() each; true once `u` is whole.
struct FlagUpdateReader {
	FlagUpdate u;
	bool started = false;
	bool add(const uint8_t* data, size_t size);
};
struct FlagReportReader {
	FlagReport r;
	bool started = false;
	bool add(const uint8_t* data, size_t size);
};

// A client's side of the merge.
class ClientFlags {
public:
	// The host's full values (on joining, and again on each stage): the
	// game's flags become exactly these, pending reports aside.
	void reset();
	bool hasHost() const { return hasHost_; }
	// Once a frame: `game` is what the game's flags are now; on return it is
	// what they should become (write back the ones that changed). Returns a
	// report to send, or one with no changes.
	FlagReport frame(FlagValues& game);
	// A message from the host.
	void hostUpdate(const FlagUpdate& u);

private:
	bool hasHost_ = false;
	bool exact_   = false; // next frame: take the host's values as they are
	FlagValues host_, seen_;
	uint32_t nextSeq_ = 1;
	struct Pending {
		uint32_t seq;
		uint16_t index;
		int32_t delta;
	};
	std::vector<Pending> pending_;
};

// The host's side.
class HostFlags {
public:
	// Merges a client's report into `game` (the host's flags, as read this
	// frame); remembers it was applied for `slot`.
	void report(int slot, const FlagReport& r, FlagValues& game);
	// After the game's flags are written back: the update for `slot`, if
	// there is anything to say (values changed since the last frame, or a
	// report to acknowledge). Call for every client, then endFrame().
	bool updateFor(int slot, const FlagValues& game, FlagUpdate& out) const;
	void endFrame(const FlagValues& game);
	// Everything, for a client that joins or loads a stage.
	FlagUpdate full(int slot, const FlagValues& game) const;
	void forget(int slot);

private:
	FlagValues sent_; // what clients were last told
	uint32_t applied_[8]     = {};
	uint32_t acknowledged_[8] = {};
};

} // namespace mp

#endif
