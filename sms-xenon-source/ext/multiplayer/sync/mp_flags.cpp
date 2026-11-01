// The shared save file's merge rules (mp_flags.h).
#include "mp_flags.h"
#include "../net/mp_wire.h"

#include <algorithm>

namespace mp {

const std::vector<FlagSpec>& sharedFlags()
{
	static const std::vector<FlagSpec> specs = [] {
		std::vector<FlagSpec> v;
		for (uint32_t i = 0; i < FLAG_CARD_BOOLS_N; i++)
			v.push_back({FLAG_CARD_BOOLS + i, FLAG_ONCE});
		v.push_back({FLAG_LIVES, FLAG_COUNT});
		for (uint32_t i = 0; i < FLAG_BEST_COINS_N; i++)
			v.push_back({FLAG_BEST_COINS + i, FLAG_MAX});
		v.push_back({FLAG_COINS, FLAG_COUNT_UP});
		v.push_back({FLAG_RED_COINS, FLAG_COUNT_UP});
		v.push_back({FLAG_BALLOONS, FLAG_COUNT_UP});
		v.push_back({FLAG_ROCKETS, FLAG_COUNT_UP});
		return v;
	}();
	return specs;
}

int sharedFlagIndex(uint32_t id)
{
	if (id >= FLAG_CARD_BOOLS && id < FLAG_CARD_BOOLS + FLAG_CARD_BOOLS_N)
		return (int)(id - FLAG_CARD_BOOLS);
	const std::vector<FlagSpec>& specs = sharedFlags();
	for (size_t i = FLAG_CARD_BOOLS_N; i < specs.size(); i++)
		if (specs[i].id == id)
			return (int)i;
	return -1;
}

namespace {

enum : uint8_t {
	CHUNK_LAST  = 1,
	CHUNK_FULL  = 2,
	CHUNK_FIRST = 4,
};
const size_t kEntry = 6; // u16 index, s32 value

template <class Entries, class Emit>
void chunked(const Entries& entries, size_t limit, size_t headerSize, Emit emit)
{
	const size_t per = limit > headerSize + kEntry ? (limit - headerSize) / kEntry : 1;
	size_t at = 0;
	do {
		size_t n = std::min(per, entries.size() - at);
		emit(at, n, at == 0, at + n >= entries.size());
		at += n;
	} while (at < entries.size());
}

// lives: 0 to 99, or -1 once they are used up (players then watch the others)
int32_t lives(int64_t v) { return v < -1 ? -1 : v > 99 ? 99 : (int32_t)v; }

// What a flag can hold in a real game. A player's report comes from the
// network, so the host keeps every value it takes in these bounds (a changed
// or broken game cannot write a huge coin count into the host's save).
int32_t clampFlag(uint32_t id, int64_t v)
{
	int64_t lo = 0, hi = 1;
	if (id == FLAG_LIVES)
		return lives(v);
	if (id == FLAG_COINS || (id >= FLAG_BEST_COINS && id < FLAG_BEST_COINS + FLAG_BEST_COINS_N))
		hi = 999;
	else if (id == FLAG_RED_COINS || id == FLAG_BALLOONS || id == FLAG_ROCKETS)
		hi = 99;
	return (int32_t)(v < lo ? lo : v > hi ? hi : v);
}

} // namespace

void writeReport(const FlagReport& r, size_t limit, std::vector<std::vector<uint8_t>>& out)
{
	out.clear();
	chunked(r.changes, limit, 7, [&](size_t at, size_t n, bool, bool last) {
		out.emplace_back(7 + n * kEntry);
		Writer w(out.back().data(), out.back().size());
		w.u8(last ? CHUNK_LAST : 0);
		w.u32(r.seq);
		w.u16((uint16_t)n);
		for (size_t i = at; i < at + n; i++) {
			w.u16(r.changes[i].index);
			w.s32(r.changes[i].value);
		}
	});
}

bool readReport(const uint8_t* data, size_t size, FlagReport& r, bool& last)
{
	Reader rd(data, size);
	uint8_t flags = rd.u8();
	r.seq         = rd.u32();
	uint16_t n    = rd.u16();
	const size_t count = sharedFlags().size();
	if (r.changes.size() + n > count)
		return false; // more changes than there are flags: not from a game
	for (int i = 0; i < n && rd.ok; i++) {
		FlagReport::Change c;
		c.index = rd.u16();
		c.value = rd.s32();
		if (c.index >= count)
			return false;
		r.changes.push_back(c);
	}
	last = (flags & CHUNK_LAST) != 0;
	return rd.ok;
}

void writeUpdate(const FlagUpdate& u, size_t limit, std::vector<std::vector<uint8_t>>& out)
{
	out.clear();
	chunked(u.values, limit, 7, [&](size_t at, size_t n, bool first, bool last) {
		out.emplace_back(7 + n * kEntry);
		Writer w(out.back().data(), out.back().size());
		w.u8((last ? CHUNK_LAST : 0) | (first ? CHUNK_FIRST : 0) | (u.full ? CHUNK_FULL : 0));
		w.u32(u.applied);
		w.u16((uint16_t)n);
		for (size_t i = at; i < at + n; i++) {
			w.u16(u.values[i].first);
			w.s32(u.values[i].second);
		}
	});
}

bool readUpdate(const uint8_t* data, size_t size, FlagUpdate& u, bool& last)
{
	Reader rd(data, size);
	uint8_t flags = rd.u8();
	u.applied     = rd.u32();
	u.full        = (flags & CHUNK_FULL) != 0;
	uint16_t n    = rd.u16();
	const size_t count = sharedFlags().size();
	if (u.values.size() + n > count)
		return false;
	for (int i = 0; i < n && rd.ok; i++) {
		uint16_t index = rd.u16();
		int32_t value  = rd.s32();
		if (index >= count)
			return false;
		u.values.emplace_back(index, value);
	}
	last = (flags & CHUNK_LAST) != 0;
	return rd.ok;
}

bool FlagUpdateReader::add(const uint8_t* data, size_t size)
{
	if (size && (data[0] & CHUNK_FIRST)) {
		u       = FlagUpdate();
		started = true;
	}
	if (!started)
		return false;
	bool last = false;
	if (!readUpdate(data, size, u, last)) {
		started = false;
		return false;
	}
	if (last)
		started = false;
	return last;
}

bool FlagReportReader::add(const uint8_t* data, size_t size)
{
	if (!started) {
		r       = FlagReport();
		started = true;
	}
	bool last = false;
	if (!readReport(data, size, r, last)) {
		started = false;
		return false;
	}
	if (last)
		started = false;
	return last;
}

void ClientFlags::reset()
{
	hasHost_ = false;
	exact_   = false;
	host_.clear();
	seen_.clear();
	pending_.clear();
}

FlagReport ClientFlags::frame(FlagValues& game)
{
	const std::vector<FlagSpec>& specs = sharedFlags();
	FlagReport rep;
	rep.seq = 0;
	if (!hasHost_ || game.size() != specs.size()) {
		seen_ = game;
		return rep;
	}
	// after a full update the game takes the host's values as they are (its
	// own older progress does not leak in), except what it did this frame
	const bool exact = exact_ || seen_.size() != game.size();
	if (seen_.size() != game.size())
		seen_ = game;
	exact_ = false;
	std::vector<uint8_t> mine(specs.size(), 0);
	// what our own game did since the last frame
	for (size_t i = 0; i < specs.size(); i++) {
		const int32_t cur = game[i], was = seen_[i];
		if (cur == was)
			continue;
		switch (specs[i].rule) {
		case FLAG_ONCE:
			if (cur && !was)
				rep.changes.push_back({(uint16_t)i, 1});
			break;
		case FLAG_COUNT:
			rep.changes.push_back({(uint16_t)i, cur - was});
			break;
		case FLAG_COUNT_UP:
			if (cur > was)
				rep.changes.push_back({(uint16_t)i, cur - was});
			break;
		case FLAG_MAX:
			if (cur > was)
				rep.changes.push_back({(uint16_t)i, cur});
			break;
		}
	}
	if (!rep.changes.empty()) {
		rep.seq = nextSeq_++;
		for (const FlagReport::Change& c : rep.changes) {
			mine[c.index] = 1;
			FlagRule rule = specs[c.index].rule;
			if (rule == FLAG_COUNT || rule == FLAG_COUNT_UP)
				pending_.push_back({rep.seq, c.index, c.value});
		}
	}
	// what the game shows: the host's values, with ours not yet merged on top
	for (size_t i = 0; i < specs.size(); i++) {
		const bool keep = !exact || mine[i];
		switch (specs[i].rule) {
		case FLAG_ONCE:
			game[i] = (keep && game[i]) || host_[i] ? 1 : 0;
			break;
		case FLAG_COUNT:
		case FLAG_COUNT_UP:
			game[i] = host_[i];
			break;
		case FLAG_MAX:
			game[i] = keep ? std::max(game[i], host_[i]) : host_[i];
			break;
		}
	}
	for (const Pending& p : pending_)
		game[p.index] += p.delta;
	seen_ = game;
	return rep;
}

void ClientFlags::hostUpdate(const FlagUpdate& u)
{
	const size_t n = sharedFlags().size();
	if (u.full || host_.size() != n) {
		host_.assign(n, 0);
		exact_ = true;
	}
	for (const auto& v : u.values)
		if (v.first < n)
			host_[v.first] = v.second;
	hasHost_ = true;
	pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
	                              [&](const Pending& p) { return (int32_t)(p.seq - u.applied) <= 0; }),
	               pending_.end());
}

