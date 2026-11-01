// Host test for the shared save file's merge rules (mp_flags.h): a host and
// two clients exchanging reports and updates as the game would, every
// message through the same chunked encoding.
//   make run
#include "mp_flags.h"
#include "../../net/mp_wire.h"

#include <stdio.h>
#include <stdlib.h>

using namespace mp;

#define CHECK(c)                                                              \
	do {                                                                      \
		if (!(c)) {                                                           \
			fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #c);    \
			exit(1);                                                          \
		}                                                                     \
	} while (0)

static int idx(uint32_t id)
{
	int i = sharedFlagIndex(id);
	CHECK(i >= 0);
	return i;
}

struct Game {
	FlagValues v;
	Game() : v(sharedFlags().size(), 0) {}
	int32_t& operator[](uint32_t id) { return v[idx(id)]; }
};

struct World {
	Game host;
	HostFlags hostFlags;
	Game clients[2];
	ClientFlags clientFlags[2];
	FlagUpdateReader readers[2];
	std::vector<std::vector<uint8_t>> toHost[2], toClient[2];
	size_t limit = 120; // small, so everything is split up

	void deliverToClients()
	{
		for (int c = 0; c < 2; c++) {
			for (auto& m : toClient[c])
				if (readers[c].add(m.data(), m.size()))
					clientFlags[c].hostUpdate(readers[c].u);
			toClient[c].clear();
		}
	}
	// Two frames everywhere: clients report, the host merges and answers,
	// and the clients' games show the answer.
	void frame()
	{
		round();
		round();
	}
	void round()
	{
		for (int c = 0; c < 2; c++) {
			FlagReport r = clientFlags[c].frame(clients[c].v);
			if (!r.changes.empty()) {
				std::vector<std::vector<uint8_t>> msgs;
				writeReport(r, limit, msgs);
				for (auto& m : msgs)
					toHost[c].push_back(m);
			}
		}
		for (int c = 0; c < 2; c++) {
			FlagReportReader rr;
			for (auto& m : toHost[c])
				if (rr.add(m.data(), m.size()))
					hostFlags.report(c + 1, rr.r, host.v);
			toHost[c].clear();
		}
		for (int c = 0; c < 2; c++) {
			FlagUpdate u;
			if (hostFlags.updateFor(c + 1, host.v, u)) {
				std::vector<std::vector<uint8_t>> msgs;
				writeUpdate(u, limit, msgs);
				for (auto& m : msgs)
					toClient[c].push_back(m);
			}
		}
		hostFlags.endFrame(host.v);
		deliverToClients();
	}
	void join(int c)
	{
		std::vector<std::vector<uint8_t>> msgs;
		writeUpdate(hostFlags.full(c + 1, host.v), limit, msgs);
		toClient[c] = msgs;
		deliverToClients();
	}
};

