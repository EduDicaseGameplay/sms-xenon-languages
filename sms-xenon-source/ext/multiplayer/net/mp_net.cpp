// Multiplayer sessions over UDP (mp_net.h): joining, slots, the reliable
// channel and the relay of player states.
//
// Every packet starts "SMMP", the protocol version and its type:
//   PROBE   nonce, padding                 anyone -> host: is this a game?
//   INFO    nonce, players, max, open, build, name, status, protocol, mode, server   host's answer
//   CONNECT nonce, build, name, cookie     client -> host, until answered
//   CHALLENGE nonce, cookie                host -> client: say that again with this
//   ACCEPT  nonce, slot, token             host -> client
//   REJECT  nonce, reason, message         host -> client
//   DATA    token, ack, time, echo, states, world, messages   both ways, ~30 a second
//   BYE     token                          either side, when leaving
// DATA carries the newest player states and the host's world snapshot
// (unreliable: a lost one is replaced by the next) and every reliable message the other side has not
// acknowledged, resent until it is. `ack` is the next reliable sequence
// number the sender expects, so everything before it has arrived.
//
// Packets come from anyone on the internet, so the host keeps its answers no
// bigger than what was asked (a PROBE is padded to more than an INFO), gives
// out a slot only to a CONNECT that echoes a cookie sent to its address (a
// forged sender address never sees the cookie), and bounds what one player can
// make it hold or do.
#include "mp_net.h"
#include "mp_socket.h"
#include "mp_wire.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <thread>
#include <vector>

