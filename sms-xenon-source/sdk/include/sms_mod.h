/* sms_mod.h: the native mod API of the Super Mario Sunshine PC port.
 *
 * A native mod is a shared library (.so, .dll, .dylib) in its own folder
 * under mods/, beside a mod.ini that names it (docs/MODDING.md). The game
 * loads every such mod at start and calls its sms_mod_load with an SmsApi:
 * tables of functions to hear of the game's events, read and change the
 * game, draw over it, read the controllers and keyboard, keep settings, and
 * talk to other mods. A mod needs nothing but this header to build: it links
 * against nothing of the game's, so one build of a mod runs with every build
 * of the game that has the same major API version.
 *
 * Rules that keep several mods working together:
 *   - Every function is called on the game thread, inside the game's own
 *     steps (the log is the one function any thread may call). Do the slow
 *     work on threads of your own and hand results over.
 *   - Events reach each mod in priority order; an event's documentation says
 *     whether a handler may change its payload, cancel it, or consume it.
 *   - Draw only inside SMS_EV_DRAW. Take screen space from draw->hud_slot, so
 *     panels of different mods stack instead of overlapping.
 *   - Take the controllers and keyboard (input->capture) only while a menu of
 *     yours is open, and bind keys through input->bind_key, which reports
 *     clashes with the port's keys and other mods'.
 *   - Pointers the game hands you (SmsObject, payload pointers) are good for
 *     the call, and objects until the stage changes. Copy what you keep.
 *
 * Compatibility: the tables only grow at their ends, and each starts with its
 * size as the game built it, so a mod built against a newer minor version
 * checks a function is there with SMS_API_HAS before calling it. A new major
 * version is an incompatible change; the game refuses mods of another major.
 *
 * Pre-releases: v1.6.0-beta.1, -beta.2, ... lead to v1.6.0. A beta has the
 * next minor version and SMS_MOD_API_PRERELEASE its number; what a beta added
 * may still change before the release, so a mod built against one is rebuilt
 * against the release (the game warns of a mod built against another beta).
 */
#ifndef SMS_MOD_H
#define SMS_MOD_H

#include <stddef.h>
#include <stdint.h>

#define SMS_MOD_API_MAJOR 1
#define SMS_MOD_API_MINOR 6
/* 0 in a release; N in the pre-release vMAJOR.MINOR.0-beta.N (API 1.5) */
#define SMS_MOD_API_PRERELEASE 1
#define SMS_MOD_API_VERSION ((uint32_t)SMS_MOD_API_MAJOR << 16 | SMS_MOD_API_MINOR)

#if defined(_WIN32)
#define SMS_MOD_EXPORT __declspec(dllexport)
#else
#define SMS_MOD_EXPORT __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
#define SMS_MOD_EXTERN_C extern "C"
#else
#define SMS_MOD_EXTERN_C
#endif
#if defined(__GNUC__) || defined(__clang__)
#define SMS_MOD_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define SMS_MOD_PRINTF(f, a)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ basics */

typedef struct SmsMod SmsMod;     /* one loaded mod: yours is passed to sms_mod_load */
typedef struct SmsApi SmsApi;
typedef void* SmsObject;          /* a game object; good until the stage changes */
typedef uint32_t SmsImage;        /* an image loaded with draw->image_load; 0 = none */
typedef uint32_t SmsColor;        /* 0xRRGGBBAA */

#define SMS_RGBA(r, g, b, a) \
	((SmsColor)(((uint32_t)(r) & 0xFF) << 24 | ((uint32_t)(g) & 0xFF) << 16 | ((uint32_t)(b) & 0xFF) << 8 | ((uint32_t)(a) & 0xFF)))
#define SMS_RGB(r, g, b) SMS_RGBA(r, g, b, 255)

typedef struct SmsVec3 {
	float x, y, z;
} SmsVec3;

typedef struct SmsRect {
	float x, y, w, h;
} SmsRect;

/* Is `member` in this table (the game is new enough to have it)? Plain C and
   C++, no compiler extensions: the member's offset is taken from its address. */
#define SMS_API_HAS(table, member) \
	((table) && \
	 (size_t)(table)->size >= (size_t)((const char*)&(table)->member - (const char*)(table)) + sizeof((table)->member) && \
	 (table)->member)

typedef enum SmsLogLevel {
	SMS_LOG_DEBUG = 0, /* shown with SMS_NATIVE_MODS_DEBUG=1 */
	SMS_LOG_INFO  = 1,
	SMS_LOG_WARN  = 2,
	SMS_LOG_ERROR = 3
} SmsLogLevel;

/* ------------------------------------------------------------------ events */