int main()
{
	World w;
	// the host's save: 30 Shines, 5 lives, 12 coins
	for (uint32_t i = 0; i < 30; i++)
		w.host[FLAG_SHINES_FIRST + i] = 1;
	w.host[FLAG_LIVES] = 5;
	w.host[FLAG_COINS] = 12;
	// client 0 picked its own save with other progress: it must not leak in
	w.clients[0][FLAG_SHINES_FIRST + 100] = 1;
	w.clients[0][FLAG_LIVES]              = 40;
	w.join(0);
	w.join(1);
	w.frame();
	for (int c = 0; c < 2; c++) {
		CHECK(w.clients[c].v == w.host.v);
		int32_t shines, blue;
		flagTotals([&](uint32_t id) { return w.clients[c][id]; }, &shines, &blue);
		CHECK(shines == 30 && blue == 0);
	}

	// both clients and the host pick up coins in the same frame: all count
	w.clients[0][FLAG_COINS] += 1;
	w.clients[1][FLAG_COINS] += 2;
	w.host[FLAG_COINS] += 3;
	// the client sees its own coin at once, before the host answers
	FlagValues before = w.clients[0].v;
	w.frame();
	CHECK(w.host[FLAG_COINS] == 18);
	CHECK(w.clients[0][FLAG_COINS] == 18 && w.clients[1][FLAG_COINS] == 18);

	// a pickup whose answer is slow: the client keeps it on top meanwhile
	w.clients[1][FLAG_COINS] += 1;
	{
		FlagReport r = w.clientFlags[1].frame(w.clients[1].v);
		CHECK(r.changes.size() == 1);
		CHECK(w.clients[1][FLAG_COINS] == 19);
		// an older update arrives that does not include it yet
		FlagUpdate stale;
		stale.applied = r.seq - 1;
		stale.values.emplace_back((uint16_t)idx(FLAG_COINS), 18);
		w.clientFlags[1].hostUpdate(stale);
		FlagReport none = w.clientFlags[1].frame(w.clients[1].v);
		CHECK(none.changes.empty());
		CHECK(w.clients[1][FLAG_COINS] == 19); // not 18 and back
		w.hostFlags.report(2, r, w.host.v);
		w.frame();
		CHECK(w.host[FLAG_COINS] == 19 && w.clients[1][FLAG_COINS] == 19 && w.clients[0][FLAG_COINS] == 19);
	}

	// a Shine from a client: everyone has it, and the totals follow
	w.clients[1][FLAG_SHINES_FIRST + 40] = 1;
	w.frame();
	CHECK(w.host[FLAG_SHINES_FIRST + 40] == 1 && w.clients[0][FLAG_SHINES_FIRST + 40] == 1);

	// a blue coin from the host
	w.host[FLAG_BLUE_FIRST + 7] = 1;
	w.frame();
	CHECK(w.clients[0][FLAG_BLUE_FIRST + 7] == 1 && w.clients[1][FLAG_BLUE_FIRST + 7] == 1);

	// a client loses a life: lives drop for everyone; its coins reset only for it
	w.clients[0][FLAG_LIVES] -= 1;
	w.clients[0][FLAG_COINS] = 0;
	w.frame();
	CHECK(w.host[FLAG_LIVES] == 4 && w.clients[1][FLAG_LIVES] == 4 && w.clients[0][FLAG_LIVES] == 4);
	CHECK(w.host[FLAG_COINS] == 19 && w.clients[0][FLAG_COINS] == 19);

	// a client's game over takes its flags back to its last save: no
	// progress is lost, for it or anyone
	w.clients[0][FLAG_SHINES_FIRST + 40] = 0;
	w.clients[0][FLAG_BLUE_FIRST + 7]    = 0;
	w.frame();
	CHECK(w.host[FLAG_SHINES_FIRST + 40] == 1 && w.clients[0][FLAG_SHINES_FIRST + 40] == 1);
	CHECK(w.clients[0][FLAG_BLUE_FIRST + 7] == 1);

	// red coins add up; best coin counts keep the highest
	w.clients[0][FLAG_RED_COINS] += 1;
	w.clients[1][FLAG_RED_COINS] += 1;
	w.host[FLAG_RED_COINS] += 1;
	w.clients[0][FLAG_BEST_COINS + 2] = 80;
	w.clients[1][FLAG_BEST_COINS + 2] = 60;
	w.frame();
	CHECK(w.host[FLAG_RED_COINS] == 3 && w.clients[0][FLAG_RED_COINS] == 3 && w.clients[1][FLAG_RED_COINS] == 3);
	CHECK(w.host[FLAG_BEST_COINS + 2] == 80 && w.clients[1][FLAG_BEST_COINS + 2] == 80);

	// lives stay within the game's 0..99
	w.clients[0][FLAG_LIVES] += 200;
	w.frame();
	CHECK(w.host[FLAG_LIVES] == 99);
	// used up: -1 (a player out of lives watches), and no lower
	w.host[FLAG_LIVES] = 0;
	w.frame();
	w.clients[0][FLAG_LIVES] -= 1;
	w.frame();
	CHECK(w.host[FLAG_LIVES] == -1 && w.clients[0][FLAG_LIVES] == -1 && w.clients[1][FLAG_LIVES] == -1);
	w.clients[1][FLAG_LIVES] -= 1;
	w.frame();
	CHECK(w.host[FLAG_LIVES] == -1);
	w.host[FLAG_LIVES] = 3; // everyone out: the host starts the stage again
	w.frame();
	CHECK(w.clients[0][FLAG_LIVES] == 3 && w.clients[1][FLAG_LIVES] == 3);

	// late game: everything set, the full update still arrives whole
	for (uint32_t i = 0; i < FLAG_CARD_BOOLS_N; i++)
		w.host[FLAG_CARD_BOOLS + i] = 1;
	w.join(1);
	w.frame();
	CHECK(w.clients[1].v == w.host.v);
	int32_t shines, blue;
	flagTotals([&](uint32_t id) { return w.clients[1][id]; }, &shines, &blue);
	CHECK(shines == 120 && blue == (int32_t)FLAG_BLUE_N);
	// a report from a changed or broken game cannot push the host's save out
	// of the game's bounds, or overflow it
	{
		FlagReport bad;
		bad.seq = 1000;
		bad.changes.push_back({(uint16_t)sharedFlagIndex(FLAG_COINS), 2000000000});
		bad.changes.push_back({(uint16_t)sharedFlagIndex(FLAG_RED_COINS), 2000000000});
		bad.changes.push_back({(uint16_t)sharedFlagIndex(FLAG_BEST_COINS + 3), 2147483647});
		bad.changes.push_back({(uint16_t)sharedFlagIndex(FLAG_LIVES), 2147483647});
		w.host[FLAG_COINS] = 500;
		w.hostFlags.report(1, bad, w.host.v);
		CHECK(w.host[FLAG_COINS] == 999 && w.host[FLAG_RED_COINS] == 99);
		CHECK(w.host[FLAG_BEST_COINS + 3] == 999 && w.host[FLAG_LIVES] == 99);
		FlagReport low;
		low.seq = 1001;
		low.changes.push_back({(uint16_t)sharedFlagIndex(FLAG_LIVES), -2147483647 - 1});
		w.hostFlags.report(1, low, w.host.v);
		CHECK(w.host[FLAG_LIVES] == -1);
		// more changes than there are flags is not a report from a game
		std::vector<uint8_t> chunk;
		{
			const size_t n = sharedFlags().size() + 1;
			chunk.resize(7 + n * 6);
			mp::Writer cw(chunk.data(), chunk.size());
			cw.u8(0);
			cw.u32(1002);
			cw.u16((uint16_t)n);
			for (size_t i = 0; i < n; i++) {
				cw.u16(0);
				cw.s32(1);
			}
		}
		FlagReportReader rr;
		CHECK(!rr.add(chunk.data(), chunk.size()));
	}
	(void)before;
	fprintf(stderr, "PASS (%zu shared flags)\n", sharedFlags().size());
	return 0;
}
