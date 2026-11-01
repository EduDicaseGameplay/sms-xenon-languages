#ifndef SMS_MP_NET_H
#define SMS_MP_NET_H

// Multiplayer networking (ext/multiplayer/net): the game-agnostic half of the
// multiplayer extension. It knows sockets, peers, a reliable message channel
// and per-player state blobs; it knows nothing of Mario, flags or stages.
// The game half (ext/multiplayer/game) is the only caller.
//
// Topology: one host, up to MP_MAX_PLAYERS - 1 clients, all over UDP straight
// to the host (no relay servers). The host is authoritative: clients send it
// their own player state and their requests; the host sends every client the
// other players' states and its decisions. Slot 0 is always the host.
//
// Threading: a network thread owns the socket, answers probes, keeps peers
// alive and resends reliable messages, so loading screens on the game thread
// do not drop anyone. The game thread calls the functions below; every one of
// them is safe to call from any thread.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MP_MAX_PLAYERS 8 // slots a session has; the host takes MpConfig::maxPlayers of them
#define MP_DEFAULT_PORT 24002
#define MP_NAME_LEN 24
// The largest payload of one reliable message (a packet stays below the
// smallest common path MTU with room for its header and acks).
#define MP_MAX_MESSAGE 1000
// The largest per-player state blob carried each tick.
#define MP_MAX_STATE 224
// The largest world snapshot the host sends with each update (enemies).
#define MP_MAX_WORLD 600

typedef enum MpRole {
	MP_ROLE_OFF = 0,
	MP_ROLE_HOST,
	MP_ROLE_CLIENT,
} MpRole;

typedef enum MpLinkState {
	MP_LINK_OFF = 0,
	MP_LINK_CONNECTING, // client: asking the host for a slot
	MP_LINK_CONNECTED,  // host: listening; client: has a slot
	MP_LINK_FAILED,     // client: refused, unreachable or dropped (mp_link_message says why)
} MpLinkState;

typedef enum MpEventType {
	MP_EV_NONE = 0,
	MP_EV_CONNECTED,     // client: we have slot `slot`
	MP_EV_FAILED,        // client: could not join, or lost the host
	MP_EV_PLAYER_JOINED, // `slot` joined (name in `name`)
	MP_EV_PLAYER_LEFT,   // `slot` left or timed out
	MP_EV_MESSAGE,       // a reliable message from `slot` (`kind`, `data`, `size`)
} MpEventType;

typedef struct MpEvent {
	MpEventType type;
	int slot;
	uint8_t kind;
	uint16_t size;
	uint8_t data[MP_MAX_MESSAGE];
	char name[MP_NAME_LEN];
} MpEvent;

enum { MP_VERSIONS_EXACT = 0, MP_VERSIONS_DRIFT = 1 };

typedef struct MpConfig {
	MpRole role;
	char address[256]; // client: host name or address to join
	uint16_t port;     // host: port to listen on; client: the host's port
	char name[MP_NAME_LEN];
	char build[64];    // the game's build (its commit, "-eclipse" for Eclipse): the same on both sides unless the
	                   // host lets versions drift
	// this game's version (the launcher's, "2026.10.18.1": SMS_GAME_VERSION; empty: not known), the launcher
	// that started it (SMS_LAUNCHER_VERSION), and the mod API it carries (major << 8 | minor): what a host may ask of
	// the games that join it
	char version[32], launcher[32];
	uint16_t api;
	// host: whom it takes. MP_VERSIONS_EXACT: its own build only; MP_VERSIONS_DRIFT: any build with the same
	// protocol and the same Eclipse setting (SMS_MULTIPLAYER_VERSIONS=exact|drift); and at least these versions
	// of the game and the launcher, when given (SMS_MULTIPLAYER_MIN_VERSION, SMS_MULTIPLAYER_MIN_LAUNCHER)
	int versions;
	char minVersion[32], minLauncher[32];
	// tests: drop this share of incoming packets (0..1), to exercise resends
	float dropRate;
	// tests: hold incoming packets this long, give or take up to the jitter
	// (ms), as a far-off player's would be (late ones arrive out of order)
	int delayMs, jitterMs;
	// seconds of silence after which a peer counts as gone
	float timeout;
	// host: listen on this computer only (the launcher passes the other
	// players' packets on)
	int loopback;
	// host: the most players it takes, itself included (2 to MP_MAX_PLAYERS;
	// 0: MP_MAX_PLAYERS)
	int maxPlayers;
} MpConfig;