typedef enum SmsEventType {
	/* Top of every frame of the game's loop: title, file select, stages and
	   movies alike. Payload SmsEvFrame. */
	SMS_EV_FRAME = 1,
	/* Every simulation tick of a stage, after everything moved and before
	   anything is posed for drawing: the place to move things. Not while
	   the pause menu, the map or a save screen is up. SmsEvTick. */
	SMS_EV_TICK = 2,
	/* Once a frame, after SMS_EV_FRAME: build what you draw over the game
	   (the draw table). SmsEvDraw. */
	SMS_EV_DRAW = 3,
	/* A stage's objects are loaded and set up. SmsEvStage. */
	SMS_EV_STAGE_LOADED = 4,
	/* The game asks to leave for another area (pipes, paintings, scripts).
	   Cancellable: cancel to stay. SmsEvStageExit. */
	SMS_EV_STAGE_EXIT_REQUEST = 5,
	/* The stage is about to change (every way out). The payload's area and
	   episode may be changed: the game goes there instead. SmsEvStage. */
	SMS_EV_STAGE_LEAVING = 6,
	/* Mario lost a life and has lives left; area/episode is where he goes
	   next and may be changed. SmsEvStage. */
	SMS_EV_LIFE_LOST = 7,
	/* Mario lost his last life. Cancellable: cancel to carry on as after any
	   other lost life instead of the game over screen. No payload. */
	SMS_EV_GAME_OVER = 8,
	/* Losing a life is about to reset the coins. Cancellable: cancel to keep
	   them. No payload. */
	SMS_EV_COINS_RESET = 9,
	/* A file was chosen at file select; area/episode is where it starts and
	   may be changed. SmsEvStage. */
	SMS_EV_FILE_SELECTED = 10,
	/* An item (coin, blue or red coin, 1-up, ...) was picked up.
	   SmsEvItem. */
	SMS_EV_ITEM_COLLECTED = 11,
	/* The controllers were read, before the game sees them. The pads may be
	   changed (scripted play, input remapping). SmsEvPadRead. */
	SMS_EV_PAD_READ = 12,
	/* A key went down or up in the game's window. Consumable: consume it and
	   neither later mods nor the game's controls see it. SmsEvKey. */
	SMS_EV_KEY = 13,
	/* A camera cutscene was asked for. SmsEvCutscene. */
	SMS_EV_CUTSCENE = 14,
	/* Goop is about to be cleaned at a place (water, Mario's feet).
	   SmsEvGoop. */
	SMS_EV_GOOP_CLEANED = 15,
	/* The player, or their water, hit an object (sprayed, stomped, punched,
	   ground-pounded). Cancellable: cancel and the object never hears of
	   it. SmsEvHit. */
	SMS_EV_OBJECT_HIT = 16,
	/* A save state was asked for. Cancellable: cancel to refuse; set the
	   payload's reason for the message shown. SmsEvSaveState. */
	SMS_EV_SAVE_STATE_REQUEST = 17,
	/* Mario's models are about to be drawn. Consumable: consume to leave
	   them out (draw the player your own way). SmsEvObject. */
	SMS_EV_HIDE_PLAYER_MODEL = 18,
	/* Mario's status (his action: running, jumping, swimming...) is about
	   to change. `to` and `arg` may be changed. Cancellable: cancel and he
	   stays as he is. SmsEvPlayerStatus. */
	SMS_EV_PLAYER_STATUS = 19,
	/* Mario is about to lose health (or air, under water). `amount` may be
	   changed. Cancellable: cancel and he loses nothing. SmsEvDamage. */
	SMS_EV_PLAYER_DAMAGE = 20,
	/* A Shine was collected for the first time on this file. SmsEvShine. */
	SMS_EV_SHINE_COLLECTED = 21,
	/* The game camera placed itself for this tick and is about to make its
	   matrices: position, target, field of view may be changed (a
	   first-person camera, a field of view setting). SmsEvCamera. */
	SMS_EV_CAMERA = 22,
	/* An object is about to move this tick (after = 0) or has (after = 1);
	   each "before" has its "after", nested as objects move their parts.
	   Many of them: subscribe only when you need it. SmsEvObjectMove. */
	SMS_EV_OBJECT_MOVE = 23,
	/* Another mod finished loading (or this event is replayed for each mod
	   that loaded before you subscribed). SmsEvMod. */
	SMS_EV_MOD_LOADED = 24,
	/* The game is closing. No payload. */
	SMS_EV_SHUTDOWN = 25,
	/* Multiplayer (api->net). A player joined the game (SmsEvNetPlayer).
	   Every game hears of it; a server mod greets them here. */
	SMS_EV_NET_PLAYER_JOINED = 26,
	/* A player left (SmsEvNetPlayer). */
	SMS_EV_NET_PLAYER_LEFT = 27,
	/* Host: a player arrived in the stage it plays (SmsEvNetPlayer): just
	   joined, after a lost life's reload, or following the host to a new
	   stage. Sticky commands have been replayed to them by then. */
	SMS_EV_NET_PLAYER_ARRIVED = 28,
	/* Host: it opened a new stage session, which every player follows
	   (SmsEvNetSession). Sticky commands of the last session are gone. */
	SMS_EV_NET_SESSION = 29,
	/* API 1.4. The stage's opening is over (its camera cutscene, the
	   episode's name, "Go!") and the player has the controls: the first
	   tick of play. SmsEvStage: the stage, and how it was reached (reason). */
	SMS_EV_STAGE_START = 30,
	/* API 1.5. FLUDD is about to change nozzle, or a nozzle's box is about
	   to be set up as the stage loads (SmsEvPlayerNozzle, SmsNozzleReason).
	   Only the player's Mario. Cancellable: the nozzle stays as it is (a
	   box is left out of the stage). */
	SMS_EV_PLAYER_NOZZLE = 31,

	/* API 1.5: events for every kind of mod. Those of Mario are only
	   for the player's Mario (not Shadow Mario); actors are the stage's
	   enemies, items and other objects (SmsObject, with their actor type). */

	/* Mario's status (his action) changed: `from` the one he had, `to` the
	   one he has now (after the game's own rewrites), `arg` its argument;
	   `forced` 0. SMS_EV_PLAYER_STATUS asks; this tells. SmsEvPlayerStatus. */
	SMS_EV_PLAYER_STATUS_CHANGED = 32,
	/* Mario jumped: a jump, second or triple jump, back flip, side flip,
	   long jump, wall jump, spin jump, dive, a stomp's bounce, the rocket
	   nozzle... SmsEvPlayerJump. */
	SMS_EV_PLAYER_JUMP = 33,
	/* Mario landed on the ground after being in the air, from how high.
	   SmsEvPlayerLand. */
	SMS_EV_PLAYER_LAND = 34,
	/* Mario is about to gain health (a coin, a heart, a 1-up, a Shine) or,
	   under water, air. `amount` may be changed. Cancellable: cancel and he
	   gains nothing. SmsEvDamage (its `source` NULL). */
	SMS_EV_PLAYER_HEAL = 35,
	/* Mario lost his life (before SMS_EV_LIFE_LOST or SMS_EV_GAME_OVER, as
	   he falls), and why. SmsEvPlayerDeath. */
	SMS_EV_PLAYER_DEATH = 36,
	/* Mario is about to be put at the stage's start, as it loads: where and
	   facing which way may be changed. SmsEvPlayerSpawn. */
	SMS_EV_PLAYER_SPAWN = 37,
	/* Something touched or hit Mario: an enemy's attack, fire, a coin or an
	   item taken, a Blooper ridden. Cancellable: cancel and he ignores it
	   (no damage, no pickup). SmsEvHit: `object` Mario, `sender` the other. */
	SMS_EV_PLAYER_HIT = 38,
	/* Mario got on or off Yoshi or a Blooper. SmsEvPlayerMount. */
	SMS_EV_PLAYER_MOUNT = 39,
	/* Mario went into the water or came out of it. SmsEvPlayerWater. */
	SMS_EV_PLAYER_WATER = 40,
	/* Yoshi hatched from his egg (the fruit it was given). SmsEvYoshi. */
	SMS_EV_YOSHI_HATCH = 41,
	/* Yoshi ate a fruit (his colour may change). SmsEvYoshi. */
	SMS_EV_YOSHI_EAT = 42,
	/* An actor came into the stage while it is played: an enemy placed (or
	   placed again) by its generator, an item made to appear (a coin out of
	   a box, a 1-up). Not the actors the stage loads with. SmsEvActor. */
	SMS_EV_ACTOR_SPAWN = 43,
	/* The player hit an enemy and it lost health (sprayed, stomped,
	   ground-pounded, punched). SmsEvActorDamage. */
	SMS_EV_ACTOR_DAMAGE = 44,
	/* An enemy was defeated (as it leaves its coins), one of the bosses
	   docs/MODDING.md names beaten, or an enemy killed by the stage (a pit:
	   `forced` 1). SmsEvActor: `other` the last of the player's hits on it,
	   or NULL. */
	SMS_EV_ACTOR_DEATH = 45,
	/* A Shine appears during play: an event's, a boss's, the red coins'.
	   SmsEvShineActor. */
	SMS_EV_SHINE_APPEAR = 46,
	/* Mario took a Shine: every time, a Shine the file has too (before its
	   Shine Get; SMS_EV_SHINE_COLLECTED comes when a new one is counted).
	   SmsEvShineActor. */
	SMS_EV_SHINE_GET = 47,
	/* A save-file flag (game->flag_get's) is about to change: coins, lives,
	   Shines, blue coins, red coins, events... `value` may be changed.
	   Cancellable: cancel and it stays. Not when a file is loaded or the
	   stage's flags reset. SmsEvFlag. */
	SMS_EV_FLAG_CHANGED = 48,
	/* A sound effect is about to play (every one the game plays: Mario's
	   voice, footsteps, enemies, menus). `id` may be changed. Cancellable:
	   cancel and it is not played. Many a frame: subscribe only when you
	   need it. SmsEvSound. */
	SMS_EV_SOUND = 49,
	/* Music is about to start (`start` 1: `id` may be changed; cancel and
	   it is not played) or stop (`start` 0). SmsEvMusic. */
	SMS_EV_MUSIC = 50,
	/* A talk with an NPC (or a sign) starts, shows a message (`message` may
	   be changed), or ends. SmsEvTalk. */
	SMS_EV_TALK = 51,
	/* The pause menu, the map or a save screen opened (`paused` 1) or closed
	   (0): the stage stands still meanwhile (no SMS_EV_TICK). SmsEvPause. */
	SMS_EV_PAUSE = 52,
	/* The game mode (SmsGameMode) changed: the title, a stage, a movie.
	   SmsEvMode. */
	SMS_EV_MODE_CHANGED = 53,
	/* A camera cutscene (SMS_EV_CUTSCENE's) is over. SmsEvCutsceneEnd. */
	SMS_EV_CUTSCENE_END = 54,
	/* The game was saved: the pause menu's save, the save prompt, auto
	   save. SmsEvGameSaved. */
	SMS_EV_GAME_SAVED = 55,
	/* The HUD's timer (a timed event's, the red coins') started, stopped,
	   or ran out. SmsEvTimer. */
	SMS_EV_TIMER = 56,
	/* A stage's event script calls one of the game's functions by name
	   ("startTimer", "setNextStage", "appearShine"...). Many a frame: the
	   scripts poll. SmsEvScriptCall. */
	SMS_EV_SCRIPT_CALL = 57,
	/* One of the stage's map events is set off: its goop was cleaned and a
	   building rises, a gate opens. SmsEvObject: the event's object
	   (game->object_name names it). */
	SMS_EV_MAP_EVENT = 58,
	/* The stage is about to ask the player to save (after a Shine, a blue
	   coin, a nozzle's box, Yoshi). Cancellable: cancel and it does not ask
	   (save yourself, or not); the stage goes on as after the prompt.
	   SmsEvSavePrompt. */
	SMS_EV_SAVE_PROMPT = 59,
	/* (API 1.5) A stage is about to load, its episode chosen: every
	   stage the game loads, as SMS_EV_STAGE_LOADED is sent for, but before it
	   loads. After a world's episode select (the episode picked), after the
	   game picks the episode itself (the plaza, Hotel Delfino, Pinna Park and
	   Casino Delfino with SMS_EPISODE_SELECT), on a lost life's reload, a warp,
	   a file chosen. Changeable: another area/episode and that stage loads
	   instead; from_area/from_episode and Mario comes from there. Once a load,
	   before the movie some stages open with (that stage's: the one that
	   loads). SmsEvStageLoading. */
	SMS_EV_STAGE_LOADING = 60,
	/* (API 1.5) Host: a player's FLUDD water hit another player, as
	   the shooter's game saw it (only while SMS_RULE_WATER_HITS is in force:
	   docs/MODDING.md, "Water fights"). The drops that hit since the last
	   report, about ten reports a second while they keep hitting.
	   SmsEvNetSplash. */
	SMS_EV_NET_PLAYER_SPLASHED = 61,
	/* (API 1.5) A save file was loaded or started anew at file
	   select, saved (the pause menu's save, the save prompt, auto save),
	   copied or erased. Its files (files->*, scope 0..2) are as the card has
	   them now. SmsEvSaveFile. */
	SMS_EV_SAVE_FILE = 62,
	/* (API 1.5) FLUDD's trigger is about to fire: a burst (A while R
	   is held, with the spray) or the pressure built up (the rocket, hover
	   and turbo nozzles). Only the player's Mario. Cancellable: cancel and it
	   does not fire (R is pressed again to try again). SmsEvNozzleFire. */
	SMS_EV_NOZZLE_FIRE = 63,
	/* (API 1.5) A Shine is made, as its stage loads: its model may be
	   one of the mod's (`model`). SmsEvShineSetup. */
	SMS_EV_SHINE_SETUP = 64,
	/* (API 1.5) Whether Yoshi's egg is there: an egg the stage
	   places, or the plaza's egg event. Changeable (`present`).
	   SmsEvYoshiEgg. */
	SMS_EV_YOSHI_EGG = 65,
	/* (API 1.5) Which Shine an episode stands for, as the episode
	   select and the pause menu show it: its name, and whether it is shown
	   collected (the episode select's marks, and so how many episodes it
	   offers). Changeable (`shine`). Many a time: the select asks as its
	   cursor moves. SmsEvEpisodeShine. */
	SMS_EV_EPISODE_SHINE = 66,
	/* (API 1.5) A file chosen at file select is about to start (the
	   game leaves for its first stage): after SMS_EV_SAVE_FILE's LOADED or
	   NEW, again at each step of the file select (more than once a frame;
	   time a wait with core->time) while a mod cancels it, then
	   SMS_EV_FILE_SELECTED once. Cancellable: cancel and the file select
	   waits as it is, this step (a mod's own screen first: a game's options,
	   a seed). SmsEvFileStarting. */
	SMS_EV_FILE_STARTING = 67,
	/* (API 1.6) A save file's menu at file select (START, COPY, ERASE,
	   SCORE; the player jumped under its block) opened, ready for the
	   player's choice, or closed: an item chosen, or B. Before the game
	   acts on the choice, after its sound. SmsEvFileMenu. */
	SMS_EV_FILE_MENU = 68,

	/* core->event_id hands out ids from here for mods' own events. */
	SMS_EV_CUSTOM_BASE = 0x10000
} SmsEventType;

typedef struct SmsEvent {
	uint32_t type;            /* SmsEventType, or a custom id */
	uint32_t size;            /* of *data, as the game built it */
	void* data;               /* the payload for the type, or NULL */
	int cancelled;            /* an earlier handler cancelled it */
	const char* cancelled_by; /* that mod's id */
} SmsEvent;

/* A handler's answer. */
#define SMS_CONTINUE 0
#define SMS_CANCEL   1 /* cancellable events: veto; consumable events: consume */

typedef int (*SmsEventFn)(SmsMod* mod, SmsEvent* ev, void* user);

/* A script runtime's start and stop (core->host_runtime, API 1.1): `script`
   is a mod whose mod.ini says `runtime = <name>`; start runs it (its file is
   mod_dir(script) + "/" + mod_info(script, "script")) and returns 0, or
   refuses; stop ends it. Every API call made for the script passes `script`,
   so it has its own settings, options, keys and HUD slots. */
typedef int (*SmsScriptStartFn)(SmsMod* host, SmsMod* script, void* user);
typedef void (*SmsScriptStopFn)(SmsMod* host, SmsMod* script, void* user);

/* Priorities: lower runs first. Handlers of the same priority run in the
   order the mods loaded. Use MONITOR for handlers that only watch the
   outcome (they run last and must not change anything). */
#define SMS_PRIORITY_FIRST   (-1000)
#define SMS_PRIORITY_EARLY   (-100)
#define SMS_PRIORITY_NORMAL  0
#define SMS_PRIORITY_LATE    100
#define SMS_PRIORITY_MONITOR 1000

typedef enum SmsGameMode {
	SMS_MODE_BOOT = 0,     /* starting, logos */
	SMS_MODE_TITLE = 1,    /* title screen */
	SMS_MODE_MENU = 2,     /* file select and other menus */
	SMS_MODE_STAGE = 3,    /* a stage is being played */
	SMS_MODE_MOVIE = 4,    /* a full-screen movie */
	SMS_MODE_OTHER = 5
} SmsGameMode;

typedef struct SmsEvFrame {
	uint32_t frame; /* frames since start */
	int mode;       /* SmsGameMode */
} SmsEvFrame;

typedef struct SmsEvTick {
	uint32_t tick;        /* ticks since the stage loaded */
	float dt;             /* seconds per tick (1 / ticks_per_second) */
	int ticks_per_second; /* 30, 60 or 120 (the frame rate setting) */
} SmsEvTick;

typedef struct SmsEvDraw {
	float canvas_w, canvas_h; /* draw->canvas: the game picture, 480 high */
	uint32_t frame;
	int mode;                 /* SmsGameMode */
} SmsEvDraw;

typedef struct SmsEvStage {
	int area, episode; /* the stage: see docs/MODDING.md for the area numbers */
	/* API 1.4 (check ev->size): why the game leaves (SMS_EV_STAGE_LEAVING,
	   SMS_EV_LIFE_LOST, SMS_EV_FILE_SELECTED) or came (SMS_EV_STAGE_LOADED,
	   SMS_EV_STAGE_START): SMS_LEAVE_*. For SMS_LEAVE_SHINE, the Shine
	   (0..119); otherwise, or not known, -1. */
	int reason;
	int shine;
} SmsEvStage;