void HostFlags::report(int slot, const FlagReport& r, FlagValues& game)
{
	const std::vector<FlagSpec>& specs = sharedFlags();
	if (slot < 0 || slot >= 8 || game.size() != specs.size())
		return;
	for (const FlagReport::Change& c : r.changes) {
		if (c.index >= specs.size())
			continue;
		int32_t& v        = game[c.index];
		const uint32_t id = specs[c.index].id;
		switch (specs[c.index].rule) {
		case FLAG_ONCE:
			if (c.value)
				v = 1;
			break;
		case FLAG_COUNT:
			v = clampFlag(id, (int64_t)v + c.value);
			break;
		case FLAG_COUNT_UP:
			if (c.value > 0)
				v = clampFlag(id, (int64_t)v + c.value);
			break;
		case FLAG_MAX:
			v = std::max(v, clampFlag(id, c.value));
			break;
		}
	}
	if ((int32_t)(r.seq - applied_[slot]) > 0)
		applied_[slot] = r.seq;
}

bool HostFlags::updateFor(int slot, const FlagValues& game, FlagUpdate& out) const
{
	if (slot < 0 || slot >= 8)
		return false;
	if (sent_.size() != game.size()) {
		out = full(slot, game);
		return true;
	}
	out         = FlagUpdate();
	out.applied = applied_[slot];
	for (size_t i = 0; i < game.size(); i++)
		if (game[i] != sent_[i])
			out.values.emplace_back((uint16_t)i, game[i]);
	if (out.values.empty() && applied_[slot] == acknowledged_[slot])
		return false;
	const_cast<HostFlags*>(this)->acknowledged_[slot] = applied_[slot];
	return true;
}

void HostFlags::endFrame(const FlagValues& game) { sent_ = game; }

FlagUpdate HostFlags::full(int slot, const FlagValues& game) const
{
	FlagUpdate u;
	u.full    = true;
	u.applied = slot >= 0 && slot < 8 ? applied_[slot] : 0;
	for (size_t i = 0; i < game.size(); i++)
		if (game[i])
			u.values.emplace_back((uint16_t)i, game[i]);
	if (slot >= 0 && slot < 8)
		const_cast<HostFlags*>(this)->acknowledged_[slot] = applied_[slot];
	return u;
}

void HostFlags::forget(int slot)
{
	if (slot >= 0 && slot < 8)
		applied_[slot] = acknowledged_[slot] = 0;
}

} // namespace mp