// Reads SMS_MULTIPLAYER (host|join), SMS_MULTIPLAYER_ADDRESS (host[:port]),
// SMS_MULTIPLAYER_PORT, SMS_MULTIPLAYER_NAME, SMS_MULTIPLAYER_LOCAL (1:
// listen on this computer only) and SMS_MULTIPLAYER_MAX_PLAYERS (host: 2 to
// MP_MAX_PLAYERS) into *cfg. Returns 0 when
// multiplayer is not asked for, -1 when it is but the settings are wrong
// (cfg->address then holds why).
int mp_config_from_env(MpConfig* cfg, const char* build);

// Starts hosting or joining. Returns 0, or -1 when the socket could not be
// opened (mp_link_message says why).
int mp_start(const MpConfig* cfg);
void mp_stop(void);
// Client: leaves the host's game, saying goodbye (or stops connecting). The
// link is then MP_LINK_FAILED with `why` as its message, and MP_EV_FAILED
// comes as when the host drops us.
void mp_leave(const char* why);

MpRole mp_role(void);
MpLinkState mp_link(void);
// A sentence for the player about the link ("Connecting to 1.2.3.4:24002...").
void mp_link_message(char* out, size_t size);
int mp_local_slot(void); // -1 until a client has a slot
// The players now in the session (bit n: slot n), the local one included.
unsigned mp_players(void);
void mp_player_name(int slot, char* out, size_t size);
// Host: the most players the session takes (MpConfig::maxPlayers, itself
// included); a client: MP_MAX_PLAYERS (the host decides).
int mp_max_players(void);
// Round-trip time to the host (clients) or to `slot` (host), in ms; -1 unknown.
int mp_ping(int slot);

// This player's state, sent unreliably about 30 times a second (the newest
// one only). `size` <= MP_MAX_STATE.
void mp_set_local_state(const void* data, size_t size);
// The newest state from `slot`: copies at most `size` bytes, returns its size
// (0 when there is none) and how many seconds ago it arrived.
size_t mp_remote_state(int slot, void* data, size_t size, float* age);

// Host: the newest snapshot of the shared world (where enemies are), sent
// unreliably with each update, like player states. Client: the newest one
// received, its size (0 when none) and how many seconds ago it arrived.
void mp_set_world(const void* data, size_t size);
size_t mp_world(void* data, size_t size, float* age);
// Host: whether the snapshot last set has gone out (in at least one update).
int mp_world_sent(void);

// The states of `slot` in the order they arrived, each once (the oldest
// unread first): its size, or 0 when none is left; *receivedMs on the
// mp_clock_ms clock.
size_t mp_remote_next(int slot, void* data, size_t size, uint32_t* receivedMs);
// Milliseconds on a steady clock, the one states are stamped with.
// Client: the number of the newest world snapshot (a new one, a new number).
uint32_t mp_world_seq(void);
uint32_t mp_clock_ms(void);

// A reliable, ordered message. A client's goes to the host; the host's goes
// to `slot`, or to every client with slot -1. Returns -1 when it cannot be
// sent (too big, not connected).
int mp_send(int slot, uint8_t kind, const void* data, size_t size);
// An unreliable message (at most 400 bytes): it goes once in the next update
// and may be lost or arrive out of order (for state sent again and again,
// the newest of which is all that matters). Arrives as MP_EV_MESSAGE.
int mp_send_unreliable(int slot, uint8_t kind, const void* data, size_t size);

// The next event for the game, oldest first; 0 when there is none.
int mp_poll_event(MpEvent* ev);

// What a host answers to a probe (the launcher's join check): the host
// sets a short status ("Delfino Plaza") as it plays.
void mp_set_host_status(const char* status);
// Host: its game mode for probes (a server mod's name, "squid-race"; empty:
// playing together, at most 15 characters), and whether it is a server
// rather than a player (probes then count the players without it).
void mp_set_host_mode(const char* mode, int dedicated);
// Host: the mod API a joining game must carry at least (major << 8 | minor; 0: any), the server mods' need.
void mp_set_min_api(uint16_t api);
// A player's game version, launcher and mod API as their game said when it joined (host; the host's own for
// slot 0); "" and 0 when not known.
void mp_player_version(int slot, char* version, int versionSize, char* launcher, int launcherSize, uint16_t* api);
// Versions "2026.10.18.1" compared a number at a time (a missing one counts as 0): <0, 0 or >0.
int mp_version_compare(const char* a, const char* b);
// Host: whether a game of `build`, `version`, `launcher`, `api` may join under `cfg` and the server mods' minimum
// `minApi`: NULL, or why not (for the player).
const char* mp_join_refusal(const MpConfig* cfg, uint16_t minApi, uint16_t protocol, const char* build,
                            const char* version, const char* launcher, uint16_t api, char* why, int whySize);
// Host: whether players may join now (they are refused, and probes answer
// "not open", while not); yes until set.
void mp_set_accepting(int accepting);

#ifdef __cplusplus
}
#endif

#endif