/* SmsEvStage.reason (API 1.4) */
typedef enum SmsLeaveReason {
	SMS_LEAVE_OTHER = 0,     /* none of these: the title, a movie's way on */
	SMS_LEAVE_WAY_OUT = 1,   /* the stage's own way out: a pipe, a painting, a door, an event */
	SMS_LEAVE_SHINE = 2,     /* a Shine ended the stage; its Shine Get and its save are over */
	SMS_LEAVE_EXIT_AREA = 3, /* the pause menu's Exit Area */
	SMS_LEAVE_LIFE_LOST = 4, /* Mario lost a life */
	SMS_LEAVE_GAME_OVER = 5, /* Mario lost his last life */
	SMS_LEAVE_WARP = 6,      /* a mod's game->warp */
	SMS_LEAVE_FILE = 7       /* a file was chosen at file select */
} SmsLeaveReason;

/* SMS_EV_STAGE_LOADING (API 1.5). Its first fields are SmsEvStage's. */
typedef struct SmsEvStageLoading {
	/* The stage about to load. Changeable: that one loads instead, as if the
	   game had chosen it (a world's episode is recorded as its episode select
	   records it, and a new visit's coins start afresh). An area and episode
	   the game has no stage for are not taken; nor is the title's (area 15),
	   to or from. */
	int area, episode;
	int reason; /* SMS_LEAVE_*: how the last stage was left (SmsEvStage.reason) */
	int shine;  /* for SMS_LEAVE_SHINE, the Shine (0..119); otherwise -1 */
	/* Where the game has Mario come from: the stage before; after a world's
	   episode select, that world with SMS_EPISODE_SELECT. The plaza starts
	   him where that world's way in is (its painting, its pipe, the boat),
	   and a stage reads it for how he arrives. Changeable (an area 0..254,
	   an episode 0..255). */
	int from_area, from_episode;
} SmsEvStageLoading;

/* SmsEvStage.episode: no episode named; for a world's way in from the plaza
   (SMS_AREA_WORLD), its episode select chooses. */
#define SMS_EPISODE_SELECT 255

typedef struct SmsEvStageExit {
	uint32_t code;     /* the game's (area + 1) << 8 | episode request */
	int area, episode; /* decoded from code */
	SmsObject actor;   /* what asked (a pipe, a painting), or NULL */
} SmsEvStageExit;

typedef enum SmsItemKind {
	SMS_ITEM_OTHER = 0,
	SMS_ITEM_COIN = 1,
	SMS_ITEM_RED_COIN = 2,
	SMS_ITEM_BLUE_COIN = 3,
	SMS_ITEM_ONE_UP = 4
} SmsItemKind;

typedef struct SmsEvItem {
	SmsObject item, taker;
	int kind;            /* SmsItemKind */
	uint32_t actor_type; /* the game's actor type id */
	SmsVec3 position;
} SmsEvItem;

/* The GameCube controller as the game reads it (PADStatus). */
typedef struct SmsPadStatus {
	uint16_t buttons; /* SMS_PAD_* */
	int8_t stick_x, stick_y, cstick_x, cstick_y;
	uint8_t trigger_l, trigger_r, analog_a, analog_b;
	int8_t err;       /* 0 = connected */
} SmsPadStatus;

#define SMS_PAD_LEFT  0x0001
#define SMS_PAD_RIGHT 0x0002
#define SMS_PAD_DOWN  0x0004
#define SMS_PAD_UP    0x0008
#define SMS_PAD_Z     0x0010
#define SMS_PAD_R     0x0020
#define SMS_PAD_L     0x0040
#define SMS_PAD_A     0x0100
#define SMS_PAD_B     0x0200
#define SMS_PAD_X     0x0400
#define SMS_PAD_Y     0x0800
#define SMS_PAD_START 0x1000

typedef struct SmsEvPadRead {
	SmsPadStatus* pads; /* [4], changeable */
} SmsEvPadRead;

typedef struct SmsEvKey {
	int scancode; /* SDL scancode (USB HID usage): input->key_from_name("F3") */
	int down;     /* 1 pressed, 0 released */
	int repeat;   /* held down: a key repeat */
	int modifiers;/* SMS_KEYMOD_* */
} SmsEvKey;

#define SMS_KEYMOD_SHIFT 1
#define SMS_KEYMOD_CTRL  2
#define SMS_KEYMOD_ALT   4

typedef struct SmsEvCutscene {
	const char* name; /* the camera's name */
	int has_position;
	SmsVec3 position; /* the point it centres on */
	int frames;
	SmsObject actor;
} SmsEvCutscene;

typedef struct SmsEvGoop {
	SmsVec3 position;
	float size;
} SmsEvGoop;

typedef struct SmsEvHit {
	SmsObject object, sender;
	uint32_t message; /* the game's HIT_MESSAGE_* (see docs/MODDING.md) */
} SmsEvHit;

typedef struct SmsEvSaveState {
	const char* reason; /* set when cancelling: shown to the player */
} SmsEvSaveState;

typedef struct SmsEvObject {
	SmsObject object;
} SmsEvObject;

typedef struct SmsEvPlayerStatus {
	uint32_t from;     /* the status he has */
	uint32_t to, arg;  /* the status asked for, and its argument; changeable */
	int forced;
} SmsEvPlayerStatus;

typedef struct SmsEvDamage {
	int amount;     /* changeable */
	int underwater; /* air is lost (or gained), not health */
	/* API 1.5 (check ev->size): what hurts him, the enemy or object
	   that hit him; NULL for the ground's damage (fire, goop, a fall), his
	   air under water, and SMS_EV_PLAYER_HEAL. */
	SmsObject source;
} SmsEvDamage;

/* SmsEvPlayerNozzle.reason (API 1.5) */
typedef enum SmsNozzleReason {
	/* A switch: a nozzle picked up, getting on or off Yoshi (his juice is
	   nozzle 3), the game's own (a cutscene, a stage's script), or
	   player->set_nozzle. */
	SMS_NOZZLE_SWITCH = 0,
	/* The nozzle button: FLUDD swaps to its second nozzle (`to`), or back to
	   the spray (`to` 0). */
	SMS_NOZZLE_SWAP = 1,
	/* A nozzle's box as the stage loads (`to` is its nozzle, `box` the box).
	   Cancel: the box is not in the stage. */
	SMS_NOZZLE_BOX = 2,
	/* A stage that carries FLUDD's nozzle over (Hotel Delfino) gives it the
	   second nozzle the file remembers (`to`), the spray in front; Bowser's
	   stage gives the rocket. Cancel: it keeps the ones it has. */
	SMS_NOZZLE_STAGE_START = 3
} SmsNozzleReason;

typedef struct SmsEvPlayerNozzle {
	int from;      /* the nozzle FLUDD has now (docs/MODDING.md, Nozzles); -1 for a box */
	int to;        /* the nozzle asked for; changeable, except for a box and
	                  the swap back to the spray (0 spray, 1 rocket,
	                  2 underwater, 4 hover, 5 turbo; 3, Yoshi's, only to
	                  get on him). Another number is not taken. */
	int animate;   /* the change is shown */
	int reason;    /* SmsNozzleReason */
	SmsObject box; /* SMS_NOZZLE_BOX: the box; else NULL */
} SmsEvPlayerNozzle;

/* SmsEvPlayerJump (API 1.5) */
typedef struct SmsEvPlayerJump {
	uint32_t status;   /* the jump (docs/MODDING.md, Mario statuses): 0x02000880 a jump,
	                      0x02000881 second, 0x00000882 triple, 0x00000883 back flip... */
	uint32_t from;     /* the status he jumped from */
	SmsVec3 position;
	SmsVec3 velocity;  /* units a tick, as he leaves the ground */
} SmsEvPlayerJump;

/* SmsEvPlayerLand (API 1.5) */
typedef struct SmsEvPlayerLand {
	uint32_t status;   /* the status he lands into */
	uint32_t from;     /* the one he had in the air */
	float fall;        /* how far he fell from the highest point of the jump or fall */
	float speed;       /* how fast he was falling, units a tick (positive) */
	SmsVec3 position;
} SmsEvPlayerLand;

/* SmsEvPlayerDeath.cause (API 1.5) */
typedef enum SmsDeathCause {
	SMS_DEATH_OTHER = 0,    /* a stage's script (a timed event failed), the coaster */
	SMS_DEATH_HEALTH = 1,   /* no health left: an enemy, fire, a hard fall */
	SMS_DEATH_DROWN = 2,    /* no air left, under water */
	SMS_DEATH_PIT = 3,      /* a bottomless pit: the stage's floor of death */
	SMS_DEATH_GOOP = 4,     /* sinking goop, deadly goop */
	SMS_DEATH_ELECTRIC = 5, /* electric goop */
	SMS_DEATH_CRASH = 6     /* a Blooper ridden into a wall */
} SmsDeathCause;

typedef struct SmsEvPlayerDeath {
	int cause;          /* SmsDeathCause */
	SmsVec3 position;
	SmsObject source;   /* SMS_DEATH_HEALTH: what took his last health, when an enemy did; else NULL */
} SmsEvPlayerDeath;

/* SmsEvPlayerSpawn.kind (API 1.5): how the stage brings him in */
#define SMS_START_WAIT    0 /* standing */
#define SMS_START_ROLL    1 /* rolling in (out of a pipe, through a painting) */
#define SMS_START_RETURN  2 /* back from a world */
#define SMS_START_COASTER 4 /* Pinna Park's coaster (position and yaw are not taken) */

typedef struct SmsEvPlayerSpawn {
	SmsVec3 position;  /* changeable */
	float yaw;         /* degrees, as SmsPlayer's; changeable */
	int has_position;  /* 0: the stage puts him where its scene file does (position is
	                      that); set 1 with a position to put him somewhere */
	/* SMS_START_*. Changeable: WAIT, ROLL or RETURN starts
	   him that way; a COASTER start changed to one of them is a stage start
	   like any other's, without the coaster (none is changed to COASTER). */
	int kind;
} SmsEvPlayerSpawn;

/* SmsEvPlayerMount.mount (API 1.5) */
#define SMS_MOUNT_YOSHI   0
#define SMS_MOUNT_BLOOPER 1

typedef struct SmsEvPlayerMount {
	int mount;         /* SMS_MOUNT_* */
	int on;            /* 1 got on, 0 got off */
	SmsObject object;  /* the Blooper as he gets on; NULL for Yoshi (not an object) */
	int color;         /* Yoshi's colour (SMS_YOSHI_*); a Blooper: 0 red, 1 yellow, 2 green; -1 off */
} SmsEvPlayerMount;

typedef struct SmsEvPlayerWater {
	int entered;       /* 1 into the water, 0 out of it */
	float surface_y;   /* the water's surface */
	SmsVec3 position;
} SmsEvPlayerWater;

/* Yoshi's colours (API 1.5) */
#define SMS_YOSHI_GREEN  0
#define SMS_YOSHI_ORANGE 1
#define SMS_YOSHI_PURPLE 2
#define SMS_YOSHI_PINK   3

typedef struct SmsEvYoshi {
	uint32_t fruit;    /* the fruit's actor type (0x40000390..0x40000395), 0 not known */
	int color;         /* his colour now, SMS_YOSHI_* */
	SmsVec3 position;
} SmsEvYoshi;

/* SMS_EV_ACTOR_SPAWN, SMS_EV_ACTOR_DEATH (API 1.5) */
typedef struct SmsEvActor {
	SmsObject object;
	uint32_t actor_type; /* the game's actor type (SMS_ACTOR_* bits) */
	SmsVec3 position;
	SmsObject other;     /* SMS_EV_ACTOR_DEATH: the last of the player's hits on it, or NULL */
	int forced;          /* SMS_EV_ACTOR_DEATH: killed by the stage (a pit), not defeated */
} SmsEvActor;