namespace mp {
namespace {

const uint32_t kMagic    = 0x504D4D53; // "SMMP"
const uint16_t kProtocol = 2;
enum PacketType : uint8_t {
	PK_PROBE = 1,
	PK_INFO,
	PK_CONNECT,
	PK_ACCEPT,
	PK_REJECT,
	PK_DATA,
	PK_BYE,
	PK_CHALLENGE,
};
enum RejectReason : uint8_t {
	REJECT_FULL = 1,
	REJECT_BUILD,
	REJECT_VERSION, // an older game or launcher than the host takes
	REJECT_CLOSED,
};
// Messages the session itself sends (above the game's kinds).
enum SystemKind : uint8_t {
	SYS_PLAYER_JOINED = 0xF0, // slot, name
	SYS_PLAYER_LEFT,          // slot
};

const size_t kPacketMax    = 1400; // the most read (and sent, by a full update of four players)
const size_t kPacketFill   = 1200; // what messages fill packets to: through any link (tunnels, VPNs) unsplit
const int kSendIntervalMs  = 33;  // states ~30 a second
const int kResendMs        = 150; // an unacknowledged message goes again
const int kConnectRetryMs  = 500;
const int kConnectGiveUpMs = 10000;
// A PROBE is at least this long, more than any INFO, so answering one sends
// no more than it took to ask (no use for flooding someone else).
const size_t kProbeSize = 200;
const int kCookieWindowMs  = 10000; // a challenge cookie lasts one or two of these
const int kRecvPerPass     = 64;    // packets read between two looks at the clock
const size_t kMaxEvents    = 2048;  // waiting for the game; more and the sender is dropped
const uint32_t kEarlyWindow = 256;  // out-of-order messages kept ahead of the next one

typedef std::chrono::steady_clock Clock;

// SipHash-2-4: the host's challenge cookies, keyed with a secret of its own.
uint64_t sipHash(const uint8_t key[16], const uint8_t* m, size_t len)
{
	auto rd = [](const uint8_t* p) {
		uint64_t v = 0;
		for (int i = 7; i >= 0; i--)
			v = v << 8 | p[i];
		return v;
	};
	auto rotl = [](uint64_t x, int b) { return (x << b) | (x >> (64 - b)); };
	const uint64_t k0 = rd(key), k1 = rd(key + 8);
	uint64_t v0 = 0x736f6d6570736575ULL ^ k0, v1 = 0x646f72616e646f6dULL ^ k1;
	uint64_t v2 = 0x6c7967656e657261ULL ^ k0, v3 = 0x7465646279746573ULL ^ k1;
	auto round = [&]() {
		v0 += v1, v1 = rotl(v1, 13), v1 ^= v0, v0 = rotl(v0, 32);
		v2 += v3, v3 = rotl(v3, 16), v3 ^= v2;
		v0 += v3, v3 = rotl(v3, 21), v3 ^= v0;
		v2 += v1, v1 = rotl(v1, 17), v1 ^= v2, v2 = rotl(v2, 32);
	};
	const size_t whole = len & ~(size_t)7;
	for (size_t i = 0; i < whole; i += 8) {
		const uint64_t mi = rd(m + i);
		v3 ^= mi, round(), round(), v0 ^= mi;
	}
	uint64_t last = (uint64_t)len << 56;
	for (size_t i = whole; i < len; i++)
		last |= (uint64_t)m[i] << (8 * (i - whole));
	v3 ^= last, round(), round(), v0 ^= last;
	v2 ^= 0xff;
	round(), round(), round(), round();
	return v0 ^ v1 ^ v2 ^ v3;
}

uint32_t nowMs()
{
	static const Clock::time_point start = Clock::now();
	return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

struct Outgoing {
	uint32_t seq;
	uint8_t kind;
	std::vector<uint8_t> data;
	uint32_t lastSent; // 0: not sent yet
};

struct Incoming {
	uint8_t kind;
	std::vector<uint8_t> data;
};

struct Peer {
	bool active = false;
	int stateNext = 0; // host: whose state goes first in the next packet to this player
	Address addr;
	uint32_t token = 0;
	uint32_t nonce = 0;
	char name[MP_NAME_LEN] = {};
	char version[32] = {}, launcher[32] = {}; // as their game said when it joined
	uint16_t api     = 0;
	uint32_t lastHeard     = 0;
	uint32_t nextSeq       = 1; // ours, outgoing
	std::deque<Outgoing> out;
	uint32_t recvNext = 1; // theirs, the next one to deliver
	std::map<uint32_t, Incoming> early;
	uint32_t peerTime  = 0; // their clock in their newest DATA, echoed back
	uint32_t peerTimeAt = 0;
	int rtt            = -1;
	float srtt         = 0.0f;  // the round trip, smoothed (ms; 0: not measured yet)
	// unreliable messages for the next packet (sent once, never again)
	std::deque<std::pair<uint8_t, std::vector<uint8_t>>> loose;
	bool flush         = false; // a new reliable message: send now
};

struct PlayerSlot {
	bool present = false;
	char name[MP_NAME_LEN] = {};
	uint8_t state[MP_MAX_STATE];
	size_t stateSize  = 0;
	uint32_t stateSeq = 0;
	uint32_t stateAt  = 0;
	// every state received since the game last asked (mp_remote_next), so
	// none is lost between two of its frames
	struct Received {
		std::vector<uint8_t> data;
		uint32_t at;
	};
	std::deque<Received> unread;
};

struct Session {
	std::mutex lock;
	MpConfig cfg;
	MpLinkState link = MP_LINK_OFF;
	std::string message;
	Socket sock;
	Address hostAddr; // client
	int localSlot = -1;
	uint32_t connectNonce = 0, connectStarted = 0, connectLastTry = 0;
	uint64_t connectCookie = 0; // client: the host's challenge, echoed in CONNECT
	uint8_t cookieKey[16]  = {}; // host: the secret behind its cookies
	Peer peers[MP_MAX_PLAYERS]; // host: by slot (0 unused); client: [0] is the host
	int hostMaxPlayers = 0;     // client: the most players the host takes (from its ACCEPT)
	PlayerSlot players[MP_MAX_PLAYERS];
	uint32_t localStateSeq = 0;
	std::deque<MpEvent> events;
	std::string status;
	uint16_t minApi = 0;       // host: the mod API joining games need (the server mods'), mp_set_min_api
	std::string mode;          // host: its game mode (a server mod's name; empty: playing together), for the launcher
	bool dedicated = false;    // host: a server, not a player (left out of the count probes see)
	std::thread thread;
	std::atomic<bool> running{false};
	std::mt19937 rng{std::random_device{}()};
	uint32_t lastSend = 0;
	std::vector<uint8_t> world; // host: to send; client: the newest received
	bool accepting = true;      // host: whether players may join now (mp_set_accepting)
	uint32_t worldSeq = 0, worldAt = 0;
	uint32_t worldSentSeq = 0; // host: the snapshot last sent
};

Session* g_session;
std::mutex g_sessionLock; // guards g_session itself
// A client's last slot and the host's token for it: sent when joining again
// (after losing the host), so the host gives the same slot back.
int g_rejoinSlot       = 0;
uint32_t g_rejoinToken = 0;

void copyName(char (&dst)[MP_NAME_LEN], const char* src)
{
	size_t n = strnlen(src, MP_NAME_LEN - 1);
	memmove(dst, src, n);
	dst[n] = 0;
}

void header(Writer& w, PacketType type)
{
	w.u32(kMagic);
	w.u16(kProtocol);
	w.u8(type);
}

void pushEvent(Session& s, MpEventType type, int slot, uint8_t kind = 0, const void* data = nullptr,
               size_t size = 0, const char* name = nullptr)
{
	// the game reads these every frame; only a peer flooding us (or a game
	// stuck loading for long) fills the queue, and deliver() drops the peer
	if (s.events.size() >= kMaxEvents + 16)
		return;
	s.events.emplace_back();
	MpEvent& ev = s.events.back();
	memset(&ev, 0, offsetof(MpEvent, data));
	ev.type = type;
	ev.slot = slot;
	ev.kind = kind;
	ev.size = (uint16_t)size;
	if (size)
		memcpy(ev.data, data, size);
	if (name)
		snprintf(ev.name, sizeof ev.name, "%s", name);
}

void queueReliable(Peer& p, uint8_t kind, const void* data, size_t size)
{
	Outgoing o;
	o.seq      = p.nextSeq++;
	o.kind     = kind;
	o.lastSent = 0;
	o.data.assign((const uint8_t*)data, (const uint8_t*)data + size);
	p.out.push_back(std::move(o));
	p.flush = true;
}

int peerIndexForSlot(Session& s, int slot)
{
	if (s.cfg.role == MP_ROLE_HOST)
		return slot >= 1 && slot < MP_MAX_PLAYERS && s.peers[slot].active ? slot : -1;
	return s.peers[0].active ? 0 : -1;
}

// Writes the reliable messages due for `p` that fit in `w`, oldest first;
// returns how many. Stops at the first that does not fit, so they go in order.
// How long a message waits for its acknowledgement before going again: a bit
// more than the round trip (so a slow link does not send everything twice).
// The most players the host takes (itself included): its setting, or all
// there are room for.
int maxPlayers(const Session& s)
{
	const int m = s.cfg.maxPlayers;
	return m >= 2 && m <= MP_MAX_PLAYERS ? m : MP_MAX_PLAYERS;
}

uint32_t resendMs(const Peer& p)
{
	if (p.srtt <= 0.0f)
		return (uint32_t)kResendMs;
	const float ms = p.srtt * 1.5f + 30.0f;
	return (uint32_t)(ms < 100.0f ? 100.0f : ms > 300.0f ? 300.0f : ms);
}

int writeMessages(Peer& p, Writer& w, size_t room, uint32_t now, bool force, bool& left)
{
	const size_t countAt = w.len;
	int count            = 0;
	left                 = false;
	const uint32_t wait  = resendMs(p);
	w.u8(0);
	for (Outgoing& o : p.out) {
		if (o.lastSent && now - o.lastSent < wait && !force)
			continue;
		if (count == 255 || w.len + 7 + o.data.size() > room) {
			left = true;
			break;
		}
		w.u32(o.seq);
		w.u8(o.kind);
		w.u16((uint16_t)o.data.size());
		w.bytes(o.data.data(), o.data.size());
		o.lastSent = now;
		count++;
	}
	w.p[countAt] = (uint8_t)count;
	return count;
}

// Unreliable messages, as many as fit in `room`, each sent once (what does
// not fit goes next time).
int writeLoose(Peer& p, Writer& w, size_t room)
{
	const size_t countAt = w.len;
	int count            = 0;
	w.u8(0);
	while (!p.loose.empty() && count < 255 && w.len + 3 + p.loose.front().second.size() <= room) {
		w.u8(p.loose.front().first);
		w.u16((uint16_t)p.loose.front().second.size());
		w.bytes(p.loose.front().second.data(), p.loose.front().second.size());
		p.loose.pop_front();
		count++;
	}
	if (w.ok)
		w.p[countAt] = (uint8_t)count;
	return count;
}

void dataHeader(Writer& w, Peer& p, uint32_t now)
{
	header(w, PK_DATA);
	w.u32(p.token);
	w.u32(p.recvNext);
	w.u32(now);
	// their clock as it is now (0 until we have heard it: no ping yet)
	w.u32(p.peerTimeAt ? p.peerTime + (now - p.peerTimeAt) : 0);
}

// `lean`: messages and acknowledgements only, between the regular updates
// (no states, no world: they come with the next update anyway).
void sendData(Session& s, Peer& p, int toSlot, uint32_t now, bool force, bool lean = false)
{
	uint8_t buf[kPacketMax];
	Writer w(buf, sizeof buf);
	dataHeader(w, p, now);

	// player states: the client sends its own; the host sends everyone else's,
	// as many as fit beside the world snapshot (eight players spraying do not),
	// starting from where the last packet to this player stopped
	size_t countAt = w.len;
	int count      = 0;
	w.u8(0);
	const size_t worldRoom = s.cfg.role == MP_ROLE_HOST && !lean ? 6 + s.world.size() : 6;
	int next = p.stateNext;
	for (int k = 0; k < MP_MAX_PLAYERS && !lean; k++) {
		const int i          = (p.stateNext + k) % MP_MAX_PLAYERS;
		const PlayerSlot& pl = s.players[i];
		if (!pl.present || !pl.stateSize || i == toSlot)
			continue;
		if (s.cfg.role == MP_ROLE_CLIENT && i != s.localSlot)
			continue;
		if (w.len + 6 + pl.stateSize + worldRoom + 2 > kPacketMax)
			break; // the rest first next time
		w.u8((uint8_t)i);
		w.u32(pl.stateSeq);
		w.u8((uint8_t)pl.stateSize);
		w.bytes(pl.state, pl.stateSize);
		count++;
		next = i + 1;
	}
	if (!lean)
		p.stateNext = next % MP_MAX_PLAYERS;
	buf[countAt] = count;

	// the host's world snapshot
	if (s.cfg.role == MP_ROLE_HOST && !s.world.empty() && !lean) {
		static uint32_t countAt, sent, newer, lastSeq;
		sent++;
		newer += s.worldSeq != lastSeq;
		lastSeq = s.worldSeq;
		if (getenv("SMS_MULTIPLAYER_SPEEDLOG") && now - countAt >= 5000) {
			fprintf(stderr, "[speed] net: %u world snapshots sent in %u ms, %u new\n", sent, now - countAt, newer);
			countAt = now;
			sent = newer = 0;
		}
		s.worldSentSeq = s.worldSeq;
		w.u32(s.worldSeq);
		w.u16((uint16_t)s.world.size());
		w.bytes(s.world.data(), s.world.size());
	} else {
		w.u32(0);
		w.u16(0);
	}

	// reliable messages not acknowledged, oldest first, as many as fit, then
	// unreliable ones, each once
	bool left = false;
	writeMessages(p, w, kPacketFill, now, force, left);
	p.flush = false;
	writeLoose(p, w, kPacketFill);
	if (w.ok)
		s.sock.send(buf, w.len, p.addr);
	// what did not fit beside the states and the world goes in packets of its
	// own (no states, no world), so a big message never waits for good
	for (int extra = 0; (left || !p.loose.empty()) && extra < 4; extra++) {
		Writer m(buf, sizeof buf);
		dataHeader(m, p, now);
		m.u8(0);  // no states
		m.u32(0); // no world
		m.u16(0);
		const int sent = writeMessages(p, m, kPacketFill, now, force, left);
		if (!writeLoose(p, m, kPacketFill) && !sent)
			break;
		if (!m.ok)
			break;
		s.sock.send(buf, m.len, p.addr);
	}
}

void dropPeer(Session& s, int idx, const char* why)
{
	Peer& p = s.peers[idx];
	if (!p.active)
		return;
	if (s.cfg.role == MP_ROLE_HOST) {
		fprintf(stderr, "[multiplayer] %s (slot %d) %s\n", p.name, idx, why);
		char name[MP_NAME_LEN];
		snprintf(name, sizeof name, "%s", s.players[idx].name);
		p          = Peer();
		s.players[idx] = PlayerSlot();
		pushEvent(s, MP_EV_PLAYER_LEFT, idx, 0, nullptr, 0, name);
		uint8_t slot = (uint8_t)idx;
		for (int i = 1; i < MP_MAX_PLAYERS; i++)
			if (s.peers[i].active)
				queueReliable(s.peers[i], SYS_PLAYER_LEFT, &slot, 1);
	} else {
		if (s.localSlot > 0 && p.token) { // to ask for this slot back when joining again
			g_rejoinSlot  = s.localSlot;
			g_rejoinToken = p.token;
		}
		p      = Peer();
		s.link = MP_LINK_FAILED;
		s.message = why;
		for (int i = 0; i < MP_MAX_PLAYERS; i++)
			if (i != s.localSlot && s.players[i].present) {
				char name[MP_NAME_LEN];
				snprintf(name, sizeof name, "%s", s.players[i].name);
				s.players[i] = PlayerSlot();
				pushEvent(s, MP_EV_PLAYER_LEFT, i, 0, nullptr, 0, name);
			}
		pushEvent(s, MP_EV_FAILED, -1);
		fprintf(stderr, "[multiplayer] %s\n", why);
	}
}

void deliver(Session& s, int fromSlot, uint8_t kind, const uint8_t* data, size_t size)
{
	if (s.events.size() >= kMaxEvents) {
		// the game cannot keep up with what this peer sends
		dropPeer(s, s.cfg.role == MP_ROLE_HOST ? fromSlot : 0, s.cfg.role == MP_ROLE_HOST
		         ? "sent too much at once" : "The host sent more than this game could take.");
		return;
	}
	if (kind < SYS_PLAYER_JOINED) {
		pushEvent(s, MP_EV_MESSAGE, fromSlot, kind, data, size);
		return;
	}
	if (s.cfg.role != MP_ROLE_CLIENT)
		return; // clients never send system messages
	Reader r(data, size);
	uint8_t slot = r.u8();
	if (!r.ok || slot >= MP_MAX_PLAYERS || (int)slot == s.localSlot)
		return;
	if (kind == SYS_PLAYER_JOINED) {
		PlayerSlot& pl = s.players[slot];
		pl             = PlayerSlot();
		pl.present     = true;
		r.str(pl.name, sizeof pl.name);
		pushEvent(s, MP_EV_PLAYER_JOINED, slot, 0, nullptr, 0, pl.name);
	} else if (kind == SYS_PLAYER_LEFT && s.players[slot].present) {
		char name[MP_NAME_LEN];
		snprintf(name, sizeof name, "%s", s.players[slot].name);
		s.players[slot] = PlayerSlot();
		pushEvent(s, MP_EV_PLAYER_LEFT, slot, 0, nullptr, 0, name);
	}
}

void readData(Session& s, int idx, Reader& r, uint32_t now)
{
	Peer& p     = s.peers[idx];
	uint32_t ack  = r.u32();
	uint32_t time = r.u32();
	uint32_t echo = r.u32();
	if (!r.ok)
		return;
	p.lastHeard  = now;
	p.peerTime   = time;
	p.peerTimeAt = now;
	if (echo && now >= echo && now - echo < 10000) {
		p.rtt  = (int)(now - echo);
		p.srtt = p.srtt > 0.0f ? p.srtt * 0.875f + p.rtt * 0.125f : (float)p.rtt;
	}
	while (!p.out.empty() && (int32_t)(p.out.front().seq - ack) < 0)
		p.out.pop_front();

	const int fromSlot = s.cfg.role == MP_ROLE_HOST ? idx : 0;
	uint8_t nStates    = r.u8();
	for (int i = 0; i < nStates && r.ok; i++) {
		uint8_t slot  = r.u8();
		uint32_t seq  = r.u32();
		uint8_t size  = r.u8();
		const uint8_t* d = r.skip(size);
		if (!d || slot >= MP_MAX_PLAYERS || size > MP_MAX_STATE)
			return;
		// a client speaks only for itself; the host for anyone but us
		if (s.cfg.role == MP_ROLE_HOST ? slot != idx : slot == s.localSlot)
			continue;
		PlayerSlot& pl = s.players[slot];
		if (!pl.present || (pl.stateSize && (int32_t)(seq - pl.stateSeq) <= 0))
			continue;
		memcpy(pl.state, d, size);
		pl.stateSize = size;
		pl.stateSeq  = seq;
		pl.stateAt   = now;
		pl.unread.push_back({std::vector<uint8_t>(d, d + size), now});
		while (pl.unread.size() > 64)
			pl.unread.pop_front();
	}

	uint32_t worldSeq     = r.u32();
	uint16_t worldSize    = r.u16();
	const uint8_t* worldP = r.skip(worldSize);
	if (!r.ok || worldSize > MP_MAX_WORLD)
		return;
	if (s.cfg.role == MP_ROLE_CLIENT && worldSize && (s.world.empty() || (int32_t)(worldSeq - s.worldSeq) >= 0)) {
		// the newest snapshot (or the same one again: still current)
		s.world.assign(worldP, worldP + worldSize);
		static uint32_t countAt, got, newer;
		got++;
		newer += worldSeq != s.worldSeq;
		if (getenv("SMS_MULTIPLAYER_SPEEDLOG") && now - countAt >= 5000) {
			fprintf(stderr, "[speed] net: %u world snapshots in %u ms, %u new\n", got, now - countAt, newer);
			countAt = now;
			got = newer = 0;
		}
		s.worldSeq = worldSeq;
		s.worldAt  = now;
	}

	uint8_t nMsgs = r.u8();
	bool got      = false;
	for (int i = 0; i < nMsgs && r.ok; i++) {
		uint32_t seq     = r.u32();
		uint8_t kind     = r.u8();
		uint16_t size    = r.u16();
		const uint8_t* d = r.skip(size);
		if (!d || size > MP_MAX_MESSAGE)
			return;
		got = true;
		if ((int32_t)(seq - p.recvNext) < 0 || p.early.count(seq))
			continue; // already have it
		if (seq - p.recvNext >= kEarlyWindow)
			continue; // too far ahead: it comes again once the gap is filled
		Incoming in;
		in.kind = kind;
		in.data.assign(d, d + size);
		p.early.emplace(seq, std::move(in));
		if (p.early.size() > 4096) {
			dropPeer(s, idx, "sent too much at once");
			return;
		}
	}
	// deliver in order
	for (auto it = p.early.find(p.recvNext); it != p.early.end(); it = p.early.find(p.recvNext)) {
		deliver(s, fromSlot, it->second.kind, it->second.data.data(), it->second.data.size());
		if (!p.active)
			return; // dropped for flooding
		p.early.erase(it);
		p.recvNext++;
	}
	if (got)
		p.flush = true; // acknowledge soon
	// unreliable messages: delivered as they come
	if (r.left() >= 1) {
		const int n = r.u8();
		for (int i = 0; i < n && r.ok; i++) {
			const uint8_t kind = r.u8();
			const uint16_t size = r.u16();
			const uint8_t* d    = r.skip(size);
			if (!d || size > MP_MAX_MESSAGE || kind >= SYS_PLAYER_JOINED)
				return;
			deliver(s, fromSlot, kind, d, size);
			if (!p.active)
				return;
		}
	}
}

uint64_t makeCookie(Session& s, const Address& from, uint32_t nonce, uint32_t window)
{
	const std::string who = from.text();
	std::vector<uint8_t> m(who.begin(), who.end());
	for (int i = 0; i < 4; i++)
		m.push_back((uint8_t)(nonce >> (8 * i)));
	for (int i = 0; i < 4; i++)
		m.push_back((uint8_t)(window >> (8 * i)));
	return sipHash(s.cookieKey, m.data(), m.size()) | 1;
}

// A cookie made for this address and nonce in this window or the one before.
bool cookieValid(Session& s, const Address& from, uint32_t nonce, uint64_t cookie, uint32_t now)
{
	if (!cookie)
		return false;
	const uint32_t window = now / kCookieWindowMs;
	return cookie == makeCookie(s, from, nonce, window) || (window && cookie == makeCookie(s, from, nonce, window - 1));
}

void hostPacket(Session& s, const uint8_t* buf, int n, const Address& from, uint32_t now)
{
	Reader r(buf, (size_t)n);
	if (r.u32() != kMagic)
		return;
	uint16_t version = r.u16();
	uint8_t type     = r.u8();
	if (!r.ok)
		return;
	uint8_t out[kPacketMax];
	Writer w(out, sizeof out);

	if (type == PK_PROBE) {
		if ((size_t)n < kProbeSize)
			return; // too short to be worth an answer (see kProbeSize)
		uint32_t nonce = r.u32();
		int count      = 0;
		for (const PlayerSlot& pl : s.players)
			count += pl.present;
		// a server is not a player: the launcher shows the players, of the most it takes
		const int server = s.dedicated ? 1 : 0;
		header(w, PK_INFO);
		w.u32(nonce);
		w.u8((uint8_t)(count - server));
		w.u8((uint8_t)(maxPlayers(s) - server));
		w.u8(version == kProtocol && count < maxPlayers(s) && s.accepting);
		w.str(s.cfg.build, 63);
		w.str(s.cfg.name, MP_NAME_LEN - 1);
		w.str(s.status.c_str(), 63);
		w.u16(kProtocol);
		// after what older launchers read: the game mode, and whether this is a server
		w.str(s.mode.c_str(), 15);
		w.u8((uint8_t)server);
		// then whom it takes: its version, whether builds may drift, the least game and launcher versions, the
		// least mod API (the launcher's join check says why before starting the game)
		w.str(s.cfg.version, 31);
		w.u8((uint8_t)s.cfg.versions);
		w.str(s.cfg.minVersion, 31);
		w.str(s.cfg.minLauncher, 31);
		w.u16(s.minApi);
		s.sock.send(out, w.len, from);
		return;
	}
	if (type == PK_CONNECT) {
		uint32_t nonce = r.u32();
		char build[64], name[MP_NAME_LEN];
		r.str(build, sizeof build);
		r.str(name, sizeof name);
		if (!r.ok)
			return;
		// a retry whose ACCEPT went missing
		for (int i = 1; i < MP_MAX_PLAYERS; i++) {
			Peer& p = s.peers[i];
			if (p.active && p.addr == from && p.nonce == nonce) {
				header(w, PK_ACCEPT);
				w.u32(nonce);
				w.u8((uint8_t)i);
				w.u32(p.token);
				w.u8((uint8_t)maxPlayers(s));
				s.sock.send(out, w.len, from);
				return;
			}
		}
		// The sender must show it gets packets at `from` (a forged address
		// never sees the cookie) before the host answers at length or gives
		// it a slot.
		const uint64_t cookie = r.u64();
		if (!r.ok || !cookieValid(s, from, nonce, cookie, now)) {
			header(w, PK_CHALLENGE);
			w.u32(nonce);
			w.u64(makeCookie(s, from, nonce, now / kCookieWindowMs));
			s.sock.send(out, w.len, from);
			return;
		}
		// after what older games send: their version, launcher and mod API
		const size_t before = r.pos;
		const int rejoinSlotPeek = r.u8();
		const uint32_t rejoinTokenPeek = r.u32();
		(void)rejoinSlotPeek;
		(void)rejoinTokenPeek;
		char theirVersion[32] = "", theirLauncher[32] = "";
		uint16_t theirApi = 0;
		if (r.ok && r.pos < r.len) {
			r.str(theirVersion, sizeof theirVersion);
			r.str(theirLauncher, sizeof theirLauncher);
			theirApi = r.u16();
			if (!r.ok) {
				theirVersion[0] = theirLauncher[0] = 0;
				theirApi = 0;
			}
		}
		r.pos = before;
		r.ok  = true;
		char whyBuf[200];
		const char* why = mp_join_refusal(&s.cfg, s.minApi, version, build, theirVersion, theirLauncher, theirApi,
		                                  whyBuf, sizeof whyBuf);
		uint8_t reason  = why ? (strstr(why, "or newer") ? REJECT_VERSION : REJECT_BUILD) : 0;
		if (!why && !s.accepting) {
			reason = REJECT_CLOSED;
			why    = "The host is still choosing a save file. Join again once they're playing.";
		}
		// a player joining again names the slot they had (and its token):
		// they get it back, the old one (perhaps not timed out yet) let go
		const int rejoinSlot       = r.u8();
		const uint32_t rejoinToken = r.u32();
		int slot                   = -1;
		if (!why && r.ok && rejoinSlot > 0 && rejoinSlot < maxPlayers(s) && rejoinToken) {
			Peer& old = s.peers[rejoinSlot];
			if (old.active && old.token == rejoinToken) {
				dropPeer(s, rejoinSlot, "reconnected");
				slot = rejoinSlot;
			} else if (!old.active) {
				slot = rejoinSlot;
			}
		}
		if (!why) {
			// a player reconnecting from the same address replaces their old slot
			for (int i = 1; i < MP_MAX_PLAYERS; i++)
				if (s.peers[i].active && s.peers[i].addr == from)
					dropPeer(s, i, "reconnected");
		}
		for (int i = 1; i < maxPlayers(s) && slot < 0; i++)
			if (!s.peers[i].active)
				slot = i;
		static char full[64];
		if (!why && slot < 0) {
			reason = REJECT_FULL;
			snprintf(full, sizeof full, "The game is full (%d players).", maxPlayers(s));
			why = full;
		}
		if (why) {
			header(w, PK_REJECT);
			w.u32(nonce);
			w.u8(reason);
			w.str(why, 200);
			s.sock.send(out, w.len, from);
			return;
		}
		Peer& p     = s.peers[slot];
		p           = Peer();
		p.active    = true;
		p.addr      = from;
		p.nonce     = nonce;
		p.token     = (uint32_t)s.rng() | 1;
		p.lastHeard = now;
		snprintf(p.name, sizeof p.name, "%s", name[0] ? name : "Player");
		snprintf(p.version, sizeof p.version, "%s", theirVersion);
		snprintf(p.launcher, sizeof p.launcher, "%s", theirLauncher);
		p.api = theirApi;
		PlayerSlot& pl = s.players[slot];
		pl             = PlayerSlot();
		pl.present     = true;
		copyName(pl.name, p.name);
		fprintf(stderr, "[multiplayer] %s joined from %s as player %d\n", p.name, from.text().c_str(), slot + 1);

		header(w, PK_ACCEPT);
		w.u32(nonce);
		w.u8((uint8_t)slot);
		w.u32(p.token);
		w.u8((uint8_t)maxPlayers(s)); // how many the host takes (for the players' list)
		s.sock.send(out, w.len, from);

		// the newcomer hears of everyone; everyone hears of the newcomer
		for (int i = 0; i < MP_MAX_PLAYERS; i++) {
			if (i == slot || !s.players[i].present)
				continue;
			uint8_t msg[1 + MP_NAME_LEN];
			Writer m(msg, sizeof msg);
			m.u8((uint8_t)i);
			m.str(s.players[i].name, MP_NAME_LEN - 1);
			queueReliable(p, SYS_PLAYER_JOINED, msg, m.len);
			if (i > 0 && s.peers[i].active) {
				Writer n(msg, sizeof msg);
				n.u8((uint8_t)slot);
				n.str(pl.name, MP_NAME_LEN - 1);
				queueReliable(s.peers[i], SYS_PLAYER_JOINED, msg, n.len);
			}
		}
		pushEvent(s, MP_EV_PLAYER_JOINED, slot, 0, nullptr, 0, pl.name);
		return;
	}
	if (type == PK_DATA || type == PK_BYE) {
		uint32_t token = r.u32();
		for (int i = 1; i < MP_MAX_PLAYERS; i++) {
			Peer& p = s.peers[i];
			if (!p.active || p.token != token || p.addr != from)
				continue;
			if (type == PK_BYE)
				dropPeer(s, i, "left");
			else
				readData(s, i, r, now);
			return;
		}
	}
}

void clientPacket(Session& s, const uint8_t* buf, int n, const Address& from, uint32_t now)
{
	if (from != s.hostAddr)
		return;
	Reader r(buf, (size_t)n);
	if (r.u32() != kMagic)
		return;
	r.u16();
	uint8_t type = r.u8();
	if (!r.ok)
		return;
	if (type == PK_ACCEPT && s.link == MP_LINK_CONNECTING) {
		uint32_t nonce = r.u32();
		uint8_t slot   = r.u8();
		uint32_t token = r.u32();
		const int most = r.u8();
		if (!r.ok || nonce != s.connectNonce || slot == 0 || slot >= MP_MAX_PLAYERS)
			return;
		s.hostMaxPlayers = most >= 2 && most <= MP_MAX_PLAYERS ? most : MP_MAX_PLAYERS;
		Peer& h     = s.peers[0];
		h           = Peer();
		h.active    = true;
		h.addr      = from;
		h.token     = token;
		h.lastHeard = now;
		s.localSlot = slot;
		s.link      = MP_LINK_CONNECTED;
		PlayerSlot& me = s.players[slot];
		me.present     = true;
		snprintf(me.name, sizeof me.name, "%s", s.cfg.name);
		char msg[160];
		snprintf(msg, sizeof msg, "Connected to %s as player %d", s.hostAddr.text().c_str(), slot + 1);
		s.message = msg;
		fprintf(stderr, "[multiplayer] %s\n", msg);
		pushEvent(s, MP_EV_CONNECTED, slot);
		return;
	}
	if (type == PK_CHALLENGE && s.link == MP_LINK_CONNECTING) {
		uint32_t nonce  = r.u32();
		uint64_t cookie = r.u64();
		if (!r.ok || nonce != s.connectNonce || !cookie)
			return;
		s.connectCookie  = cookie;
		s.connectLastTry = 0; // ask again now, with it
		return;
	}
	if (type == PK_REJECT && s.link == MP_LINK_CONNECTING) {
		uint32_t nonce = r.u32();
		r.u8();
		char why[201];
		r.str(why, sizeof why);
		if (!r.ok || nonce != s.connectNonce)
			return;
		s.link    = MP_LINK_FAILED;
		s.message = why;
		fprintf(stderr, "[multiplayer] the host refused: %s\n", why);
		pushEvent(s, MP_EV_FAILED, -1);
		return;
	}
	if ((type == PK_DATA || type == PK_BYE) && s.peers[0].active) {
		uint32_t token = r.u32();
		if (token != s.peers[0].token)
			return;
		if (type == PK_BYE)
			dropPeer(s, 0, "The host closed the game.");
		else
			readData(s, 0, r, now);
	}
}

// A goodbye, sent a few times: the other side drops this one at once instead
// of waiting out the timeout, and one lost packet should not undo that.
void sayBye(Session& s, const Peer& p)
{
	uint8_t out[32];
	Writer w(out, sizeof out);
	header(w, PK_BYE);
	w.u32(p.token);
	for (int i = 0; i < 3; i++)
		s.sock.send(out, w.len, p.addr);
}

void tick(Session& s, uint32_t now)
{
	if (s.cfg.role == MP_ROLE_CLIENT && s.link == MP_LINK_CONNECTING) {
		if (now - s.connectStarted > (uint32_t)kConnectGiveUpMs) {
			s.link = MP_LINK_FAILED;
			char msg[256];
			snprintf(msg, sizeof msg,
			         "No game answered at %s. Check the address and port, and that the host has started "
			         "Host Game and forwarded the port.",
			         s.hostAddr.text().c_str());
			s.message = msg;
			fprintf(stderr, "[multiplayer] %s\n", msg);
			pushEvent(s, MP_EV_FAILED, -1);
		} else if (!s.connectLastTry || now - s.connectLastTry >= (uint32_t)kConnectRetryMs) {
			uint8_t out[256];
			Writer w(out, sizeof out);
			header(w, PK_CONNECT);
			w.u32(s.connectNonce);
			w.str(s.cfg.build, 63);
			w.str(s.cfg.name, MP_NAME_LEN - 1);
			w.u64(s.connectCookie);
			w.u8((uint8_t)g_rejoinSlot);
			w.u32(g_rejoinToken);
			// what a host may ask of us (an older host reads no further)
			w.str(s.cfg.version, 31);
			w.str(s.cfg.launcher, 31);
			w.u16(s.cfg.api);
			s.sock.send(out, w.len, s.hostAddr);
			s.connectLastTry = now;
		}
		return;
	}
	const uint32_t timeoutMs = (uint32_t)(s.cfg.timeout * 1000.0f);
	const bool sendTick      = now - s.lastSend >= (uint32_t)kSendIntervalMs;
	if (sendTick)
		s.lastSend = now;
	for (int i = 0; i < MP_MAX_PLAYERS; i++) {
		Peer& p = s.peers[i];
		if (!p.active)
			continue;
		if (now - p.lastHeard > timeoutMs) {
			dropPeer(s, i, s.cfg.role == MP_ROLE_HOST ? "timed out" : "Lost the connection to the host.");
			continue;
		}
		if (sendTick || p.flush)
			sendData(s, p, s.cfg.role == MP_ROLE_HOST ? i : -1, now, false, !sendTick);
	}
}

void run(Session* s)
{
	// read a batch without the lock, so a flood of packets never holds up
	// the game thread (every mp_* call takes the lock), then handle it
	struct Packet {
		uint8_t data[2048];
		int size;
		Address from;
	};
	std::vector<Packet> batch(kRecvPerPass);
	std::uniform_real_distribution<float> chance(0.0f, 1.0f);
	std::deque<std::pair<uint32_t, Packet>> held; // tests: delayed packets, by when they arrive
	const bool delayed = s->cfg.delayMs > 0 || s->cfg.jitterMs > 0;
	while (s->running.load()) {
		s->sock.wait(5);
		int got = 0;
		while (got < kRecvPerPass) {
			Packet& pk = batch[got];
			if ((pk.size = s->sock.recv(pk.data, sizeof pk.data, pk.from)) < 0)
				break;
			got++;
		}
		std::lock_guard<std::mutex> hold(s->lock);
		const uint32_t now = nowMs();
		for (int i = 0; i < got; i++) {
			if (s->cfg.dropRate > 0 && chance(s->rng) < s->cfg.dropRate)
				continue;
			if (delayed) {
				const uint32_t due = now + (uint32_t)s->cfg.delayMs + (uint32_t)(chance(s->rng) * s->cfg.jitterMs);
				std::deque<std::pair<uint32_t, Packet>>::iterator at = held.end();
				while (at != held.begin() && (int32_t)((at - 1)->first - due) > 0)
					--at;
				held.insert(at, std::make_pair(due, batch[i]));
				continue;
			}
			if (s->cfg.role == MP_ROLE_HOST)
				hostPacket(*s, batch[i].data, batch[i].size, batch[i].from, now);
			else
				clientPacket(*s, batch[i].data, batch[i].size, batch[i].from, now);
		}
		while (!held.empty() && (int32_t)(now - held.front().first) >= 0) {
			Packet& pk = held.front().second;
			if (s->cfg.role == MP_ROLE_HOST)
				hostPacket(*s, pk.data, pk.size, pk.from, now);
			else
				clientPacket(*s, pk.data, pk.size, pk.from, now);
			held.pop_front();
		}
		tick(*s, now);
	}
	// say goodbye so the others need not wait for a timeout
	std::lock_guard<std::mutex> hold(s->lock);
	for (Peer& p : s->peers)
		if (p.active)
			sayBye(*s, p);
}

template <class F>
auto withSession(F f, decltype(f(*(Session*)nullptr)) fallback) -> decltype(f(*(Session*)nullptr))
{
	std::lock_guard<std::mutex> g(g_sessionLock);
	if (!g_session)
		return fallback;
	std::lock_guard<std::mutex> hold(g_session->lock);
	return f(*g_session);
}

} // namespace
} // namespace mp

using namespace mp;

extern "C" int mp_config_from_env(MpConfig* cfg, const char* build)
{
	memset(cfg, 0, sizeof *cfg);
	cfg->timeout = 10.0f;
	cfg->port    = MP_DEFAULT_PORT;
	snprintf(cfg->build, sizeof cfg->build, "%s", build ? build : "");
	const char* mode = getenv("SMS_MULTIPLAYER");
	if (!mode || !*mode || !strcmp(mode, "0") || !strcmp(mode, "off"))
		return 0;
	if (!strcmp(mode, "host"))
		cfg->role = MP_ROLE_HOST;
	else if (!strcmp(mode, "join"))
		cfg->role = MP_ROLE_CLIENT;
	else {
		snprintf(cfg->address, sizeof cfg->address, "SMS_MULTIPLAYER must be host or join, not %s", mode);
		return -1;
	}
	if (const char* port = getenv("SMS_MULTIPLAYER_PORT")) {
		long v = strtol(port, nullptr, 10);
		if (v < 1 || v > 65535) {
			snprintf(cfg->address, sizeof cfg->address, "SMS_MULTIPLAYER_PORT must be 1 to 65535");
			return -1;
		}
		cfg->port = (uint16_t)v;
	}
	if (cfg->role == MP_ROLE_CLIENT) {
		const char* addr = getenv("SMS_MULTIPLAYER_ADDRESS");
		if (!addr || !*addr) {
			snprintf(cfg->address, sizeof cfg->address, "SMS_MULTIPLAYER_ADDRESS is needed to join");
			return -1;
		}
		snprintf(cfg->address, sizeof cfg->address, "%s", addr);
	}
	const char* name = getenv("SMS_MULTIPLAYER_NAME");
	snprintf(cfg->name, sizeof cfg->name, "%s",
	         name && *name ? name : cfg->role == MP_ROLE_HOST ? "Host" : "Player");
	if (const char* max = getenv("SMS_MULTIPLAYER_MAX_PLAYERS")) {
		char* end = nullptr;
		const long n = strtol(max, &end, 10);
		if (!*max || *end || n < 2 || n > MP_MAX_PLAYERS) {
			snprintf(cfg->address, sizeof cfg->address, "The most players a game takes is 2 to %d, not \"%s\".",
			         MP_MAX_PLAYERS, max);
			return -1;
		}
		cfg->maxPlayers = (int)n;
	}
	if (const char* drop = getenv("SMS_MULTIPLAYER_DROP"))
		cfg->dropRate = (float)atof(drop);
	if (const char* delay = getenv("SMS_MULTIPLAYER_DELAY")) // "ms" or "ms:jitter"
		if (sscanf(delay, "%d:%d", &cfg->delayMs, &cfg->jitterMs) < 1)
			cfg->delayMs = cfg->jitterMs = 0;
	const char* local = getenv("SMS_MULTIPLAYER_LOCAL");
	cfg->loopback     = local && !strcmp(local, "1");
	if (const char* t = getenv("SMS_MULTIPLAYER_TIMEOUT"))
		cfg->timeout = (float)atof(t) > 1.0f ? (float)atof(t) : cfg->timeout;
	// versions: ours (the launcher says which), and whom a host takes
	auto text = [](const char* name, char* out, size_t size) {
		if (const char* v = getenv(name))
			snprintf(out, size, "%s", v);
	};
	text("SMS_GAME_VERSION", cfg->version, sizeof cfg->version);
	text("SMS_LAUNCHER_VERSION", cfg->launcher, sizeof cfg->launcher);
	if (const char* v = getenv("SMS_MULTIPLAYER_VERSIONS")) {
		if (!strcmp(v, "drift"))
			cfg->versions = MP_VERSIONS_DRIFT;
		else if (*v && strcmp(v, "exact")) {
			snprintf(cfg->address, sizeof cfg->address, "SMS_MULTIPLAYER_VERSIONS must be exact or drift, not %s", v);
			return -1;
		}
	}
	text("SMS_MULTIPLAYER_MIN_VERSION", cfg->minVersion, sizeof cfg->minVersion);
	text("SMS_MULTIPLAYER_MIN_LAUNCHER", cfg->minLauncher, sizeof cfg->minLauncher);
	return 1;
}

extern "C" int mp_version_compare(const char* a, const char* b)
{
	// numbers a dot (or dash) apart; text after a number ("1.2.0-beta") counts
	// before the plain number, so a beta sorts before its release
	a = a ? a : "";
	b = b ? b : "";
	while (*a || *b) {
		char* ea = nullptr;
		char* eb = nullptr;
		const unsigned long na = strtoul(a, &ea, 10), nb = strtoul(b, &eb, 10);
		if (na != nb)
			return na < nb ? -1 : 1;
		a = ea, b = eb;
		const bool ta = *a && *a != '.', tb = *b && *b != '.';
		if (ta != tb)
			return ta ? -1 : 1; // "1-beta" before "1"
		if (ta && tb) {
			// both carry text: compare it up to the next dot
			const size_t la = strcspn(a, "."), lb = strcspn(b, ".");
			const int c = strncmp(a, b, la < lb ? la : lb);
			if (c || la != lb)
				return c ? c : (la < lb ? -1 : 1);
			a += la, b += lb;
		}
		if (*a == '.')
			a++;
		if (*b == '.')
			b++;
	}
	return 0;
}

extern "C" const char* mp_join_refusal(const MpConfig* cfg, uint16_t minApi, uint16_t protocol, const char* build,
                                       const char* version, const char* launcher, uint16_t api, char* why, int whySize)
{
	auto eclipse = [](const char* b) {
		const size_t n = strlen(b);
		return n >= 8 && !strcmp(b + n - 8, "-eclipse");
	};
	if (protocol != kProtocol) {
		snprintf(why, (size_t)whySize, "The host is playing a different version of the game. Update both games to the same version.");
		return why;
	}
	if (eclipse(build) != eclipse(cfg->build)) {
		snprintf(why, (size_t)whySize, "The host has Super Mario Eclipse %s and you have it %s. Both players need the same Eclipse setting.",
		         eclipse(cfg->build) ? "on" : "off", eclipse(build) ? "on" : "off");
		return why;
	}
	if (cfg->versions == MP_VERSIONS_EXACT && strcmp(build, cfg->build) != 0) {
		snprintf(why, (size_t)whySize, "The host is playing a different version of the game. Update both games to the same version.");
		return why;
	}
	if (cfg->minVersion[0] && (!version[0] || mp_version_compare(version, cfg->minVersion) < 0)) {
		snprintf(why, (size_t)whySize, "This game needs game version %s or newer (you have %s). Update your game to join.",
		         cfg->minVersion, version[0] ? version : "an older one that doesn't say");
		return why;
	}
	if (cfg->minLauncher[0] && (!launcher[0] || mp_version_compare(launcher, cfg->minLauncher) < 0)) {
		snprintf(why, (size_t)whySize, "This game needs launcher %s or newer (you have %s). Update your launcher to join.",
		         cfg->minLauncher, launcher[0] ? launcher : "an older one, or none");
		return why;
	}
	if (minApi && api < minApi) {
		snprintf(why, (size_t)whySize, "This game's mode needs mod API %d.%d or newer (you have %d.%d). Update your game to join.",
		         minApi >> 8, minApi & 0xFF, api >> 8, api & 0xFF);
		return why;
	}
	return nullptr;
}

extern "C" int mp_start(const MpConfig* cfg)
{
	mp_stop();
	Session* s = new Session();
	s->cfg     = *cfg;
	if (s->cfg.timeout <= 0)
		s->cfg.timeout = 10.0f;
	std::string error;
	if (cfg->role == MP_ROLE_HOST) {
		if (!s->sock.open(cfg->port, error, cfg->loopback != 0)) {
			std::lock_guard<std::mutex> g(g_sessionLock);
			s->link    = MP_LINK_FAILED;
			s->message = "Could not host on port " + std::to_string(cfg->port) + ": " + error;
			fprintf(stderr, "[multiplayer] %s\n", s->message.c_str());
			g_session = s;
			return -1;
		}
		std::random_device seed;
		for (int i = 0; i < 16; i += 4) {
			const uint32_t v = seed();
			memcpy(s->cookieKey + i, &v, 4);
		}
		s->localSlot          = 0;
		s->link               = MP_LINK_CONNECTED;
		s->players[0].present = true;
		snprintf(s->players[0].name, sizeof s->players[0].name, "%s", cfg->name);
		s->message = "Hosting on port " + std::to_string(s->sock.localPort());
	} else {
		if (!Socket::resolve(cfg->address, cfg->port, s->hostAddr, error) || !s->sock.openFor(s->hostAddr, error)) {
			std::lock_guard<std::mutex> g(g_sessionLock);
			s->link    = MP_LINK_FAILED;
			s->message = error;
			fprintf(stderr, "[multiplayer] %s\n", error.c_str());
			g_session = s;
			return -1;
		}
		s->link           = MP_LINK_CONNECTING;
		s->connectNonce   = (uint32_t)s->rng() | 1;
		s->connectStarted = nowMs();
		s->message        = "Connecting to " + s->hostAddr.text() + "...";
	}
	fprintf(stderr, "[multiplayer] %s\n", s->message.c_str());
	s->running = true;
	{
		std::lock_guard<std::mutex> g(g_sessionLock);
		g_session = s;
	}
	s->thread = std::thread(run, s);
	return 0;
}

extern "C" void mp_stop(void)
{
	Session* s;
	{
		std::lock_guard<std::mutex> g(g_sessionLock);
		s         = g_session;
		g_session = nullptr;
	}
	if (!s)
		return;
	s->running = false;
	if (s->thread.joinable())
		s->thread.join();
	if (s->cfg.role == MP_ROLE_CLIENT && s->localSlot > 0 && s->peers[0].token) {
		g_rejoinSlot  = s->localSlot;
		g_rejoinToken = s->peers[0].token;
	}
	delete s;
}

extern "C" void mp_leave(const char* why)
{
	withSession(
	    [why](Session& s) {
		    if (s.cfg.role != MP_ROLE_CLIENT || s.link == MP_LINK_FAILED)
			    return 0;
		    if (s.peers[0].active) {
			    sayBye(s, s.peers[0]);
			    dropPeer(s, 0, why);
		    } else { // still connecting: stop asking
			    s.link    = MP_LINK_FAILED;
			    s.message = why;
			    pushEvent(s, MP_EV_FAILED, -1);
			    fprintf(stderr, "[multiplayer] %s\n", why);
		    }
		    return 0;
	    },
	    0);
}

extern "C" MpRole mp_role(void)
{
	return withSession([](Session& s) { return s.cfg.role; }, MP_ROLE_OFF);
}

extern "C" MpLinkState mp_link(void)
{
	return withSession([](Session& s) { return s.link; }, MP_LINK_OFF);
}

extern "C" void mp_link_message(char* out, size_t size)
{
	if (!size)
		return;
	out[0] = 0;
	withSession(
	    [&](Session& s) {
		    snprintf(out, size, "%s", s.message.c_str());
		    return 0;
	    },
	    0);
}

extern "C" int mp_local_slot(void)
{
	return withSession([](Session& s) { return s.localSlot; }, -1);
}

extern "C" unsigned mp_players(void)
{
	return withSession(
	    [](Session& s) {
		    unsigned mask = 0;
		    for (int i = 0; i < MP_MAX_PLAYERS; i++)
			    if (s.players[i].present)
				    mask |= 1u << i;
		    return mask;
	    },
	    0u);
}

extern "C" void mp_player_name(int slot, char* out, size_t size)
{
	if (!size)
		return;
	out[0] = 0;
	if (slot < 0 || slot >= MP_MAX_PLAYERS)
		return;
	withSession(
	    [&](Session& s) {
		    snprintf(out, size, "%s", s.players[slot].name);
		    return 0;
	    },
	    0);
}

extern "C" int mp_max_players(void)
{
	return withSession([](Session& s) { return s.cfg.role == MP_ROLE_HOST ? maxPlayers(s)
	                                           : s.hostMaxPlayers ? s.hostMaxPlayers : MP_MAX_PLAYERS; },
	                   MP_MAX_PLAYERS);
}

extern "C" int mp_ping(int slot)
{
	return withSession(
	    [&](Session& s) {
		    int idx = peerIndexForSlot(s, slot);
		    return idx < 0 ? -1 : s.peers[idx].rtt;
	    },
	    -1);
}

extern "C" void mp_set_local_state(const void* data, size_t size)
{
	if (size > MP_MAX_STATE)
		return;
	withSession(
	    [&](Session& s) {
		    if (s.localSlot < 0)
			    return 0;
		    PlayerSlot& me = s.players[s.localSlot];
		    memcpy(me.state, data, size);
		    me.stateSize = size;
		    me.stateSeq  = ++s.localStateSeq;
		    me.stateAt   = nowMs();
		    return 0;
	    },
	    0);
}

extern "C" size_t mp_remote_state(int slot, void* data, size_t size, float* age)
{
	if (slot < 0 || slot >= MP_MAX_PLAYERS)
		return 0;
	return withSession(
	    [&](Session& s) -> size_t {
		    const PlayerSlot& pl = s.players[slot];
		    if (!pl.present || !pl.stateSize || slot == s.localSlot)
			    return 0;
		    memcpy(data, pl.state, pl.stateSize < size ? pl.stateSize : size);
		    if (age)
			    *age = (nowMs() - pl.stateAt) / 1000.0f;
		    return pl.stateSize;
	    },
	    (size_t)0);
}

extern "C" int mp_send(int slot, uint8_t kind, const void* data, size_t size)
{
	if (size > MP_MAX_MESSAGE || kind >= SYS_PLAYER_JOINED)
		return -1;
	return withSession(
	    [&](Session& s) {
		    if (s.link != MP_LINK_CONNECTED)
			    return -1;
		    if (s.cfg.role == MP_ROLE_CLIENT) {
			    queueReliable(s.peers[0], kind, data, size);
			    return 0;
		    }
		    int sent = 0;
		    for (int i = 1; i < MP_MAX_PLAYERS; i++)
			    if (s.peers[i].active && (slot < 0 || slot == i)) {
				    queueReliable(s.peers[i], kind, data, size);
				    sent++;
			    }
		    return sent || slot < 0 ? 0 : -1;
	    },
	    -1);
}

extern "C" int mp_poll_event(MpEvent* ev)
{
	return withSession(
	    [&](Session& s) {
		    if (s.events.empty())
			    return 0;
		    *ev = s.events.front();
		    s.events.pop_front();
		    return 1;
	    },
	    0);
}

extern "C" void mp_set_accepting(int accepting)
{
	withSession([&](Session& s) {
		s.accepting = accepting != 0;
		return 0;
	}, 0);
}

extern "C" void mp_set_min_api(uint16_t api)
{
	withSession(
	    [&](Session& s) {
		    s.minApi = api;
		    return 0;
	    },
	    0);
}

extern "C" void mp_player_version(int slot, char* version, int versionSize, char* launcher, int launcherSize, uint16_t* api)
{
	if (version && versionSize > 0)
		version[0] = 0;
	if (launcher && launcherSize > 0)
		launcher[0] = 0;
	if (api)
		*api = 0;
	withSession(
	    [&](Session& s) {
		    const bool own = slot == s.localSlot;
		    if (!own && (slot < 1 || slot >= MP_MAX_PLAYERS || s.cfg.role != MP_ROLE_HOST || !s.peers[slot].active))
			    return 0;
		    const char* v = own ? s.cfg.version : s.peers[slot].version;
		    const char* l = own ? s.cfg.launcher : s.peers[slot].launcher;
		    if (version && versionSize > 0)
			    snprintf(version, (size_t)versionSize, "%s", v);
		    if (launcher && launcherSize > 0)
			    snprintf(launcher, (size_t)launcherSize, "%s", l);
		    if (api)
			    *api = own ? s.cfg.api : s.peers[slot].api;
		    return 0;
	    },
	    0);
}

extern "C" void mp_set_host_mode(const char* mode, int dedicated)
{
	withSession(
	    [&](Session& s) {
		    s.mode      = mode ? mode : "";
		    s.dedicated = dedicated != 0;
		    return 0;
	    },
	    0);
}

extern "C" void mp_set_host_status(const char* status)
{
	withSession(
	    [&](Session& s) {
		    s.status = status ? status : "";
		    return 0;
	    },
	    0);
}

extern "C" void mp_set_world(const void* data, size_t size)
{
	if (size > MP_MAX_WORLD)
		return;
	withSession(
	    [&](Session& s) {
		    if (s.cfg.role != MP_ROLE_HOST)
			    return 0;
		    s.world.assign((const uint8_t*)data, (const uint8_t*)data + size);
		    s.worldSeq++;
		    return 0;
	    },
	    0);
}

extern "C" size_t mp_world(void* data, size_t size, float* age)
{
	return withSession(
	    [&](Session& s) -> size_t {
		    if (s.cfg.role != MP_ROLE_CLIENT || s.world.empty())
			    return 0;
		    memcpy(data, s.world.data(), s.world.size() < size ? s.world.size() : size);
		    if (age)
			    *age = (nowMs() - s.worldAt) / 1000.0f;
		    return s.world.size();
	    },
	    (size_t)0);
}

extern "C" uint32_t mp_world_seq(void)
{
	return withSession([](Session& s) -> uint32_t { return s.cfg.role == MP_ROLE_CLIENT ? s.worldSeq : 0; }, 0u);
}

extern "C" uint32_t mp_clock_ms(void) { return nowMs(); }

extern "C" size_t mp_remote_next(int slot, void* data, size_t size, uint32_t* receivedMs)
{
	if (slot < 0 || slot >= MP_MAX_PLAYERS)
		return 0;
	return withSession(
	    [&](Session& s) -> size_t {
		    PlayerSlot& pl = s.players[slot];
		    if (pl.unread.empty() || slot == s.localSlot)
			    return 0;
		    PlayerSlot::Received r = std::move(pl.unread.front());
		    pl.unread.pop_front();
		    memcpy(data, r.data.data(), r.data.size() < size ? r.data.size() : size);
		    if (receivedMs)
			    *receivedMs = r.at;
		    return r.data.size();
	    },
	    (size_t)0);
}

extern "C" int mp_world_sent(void)
{
	return withSession([](Session& s) { return s.world.empty() || s.worldSentSeq == s.worldSeq ? 1 : 0; }, 1);
}

extern "C" int mp_send_unreliable(int slot, uint8_t kind, const void* data, size_t size)
{
	if (size > 400 || kind >= SYS_PLAYER_JOINED)
		return -1;
	return withSession([&](Session& s) {
		std::vector<uint8_t> d((const uint8_t*)data, (const uint8_t*)data + size);
		int queued = 0;
		for (int i = 0; i < MP_MAX_PLAYERS; i++) {
			Peer& p = s.peers[i];
			const bool to = s.cfg.role == MP_ROLE_HOST ? (slot < 0 ? i > 0 : i == slot) : i == 0;
			if (!p.active || !to)
				continue;
			p.loose.emplace_back(kind, d);
			while (p.loose.size() > 64)
				p.loose.pop_front(); // the oldest give way
			queued++;
		}
		return queued ? 0 : -1;
	}, -1);
}