/* Actor type bits (SmsEvActor.actor_type, game->object_type) */
#define SMS_ACTOR_PLAYER 0x80000000u
#define SMS_ACTOR_OBJECT 0x40000000u /* map objects and NPCs */
#define SMS_ACTOR_ITEM   0x20000000u /* coins, Shines, fruit, nozzles */
#define SMS_ACTOR_ENEMY  0x10000000u
#define SMS_ACTOR_BOSS   0x08000000u

typedef struct SmsEvActorDamage {
	SmsObject object;    /* the enemy */
	SmsObject attacker;  /* Mario, or his water */
	uint32_t actor_type;
	uint32_t message;    /* the hit (HIT_MESSAGE_*, see docs/MODDING.md) */
	int health_before;
	int health;          /* after the hit */
	int max_health;
	SmsVec3 position;
} SmsEvActorDamage;

/* SMS_EV_SHINE_APPEAR, SMS_EV_SHINE_GET (API 1.5) */
typedef struct SmsEvShineActor {
	SmsObject object;
	int shine;          /* its number (0..119; Eclipse's own beyond), -1 none */
	int had;            /* the file has it already */
	SmsVec3 position;
} SmsEvShineActor;

/* SmsEvSaveFile.what (API 1.5) */
#define SMS_SAVE_FILE_LOADED 0 /* chosen at file select, read from the card */
#define SMS_SAVE_FILE_NEW    1 /* chosen at file select, empty: a new game */
#define SMS_SAVE_FILE_SAVED  2
#define SMS_SAVE_FILE_COPIED 3 /* `from` copied over `file` */
#define SMS_SAVE_FILE_ERASED 4

typedef struct SmsEvSaveFile {
	int what;  /* SMS_SAVE_FILE_* */
	int file;  /* 0..2 (A, B, C) */
	int from;  /* SMS_SAVE_FILE_COPIED: the file copied; otherwise -1 */
} SmsEvSaveFile;

typedef struct SmsEvFileStarting {
	int file;   /* 0..2 (A, B, C); -1 a game without a memory card */
	int is_new; /* 1 a new game (SMS_SAVE_FILE_NEW), 0 a file loaded */
} SmsEvFileStarting;

/* SmsEvFileMenu.item (API 1.6): the file menu's items, in order */
#define SMS_FILE_MENU_START 0
#define SMS_FILE_MENU_COPY  1
#define SMS_FILE_MENU_ERASE 2
#define SMS_FILE_MENU_SCORE 3

typedef struct SmsEvFileMenu {
	int file;     /* 0..2 (A, B, C) */
	int is_empty; /* 1 the file has no game ("New"): the menu offers START alone */
	int open;     /* 1 opened, 0 closed */
	int item;     /* opened: the item the cursor is on (START); closed: the
	                 item chosen (SMS_FILE_MENU_*), -1 closed without a
	                 choice (B, or the memory card taken out) */
} SmsEvFileMenu;

/* SmsEvNozzleFire.how (API 1.5) */
#define SMS_FIRE_BURST    0 /* A while R is held */
#define SMS_FIRE_PRESSURE 1 /* R held until the pressure is full */

typedef struct SmsEvNozzleFire {
	int nozzle; /* FLUDD's nozzle out: 0 spray, 1 rocket, 2 underwater, 4 hover, 5 turbo */
	int how;    /* SMS_FIRE_* */
} SmsEvNozzleFire;

typedef struct SmsEvShineSetup {
	SmsObject object;
	int shine;     /* its number (0..119; Eclipse's own beyond), -1 none */
	int collected; /* the file has it: the game gives it its see-through model */
	/* The model it takes: NULL, the game's. Set it to a model file (.bmd)
	   of the mod's: a path relative to the mod's folder (core->mod_dir), or
	   a full one; a model that cannot be loaded leaves the game's. The
	   Shine's animations play on it, so it keeps the Shine model's joints
	   and materials (a recoloured Shine, say). Several Shines may name the
	   same file: it is loaded once a stage. game->object_set_color colours
	   it. */
	const char* model;
} SmsEvShineSetup;

/* SmsEvYoshiEgg.kind (API 1.5) */
#define SMS_EGG_STAGE       0 /* an egg the stage places: there when the file has Yoshi's Shine (0x21; in the plaza, once Yoshi has hatched there: flag 0x1038F) */
#define SMS_EGG_PLAZA_EVENT 1 /* the plaza's egg event is set up (its episode 8): when Yoshi has not hatched yet and the file has Yoshi's Shine */
#define SMS_EGG_EVENT_EGG   2 /* the egg of the plaza's egg event: there when the event was set up (flag 0x60003 is 1) */

typedef struct SmsEvYoshiEgg {
	int kind;         /* SMS_EGG_* */
	int present;      /* changeable: 1 there (set up), 0 not */
	SmsObject object; /* the egg (SMS_EGG_STAGE, SMS_EGG_EVENT_EGG), else NULL */
} SmsEvYoshiEgg;

typedef struct SmsEvEpisodeShine {
	int area;    /* the world's area (game->state's numbers) */
	int episode; /* 0..7 */
	int shine;   /* the Shine (0..119); changeable */
} SmsEvEpisodeShine;

typedef struct SmsEvFlag {
	uint32_t id;       /* the flag (docs/MODDING.md, Flags) */
	int32_t old;       /* its value now */
	int32_t value;     /* the value it is about to take; changeable */
} SmsEvFlag;

typedef struct SmsEvSound {
	uint32_t id;       /* the game's MSD_SE_* (SMS_SOUND_* are some); changeable */
	int system;        /* a system sound, played at no place */
	int has_position;
	SmsVec3 position;  /* where it plays */
} SmsEvSound;

typedef struct SmsEvMusic {
	uint32_t id;       /* the game's MSD_BGM_* (SMS_MUSIC_* are some); changeable when starting */
	int start;         /* 1 starting, 0 stopping */
} SmsEvMusic;

/* SmsEvTalk.what (API 1.5) */
#define SMS_TALK_START   0
#define SMS_TALK_MESSAGE 1
#define SMS_TALK_END     2

typedef struct SmsEvTalk {
	int what;          /* SMS_TALK_* */
	SmsObject npc;     /* the one talked to (a sign is an NPC too) */
	uint32_t message;  /* SMS_TALK_MESSAGE: the message's id, as the stage's script asks for
	                      it; changeable (another of the stage's or the game's messages) */
	int last;          /* SMS_TALK_MESSAGE: the talk ends after this one */
} SmsEvTalk;

/* SmsEvPause.kind (API 1.5) */
#define SMS_PAUSE_MENU 0 /* the pause menu */
#define SMS_PAUSE_MAP  1 /* the map (the guide) */
#define SMS_PAUSE_SAVE 2 /* a save screen */

typedef struct SmsEvPause {
	int paused;        /* 1 opened, 0 closed (back to play) */
	int kind;          /* SMS_PAUSE_*: what opened, or what closed */
} SmsEvPause;

typedef struct SmsEvMode {
	int from, to;      /* SmsGameMode */
} SmsEvMode;

typedef struct SmsEvCutsceneEnd {
	const char* name;  /* the camera's name, as SMS_EV_CUTSCENE gave it ("" an actor's) */
	SmsObject actor;
	int forced;        /* cut short (skipped, or the game ended it), not played out */
} SmsEvCutsceneEnd;

typedef struct SmsEvGameSaved {
	int file;          /* 0..2 */
} SmsEvGameSaved;

/* SmsEvTimer.what (API 1.5) */
#define SMS_TIMER_STARTED 0 /* `time` a countdown's, 0 counting up */
#define SMS_TIMER_STOPPED 1 /* `time` what it shows: a race's time */
#define SMS_TIMER_OUT     2 /* a countdown reached 0 */

typedef struct SmsEvTimer {
	int what;          /* SMS_TIMER_STARTED, _STOPPED, _OUT */
	int time;          /* hundredths of a second */
} SmsEvTimer;

typedef struct SmsEvScriptCall {
	const char* name;  /* the game function the script calls */
	SmsObject owner;   /* the script's object (the stage's event, or an actor) */
	int argc;
} SmsEvScriptCall;

/* SmsEvSavePrompt.kind (API 1.5) */
#define SMS_SAVE_BLUE_COIN 1
#define SMS_SAVE_NOZZLE    3 /* a nozzle's box opened for good (3 or 4) */
#define SMS_SAVE_YOSHI     5
#define SMS_SAVE_SHINE     6

typedef struct SmsEvSavePrompt {
	int kind;          /* SMS_SAVE_* */
} SmsEvSavePrompt;

typedef struct SmsEvShine {
	int shine;      /* the Shine's number (0..119) */
	int total;      /* Shines on the file, counting this one */
} SmsEvShine;

typedef struct SmsEvCamera {
	SmsVec3 position, target; /* changeable */
	float fovy;               /* degrees, changeable */
	float aspect;
	float roll;               /* reserved: 0 */
} SmsEvCamera;

typedef struct SmsEvObjectMove {
	SmsObject object;
	int after;
} SmsEvObjectMove;

typedef struct SmsEvNetPlayer {
	int slot;          /* 0..7; the host is 0 */
	const char* name;  /* as the player set it, or their character's */
	int character;     /* SMS_NET_CHAR_* */
} SmsEvNetPlayer;

typedef struct SmsEvNetSession {
	int area, episode;
	uint32_t session;  /* its number, for api->net->session() */
} SmsEvNetSession;

/* SMS_EV_NET_PLAYER_SPLASHED (API 1.5) */
typedef struct SmsEvNetSplash {
	int slot;          /* the player the water hit */
	int by;            /* whose water it was */
	int drops;         /* how many of their drops hit since the last report (1 or more) */
	SmsVec3 position;  /* where the last of them hit */
} SmsEvNetSplash;

typedef struct SmsEvMod {
	const char* id;
	const char* name;
	const char* version;
} SmsEvMod;

/* ------------------------------------------------------------------ tables */

typedef struct SmsCoreApi {
	uint32_t size;
	/* printf to the game's log, prefixed with the mod's id. Any thread. */
	void (*log)(SmsMod* mod, int level, const char* fmt, ...) SMS_MOD_PRINTF(3, 4);
	/* Hear of an event. Returns a handle (> 0) for unsubscribe, 0 on error. */
	int (*subscribe)(SmsMod* mod, uint32_t type, SmsEventFn fn, void* user, int priority);
	void (*unsubscribe)(SmsMod* mod, int handle);
	/* Custom events: the id for a name ("author.mod.event"); the same name
	   gives every mod the same id. */
	uint32_t (*event_id)(const char* name);
	/* Send a custom event to its subscribers; returns 1 if one cancelled. */
	int (*emit)(SmsMod* mod, uint32_t type, void* data, uint32_t size);
	/* Services: an interface (a struct of functions, say) under a name for
	   other mods. `version` is yours to bump. */
	int (*provide)(SmsMod* mod, const char* name, uint32_t version, const void* iface);
	/* The interface provided under `name` at `min_version` or later, or NULL.
	   List the providing mod in your mod.ini's `after` so it loads first. */
	const void* (*require)(SmsMod* mod, const char* name, uint32_t min_version);
	/* About mods. */
	const char* (*mod_id)(SmsMod* mod);
	const char* (*mod_dir)(SmsMod* mod);  /* the mod's folder (read only) */
	const char* (*data_dir)(SmsMod* mod); /* a folder the mod may write, made on first use */
	int (*is_loaded)(const char* id);
	/* The game build: "<commit>[-eclipse]", and pointer size (4 or 8). */
	const char* (*game_build)(void);
	int (*pointer_size)(void);
	/* Seconds since start, steady. */
	double (*time)(void);
	/* API 1.1. A value of the mod's mod.ini ("name", "version", "script",
	   or a key of the mod's own), or NULL. */
	const char* (*mod_info)(SmsMod* mod, const char* key);
	/* API 1.1. Run the script mods whose mod.ini says `runtime = <name>`
	   (the lua mod hosts "lua"). One host a runtime; script mods load after
	   every native mod, and stop before their host unloads. */
	int (*host_runtime)(SmsMod* host, const char* runtime, SmsScriptStartFn start, SmsScriptStopFn stop, void* user);
} SmsCoreApi;

typedef struct SmsGameState {
	uint32_t size;
	int mode;             /* SmsGameMode */
	int area, episode;    /* the current stage, or -1 */
	int stage_ready;      /* a stage is loaded and Mario exists */
	uint32_t frame;       /* frames since start */
	uint32_t tick;        /* ticks since the stage loaded */
	int ticks_per_second; /* 30, 60 or 120 */
	int eclipse;          /* a Super Mario Eclipse build */
	int save_file;        /* the chosen file 0..2, or -1 */
	int paused;           /* the pause menu, map or a save screen is up (no ticks) */
} SmsGameState;

/* API 1.4: game->object_info's */
typedef struct SmsObjectInfo {
	uint32_t size;      /* sizeof, set before calling */
	uint32_t type;      /* its actor type */
	char kind[32];      /* as the stage names it ("WoodBlockCone"); "" not a map object */
	SmsVec3 position, rotation, scale; /* rotation: degrees about x, y, z */
	int alive;          /* in the stage (not killed) */
	int hidden;         /* out of the stage until shown */
	int spawned;        /* a copy SMS_CMD_SPAWN made: its id; 0 the stage's own */
} SmsObjectInfo;

/* API 1.4: game->ground's */
typedef struct SmsGround {
	uint32_t size;      /* sizeof, set before calling */
	float floor;        /* the floor's height below the point */
	uint32_t floor_type;/* its collision type (TBGCheckData::mBGType, without the camera and shadow bits) */
	int water;          /* the floor is water's surface */
	int has_water;      /* there is water's surface over or at the point: water_y */
	float water_y;
	float roof;         /* the roof's height above (a large number when there is none) */
} SmsGround;

typedef struct SmsGameApi {
	uint32_t size;
	void (*state)(SmsGameState* out); /* set out->size = sizeof *out first */
	/* The save file's flags (TFlagManager): numbers in docs/MODDING.md. */
	int32_t (*flag_get)(uint32_t id);
	void (*flag_set)(uint32_t id, int32_t value);
	/* Shorthands over the flags. */
	int (*coins)(void);
	int (*lives)(void);
	int (*shines)(void);
	int (*blue_coins)(void);
	int (*has_shine)(int shine);
	/* Leave for another stage as a pipe would; 0 when no stage is played. */
	int (*warp)(int area, int episode);
	/* Objects. find_object: by the name the stage gives it, or NULL. */
	SmsObject (*find_object)(const char* name);
	const char* (*object_name)(SmsObject obj);
	/* Objects that came with events, or from find_object, that have a place
	   in the world: 1 and the position, else 0. */
	int (*object_position)(SmsObject obj, SmsVec3* out);
	int (*object_set_position)(SmsObject obj, const SmsVec3* pos);
	/* The display name of an area ("Delfino Plaza"), or "". */
	const char* (*area_name)(int area);
	/* API 1.2: the stage's trigger volumes ("cubes", SMS_CUBE_*), as the
	   game's own scripts test them: how many a set has (0 when the stage has
	   none), and whether point p is in cube `index` of a set (1 or 0; -1:
	   no such cube). */
	int (*cube_count)(int set);
	int (*in_cube)(int set, int index, const SmsVec3* p);
	/* API 1.4: the stage's objects as this game has them (on the host, the
	   copy its server mods judge by), picked as SMS_CMD_OBJECT picks them:
	   actor types lo to hi, kind ("WoodBlockCone"; "@id" a copy SMS_CMD_SPAWN
	   made; "" any). object_count: how many; object_info: the index-th of
	   them into out (out->size set first), 1, or 0 when there is none. */
	int (*object_count)(uint32_t type_lo, uint32_t type_hi, const char* kind);
	int (*object_info)(uint32_t type_lo, uint32_t type_hi, const char* kind, int index, SmsObjectInfo* out);
	/* The ground at a point, as Mario finds it: the floor below p (from a
	   little above it), whether that is water, the water's surface over p,
	   and the roof above. 0 off the stage's map. */
	int (*ground)(const SmsVec3* p, SmsGround* out);
	/* API 1.5: an object's actor type (SMS_ACTOR_* bits and its
	   number), 0 when it has none; an enemy's health (and its most, into max,
	   which may be NULL), -1 when it is not one with health. */
	uint32_t (*object_type)(SmsObject obj);
	int (*object_health)(SmsObject obj, int* max);
	/* API 1.5: hide parts of the game's own HUD in this game, in any
	   game (single player too, unlike the host-only SMS_CMD_HUD): mask is
	   SMS_HUD_* bits ORed, 0 shows them all again. The parts are left out of
	   the HUD's drawing only, so the HUD carries on as ever underneath (its
	   animations, sounds and FLUDD's messages), and a part shown again is
	   exactly as the game has it; the coin, Shine and blue coin counters'
	   sparkles go with them. Each mod's mask is its own, the HUD hides every
	   mod's, and a mod's goes when it unloads; it lasts across stages.
	   hud_hidden: the bits hidden now, whoever hides them (any mod, a server,
	   or the player's own HUD settings, where the game has them). Panels a
	   mod draws with draw->* are not affected. */
	void (*set_hud_hidden)(SmsMod* mod, uint32_t mask);
	uint32_t (*hud_hidden)(void);
	/* API 1.5: one of the colours of an object's model, in every
	   material of it (SMS_COLOR_*): its material colour (with alpha), or a
	   TEV colour register. What it changes is what the model's materials
	   make of that colour; a model's own colour animation sets it again as
	   it plays (set it every tick to keep it). The colours are the model's:
	   every object of the same model shares them (all the Shines given the
	   same model file, say). 0 when the object has no model. */
	int (*object_set_color)(SmsObject obj, int which, SmsColor color);
} SmsGameApi;

/* game->object_set_color's `which` (API 1.5) */
#define SMS_COLOR_MATERIAL 0 /* the material colour (channel 0), with alpha */
#define SMS_COLOR_TEV0     1 /* TEV colour registers 0..2 */
#define SMS_COLOR_TEV1     2
#define SMS_COLOR_TEV2     3

/* The game's cube sets (api->game->in_cube). */
typedef enum SmsCubeSet {
	SMS_CUBE_CAMERA = 0,  /* camera changes */
	SMS_CUBE_AREA = 1,    /* the stage's areas */
	SMS_CUBE_FAST_A = 2,  /* "fast cubes": what event scripts test (a course's start, its checkpoints) */
	SMS_CUBE_FAST_B = 3,
	SMS_CUBE_FAST_C = 4,
	SMS_CUBE_MIRROR = 5,
	SMS_CUBE_WIRE = 6,
	SMS_CUBE_STREAM = 7,  /* water currents */
	SMS_CUBE_SHADOW = 8,
	SMS_CUBE_SOUND_CHANGE = 9,
	SMS_CUBE_SOUND_EFFECT = 10
} SmsCubeSet;

typedef struct SmsPlayer {
	uint32_t size;       /* set to sizeof before calling player->get */
	int valid;           /* 0: no Mario now (menus, loading) */
	SmsVec3 position;
	SmsVec3 velocity;    /* units a tick */
	float forward_speed; /* units a tick along his facing */
	float yaw;           /* degrees: 0 faces +Z, 90 faces +X */
	int16_t face_angle[3];
	int health, max_health; /* 0..8 */
	int water, max_water;   /* FLUDD's tank */
	int has_fludd;
	int nozzle;          /* the second nozzle FLUDD has: 0 none, see docs */
	uint32_t status;     /* MARIO_STATUS_* */
	uint32_t status_timer;
	int on_ground, in_water, on_yoshi;
	SmsVec3 floor;       /* the ground below him */
	SmsObject object;    /* the game's TMario */
	/* API 1.5 (check size) */
	int current_nozzle;  /* the nozzle FLUDD has out now: 0 spray, 3 on Yoshi */
	int shirt, sunglasses, helmet; /* what he wears (player->set_outfit) */
} SmsPlayer;

typedef struct SmsPlayerApi {
	uint32_t size;
	int (*get)(SmsPlayer* out); /* returns out->valid */
	int (*set_position)(const SmsVec3* pos);
	int (*set_velocity)(const SmsVec3* vel);
	int (*set_forward_speed)(float speed);
	int (*set_health)(int health);
	int (*set_water)(int water);
	/* Ask for a status as the game does (it may refuse). */
	int (*set_status)(uint32_t status, uint32_t arg);
	/* API 1.5. FLUDD switches to a nozzle (0 spray, 1 rocket, 2 underwater,
	   4 hover, 5 turbo), as when one is picked up, `animate` showing it.
	   SMS_EV_PLAYER_NOZZLE handlers hear of it (SMS_NOZZLE_SWITCH) and may
	   refuse it. 1 when FLUDD has that nozzle out then; 0 with no Mario, no
	   FLUDD, on Yoshi, or another number. */
	int (*set_nozzle)(int nozzle, int animate);
	/* API 1.5. Mario has FLUDD (1) or not (0), as in the stages without it;
	   0 when there is no Mario. */
	int (*set_fludd)(int has);
	/* API 1.5. What he wears: the aloha shirt, the sunglasses, the diving
	   helmet; each 1 on, 0 off, -1 as it is. 0 when there is no Mario. */
	int (*set_outfit)(int shirt, int sunglasses, int helmet);
	/* API 1.5. Mario turns to face `degrees` (SmsPlayer.yaw's: 0 faces
	   +Z, 90 faces +X) at once, his model too: FLUDD sprays that way. A
	   first-person camera's aim. 0 when there is no Mario. */
	int (*set_yaw)(float degrees);
	/* API 1.5. While `on`, Mario's body and FLUDD face `yaw` degrees
	   (as set_yaw) whichever way he moves (he still walks where the stick
	   says), and FLUDD tilts `pitch` degrees up (negative down) whatever his
	   stance, so FLUDD sprays that way: a first-person camera's aim, spraying
	   where it looks while walking sideways. 0 gives him his own aim back;
	   so does the mod unloading. 0 when there is no Mario. */
	int (*set_aim)(int on, float yaw, float pitch);
	/* API 1.5. Parts of Mario left out while the rest of him is
	   drawn (SMS_PLAYER_PART_*, ORed; 0 draws all of him again): a
	   first-person camera, where the player sees his body looking down.
	   Each mod's parts are its own and go when it unloads; the game leaves
	   out every mod's. 0 when there is no Mario. */
	int (*set_hidden_parts)(uint32_t parts);
} SmsPlayerApi;

/* player->set_hidden_parts (API 1.5) */
#define SMS_PLAYER_PART_HEAD  1u /* his head: face, hair and cap */
#define SMS_PLAYER_PART_FLUDD 2u /* FLUDD (it still sprays) */

typedef struct SmsCamera {
	uint32_t size;
	int valid;
	SmsVec3 position, target, up;
	float fovy, aspect, near_z, far_z;
	float view[3][4]; /* world to camera, as drawn last */
	float proj[4][4];
} SmsCamera;

typedef struct SmsCameraApi {
	uint32_t size;
	int (*get)(SmsCamera* out);
	/* Where a world point is on the canvas (draw->canvas units); returns 1
	   when it is in front of the camera. depth (may be NULL): distance along
	   the view. */
	int (*world_to_canvas)(const SmsVec3* p, float* x, float* y, float* depth);
} SmsCameraApi;

typedef enum SmsAnchor {
	SMS_ANCHOR_TOP_LEFT = 0,
	SMS_ANCHOR_TOP = 1,
	SMS_ANCHOR_TOP_RIGHT = 2,
	SMS_ANCHOR_LEFT = 3,
	SMS_ANCHOR_CENTER = 4,
	SMS_ANCHOR_RIGHT = 5,
	SMS_ANCHOR_BOTTOM_LEFT = 6,
	SMS_ANCHOR_BOTTOM = 7,
	SMS_ANCHOR_BOTTOM_RIGHT = 8
} SmsAnchor;

/* text flags */
#define SMS_TEXT_SHADOW  1
#define SMS_TEXT_OUTLINE 2
#define SMS_TEXT_CENTER  4  /* x is the middle */
#define SMS_TEXT_RIGHT   8  /* x is the right end */

/* The canvas is the game's picture, 480 units high and as wide as the
   picture's aspect (640 at 4:3, about 853 at 16:9), from its top left. It is
   drawn at the window's resolution over the game and under the port's own
   menus. Text is ASCII. */
typedef struct SmsDrawApi {
	uint32_t size;
	void (*canvas)(float* w, float* h);
	void (*rect)(float x, float y, float w, float h, SmsColor c);
	void (*rect_outline)(float x, float y, float w, float h, float thickness, SmsColor c);
	void (*rect_gradient)(float x, float y, float w, float h, SmsColor top, SmsColor bottom);
	void (*line)(float x0, float y0, float x1, float y1, float thickness, SmsColor c);
	void (*triangle)(float x0, float y0, float x1, float y1, float x2, float y2, SmsColor c);
	void (*circle)(float cx, float cy, float radius, SmsColor c);
	void (*circle_outline)(float cx, float cy, float radius, float thickness, SmsColor c);
	/* `size` is the line height; returns the width drawn. */
	float (*text)(float x, float y, float size, SmsColor c, int flags, const char* text);
	float (*text_width)(float size, const char* text);
	/* Images: PNG, JPEG, TGA or BMP, from a path in the mod's folder. */
	SmsImage (*image_load)(SmsMod* mod, const char* path);
	/* An image from pixels: RGBA8, w x h, row 0 at the top (copied). */
	SmsImage (*image_create)(SmsMod* mod, const uint8_t* rgba, int w, int h);
	void (*image_free)(SmsMod* mod, SmsImage image);
	int (*image_size)(SmsImage image, int* w, int* h);
	void (*image)(SmsImage image, float x, float y, float w, float h, SmsColor tint);
	void (*image_region)(SmsImage image, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
	                     SmsColor tint);
	/* Only draw inside a rectangle until the matching pop (nest freely). */
	void (*clip_push)(float x, float y, float w, float h);
	void (*clip_pop)(void);
	/* The world, through the game camera, drawn over the scene (not hidden
	   behind walls). */
	void (*world_line)(const SmsVec3* a, const SmsVec3* b, float thickness, SmsColor c);
	void (*world_box)(const SmsVec3* center, const SmsVec3* half_size, float thickness, SmsColor c);
	void (*world_ring)(const SmsVec3* center, float radius, float thickness, SmsColor c); /* level */
	/* Text whose middle bottom is at a world point; size shrinks with
	   distance when `scaled`. Returns 1 if drawn. */
	int (*world_text)(const SmsVec3* p, float size, int scaled, SmsColor c, int flags, const char* text);
	/* A w x h rectangle at an anchor of the canvas for this frame, below (or
	   above, at the bottom anchors) the ones other mods took there this
	   frame, so panels never overlap. Returns 0 if it does not fit. */
	int (*hud_slot)(SmsMod* mod, int anchor, float w, float h, SmsRect* out);
} SmsDrawApi;

typedef struct SmsPad {
	uint32_t size;
	int connected;
	uint16_t held, pressed, released; /* SMS_PAD_*; pressed/released: this frame */
	float stick_x, stick_y;           /* -1..1, up is +y */
	float cstick_x, cstick_y;
	float trigger_l, trigger_r;       /* 0..1 */
} SmsPad;

typedef struct SmsInputApi {
	uint32_t size;
	/* Controller `port` (0..3) as read this frame, before any mod changed it
	   and before a capture blanked it. */
	int (*pad)(int port, SmsPad* out);
	/* Keyboard, by scancode (key_from_name): down now; went down this frame. */
	int (*key_down)(int scancode);
	int (*key_pressed)(int scancode);
	int (*key_from_name)(const char* name); /* SDL key names: "F3", "Space", "A"; -1 if unknown */
	const char* (*key_name)(int scancode);
	/* A key for a named action of the mod: `default_key` unless the player
	   set another in the mod's settings (key.<action>). Warns when the key
	   is the port's or another mod's. Returns the scancode or -1. */
	int (*bind_key)(SmsMod* mod, const char* action, const char* default_key);
	/* Take the controllers and keyboard from the game while a menu is open:
	   the game sees nothing pressed and keys reach only you. Returns 0 if
	   another mod has them. Release with on = 0. */
	int (*capture)(SmsMod* mod, int on);
	/* The id of the mod that has them, or NULL. */
	const char* (*captured_by)(void);
} SmsInputApi;

typedef enum SmsOptionType {
	SMS_OPTION_BOOL = 0,
	SMS_OPTION_INT = 1,
	SMS_OPTION_FLOAT = 2,
	SMS_OPTION_CHOICE = 3, /* an int index into `choices` */
	SMS_OPTION_ACTION = 4  /* a button: on_change is called when chosen */
} SmsOptionType;

/* A setting the player can change in the Mods menu (F1). Its value is kept
   in the mod's settings under `key`, read with config->get_*. */
typedef struct SmsOption {
	uint32_t size; /* sizeof(SmsOption) */
	const char* key;
	const char* label;
	int type;      /* SmsOptionType */
	float def, min, max, step;
	const char* const* choices;
	int choice_count;
	void (*on_change)(SmsMod* mod, const char* key, void* user);
	void* user;
} SmsOption;

/* Settings, kept per mod in <data_dir>/settings.ini (key = value). */
typedef struct SmsConfigApi {
	uint32_t size;
	int (*get_int)(SmsMod* mod, const char* key, int def);
	float (*get_float)(SmsMod* mod, const char* key, float def);
	/* The returned string is good until the key is set again. */
	const char* (*get_string)(SmsMod* mod, const char* key, const char* def);
	void (*set_int)(SmsMod* mod, const char* key, int value);
	void (*set_float)(SmsMod* mod, const char* key, float value);
	void (*set_string)(SmsMod* mod, const char* key, const char* value);
	/* Written at once and at exit; call after a batch of sets if you like. */
	void (*save)(SmsMod* mod);
	int (*add_option)(SmsMod* mod, const SmsOption* option);
} SmsConfigApi;

/* Some of the game's system sounds, for audio->play_system_sound. */
#define SMS_SOUND_HEAL   0x4801 /* health back */
#define SMS_SOUND_APPEAR 0x480D /* something appears */
#define SMS_SOUND_CURSOR 0x480F /* menu cursor */
#define SMS_SOUND_DECIDE 0x4810 /* menu choice */
#define SMS_SOUND_COIN   0x4811
#define SMS_SOUND_NEWS   0x4812 /* a message arrives */
#define SMS_SOUND_SHINE  0x4814 /* Shine get */
#define SMS_SOUND_CANCEL 0x481D /* menu back */
#define SMS_SOUND_DAMAGE 0x4823

typedef struct SmsAudioApi {
	uint32_t size;
	/* One of the game's system sound effects (MSD_SE_SY_*: menu blips,
	   coin, 1-up...), not placed in the world. */
	void (*play_system_sound)(uint32_t id);
} SmsAudioApi;

/* The game's own objects, for mods built against the decomp's headers with
   the same compiler and pointer size as the game. Layouts change between
   game builds: check native->build() against what you were built for. */
typedef struct SmsNativeApi {
	uint32_t size;
	const char* (*build)(void);
	/* "gpMarioOriginal", "gpMarDirector", "gpApplication", "gpCamera",
	   "gpItemManager", "gpMap", "TFlagManager": the object, or NULL. */
	void* (*get)(const char* name);
} SmsNativeApi;

/* ------------------------------------------------------------------ multiplayer (API 1.2) */

/* api->net: the multiplayer game this one is in, and, on the host, the word
 * it sends players' games. A server mod (`side = server` in its mod.ini)
 * runs on the host only, and players join with the plain game: everything it
 * shows or does in their games goes through these commands, which every
 * build of the game carries out (docs/MODDING.md, "Server mods"). */

typedef enum SmsNetRole {
	SMS_NET_OFF = 0,    /* not in a multiplayer game */
	SMS_NET_HOST = 1,   /* hosting it (a dedicated server too) */
	SMS_NET_CLIENT = 2  /* joined someone's */
} SmsNetRole;

#define SMS_NET_MAX_PLAYERS 8
#define SMS_NET_SLOT(n) (1u << (n)) /* one player, for `to` */
#define SMS_NET_ALL 0xFFu           /* every player (the host's own game too) */

typedef enum SmsNetCharacter {
	SMS_NET_CHAR_MARIO = 0, SMS_NET_CHAR_LUIGI, SMS_NET_CHAR_WARIO, SMS_NET_CHAR_METAL_MARIO,
	SMS_NET_CHAR_FIRE_MARIO, SMS_NET_CHAR_PURPLE_MARIO, SMS_NET_CHAR_ORANGE_MARIO, SMS_NET_CHAR_ICE_MARIO
} SmsNetCharacter;

/* SmsNetPlayer.flags */
#define SMS_NETP_VISIBLE    1u   /* in play and drawn */
#define SMS_NETP_SURFING    2u   /* riding a Blooper */
#define SMS_NETP_YOSHI      4u   /* riding Yoshi */
#define SMS_NETP_SPECTATING 8u   /* out of lives, watching */
#define SMS_NETP_PAUSED     16u  /* their game is paused */
#define SMS_NETP_CUTSCENE   32u  /* in a cutscene */

typedef struct SmsNetPlayer {
	uint32_t size;     /* set to sizeof before calling net->player */
	int slot;
	int server;        /* the dedicated server itself: no player (slot 0) */
	int character;     /* SMS_NET_CHAR_* */
	char name[24];
	SmsColor color;    /* their character's colour, for text about them */
	int in_stage;      /* host: arrived in the stage session it plays */
	SmsVec3 position;  /* where they are, as their last update said */
	float yaw;         /* their facing, degrees as SmsPlayer's */
	uint32_t flags;    /* SMS_NETP_* */
	int ping;          /* round trip to them (host) or the host (client), ms; -1 unknown */
	/* API 1.3 (check out->size): as their game said when it joined (host;
	   any game knows its own): its version ("2026.10.18.1", the launcher's;
	   "" unknown) and the mod API it carries ((major << 16) | minor; 0
	   unknown), so a server mod knows which commands it understands. */
	char game_version[32];
	uint32_t api_version;
	/* API 1.4 (check out->size): what they press, as their last update
	   said (their own controller before the game sees it; held players'
	   too): SMS_PAD_* buttons, the sticks (-128..127) and triggers (0..255). */
	SmsPadStatus pad;
} SmsNetPlayer;

/* Commands: what the host's server mod has players' games do. Each fills
 * the fields its type names below; the rest are ignored.
 *   to:        the players (SMS_NET_ALL, SMS_NET_SLOT(n), or bits ORed);
 *              the host's own game carries out its own bit (slot 0) too.
 *   delay_ms:  carried out this long after it was sent, at the same moment
 *              in every game (each takes off half its round trip): a start
 *              lined up across the players.
 *   flags:     SMS_CMD_STICKY: kept for the stage session, and carried out
 *              again for each player who arrives in it later (a newcomer, a
 *              player back after falling in). A sticky command replaces the
 *              last one of its kind (its type; for ACTORS, its type range;
 *              for STOP_SCRIPT, its script). */
typedef enum SmsCmdType {
	/* Big text in the middle of the screen, in color (i[0], 0xRRGGBBAA);
	   text "" hides it. */
	SMS_CMD_BANNER = 1,
	/* A line of news at the top for a few seconds: text. */
	SMS_CMD_NOTICE = 2,
	/* A standing line at the top, shown when there is no news: text ("": none). */
	SMS_CMD_STATUS = 3,
	/* The HUD's clock: i[0] SMS_TIMER_*, i[1] its time in hundredths of a
	   second, from which it counts on by itself. */
	SMS_CMD_TIMER = 4,
	/* The game's "Ready..." then "Go!", "Go!" i[0] ms after the command is
	   carried out (each game starts "Ready..." as long before as its frame
	   rate needs: about 2 s at 30 fps, 1 s at 60). */
	SMS_CMD_READY_GO = 5,
	/* A sound: i[0] a system sound (audio->play_system_sound's numbers,
	   SMS_SOUND_*; an unsigned number stored in the int). */
	SMS_CMD_SOUND = 6, /* API 1.4: i[1] 1 plays it at f[0..2] in the world (older games: as a system sound) */
	/* Music: i[0] the game's background music id (MSD_BGM_*, an unsigned
	   number stored in the int: SMS_MUSIC_*); 0 stops the music a command started. */
	SMS_CMD_MUSIC = 7,
	/* The player to f[0..2] facing f[3] degrees (as a warp pipe puts them). */
	SMS_CMD_WARP = 8,
	/* i[0] 1: the player's controls do nothing (and a Blooper they ride stops); 0: theirs again. */
	SMS_CMD_HOLD = 9,
	/* The player onto a Blooper of their own game, at f[0..2] facing f[3]:
	   the first of actor types i[0] to i[1] that is free (0x400000C5 to
	   0x400000C7: red, yellow, green), brought out if hidden. */
	SMS_CMD_MOUNT = 10,
	/* i[0] a slot: the player's camera rides with that player, theirs out of
	   play, as a player out of lives watches; -1: back in play where they are. */
	SMS_CMD_WATCH = 11,
	/* The stage's actors of types i[0] to i[1] (TLiveActor::getActorType;
	   Shines are 0x20000013): i[2] SMS_ACTORS_*. */
	SMS_CMD_ACTORS = 12,
	/* Stop the stage's event script loaded from text (its resource path,
	   "/scene/map/sp/gessoride.sb"): the time trial of a course, a story event. */
	SMS_CMD_STOP_SCRIPT = 13,
	/* i[0] 1: the player stays in this stage: their own ways out (pipes,
	   doors, Exit Area) lead nowhere, and only the host moves them on; 0: free. */
	SMS_CMD_STAY = 14,
	/* API 1.4: one of the stage's objects: of actor types i[0] to i[1] and of
	   the kind text, as the stage names it ("WoodBlockCone", "normallift",
	   "rollblocky"; "" any), the i[2]th (0 first, in the order every game
	   loads them; -1: each of them). i[3] SMS_OBJECT_* says what, with f[]. */
	SMS_CMD_OBJECT = 15,
	/* API 1.4. A copy of one of the stage's objects (picked as for
	   SMS_CMD_OBJECT: i[0]..i[1] types, text its kind, i[2] which), or with
	   types 0 and 0 an item by its name in text (SMS_SPAWN_*: "coin",
	   "coin_red", "1up", "FruitBanana"...), at f[0..2] turned f[3]
	   degrees. i[3] is its id (1 or more, the mod's own): SMS_CMD_OBJECT and
	   game->object_info reach it as kind "@<id>". Sticky per id, as every
	   copy should be: a player arriving later has the same. */
	SMS_CMD_SPAWN = 16,
	/* The player: i[0] SMS_PLAYER_*. */
	SMS_CMD_PLAYER = 17,
	/* What the player may do: i[0] SMS_RULE_* bits (0: all of it). */
	SMS_CMD_RULES = 18,
	/* A board at the top right: row i[0] (0 to 7; -1 every row) is text, in
	   i[1]'s colour (0xRRGGBBAA; 0 white); "" clears the row. Sticky per row. */
	SMS_CMD_BOARD = 19,
	/* The HUD's parts hidden: i[0] SMS_HUD_* bits (0 all shown again). */
	SMS_CMD_HUD = 20,
	/* Marker i[0] (0 to 7): text (an arrow when "") in i[2]'s colour over
	   f[0..2] in the world, or over player i[1] (-1: the place); i[3] 1 takes
	   it away. Sticky per marker. */
	SMS_CMD_MARKER = 21,
	/* The screen: i[0] SMS_SCREEN_*, in i[1]'s colour (0xRRGGBBAA), over f[0] seconds. */
	SMS_CMD_SCREEN = 22,
	/* The camera shakes, f[0] strong (1 a hard hit), for f[1] seconds. */
	SMS_CMD_SHAKE = 23,
	/* The controller rumbles: i[0] 0 weak, 1 hard, i[1] times (1 or more). */
	SMS_CMD_RUMBLE = 24,
	/* API 1.5. Player i[0] was splashed, as Shadow Mario is when
	   FLUDD's water hits him: a splash at f[0..2] with his hit sound, the
	   player's model flushed for a moment (f[3] seconds; 0 about 0.8) and,
	   when i[2] > 0, his water meter over their head, i[1] of i[2] full,
	   for twenty seconds. i[3] SMS_SPLASH_* bits. Send it to everyone:
	   each game shows it on its own copy of that player (the player's own
	   game on its Mario). */
	SMS_CMD_SPLASH = 25,
	/* API 1.5. An arena's edge: the player is kept within f[3] units
	   (across the ground) of f[0..2], pushed back in as they reach it, and
	   with SMS_BOUNDS_RING in i[0] a solid line on the ground and the water
	   marks it, in i[1]'s colour (0xRRGGBBAA; 0 red). f2[0] seconds: the
	   edge glides there from where it was (a closing arena). f[3] 0: none.
	   Kept per session as sticky commands are, when sent sticky. */
	SMS_CMD_BOUNDS = 26
} SmsCmdType;

#define SMS_CMD_STICKY 1u
/* API 1.4: send nothing, and forget the sticky command of this kind (as
   sticky commands replace each other: the same type, to the same players,
   and for those keyed so, the same object, id, row or marker). */
#define SMS_CMD_UNSTICK 2u

/* Some of the game's music (SMS_CMD_MUSIC) */
#define SMS_MUSIC_EVENT        ((int32_t)0x8001001Fu) /* a race's, a timed event's */
#define SMS_MUSIC_FANFARE_RACE ((int32_t)0x80010026u) /* a race won */

/* SMS_CMD_TIMER's i[0] */
#define SMS_TIMER_HIDE  0
#define SMS_TIMER_SHOW  1  /* stopped at i[1] */
#define SMS_TIMER_UP    2  /* counting up from i[1] */
#define SMS_TIMER_DOWN  3  /* counting down from i[1] to 0 */

/* SMS_CMD_ACTORS's i[2] */
#define SMS_ACTORS_SHOW 0  /* as the stage has them */
#define SMS_ACTORS_HIDE 1  /* out of the stage (unseen, touching nothing), until shown */
#define SMS_ACTORS_KILL 2  /* gone for this stage (a Shine nobody should get) */

/* SMS_CMD_OBJECT's i[3] (API 1.4). Sticky, one of each per object (SHOW and
   HIDE are one: the last says which). */
#define SMS_OBJECT_PLACE 0 /* to f[0..2], turned f[3] degrees about the vertical; where it goes back to as well
                              (not one on a rail: its rail says where it goes) */
#define SMS_OBJECT_SPEED 1 /* f[0] times its own speed (up to 20): a spinner's turning, the other way round
                              when negative; a lift's or a platform's along its rail (0.05 at least) */
#define SMS_OBJECT_PHASE 2 /* back to how the stage loaded it, then f[0] seconds (up to 60) of its own
                              movement at once: a lift or spinner where it would be by then, the same in
                              every game (send it with a delay_ms for the moment it should be so) */
#define SMS_OBJECT_SHOW  3 /* back */
#define SMS_OBJECT_HIDE  4 /* out of the stage: unseen, nothing to touch or stand on, until shown */
#define SMS_OBJECT_SCALE 5 /* f[0..2] its size about x, y, z (1 as made) */
#define SMS_OBJECT_TURN  6 /* f[0..2] its turn about x, y, z, degrees (not one on a rail) */
#define SMS_OBJECT_MOVE  7 /* glides to f[0..2] turned f[3] degrees about the vertical, over f[4] seconds
                              (SmsCmd.f2[0]); where it goes back to as well (not one on a rail) */
#define SMS_OBJECT_KILL  8 /* gone for this stage */

/* SMS_CMD_SPAWN's items (text, with types 0 and 0); the stage must have the
   model of one that is not a coin (one of its kind there, or in its archive) */
#define SMS_SPAWN_COIN     "coin"
#define SMS_SPAWN_RED_COIN "coin_red"
#define SMS_SPAWN_1UP      "1up"

/* SMS_CMD_PLAYER's i[0] */
#define SMS_PLAYER_LOSE_LIFE 0 /* the game's own miss, now */
#define SMS_PLAYER_LAUNCH    1 /* thrown along f[0..2] (a direction) at f[3] units a frame (40 a big push) */
#define SMS_PLAYER_DISMOUNT  2 /* off a Blooper or Yoshi */
#define SMS_PLAYER_HEALTH    3 /* i[1]: 0 to 8 (0 does not lose a life) */
#define SMS_PLAYER_WATER     4 /* i[1]: FLUDD's tank, 0 to 100 percent */
#define SMS_PLAYER_NOZZLE    5 /* i[1]: 0 spray, 1 rocket, 2 underwater, 4 hover, 5 turbo */
#define SMS_PLAYER_ADD_WATER 6 /* API 1.5. i[1]: percent of a full tank added (less when negative) */
#define SMS_PLAYER_WATER_USE 7 /* API 1.5. i[1]: FLUDD uses water at this percent of its own rate
                                  (200 twice as fast; 100 as the game does), for the session */
#define SMS_PLAYER_FLUDD     8 /* API 1.5. i[1]: 1 FLUDD on the player's back, 0 off it */

/* SMS_CMD_RULES's bits */
#define SMS_RULE_NO_FLUDD   1u /* FLUDD sprays nothing */
#define SMS_RULE_NO_DAMAGE  2u /* no health lost (falls and drops still cost a life) */
#define SMS_RULE_NO_PAUSE   4u /* no pause menu or map */
#define SMS_RULE_NO_PICKUPS 8u /* coins and items stay where they are */
/* API 1.5: FLUDD's water hits the other players. The shooter's game
   reports each hit to the host (SMS_EV_NET_PLAYER_SPLASHED) and the drop
   stops there; a server mod says what a hit does (SMS_CMD_SPLASH). */
#define SMS_RULE_WATER_HITS 16u
/* API 1.5: FLUDD fills up only as the server says (SMS_PLAYER_WATER,
   SMS_PLAYER_ADD_WATER): not from the water around (sea, pools, fountains),
   bottles (a server's spawned ones too: their look and sparkle without the
   water, which the server gives), fruit and 1-ups, or a barrel's hip drop. */
#define SMS_RULE_NO_REFILL 32u

/* SMS_CMD_BOUNDS's i[0] bits (API 1.5) */
#define SMS_BOUNDS_RING 1u /* draw the edge: a solid line along the ground */

/* SMS_CMD_SPLASH's i[3] bits (API 1.5) */
#define SMS_SPLASH_DOWN  1u /* the meter is empty: they are knocked down, as Shadow Mario falls */
#define SMS_SPLASH_QUIET 2u /* no splash or sound: the meter alone (a player joining sees it) */

/* SMS_CMD_HUD's bits */
#define SMS_HUD_COINS      1u
#define SMS_HUD_SHINES     2u
#define SMS_HUD_BLUE_COINS 4u
#define SMS_HUD_HEALTH     8u
#define SMS_HUD_WATER      16u
#define SMS_HUD_LIVES      32u
#define SMS_HUD_RED_COINS  64u
#define SMS_HUD_ALL        0xFFFFu

/* SMS_CMD_SCREEN's i[0] */
#define SMS_SCREEN_CLEAR    0 /* the picture back at once */
#define SMS_SCREEN_FADE_OUT 1 /* to the colour */
#define SMS_SCREEN_FADE_IN  2 /* from the colour */
#define SMS_SCREEN_FLASH    3 /* the colour, fading away */

/* net->set_respawn's how */
#define SMS_RESPAWN_NONE    0
#define SMS_RESPAWN_WARP    1 /* there, as a warp puts them */
#define SMS_RESPAWN_BLOOPER 2 /* onto a Blooper there */

typedef struct SmsCmd {
	uint32_t size;     /* sizeof(SmsCmd) */
	uint32_t type;     /* SMS_CMD_* */
	uint32_t flags;    /* SMS_CMD_STICKY */
	uint32_t delay_ms;
	int32_t i[4];
	float f[4];
	const char* text;  /* copied; at most 159 bytes are sent */
	float f2[4];       /* API 1.4 (taken when size covers it): the commands' f[4..7] */
} SmsCmd;

typedef struct SmsNetApi {
	uint32_t size;
	int (*role)(void);              /* SMS_NET_* */
	int (*dedicated)(void);         /* the host is a server, not a player */
	int (*local_slot)(void);        /* this game's player; -1 before a client has one */
	uint32_t (*players)(void);      /* bit n: slot n is in the game (a server's slot 0 too) */
	int (*player)(int slot, SmsNetPlayer* out); /* 1 when that slot is in the game */
	uint32_t (*session)(void);      /* the stage session played (0: none) */
	/* Host only (0 elsewhere): commands to players' games. Returns the
	   players it went to. */
	uint32_t (*send)(uint32_t to, const SmsCmd* cmd);
	/* Host: everyone to that stage, afresh (a new session, even for the
	   stage played now). A dedicated server waiting at the title goes there. */
	int (*load_stage)(int area, int episode);
	/* Host: what the launcher's list of games shows: the game mode's name
	   ("squid-race", at most 15 bytes) and a line of its state. */
	void (*set_mode)(const char* name);
	void (*set_status)(const char* text);
	/* API 1.4, host only. A player's Customize Mario look, Mario's and
	   FLUDD's colours ("hat=2fa846;shirt=2fa846;fluddBody=..."), forced: in
	   their own game and every other, newcomers too, until set again; NULL
	   gives them their own back. look(): theirs as it is now, into out. */
	int (*set_look)(int slot, const char* look);
	int (*look)(int slot, char* out, int size);
	/* Where a player comes back each time they arrive in the session (just
	   joined, back after a lost life): SMS_RESPAWN_*, at p facing yaw
	   degrees, after the session's sticky commands. NONE: the stage's own. */
	int (*set_respawn)(int slot, int how, const SmsVec3* p, float yaw);
	/* API 1.5. Players collide with each other: in every game, its
	   player is kept out of the others (running into one bumps them back, as
	   into an enemy) and lands on their heads as on an enemy's. The host's
	   setting (its settings menu, SMS_MULTIPLAYER_COLLISION) until a server
	   mod sets it: host only, 1 on, 0 off, -1 the host's setting again;
	   returns 1 when done. collision(): whether players collide now (any game). */
	int (*set_collision)(int on);
	int (*collision)(void);
} SmsNetApi;

/* ------------------------------------------------------------------ worlds and episodes (API 1.4) */

/* api->world: the game's map of its stages, as its own menus see it. An area
 * is one stage (SmsEvStage.area); a world is a place the plaza leads to, with
 * its areas (Sirena Beach and Hotel Delfino), its secret courses and its
 * eight episodes, each with its Shine. Worlds are named by the area the
 * plaza leads into (Bianco Hills: 2). Super Mario Eclipse numbers its stages
 * its own way: in its builds every area is SMS_AREA_NONE. */

typedef enum SmsAreaKind {
	SMS_AREA_NONE = 0,       /* no such area (or an Eclipse build) */
	SMS_AREA_AIRSTRIP = 1,   /* Delfino Airstrip, where the story starts */
	SMS_AREA_PLAZA = 2,      /* Delfino Plaza, and its parts */
	SMS_AREA_WORLD = 3,      /* a world's way in from the plaza, with its episode select */
	SMS_AREA_WORLD_PART = 4, /* another part of a world (Hotel Delfino, the park of Pinna Park) */
	SMS_AREA_SECRET = 5,     /* a secret course */
	SMS_AREA_FINAL = 6,      /* Corona Mountain */
	SMS_AREA_TITLE = 7       /* the title screen's plaza (15) */
} SmsAreaKind;

typedef struct SmsWorldApi {
	uint32_t size;
	/* What an area is (SMS_AREA_*), and the world it belongs to: the area
	   the plaza leads into (2 for every area of Bianco Hills, its secret
	   courses too; 1 for the plaza's own; 0 the Airstrip; 52 Corona
	   Mountain), or -1. */
	int (*area_kind)(int area);
	int (*world_of)(int area);
	/* A world's episodes (any of its areas names it): how many (8 for the
	   seven worlds, else 0); the Shine an episode is for (0..119, or -1);
	   and its name as the game shows it, copied into out (one line, ASCII;
	   returns its length, 0 when there is none). Episodes count from 0. */
	int (*episode_count)(int area);
	int (*episode_shine)(int area, int episode);
	int (*episode_name)(int area, int episode, char* out, int size);
	/* The episode the world's episode select offers first: the one after
	   the newest whose Shine the file has (the first with none; the last
	   when that is had). -1: no episodes. */
	int (*next_episode)(int area);
	/* The episode a Shine is the story Shine of, with its world in *area
	   (may be NULL); -1 for the others (100 coins, blue coins, the plaza's). */
	int (*shine_episode)(int shine, int* area);
	/* The stage's camera cutscenes, by the names SMS_EV_CUTSCENE gives
	   ("camera_sirena_gate_in"): whether the stage has one by that name; and
	   play one as the game plays its own (the player waits, the HUD hides),
	   centred on focus (NULL: as it was made), for `frames` (-1: as long as
	   it is). Returns 1 when it is on its way: in a stage that is played,
	   with no cutscene, talk or menu on. */
	int (*has_cutscene)(const char* name);
	int (*play_cutscene)(const char* name, const SmsVec3* focus, int frames);
	/* A cutscene (the game's or a mod's) or a talk is on now. */
	int (*cutscene_playing)(void);
} SmsWorldApi;

/* Files (API 1.5, SMS_API_HAS(api, files)): the mod's own, beside
   the memory card, in <card folder>-mods/<mod id>/ (card-a-mods/<id>/ by
   default), so they go with the player's card wherever it is kept.

   scope SMS_FILES_MOD: the mod's own files, written at once.
   scope 0..2: save file A, B or C of the game being played (Super Mario
   Sunshine's and Super Mario Eclipse's are kept apart), kept as the card
   keeps the file: what is written to the file being played is written when
   the game saves it (so a game quit without saving keeps the files as they
   were), a new game starts with none, and copying or erasing a file at file
   select copies or erases them too. SMS_FILES_CURRENT: the file being played
   (0 when there is none). Writes to another file than the one played are
   written at once.

   Names: letters, digits, '.', '_' and '-', 1..64 characters, not starting
   with '.'; a file at most 1 MiB. */
#define SMS_FILES_MOD     (-1)
#define SMS_FILES_CURRENT (-2)

typedef struct SmsFilesApi {
	uint32_t size;
	/* The mod's folder beside the card (made on first use). */
	const char* (*dir)(SmsMod* mod);
	/* The file's size (it may be more than cap: that much is read), or -1
	   when there is none. buf may be NULL to ask the size. */
	int (*read)(SmsMod* mod, int scope, const char* name, void* buf, uint32_t cap);
	/* 1 written (or kept for the save), 0 not (a bad name or scope, too big). */
	int (*write)(SmsMod* mod, int scope, const char* name, const void* data, uint32_t size);
	/* 1 when it was there. */
	int (*remove)(SmsMod* mod, int scope, const char* name);
} SmsFilesApi;

struct SmsApi {
	uint32_t size;
	uint16_t major, minor;
	const SmsCoreApi* core;
	const SmsGameApi* game;
	const SmsPlayerApi* player;
	const SmsCameraApi* camera;
	const SmsDrawApi* draw;
	const SmsInputApi* input;
	const SmsConfigApi* config;
	const SmsAudioApi* audio;
	const SmsNativeApi* native;
	const SmsNetApi* net;           /* API 1.2 (SMS_API_HAS(api, net)) */
	const SmsWorldApi* world;       /* API 1.4 (SMS_API_HAS(api, world)) */
	/* API 1.5: the game's SMS_MOD_API_PRERELEASE, 0 when it carries a release
	   of its minor version (check api->size). */
	uint32_t prerelease;
	const SmsFilesApi* files;       /* API 1.5 (SMS_API_HAS(api, files)) */
};

/* ------------------------------------------------------------------ the mod's side */

/* Every mod defines these with SMS_MOD_DEFINE() and sms_mod_load:
 *
 *   SMS_MOD_DEFINE()
 *   SMS_MOD_EXTERN_C SMS_MOD_EXPORT int sms_mod_load(SmsMod* mod, const SmsApi* api) { ...; return 0; }
 *
 * sms_mod_load returns 0 when loaded; anything else refuses (the game logs it
 * and goes on without the mod). sms_mod_unload, if defined, is called at
 * exit and before a hot reload; subscriptions, images, options and
 * services are dropped for you. */
typedef int (*SmsModLoadFn)(SmsMod* mod, const SmsApi* api);
typedef void (*SmsModUnloadFn)(SmsMod* mod);
typedef uint32_t (*SmsModVersionFn)(void);

/* sms_mod_api_prerelease (API 1.5): the pre-release the mod was built
   against, 0 for a release; a mod without it was built against a release. */
#define SMS_MOD_DEFINE() \
	SMS_MOD_EXTERN_C SMS_MOD_EXPORT uint32_t sms_mod_api_version(void) { return SMS_MOD_API_VERSION; } \
	SMS_MOD_EXTERN_C SMS_MOD_EXPORT uint32_t sms_mod_api_prerelease(void) { return SMS_MOD_API_PRERELEASE; }

#ifdef __cplusplus
}
#endif

#endif
