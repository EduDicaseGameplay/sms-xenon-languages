/* The lua mod: runs Lua script mods for the Super Mario Sunshine PC port.
 *
 * A script mod is a folder in mods/ whose mod.ini says
 *
 *     runtime = lua
 *     script = main.lua
 *
 * The game asks this mod to start each one (core->host_runtime). Every script
 * gets its own Lua 5.4 state and is a mod in its own right: its own id,
 * settings, options in the Mods menu, keys, HUD slots and log prefix, because
 * every API call made for it passes its own SmsMod. The `sms` table it sees
 * mirrors the C API (docs/MODDING.md, "Lua scripts"); a handler's errors are
 * logged with a traceback and never reach the game.
 */
#include <sms_mod.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include <stdlib.h>
#include <string.h>

static const SmsApi* api;

typedef struct Ref Ref;
typedef struct Script {
	SmsMod* mod;
	lua_State* L;
	int errors;
	char reason[128]; /* a save state refusal's reason, kept for the game */
	char model[512];  /* a Shine's model a handler named, as a full path, kept for the game */
	Ref* refs;
	struct Script* next;
} Script;

/* A Lua function the game calls back: an event handler or an option's on_change. */
struct Ref {
	Script* s;
	int fn;
	Ref* next;
};

static Script* s_scripts;

static Script* script(lua_State* L) { return *(Script**)lua_getextraspace(L); }
#define MOD(L) (script(L)->mod)

/* ------------------------------------------------------------------ errors */

static int traceback(lua_State* L)
{
	const char* msg = lua_tostring(L, 1);
	if (!msg)
		msg = luaL_tolstring(L, 1, NULL);
	luaL_traceback(L, L, msg, 1);
	return 1;
}

static void report(Script* s, const char* msg)
{
	s->errors++;
	if (s->errors <= 5)
		api->core->log(s->mod, SMS_LOG_ERROR, "%s", msg ? msg : "(error)");
	else if (s->errors == 6)
		api->core->log(s->mod, SMS_LOG_ERROR, "more errors follow; not shown");
}

/* lua_pcall with a traceback; on an error it is logged and nothing is left. */
static int call(Script* s, int nargs, int nres)
{
	lua_State* L   = s->L;
	const int base = lua_gettop(L) - nargs;
	lua_pushcfunction(L, traceback);
	lua_insert(L, base);
	const int rc = lua_pcall(L, nargs, nres, base);
	lua_remove(L, base);
	if (rc != LUA_OK) {
		report(s, lua_tostring(L, -1));
		lua_pop(L, 1);
	}
	return rc;
}

static Ref* newRef(lua_State* L, int idx)
{
	Script* s = script(L);
	Ref* r    = (Ref*)calloc(1, sizeof *r);
	lua_pushvalue(L, idx);
	r->fn    = luaL_ref(L, LUA_REGISTRYINDEX);
	r->s     = s;
	r->next  = s->refs;
	s->refs  = r;
	return r;
}

/* ------------------------------------------------------------------ values */

static void setnum(lua_State* L, const char* k, double v)
{
	lua_pushnumber(L, v);
	lua_setfield(L, -2, k);
}
static void setint(lua_State* L, const char* k, lua_Integer v)
{
	lua_pushinteger(L, v);
	lua_setfield(L, -2, k);
}
static void setbool(lua_State* L, const char* k, int v)
{
	lua_pushboolean(L, v);
	lua_setfield(L, -2, k);
}
static void setstr(lua_State* L, const char* k, const char* v)
{
	if (v)
		lua_pushstring(L, v);
	else
		lua_pushnil(L);
	lua_setfield(L, -2, k);
}
static void setobj(lua_State* L, const char* k, SmsObject o)
{
	if (o)
		lua_pushlightuserdata(L, o);
	else
		lua_pushnil(L);
	lua_setfield(L, -2, k);
}
static void pushvec(lua_State* L, const SmsVec3* v)
{
	lua_createtable(L, 0, 3);
	setnum(L, "x", v->x);
	setnum(L, "y", v->y);
	setnum(L, "z", v->z);
}
static void setvec(lua_State* L, const char* k, const SmsVec3* v)
{
	pushvec(L, v);
	lua_setfield(L, -2, k);
}

static float fieldnum(lua_State* L, int t, const char* k, float def)
{
	lua_getfield(L, t, k);
	const float v = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : def;
	lua_pop(L, 1);
	return v;
}

/* A vector: {x=, y=, z=} or {x, y, z}. */
static int tovec(lua_State* L, int idx, SmsVec3* out)
{
	if (!lua_istable(L, idx))
		return 0;
	idx       = lua_absindex(L, idx);
	lua_geti(L, idx, 1);
	const int seq = lua_isnumber(L, -1);
	lua_pop(L, 1);
	if (seq) {
		lua_geti(L, idx, 1);
		lua_geti(L, idx, 2);
		lua_geti(L, idx, 3);
		out->x = (float)lua_tonumber(L, -3);
		out->y = (float)lua_tonumber(L, -2);
		out->z = (float)lua_tonumber(L, -1);
		lua_pop(L, 3);
	} else {
		out->x = fieldnum(L, idx, "x", 0);
		out->y = fieldnum(L, idx, "y", 0);
		out->z = fieldnum(L, idx, "z", 0);
	}
	return 1;
}

static SmsVec3 checkvec(lua_State* L, int idx)
{
	SmsVec3 v;
	if (!tovec(L, idx, &v))
		luaL_typeerror(L, idx, "vector ({x=, y=, z=})");
	return v;
}

static SmsColor optcolor(lua_State* L, int idx, SmsColor def)
{
	return lua_isnoneornil(L, idx) ? def : (SmsColor)(lua_Unsigned)luaL_checkinteger(L, idx);
}

static float num(lua_State* L, int idx) { return (float)luaL_checknumber(L, idx); }

/* ------------------------------------------------------------------ events */

static const struct {
	const char* name;
	uint32_t type;
} kEvents[] = {
	{"frame", SMS_EV_FRAME},
	{"tick", SMS_EV_TICK},
	{"draw", SMS_EV_DRAW},
	{"stage_loaded", SMS_EV_STAGE_LOADED},
	{"stage_exit_request", SMS_EV_STAGE_EXIT_REQUEST},
	{"stage_leaving", SMS_EV_STAGE_LEAVING},
	{"life_lost", SMS_EV_LIFE_LOST},
	{"game_over", SMS_EV_GAME_OVER},
	{"coins_reset", SMS_EV_COINS_RESET},
	{"file_selected", SMS_EV_FILE_SELECTED},
	{"item_collected", SMS_EV_ITEM_COLLECTED},
	{"pad_read", SMS_EV_PAD_READ},
	{"key", SMS_EV_KEY},
	{"cutscene", SMS_EV_CUTSCENE},
	{"goop_cleaned", SMS_EV_GOOP_CLEANED},
	{"object_hit", SMS_EV_OBJECT_HIT},
	{"save_state_request", SMS_EV_SAVE_STATE_REQUEST},
	{"hide_player_model", SMS_EV_HIDE_PLAYER_MODEL},
	{"player_status", SMS_EV_PLAYER_STATUS},
	{"player_damage", SMS_EV_PLAYER_DAMAGE},
	{"shine_collected", SMS_EV_SHINE_COLLECTED},
	{"camera", SMS_EV_CAMERA},
	{"object_move", SMS_EV_OBJECT_MOVE},
	{"mod_loaded", SMS_EV_MOD_LOADED},
	{"shutdown", SMS_EV_SHUTDOWN},
	{"net_player_joined", SMS_EV_NET_PLAYER_JOINED},
	{"net_player_left", SMS_EV_NET_PLAYER_LEFT},
	{"net_player_arrived", SMS_EV_NET_PLAYER_ARRIVED},
	{"net_session", SMS_EV_NET_SESSION},
	{"stage_start", SMS_EV_STAGE_START},
	{"player_nozzle", SMS_EV_PLAYER_NOZZLE},
	{"player_status_changed", SMS_EV_PLAYER_STATUS_CHANGED},
	{"player_jump", SMS_EV_PLAYER_JUMP},
	{"player_land", SMS_EV_PLAYER_LAND},
	{"player_heal", SMS_EV_PLAYER_HEAL},
	{"player_death", SMS_EV_PLAYER_DEATH},
	{"player_spawn", SMS_EV_PLAYER_SPAWN},
	{"player_hit", SMS_EV_PLAYER_HIT},
	{"player_mount", SMS_EV_PLAYER_MOUNT},
	{"player_water", SMS_EV_PLAYER_WATER},
	{"yoshi_hatch", SMS_EV_YOSHI_HATCH},
	{"yoshi_eat", SMS_EV_YOSHI_EAT},
	{"actor_spawn", SMS_EV_ACTOR_SPAWN},
	{"actor_damage", SMS_EV_ACTOR_DAMAGE},
	{"actor_death", SMS_EV_ACTOR_DEATH},
	{"shine_appear", SMS_EV_SHINE_APPEAR},
	{"shine_get", SMS_EV_SHINE_GET},
	{"flag_changed", SMS_EV_FLAG_CHANGED},
	{"sound", SMS_EV_SOUND},
	{"music", SMS_EV_MUSIC},
	{"talk", SMS_EV_TALK},
	{"pause", SMS_EV_PAUSE},
	{"mode_changed", SMS_EV_MODE_CHANGED},
	{"cutscene_end", SMS_EV_CUTSCENE_END},
	{"game_saved", SMS_EV_GAME_SAVED},
	{"timer", SMS_EV_TIMER},
	{"script_call", SMS_EV_SCRIPT_CALL},
	{"map_event", SMS_EV_MAP_EVENT},
	{"save_prompt", SMS_EV_SAVE_PROMPT},
	{"stage_loading", SMS_EV_STAGE_LOADING},
	{"net_player_splashed", SMS_EV_NET_PLAYER_SPLASHED},
	{"save_file", SMS_EV_SAVE_FILE},
	{"nozzle_fire", SMS_EV_NOZZLE_FIRE},
	{"shine_setup", SMS_EV_SHINE_SETUP},
	{"yoshi_egg", SMS_EV_YOSHI_EGG},
	{"episode_shine", SMS_EV_EPISODE_SHINE},
	{"file_starting", SMS_EV_FILE_STARTING},
	{"file_menu", SMS_EV_FILE_MENU},
};

static const char* const kLeaves[] = {"other", "way_out", "shine", "exit_area", "life_lost", "game_over", "warp", "file"};
static const char* leaveName(int r) { return r >= 0 && r <= 7 ? kLeaves[r] : "other"; }

static const char* const kNozzleReasons[] = {"switch", "swap", "box", "stage_start"};
static const char* nozzleReasonName(int r) { return r >= 0 && r <= 3 ? kNozzleReasons[r] : "switch"; }

/* API 1.5's names */
static const char* const kDeaths[] = {"other", "health", "drown", "pit", "goop", "electric", "crash"};
static const char* deathName(int c) { return c >= 0 && c <= 6 ? kDeaths[c] : "other"; }
static const char* const kStarts[] = {"wait", "roll", "return", "other", "coaster"};
static const char* startName(int k) { return k >= 0 && k <= 4 ? kStarts[k] : "other"; }
static const char* const kTalks[] = {"start", "message", "end"};
static const char* const kSaveFiles[] = {"loaded", "new", "saved", "copied", "erased"};
static const char* const kFires[] = {"burst", "pressure"};
static const char* const kEggs[] = {"stage", "plaza_event", "event_egg"};
static const char* const kPauses[] = {"menu", "map", "save"};
static const char* const kTimers[] = {"started", "stopped", "out"};
static const char* const kFileMenu[] = {"start", "copy", "erase", "score"};
static const char* pick(const char* const* names, int n, int i) { return i >= 0 && i < n ? names[i] : "other"; }
static const char* savePromptName(int k)
{
	switch (k) {
	case SMS_SAVE_BLUE_COIN: return "blue_coin";
	case 3:
	case 4: return "nozzle";
	case SMS_SAVE_YOSHI: return "yoshi";
	case SMS_SAVE_SHINE: return "shine";
	default: return "other";
	}
}

static const char* const kModes[] = {"boot", "title", "menu", "stage", "movie", "other"};
static const char* modeName(int m) { return m >= 0 && m <= 5 ? kModes[m] : "other"; }

/* An event's id from a name ("tick", or a custom event's "author.mod.event") or a number. */
static uint32_t eventType(lua_State* L, int idx)
{
	if (lua_type(L, idx) == LUA_TNUMBER)
		return (uint32_t)lua_tointeger(L, idx);
	const char* name = luaL_checkstring(L, idx);
	for (size_t i = 0; i < sizeof kEvents / sizeof kEvents[0]; i++)
		if (!strcmp(kEvents[i].name, name))
			return kEvents[i].type;
	return api->core->event_id(name);
}

static void pushPad(lua_State* L, const SmsPadStatus* p)
{
	lua_createtable(L, 0, 10);
	setint(L, "buttons", p->buttons);
	setint(L, "stick_x", p->stick_x);
	setint(L, "stick_y", p->stick_y);
	setint(L, "cstick_x", p->cstick_x);
	setint(L, "cstick_y", p->cstick_y);
	setint(L, "trigger_l", p->trigger_l);
	setint(L, "trigger_r", p->trigger_r);
	setint(L, "analog_a", p->analog_a);
	setint(L, "analog_b", p->analog_b);
	setbool(L, "connected", p->err == 0);
}

static int clampi(lua_Number v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : (int)v; }

static void readPad(lua_State* L, int t, SmsPadStatus* p)
{
	p->buttons   = (uint16_t)fieldnum(L, t, "buttons", p->buttons);
	p->stick_x   = (int8_t)clampi(fieldnum(L, t, "stick_x", p->stick_x), -128, 127);
	p->stick_y   = (int8_t)clampi(fieldnum(L, t, "stick_y", p->stick_y), -128, 127);
	p->cstick_x  = (int8_t)clampi(fieldnum(L, t, "cstick_x", p->cstick_x), -128, 127);
	p->cstick_y  = (int8_t)clampi(fieldnum(L, t, "cstick_y", p->cstick_y), -128, 127);
	p->trigger_l = (uint8_t)clampi(fieldnum(L, t, "trigger_l", p->trigger_l), 0, 255);
	p->trigger_r = (uint8_t)clampi(fieldnum(L, t, "trigger_r", p->trigger_r), 0, 255);
	p->analog_a  = (uint8_t)clampi(fieldnum(L, t, "analog_a", p->analog_a), 0, 255);
	p->analog_b  = (uint8_t)clampi(fieldnum(L, t, "analog_b", p->analog_b), 0, 255);
}

static const char* itemKind(int k)
{
	switch (k) {
	case SMS_ITEM_COIN: return "coin";
	case SMS_ITEM_RED_COIN: return "red_coin";
	case SMS_ITEM_BLUE_COIN: return "blue_coin";
	case SMS_ITEM_ONE_UP: return "one_up";
	default: return "other";
	}
}

/* The event as a table: its name, whether it was cancelled, and the payload's fields. */
static void pushEvent(lua_State* L, const SmsEvent* ev)
{
	lua_createtable(L, 0, 8);
	const char* name = NULL;
	for (size_t i = 0; i < sizeof kEvents / sizeof kEvents[0]; i++)
		if (kEvents[i].type == ev->type)
			name = kEvents[i].name;
	if (name)
		setstr(L, "type", name);
	else
		setint(L, "type", ev->type);
	setbool(L, "cancelled", ev->cancelled);
	setstr(L, "cancelled_by", ev->cancelled_by);
	const void* d = ev->data;
	if (!d)
		return;
	switch (ev->type) {
	case SMS_EV_FRAME: {
		const SmsEvFrame* p = (const SmsEvFrame*)d;
		setint(L, "frame", p->frame);
		setstr(L, "mode", modeName(p->mode));
		break;
	}
	case SMS_EV_TICK: {
		const SmsEvTick* p = (const SmsEvTick*)d;
		setint(L, "tick", p->tick);
		setnum(L, "dt", p->dt);
		setint(L, "ticks_per_second", p->ticks_per_second);
		break;
	}
	case SMS_EV_DRAW: {
		const SmsEvDraw* p = (const SmsEvDraw*)d;
		setnum(L, "w", p->canvas_w);
		setnum(L, "h", p->canvas_h);
		setint(L, "frame", p->frame);
		setstr(L, "mode", modeName(p->mode));
		break;
	}
	case SMS_EV_STAGE_LOADED:
	case SMS_EV_STAGE_LEAVING:
	case SMS_EV_LIFE_LOST:
	case SMS_EV_FILE_SELECTED:
	case SMS_EV_STAGE_START: {
		const SmsEvStage* p = (const SmsEvStage*)d;
		setint(L, "area", p->area);
		setint(L, "episode", p->episode);
		if (ev->size >= offsetof(SmsEvStage, shine) + sizeof p->shine) { /* API 1.4 */
			setstr(L, "reason", leaveName(p->reason));
			setint(L, "shine", p->shine);
		}
		break;
	}
	case SMS_EV_STAGE_LOADING: {
		const SmsEvStageLoading* p = (const SmsEvStageLoading*)d;
		setint(L, "area", p->area);
		setint(L, "episode", p->episode);
		setstr(L, "reason", leaveName(p->reason));
		setint(L, "shine", p->shine);
		setint(L, "from_area", p->from_area);
		setint(L, "from_episode", p->from_episode);
		break;
	}
	case SMS_EV_STAGE_EXIT_REQUEST: {
		const SmsEvStageExit* p = (const SmsEvStageExit*)d;
		setint(L, "code", p->code);
		setint(L, "area", p->area);
		setint(L, "episode", p->episode);
		setobj(L, "actor", p->actor);
		break;
	}
	case SMS_EV_ITEM_COLLECTED: {
		const SmsEvItem* p = (const SmsEvItem*)d;
		setobj(L, "item", p->item);
		setobj(L, "taker", p->taker);
		setstr(L, "kind", itemKind(p->kind));
		setint(L, "actor_type", p->actor_type);
		setvec(L, "position", &p->position);
		break;
	}
	case SMS_EV_PAD_READ: {
		const SmsEvPadRead* p = (const SmsEvPadRead*)d;
		lua_createtable(L, 4, 0);
		for (int i = 0; i < 4; i++) {
			pushPad(L, &p->pads[i]);
			lua_seti(L, -2, i + 1);
		}
		lua_setfield(L, -2, "pads");
		break;
	}
	case SMS_EV_KEY: {
		const SmsEvKey* p = (const SmsEvKey*)d;
		setint(L, "scancode", p->scancode);
		setstr(L, "key", api->input->key_name(p->scancode));
		setbool(L, "down", p->down);
		setbool(L, "repeated", p->repeat);
		setbool(L, "shift", p->modifiers & SMS_KEYMOD_SHIFT);
		setbool(L, "ctrl", p->modifiers & SMS_KEYMOD_CTRL);
		setbool(L, "alt", p->modifiers & SMS_KEYMOD_ALT);
		break;
	}
	case SMS_EV_CUTSCENE: {
		const SmsEvCutscene* p = (const SmsEvCutscene*)d;
		setstr(L, "name", p->name);
		if (p->has_position)
			setvec(L, "position", &p->position);
		setint(L, "frames", p->frames);
		setobj(L, "actor", p->actor);
		break;
	}
	case SMS_EV_GOOP_CLEANED: {
		const SmsEvGoop* p = (const SmsEvGoop*)d;
		setvec(L, "position", &p->position);
		setnum(L, "size", p->size);
		break;
	}
	case SMS_EV_OBJECT_HIT:
	case SMS_EV_PLAYER_HIT: {
		const SmsEvHit* p = (const SmsEvHit*)d;
		setobj(L, "object", p->object);
		setobj(L, "sender", p->sender);
		setint(L, "message", p->message);
		break;
	}
	case SMS_EV_SAVE_STATE_REQUEST: break;
	case SMS_EV_HIDE_PLAYER_MODEL: setobj(L, "object", ((const SmsEvObject*)d)->object); break;
	case SMS_EV_PLAYER_STATUS: {
		const SmsEvPlayerStatus* p = (const SmsEvPlayerStatus*)d;
		setint(L, "from", p->from);
		setint(L, "to", p->to);
		setint(L, "arg", p->arg);
		setbool(L, "forced", p->forced);
		break;
	}
	case SMS_EV_PLAYER_NOZZLE: {
		const SmsEvPlayerNozzle* p = (const SmsEvPlayerNozzle*)d;
		setint(L, "from", p->from);
		setint(L, "to", p->to);
		setbool(L, "animate", p->animate);
		setstr(L, "reason", nozzleReasonName(p->reason));
		setobj(L, "box", p->box);
		break;
	}
	case SMS_EV_PLAYER_DAMAGE:
	case SMS_EV_PLAYER_HEAL: {
		const SmsEvDamage* p = (const SmsEvDamage*)d;
		setint(L, "amount", p->amount);
		setbool(L, "underwater", p->underwater);
		if (ev->size >= offsetof(SmsEvDamage, source) + sizeof p->source)
			setobj(L, "source", p->source);
		break;
	}
	case SMS_EV_PLAYER_STATUS_CHANGED: {
		const SmsEvPlayerStatus* p = (const SmsEvPlayerStatus*)d;
		setint(L, "from", p->from);
		setint(L, "to", p->to);
		setint(L, "arg", p->arg);
		break;
	}
	case SMS_EV_PLAYER_JUMP: {
		const SmsEvPlayerJump* p = (const SmsEvPlayerJump*)d;
		setint(L, "status", p->status);
		setint(L, "from", p->from);
		setvec(L, "position", &p->position);
		setvec(L, "velocity", &p->velocity);
		break;
	}
	case SMS_EV_PLAYER_LAND: {
		const SmsEvPlayerLand* p = (const SmsEvPlayerLand*)d;
		setint(L, "status", p->status);
		setint(L, "from", p->from);
		setnum(L, "fall", p->fall);
		setnum(L, "speed", p->speed);
		setvec(L, "position", &p->position);
		break;
	}
	case SMS_EV_PLAYER_DEATH: {
		const SmsEvPlayerDeath* p = (const SmsEvPlayerDeath*)d;
		setstr(L, "cause", deathName(p->cause));
		setvec(L, "position", &p->position);
		setobj(L, "source", p->source);
		break;
	}
	case SMS_EV_PLAYER_SPAWN: {
		const SmsEvPlayerSpawn* p = (const SmsEvPlayerSpawn*)d;
		setvec(L, "position", &p->position);
		setnum(L, "yaw", p->yaw);
		setbool(L, "has_position", p->has_position);
		setstr(L, "kind", startName(p->kind));
		break;
	}
	case SMS_EV_PLAYER_MOUNT: {
		const SmsEvPlayerMount* p = (const SmsEvPlayerMount*)d;
		setstr(L, "mount", p->mount == SMS_MOUNT_YOSHI ? "yoshi" : "blooper");
		setbool(L, "on", p->on);
		setobj(L, "object", p->object);
		setint(L, "color", p->color);
		break;
	}
	case SMS_EV_PLAYER_WATER: {
		const SmsEvPlayerWater* p = (const SmsEvPlayerWater*)d;
		setbool(L, "entered", p->entered);
		setnum(L, "surface_y", p->surface_y);
		setvec(L, "position", &p->position);
		break;
	}
	case SMS_EV_YOSHI_HATCH:
	case SMS_EV_YOSHI_EAT: {
		const SmsEvYoshi* p = (const SmsEvYoshi*)d;
		setint(L, "fruit", p->fruit);
		setint(L, "color", p->color);
		setvec(L, "position", &p->position);
		break;
	}
	case SMS_EV_ACTOR_SPAWN:
	case SMS_EV_ACTOR_DEATH: {
		const SmsEvActor* p = (const SmsEvActor*)d;
		setobj(L, "object", p->object);
		setint(L, "actor_type", p->actor_type);
		setvec(L, "position", &p->position);
		setobj(L, "other", p->other);
		setbool(L, "forced", p->forced);
		break;
	}
	case SMS_EV_ACTOR_DAMAGE: {
		const SmsEvActorDamage* p = (const SmsEvActorDamage*)d;
		setobj(L, "object", p->object);
		setobj(L, "attacker", p->attacker);
		setint(L, "actor_type", p->actor_type);
		setint(L, "message", p->message);
		setint(L, "health_before", p->health_before);
		setint(L, "health", p->health);
		setint(L, "max_health", p->max_health);
		setvec(L, "position", &p->position);
		break;
	}
	case SMS_EV_SHINE_APPEAR:
	case SMS_EV_SHINE_GET: {
		const SmsEvShineActor* p = (const SmsEvShineActor*)d;
		setobj(L, "object", p->object);
		setint(L, "shine", p->shine);
		setbool(L, "had", p->had);
		setvec(L, "position", &p->position);
		break;
	}
	case SMS_EV_FLAG_CHANGED: {
		const SmsEvFlag* p = (const SmsEvFlag*)d;
		setint(L, "id", p->id);
		setint(L, "old", p->old);
		setint(L, "value", p->value);
		break;
	}
	case SMS_EV_SOUND: {
		const SmsEvSound* p = (const SmsEvSound*)d;
		setint(L, "id", p->id);
		setbool(L, "system", p->system);
		if (p->has_position)
			setvec(L, "position", &p->position);
		break;
	}
	case SMS_EV_MUSIC: {
		const SmsEvMusic* p = (const SmsEvMusic*)d;
		setint(L, "id", p->id);
		setbool(L, "start", p->start);
		break;
	}
	case SMS_EV_TALK: {
		const SmsEvTalk* p = (const SmsEvTalk*)d;
		setstr(L, "what", pick(kTalks, 3, p->what));
		setobj(L, "npc", p->npc);
		if (p->what == SMS_TALK_MESSAGE) {
			setint(L, "message", p->message);
			setbool(L, "last", p->last);
		}
		break;
	}
	case SMS_EV_PAUSE: {
		const SmsEvPause* p = (const SmsEvPause*)d;
		setbool(L, "paused", p->paused);
		setstr(L, "kind", pick(kPauses, 3, p->kind));
		break;
	}
	case SMS_EV_MODE_CHANGED: {
		const SmsEvMode* p = (const SmsEvMode*)d;
		setstr(L, "from", modeName(p->from));
		setstr(L, "to", modeName(p->to));
		break;
	}
	case SMS_EV_CUTSCENE_END: {
		const SmsEvCutsceneEnd* p = (const SmsEvCutsceneEnd*)d;
		setstr(L, "name", p->name);
		setobj(L, "actor", p->actor);
		setbool(L, "forced", p->forced);
		break;
	}
	case SMS_EV_GAME_SAVED: setint(L, "file", ((const SmsEvGameSaved*)d)->file); break;
	case SMS_EV_TIMER: {
		const SmsEvTimer* p = (const SmsEvTimer*)d;
		setstr(L, "what", pick(kTimers, 3, p->what));
		setint(L, "time", p->time);
		break;
	}
	case SMS_EV_SCRIPT_CALL: {
		const SmsEvScriptCall* p = (const SmsEvScriptCall*)d;
		setstr(L, "name", p->name);
		setobj(L, "owner", p->owner);
		setint(L, "argc", p->argc);
		break;
	}
	case SMS_EV_MAP_EVENT: setobj(L, "object", ((const SmsEvObject*)d)->object); break;
	case SMS_EV_SAVE_PROMPT: setstr(L, "kind", savePromptName(((const SmsEvSavePrompt*)d)->kind)); break;
	case SMS_EV_SHINE_COLLECTED: {
		const SmsEvShine* p = (const SmsEvShine*)d;
		setint(L, "shine", p->shine);
		setint(L, "total", p->total);
		break;
	}
	case SMS_EV_CAMERA: {
		const SmsEvCamera* p = (const SmsEvCamera*)d;
		setvec(L, "position", &p->position);
		setvec(L, "target", &p->target);
		setnum(L, "fovy", p->fovy);
		setnum(L, "aspect", p->aspect);
		break;
	}
	case SMS_EV_OBJECT_MOVE: {
		const SmsEvObjectMove* p = (const SmsEvObjectMove*)d;
		setobj(L, "object", p->object);
		setbool(L, "after", p->after);
		break;
	}
	case SMS_EV_MOD_LOADED: {
		const SmsEvMod* p = (const SmsEvMod*)d;
		setstr(L, "id", p->id);
		setstr(L, "name", p->name);
		setstr(L, "version", p->version);
		break;
	}
	case SMS_EV_NET_PLAYER_JOINED:
	case SMS_EV_NET_PLAYER_LEFT:
	case SMS_EV_NET_PLAYER_ARRIVED: {
		const SmsEvNetPlayer* p = (const SmsEvNetPlayer*)d;
		setint(L, "slot", p->slot);
		setstr(L, "name", p->name);
		setint(L, "character", p->character);
		break;
	}
	case SMS_EV_NET_SESSION: {
		const SmsEvNetSession* p = (const SmsEvNetSession*)d;
		setint(L, "area", p->area);
		setint(L, "episode", p->episode);
		setint(L, "session", p->session);
		break;
	}
	case SMS_EV_NET_PLAYER_SPLASHED: {
		const SmsEvNetSplash* p = (const SmsEvNetSplash*)d;
		setint(L, "slot", p->slot);
		setint(L, "by", p->by);
		setint(L, "drops", p->drops);
		setvec(L, "position", &p->position);
		break;
	}
	case SMS_EV_SAVE_FILE: {
		const SmsEvSaveFile* p = (const SmsEvSaveFile*)d;
		setstr(L, "what", pick(kSaveFiles, 5, p->what));
		setint(L, "file", p->file);
		if (p->from >= 0)
			setint(L, "from", p->from);
		break;
	}
	case SMS_EV_NOZZLE_FIRE: {
		const SmsEvNozzleFire* p = (const SmsEvNozzleFire*)d;
		setint(L, "nozzle", p->nozzle);
		setstr(L, "how", pick(kFires, 2, p->how));
		break;
	}
	case SMS_EV_SHINE_SETUP: {
		const SmsEvShineSetup* p = (const SmsEvShineSetup*)d;
		setobj(L, "object", p->object);
		setint(L, "shine", p->shine);
		setbool(L, "collected", p->collected);
		setstr(L, "model", p->model);
		break;
	}
	case SMS_EV_YOSHI_EGG: {
		const SmsEvYoshiEgg* p = (const SmsEvYoshiEgg*)d;
		setstr(L, "kind", pick(kEggs, 3, p->kind));
		setbool(L, "present", p->present);
		setobj(L, "object", p->object);
		break;
	}
	case SMS_EV_FILE_STARTING: {
		const SmsEvFileStarting* p = (const SmsEvFileStarting*)d;
		setint(L, "file", p->file);
		setbool(L, "is_new", p->is_new);
		break;
	}
	case SMS_EV_FILE_MENU: {
		const SmsEvFileMenu* p = (const SmsEvFileMenu*)d;
		setint(L, "file", p->file);
		setbool(L, "is_empty", p->is_empty);
		setbool(L, "open", p->open);
		if (p->item >= 0)
			setstr(L, "item", pick(kFileMenu, 4, p->item));
		break;
	}
	case SMS_EV_EPISODE_SHINE: {
		const SmsEvEpisodeShine* p = (const SmsEvEpisodeShine*)d;
		setint(L, "area", p->area);
		setint(L, "episode", p->episode);
		setint(L, "shine", p->shine);
		break;
	}
	default:
		/* a custom event: its payload as a string of bytes (string.unpack reads structs) */
		if (ev->type >= SMS_EV_CUSTOM_BASE && ev->size) {
			lua_pushlstring(L, (const char*)d, ev->size);
			lua_setfield(L, -2, "data");
		}
		break;
	}
}

/* What a handler may change, from the table back into the payload. */
static void readEvent(lua_State* L, int t, SmsEvent* ev, Script* s)
{
	void* d = ev->data;
	if (!d)
		return;
	switch (ev->type) {
	case SMS_EV_STAGE_LEAVING:
	case SMS_EV_LIFE_LOST:
	case SMS_EV_FILE_SELECTED: {
		SmsEvStage* p = (SmsEvStage*)d;
		p->area       = (int)fieldnum(L, t, "area", (float)p->area);
		p->episode    = (int)fieldnum(L, t, "episode", (float)p->episode);
		break;
	}
	case SMS_EV_STAGE_LOADING: {
		SmsEvStageLoading* p = (SmsEvStageLoading*)d;
		p->area              = (int)fieldnum(L, t, "area", (float)p->area);
		p->episode           = (int)fieldnum(L, t, "episode", (float)p->episode);
		p->from_area         = (int)fieldnum(L, t, "from_area", (float)p->from_area);
		p->from_episode      = (int)fieldnum(L, t, "from_episode", (float)p->from_episode);
		break;
	}
	case SMS_EV_PAD_READ: {
		SmsEvPadRead* p = (SmsEvPadRead*)d;
		lua_getfield(L, t, "pads");
		if (lua_istable(L, -1))
			for (int i = 0; i < 4; i++) {
				lua_geti(L, -1, i + 1);
				if (lua_istable(L, -1))
					readPad(L, lua_gettop(L), &p->pads[i]);
				lua_pop(L, 1);
			}
		lua_pop(L, 1);
		break;
	}
	case SMS_EV_PLAYER_STATUS: {
		SmsEvPlayerStatus* p = (SmsEvPlayerStatus*)d;
		lua_getfield(L, t, "to");
		if (lua_isinteger(L, -1))
			p->to = (uint32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, t, "arg");
		if (lua_isinteger(L, -1))
			p->arg = (uint32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		break;
	}
	case SMS_EV_PLAYER_NOZZLE: {
		SmsEvPlayerNozzle* p = (SmsEvPlayerNozzle*)d;
		lua_getfield(L, t, "to");
		if (lua_isinteger(L, -1))
			p->to = (int)lua_tointeger(L, -1);
		lua_pop(L, 1);
		break;
	}
	case SMS_EV_PLAYER_DAMAGE:
	case SMS_EV_PLAYER_HEAL: {
		SmsEvDamage* p = (SmsEvDamage*)d;
		p->amount      = (int)fieldnum(L, t, "amount", (float)p->amount);
		break;
	}
	case SMS_EV_PLAYER_SPAWN: {
		SmsEvPlayerSpawn* p = (SmsEvPlayerSpawn*)d;
		SmsVec3 was         = p->position;
		lua_getfield(L, t, "position");
		tovec(L, -1, &p->position);
		lua_pop(L, 1);
		p->yaw = fieldnum(L, t, "yaw", p->yaw);
		lua_getfield(L, t, "has_position");
		if (lua_isboolean(L, -1))
			p->has_position = lua_toboolean(L, -1);
		lua_pop(L, 1);
		/* a new place is one to put him at */
		if (p->position.x != was.x || p->position.y != was.y || p->position.z != was.z)
			p->has_position = 1;
		lua_getfield(L, t, "kind"); /* API 1.5 */
		if (lua_type(L, -1) == LUA_TSTRING) {
			const char* k = lua_tostring(L, -1);
			for (int i = 0; i <= 2; i++)
				if (!strcmp(k, kStarts[i]))
					p->kind = i;
		}
		lua_pop(L, 1);
		break;
	}
	case SMS_EV_SHINE_SETUP: {
		SmsEvShineSetup* p = (SmsEvShineSetup*)d;
		lua_getfield(L, t, "model");
		if (lua_type(L, -1) == LUA_TSTRING) {
			const char* m = lua_tostring(L, -1);
			if (!p->model || strcmp(m, p->model)) {
				/* relative to the script mod's own folder */
				const int full = m[0] == '/' || m[0] == '\\' || (m[0] && m[1] == ':');
				snprintf(s->model, sizeof s->model, "%s%s%s", full ? "" : api->core->mod_dir(s->mod), full ? "" : "/", m);
				p->model = s->model;
			}
		} else if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) {
			p->model = NULL; /* false: the game's */
		}
		lua_pop(L, 1);
		break;
	}
	case SMS_EV_YOSHI_EGG: {
		SmsEvYoshiEgg* p = (SmsEvYoshiEgg*)d;
		lua_getfield(L, t, "present");
		if (lua_isboolean(L, -1))
			p->present = lua_toboolean(L, -1);
		lua_pop(L, 1);
		break;
	}
	case SMS_EV_EPISODE_SHINE: {
		SmsEvEpisodeShine* p = (SmsEvEpisodeShine*)d;
		p->shine             = (int)fieldnum(L, t, "shine", (float)p->shine);
		break;
	}
	case SMS_EV_FLAG_CHANGED: {
		SmsEvFlag* p = (SmsEvFlag*)d;
		lua_getfield(L, t, "value");
		if (lua_isinteger(L, -1))
			p->value = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		break;
	}
	case SMS_EV_SOUND:
	case SMS_EV_MUSIC: {
		uint32_t* id = ev->type == SMS_EV_SOUND ? &((SmsEvSound*)d)->id : &((SmsEvMusic*)d)->id;
		lua_getfield(L, t, "id");
		if (lua_isinteger(L, -1))
			*id = (uint32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		break;
	}
	case SMS_EV_TALK: {
		SmsEvTalk* p = (SmsEvTalk*)d;
		lua_getfield(L, t, "message");
		if (p->what == SMS_TALK_MESSAGE && lua_isinteger(L, -1))
			p->message = (uint32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		break;
	}
	case SMS_EV_CAMERA: {
		SmsEvCamera* p = (SmsEvCamera*)d;
		lua_getfield(L, t, "position");
		tovec(L, -1, &p->position);
		lua_pop(L, 1);
		lua_getfield(L, t, "target");
		tovec(L, -1, &p->target);
		lua_pop(L, 1);
		p->fovy = fieldnum(L, t, "fovy", p->fovy);
		break;
	}
	case SMS_EV_SAVE_STATE_REQUEST: {
		SmsEvSaveState* p = (SmsEvSaveState*)d;
		lua_getfield(L, t, "reason");
		if (lua_isstring(L, -1)) {
			strncpy(s->reason, lua_tostring(L, -1), sizeof s->reason - 1);
			p->reason = s->reason;
		}
		lua_pop(L, 1);
		break;
	}
	default:
		break;
	}
}

static int onEvent(SmsMod* mod, SmsEvent* ev, void* user)
{
	(void)mod;
	Ref* r       = (Ref*)user;
	Script* s    = r->s;
	lua_State* L = s->L;
	pushEvent(L, ev);                         /* ev */
	lua_rawgeti(L, LUA_REGISTRYINDEX, r->fn); /* ev fn */
	lua_pushvalue(L, -2);                     /* ev fn ev */
	if (call(s, 1, 1) != LUA_OK) {
		lua_pop(L, 1);
		return SMS_CONTINUE;
	}
	/* ev result: true (or sms.CANCEL) cancels or consumes */
	const int cancel = lua_toboolean(L, -1) && !(lua_isnumber(L, -1) && lua_tonumber(L, -1) == 0);
	readEvent(L, lua_gettop(L) - 1, ev, s);
	lua_pop(L, 2);
	return cancel ? SMS_CANCEL : SMS_CONTINUE;
}

/* ------------------------------------------------------------------ sms.* : core */

static int l_on(lua_State* L)
{
	const uint32_t type = eventType(L, 1);
	luaL_checktype(L, 2, LUA_TFUNCTION);
	const int prio = (int)luaL_optinteger(L, 3, SMS_PRIORITY_NORMAL);
	Ref* r         = newRef(L, 2);
	const int h    = api->core->subscribe(MOD(L), type, onEvent, r, prio);
	if (!h)
		return luaL_error(L, "could not subscribe to that event");
	lua_pushinteger(L, h);
	return 1;
}

static int l_off(lua_State* L)
{
	api->core->unsubscribe(MOD(L), (int)luaL_checkinteger(L, 1));
	return 0;
}

static int logAt(lua_State* L, int level)
{
	const int n = lua_gettop(L); /* before the buffer, which may take a stack slot */
	luaL_Buffer b;
	luaL_buffinit(L, &b);
	for (int i = 1; i <= n; i++) {
		if (i > 1)
			luaL_addchar(&b, ' ');
		luaL_tolstring(L, i, NULL);
		luaL_addvalue(&b);
	}
	luaL_pushresult(&b);
	api->core->log(MOD(L), level, "%s", lua_tostring(L, -1));
	return 0;
}
static int l_log(lua_State* L) { return logAt(L, SMS_LOG_INFO); }
static int l_warn(lua_State* L) { return logAt(L, SMS_LOG_WARN); }
static int l_error(lua_State* L) { return logAt(L, SMS_LOG_ERROR); }
static int l_debug(lua_State* L) { return logAt(L, SMS_LOG_DEBUG); }

static int l_event_id(lua_State* L)
{
	lua_pushinteger(L, eventType(L, 1));
	return 1;
}

static int l_emit(lua_State* L)
{
	const uint32_t type = eventType(L, 1);
	size_t n            = 0;
	const char* data    = luaL_optlstring(L, 2, NULL, &n);
	lua_pushboolean(L, api->core->emit(MOD(L), type, (void*)data, data ? (uint32_t)n : 0));
	return 1;
}

static int l_time(lua_State* L)
{
	lua_pushnumber(L, api->core->time());
	return 1;
}

static int l_is_loaded(lua_State* L)
{
	lua_pushboolean(L, api->core->is_loaded(luaL_checkstring(L, 1)));
	return 1;
}

static int l_data_dir(lua_State* L)
{
	lua_pushstring(L, api->core->data_dir(MOD(L)));
	return 1;
}

/* sms.files_read(name [, file]) -> the file's bytes as a string, or nil.
   file: nil the mod's own, 0..2 a save file, "current" the one played
   (API 1.5). */
static int filesScope(lua_State* L, int idx)
{
	if (lua_isnoneornil(L, idx))
		return SMS_FILES_MOD;
	if (lua_type(L, idx) == LUA_TSTRING && !strcmp(lua_tostring(L, idx), "current"))
		return SMS_FILES_CURRENT;
	return (int)luaL_checkinteger(L, idx);
}

static int l_files_read(lua_State* L)
{
	const char* name = luaL_checkstring(L, 1);
	const int scope  = filesScope(L, 2);
	if (!SMS_API_HAS(api, files)) {
		lua_pushnil(L);
		return 1;
	}
	const int n = api->files->read(MOD(L), scope, name, NULL, 0);
	if (n < 0) {
		lua_pushnil(L);
		return 1;
	}
	char* buf = (char*)malloc(n ? (size_t)n : 1);
	if (!buf)
		return luaL_error(L, "out of memory");
	const int got = api->files->read(MOD(L), scope, name, buf, (uint32_t)n);
	lua_pushlstring(L, buf, got < 0 ? 0 : (size_t)(got < n ? got : n));
	free(buf);
	return 1;
}

/* sms.files_write(name, data [, file]) -> whether it was written (or kept for the save). */
static int l_files_write(lua_State* L)
{
	size_t n;
	const char* name = luaL_checkstring(L, 1);
	const char* data = luaL_checklstring(L, 2, &n);
	const int scope  = filesScope(L, 3);
	lua_pushboolean(L, SMS_API_HAS(api, files) && api->files->write(MOD(L), scope, name, data, (uint32_t)n));
	return 1;
}

/* sms.files_remove(name [, file]) -> whether it was there. */
static int l_files_remove(lua_State* L)
{
	const char* name = luaL_checkstring(L, 1);
	const int scope  = filesScope(L, 2);
	lua_pushboolean(L, SMS_API_HAS(api, files) && api->files->remove(MOD(L), scope, name));
	return 1;
}

/* sms.files_dir() -> the mod's folder beside the memory card, or nil. */
static int l_files_dir(lua_State* L)
{
	if (SMS_API_HAS(api, files))
		lua_pushstring(L, api->files->dir(MOD(L)));
	else
		lua_pushnil(L);
	return 1;
}

/* sms.set_object_color(obj, which, color) -> whether it has a model; which:
   "material", "tev0", "tev1", "tev2" (API 1.5). */
static int l_set_object_color(lua_State* L)
{
	static const char* const kWhich[] = {"material", "tev0", "tev1", "tev2", NULL};
	SmsObject o     = (SmsObject)lua_touserdata(L, 1);
	const int which = luaL_checkoption(L, 2, "material", kWhich);
	const SmsColor c = (SmsColor)(lua_Unsigned)luaL_checkinteger(L, 3);
	lua_pushboolean(L, SMS_API_HAS(api->game, object_set_color) && o && api->game->object_set_color(o, which, c));
	return 1;
}

static int l_info(lua_State* L)
{
	const char* v = api->core->mod_info(MOD(L), luaL_checkstring(L, 1));
	if (v)
		lua_pushstring(L, v);
	else
		lua_pushnil(L);
	return 1;
}

/* ------------------------------------------------------------------ sms.* : game */

static int l_state(lua_State* L)
{
	SmsGameState st;
	memset(&st, 0, sizeof st);
	st.size = sizeof st;
	api->game->state(&st);
	lua_createtable(L, 0, 11);
	setstr(L, "mode", modeName(st.mode));
	setbool(L, "stage_ready", st.stage_ready);
	if (st.stage_ready) {
		setint(L, "area", st.area);
		setint(L, "episode", st.episode);
	}
	setint(L, "frame", st.frame);
	setint(L, "tick", st.tick);
	setint(L, "ticks_per_second", st.ticks_per_second);
	setbool(L, "eclipse", st.eclipse);
	setint(L, "save_file", st.save_file);
	setbool(L, "paused", st.paused);
	return 1;
}

static int l_flag(lua_State* L)
{
	lua_pushinteger(L, api->game->flag_get((uint32_t)luaL_checkinteger(L, 1)));
	return 1;
}
static int l_set_flag(lua_State* L)
{
	api->game->flag_set((uint32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2));
	return 0;
}
static int l_coins(lua_State* L) { lua_pushinteger(L, api->game->coins()); return 1; }
static int l_lives(lua_State* L) { lua_pushinteger(L, api->game->lives()); return 1; }
static int l_shines(lua_State* L) { lua_pushinteger(L, api->game->shines()); return 1; }
static int l_blue_coins(lua_State* L) { lua_pushinteger(L, api->game->blue_coins()); return 1; }
static int l_has_shine(lua_State* L)
{
	lua_pushboolean(L, api->game->has_shine((int)luaL_checkinteger(L, 1)));
	return 1;
}
static int l_warp(lua_State* L)
{
	lua_pushboolean(L, api->game->warp((int)luaL_checkinteger(L, 1), (int)luaL_optinteger(L, 2, 0)));
	return 1;
}
static int l_area_name(lua_State* L)
{
	lua_pushstring(L, api->game->area_name((int)luaL_checkinteger(L, 1)));
	return 1;
}
static int l_find_object(lua_State* L)
{
	SmsObject o = api->game->find_object(luaL_checkstring(L, 1));
	if (o)
		lua_pushlightuserdata(L, o);
	else
		lua_pushnil(L);
	return 1;
}
static SmsObject checkobj(lua_State* L, int idx)
{
	luaL_checktype(L, idx, LUA_TLIGHTUSERDATA);
	return lua_touserdata(L, idx);
}
static int l_object_name(lua_State* L)
{
	lua_pushstring(L, api->game->object_name(checkobj(L, 1)));
	return 1;
}
static int l_object_type(lua_State* L)
{
	SmsObject o = checkobj(L, 1);
	lua_pushinteger(L, SMS_API_HAS(api->game, object_type) ? api->game->object_type(o) : 0);
	return 1;
}
/* health, most (nil when it has none) */
static int l_object_health(lua_State* L)
{
	SmsObject o = checkobj(L, 1);
	int max     = 0;
	const int h = SMS_API_HAS(api->game, object_health) ? api->game->object_health(o, &max) : -1;
	if (h < 0)
		return 0;
	lua_pushinteger(L, h);
	lua_pushinteger(L, max);
	return 2;
}
static int l_object_position(lua_State* L)
{
	SmsVec3 v;
	if (!api->game->object_position(checkobj(L, 1), &v))
		return 0;
	pushvec(L, &v);
	return 1;
}
static int l_set_object_position(lua_State* L)
{
	const SmsVec3 v = checkvec(L, 2);
	lua_pushboolean(L, api->game->object_set_position(checkobj(L, 1), &v));
	return 1;
}

/* ------------------------------------------------------------------ sms.* : Mario, camera, sound */

static int l_mario(lua_State* L)
{
	SmsPlayer p;
	memset(&p, 0, sizeof p);
	p.size = sizeof p;
	if (!api->player->get(&p))
		return 0;
	lua_createtable(L, 0, 20);
	setvec(L, "position", &p.position);
	setvec(L, "velocity", &p.velocity);
	setnum(L, "forward_speed", p.forward_speed);
	setnum(L, "yaw", p.yaw);
	setint(L, "health", p.health);
	setint(L, "max_health", p.max_health);
	setint(L, "water", p.water);
	setint(L, "max_water", p.max_water);
	setbool(L, "has_fludd", p.has_fludd);
	setint(L, "nozzle", p.nozzle);
	setint(L, "status", p.status);
	setint(L, "status_timer", p.status_timer);
	setbool(L, "on_ground", p.on_ground);
	setbool(L, "in_water", p.in_water);
	setbool(L, "on_yoshi", p.on_yoshi);
	setvec(L, "floor", &p.floor);
	setobj(L, "object", p.object);
	if (p.size >= offsetof(SmsPlayer, helmet) + sizeof p.helmet) {
		setint(L, "current_nozzle", p.current_nozzle);
		setbool(L, "shirt", p.shirt);
		setbool(L, "sunglasses", p.sunglasses);
		setbool(L, "helmet", p.helmet);
	}
	return 1;
}

static int l_set_mario(lua_State* L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	SmsVec3 v;
	lua_getfield(L, 1, "position");
	if (tovec(L, -1, &v))
		api->player->set_position(&v);
	lua_getfield(L, 1, "velocity");
	if (tovec(L, -1, &v))
		api->player->set_velocity(&v);
	lua_getfield(L, 1, "forward_speed");
	if (lua_isnumber(L, -1))
		api->player->set_forward_speed((float)lua_tonumber(L, -1));
	lua_getfield(L, 1, "health");
	if (lua_isnumber(L, -1))
		api->player->set_health((int)lua_tointeger(L, -1));
	lua_getfield(L, 1, "water");
	if (lua_isnumber(L, -1))
		api->player->set_water((int)lua_tointeger(L, -1));
	lua_pop(L, 5);
	return 0;
}

static int l_set_status(lua_State* L)
{
	lua_pushboolean(L, api->player->set_status((uint32_t)luaL_checkinteger(L, 1), (uint32_t)luaL_optinteger(L, 2, 0)));
	return 1;
}

static int l_set_nozzle(lua_State* L)
{
	const int n = (int)luaL_checkinteger(L, 1);
	const int animate = lua_isnoneornil(L, 2) ? 1 : lua_toboolean(L, 2);
	lua_pushboolean(L, SMS_API_HAS(api->player, set_nozzle) && api->player->set_nozzle(n, animate));
	return 1;
}

static int fieldbool(lua_State* L, int t, const char* k);

/* the HUD's parts, by the bit (SMS_HUD_*) each is */
static const char* const kHudParts[] = {"coins", "shines", "blue_coins", "health", "water", "lives", "red_coins"};

/* sms.set_hud_hidden{ coins = true, health = true, ... } or { all = true };
   an empty table (or none) shows the HUD again. Returns whether the game
   can (API 1.5). */
static int l_set_hud_hidden(lua_State* L)
{
	uint32_t mask = 0;
	if (lua_istable(L, 1)) {
		const int all = fieldbool(L, 1, "all");
		for (int k = 0; k < 7; k++)
			if (all || fieldbool(L, 1, kHudParts[k]))
				mask |= 1u << k;
	}
	if (!SMS_API_HAS(api->game, set_hud_hidden)) {
		lua_pushboolean(L, 0);
		return 1;
	}
	api->game->set_hud_hidden(MOD(L), mask);
	lua_pushboolean(L, 1);
	return 1;
}

/* sms.hud_hidden(): { coins = true, ... } for the parts hidden now, whoever hides them */
static int l_hud_hidden(lua_State* L)
{
	const uint32_t mask = SMS_API_HAS(api->game, hud_hidden) ? api->game->hud_hidden() : 0;
	lua_newtable(L);
	for (int k = 0; k < 7; k++) {
		lua_pushboolean(L, (mask >> k) & 1u);
		lua_setfield(L, -2, kHudParts[k]);
	}
	return 1;
}

/* sms.set_yaw(degrees): Mario faces that way at once (API 1.5) */
static int l_set_yaw(lua_State* L)
{
	const float deg = (float)luaL_checknumber(L, 1);
	lua_pushboolean(L, SMS_API_HAS(api->player, set_yaw) && api->player->set_yaw(deg));
	return 1;
}

/* sms.set_aim(yaw [, pitch]) turns Mario's body and FLUDD that way (pitch
   degrees up) whichever way he moves; sms.set_aim(nil) gives his own aim
   back (API 1.5) */
static int l_set_aim(lua_State* L)
{
	const int on      = !lua_isnoneornil(L, 1);
	const float yaw   = on ? (float)luaL_checknumber(L, 1) : 0.0f;
	const float pitch = on ? (float)luaL_optnumber(L, 2, 0.0) : 0.0f;
	lua_pushboolean(L, SMS_API_HAS(api->player, set_aim) && api->player->set_aim(on, yaw, pitch));
	return 1;
}

/* sms.set_hidden_parts{ head = true, fludd = true }: parts of Mario left
   out, the rest of him drawn; an empty table (or none) draws all of him
   (API 1.5). Returns whether the game can. */
static int l_set_hidden_parts(lua_State* L)
{
	uint32_t parts = 0;
	if (lua_istable(L, 1)) {
		if (fieldbool(L, 1, "head"))
			parts |= SMS_PLAYER_PART_HEAD;
		if (fieldbool(L, 1, "fludd"))
			parts |= SMS_PLAYER_PART_FLUDD;
	}
	lua_pushboolean(L, SMS_API_HAS(api->player, set_hidden_parts) && api->player->set_hidden_parts(parts));
	return 1;
}

static int l_set_fludd(lua_State* L)
{
	lua_pushboolean(L, SMS_API_HAS(api->player, set_fludd) && api->player->set_fludd(lua_toboolean(L, 1)));
	return 1;
}

/* a piece of the outfit: true on, false off, nil as it is */
static int outfitArg(lua_State* L, int i) { return lua_isnoneornil(L, i) ? -1 : lua_toboolean(L, i); }

static int l_set_outfit(lua_State* L)
{
	int shirt, sunglasses, helmet;
	if (lua_istable(L, 1)) {
		lua_getfield(L, 1, "shirt");
		lua_getfield(L, 1, "sunglasses");
		lua_getfield(L, 1, "helmet");
		shirt      = outfitArg(L, -3);
		sunglasses = outfitArg(L, -2);
		helmet     = outfitArg(L, -1);
		lua_pop(L, 3);
	} else {
		shirt      = outfitArg(L, 1);
		sunglasses = outfitArg(L, 2);
		helmet     = outfitArg(L, 3);
	}
	lua_pushboolean(L, SMS_API_HAS(api->player, set_outfit) && api->player->set_outfit(shirt, sunglasses, helmet));
	return 1;
}

static int l_camera(lua_State* L)
{
	SmsCamera c;
	memset(&c, 0, sizeof c);
	c.size = sizeof c;
	if (!api->camera->get(&c))
		return 0;
	lua_createtable(L, 0, 7);
	setvec(L, "position", &c.position);
	setvec(L, "target", &c.target);
	setvec(L, "up", &c.up);
	setnum(L, "fovy", c.fovy);
	setnum(L, "aspect", c.aspect);
	setnum(L, "near", c.near_z);
	setnum(L, "far", c.far_z);
	return 1;
}

static int l_to_canvas(lua_State* L)
{
	const SmsVec3 v = checkvec(L, 1);
	float x, y, depth;
	if (!api->camera->world_to_canvas(&v, &x, &y, &depth))
		return 0;
	lua_pushnumber(L, x);
	lua_pushnumber(L, y);
	lua_pushnumber(L, depth);
	return 3;
}

static int l_play_sound(lua_State* L)
{
	api->audio->play_system_sound((uint32_t)luaL_checkinteger(L, 1));
	return 0;
}

/* ------------------------------------------------------------------ sms.draw */

static int d_canvas(lua_State* L)
{
	float w, h;
	api->draw->canvas(&w, &h);
	lua_pushnumber(L, w);
	lua_pushnumber(L, h);
	return 2;
}
static int d_rect(lua_State* L)
{
	api->draw->rect(num(L, 1), num(L, 2), num(L, 3), num(L, 4), optcolor(L, 5, 0xFFFFFFFFu));
	return 0;
}
static int d_rect_outline(lua_State* L)
{
	api->draw->rect_outline(num(L, 1), num(L, 2), num(L, 3), num(L, 4), num(L, 5), optcolor(L, 6, 0xFFFFFFFFu));
	return 0;
}
static int d_gradient(lua_State* L)
{
	api->draw->rect_gradient(num(L, 1), num(L, 2), num(L, 3), num(L, 4), optcolor(L, 5, 0xFFFFFFFFu),
	                         optcolor(L, 6, 0x000000FFu));
	return 0;
}
static int d_line(lua_State* L)
{
	api->draw->line(num(L, 1), num(L, 2), num(L, 3), num(L, 4), num(L, 5), optcolor(L, 6, 0xFFFFFFFFu));
	return 0;
}
static int d_triangle(lua_State* L)
{
	api->draw->triangle(num(L, 1), num(L, 2), num(L, 3), num(L, 4), num(L, 5), num(L, 6), optcolor(L, 7, 0xFFFFFFFFu));
	return 0;
}
static int d_circle(lua_State* L)
{
	api->draw->circle(num(L, 1), num(L, 2), num(L, 3), optcolor(L, 4, 0xFFFFFFFFu));
	return 0;
}
static int d_circle_outline(lua_State* L)
{
	api->draw->circle_outline(num(L, 1), num(L, 2), num(L, 3), num(L, 4), optcolor(L, 5, 0xFFFFFFFFu));
	return 0;
}
/* text(x, y, size, color, text [, flags]) -> width */
static int d_text(lua_State* L)
{
	/* the other arguments before luaL_tolstring, which pushes the text */
	const float x = num(L, 1), y = num(L, 2), size = num(L, 3);
	const SmsColor c = optcolor(L, 4, 0xFFFFFFFFu);
	const int flags  = (int)luaL_optinteger(L, 6, 0);
	const char* s    = luaL_tolstring(L, 5, NULL);
	lua_pushnumber(L, api->draw->text(x, y, size, c, flags, s));
	return 1;
}
static int d_text_width(lua_State* L)
{
	lua_pushnumber(L, api->draw->text_width(num(L, 1), luaL_tolstring(L, 2, NULL)));
	return 1;
}
static int d_image_load(lua_State* L)
{
	const SmsImage img = api->draw->image_load(MOD(L), luaL_checkstring(L, 1));
	if (!img)
		return 0;
	lua_pushinteger(L, img);
	return 1;
}
/* image_create(w, h, rgba) with rgba a string of w * h * 4 bytes */
static int d_image_create(lua_State* L)
{
	const int w = (int)luaL_checkinteger(L, 1), h = (int)luaL_checkinteger(L, 2);
	size_t n;
	const char* px = luaL_checklstring(L, 3, &n);
	if (w <= 0 || h <= 0 || n < (size_t)w * h * 4)
		return luaL_error(L, "image_create: need %d bytes of RGBA", w * h * 4);
	const SmsImage img = api->draw->image_create(MOD(L), (const uint8_t*)px, w, h);
	if (!img)
		return 0;
	lua_pushinteger(L, img);
	return 1;
}
static int d_image_free(lua_State* L)
{
	api->draw->image_free(MOD(L), (SmsImage)luaL_checkinteger(L, 1));
	return 0;
}
static int d_image_size(lua_State* L)
{
	int w, h;
	if (!api->draw->image_size((SmsImage)luaL_checkinteger(L, 1), &w, &h))
		return 0;
	lua_pushinteger(L, w);
	lua_pushinteger(L, h);
	return 2;
}
static int d_image(lua_State* L)
{
	api->draw->image((SmsImage)luaL_checkinteger(L, 1), num(L, 2), num(L, 3), num(L, 4), num(L, 5),
	                 optcolor(L, 6, 0xFFFFFFFFu));
	return 0;
}
static int d_image_region(lua_State* L)
{
	api->draw->image_region((SmsImage)luaL_checkinteger(L, 1), num(L, 2), num(L, 3), num(L, 4), num(L, 5), num(L, 6),
	                        num(L, 7), num(L, 8), num(L, 9), optcolor(L, 10, 0xFFFFFFFFu));
	return 0;
}
static int d_clip_push(lua_State* L)
{
	api->draw->clip_push(num(L, 1), num(L, 2), num(L, 3), num(L, 4));
	return 0;
}
static int d_clip_pop(lua_State* L)
{
	(void)L;
	api->draw->clip_pop();
	return 0;
}
static int d_world_line(lua_State* L)
{
	const SmsVec3 a = checkvec(L, 1), b = checkvec(L, 2);
	api->draw->world_line(&a, &b, (float)luaL_optnumber(L, 3, 2), optcolor(L, 4, 0xFFFFFFFFu));
	return 0;
}
static int d_world_box(lua_State* L)
{
	const SmsVec3 c = checkvec(L, 1), h = checkvec(L, 2);
	api->draw->world_box(&c, &h, (float)luaL_optnumber(L, 3, 2), optcolor(L, 4, 0xFFFFFFFFu));
	return 0;
}
static int d_world_ring(lua_State* L)
{
	const SmsVec3 c = checkvec(L, 1);
	api->draw->world_ring(&c, num(L, 2), (float)luaL_optnumber(L, 3, 2), optcolor(L, 4, 0xFFFFFFFFu));
	return 0;
}
/* world_text(pos, size, color, text [, flags [, scaled]]) */
static int d_world_text(lua_State* L)
{
	const SmsVec3 p    = checkvec(L, 1);
	const float size   = num(L, 2);
	const SmsColor c   = optcolor(L, 3, 0xFFFFFFFFu);
	const int flags    = (int)luaL_optinteger(L, 5, 0);
	const int scaled   = lua_isnoneornil(L, 6) ? 1 : lua_toboolean(L, 6);
	const char* s      = luaL_tolstring(L, 4, NULL); /* pushes: last */
	lua_pushboolean(L, api->draw->world_text(&p, size, scaled, c, flags, s));
	return 1;
}

static const char* const kAnchors[] = {"top_left", "top", "top_right", "left", "center",
                                       "right", "bottom_left", "bottom", "bottom_right", NULL};

/* hud_slot(anchor, w, h) -> x, y, w, h, or nothing when it does not fit */
static int d_hud_slot(lua_State* L)
{
	const int anchor = lua_type(L, 1) == LUA_TNUMBER ? (int)lua_tointeger(L, 1) : luaL_checkoption(L, 1, NULL, kAnchors);
	SmsRect r;
	if (!api->draw->hud_slot(MOD(L), anchor, num(L, 2), num(L, 3), &r))
		return 0;
	lua_pushnumber(L, r.x);
	lua_pushnumber(L, r.y);
	lua_pushnumber(L, r.w);
	lua_pushnumber(L, r.h);
	return 4;
}

/* ------------------------------------------------------------------ sms.* : input */

static int keyArg(lua_State* L, int idx)
{
	if (lua_type(L, idx) == LUA_TNUMBER)
		return (int)lua_tointeger(L, idx);
	return api->input->key_from_name(luaL_checkstring(L, idx));
}

static int l_pad(lua_State* L)
{
	SmsPad p;
	memset(&p, 0, sizeof p);
	p.size = sizeof p;
	api->input->pad((int)luaL_optinteger(L, 1, 0), &p);
	lua_createtable(L, 0, 10);
	setbool(L, "connected", p.connected);
	setint(L, "held", p.held);
	setint(L, "pressed", p.pressed);
	setint(L, "released", p.released);
	setnum(L, "stick_x", p.stick_x);
	setnum(L, "stick_y", p.stick_y);
	setnum(L, "cstick_x", p.cstick_x);
	setnum(L, "cstick_y", p.cstick_y);
	setnum(L, "trigger_l", p.trigger_l);
	setnum(L, "trigger_r", p.trigger_r);
	return 1;
}
static int l_key_down(lua_State* L)
{
	lua_pushboolean(L, api->input->key_down(keyArg(L, 1)));
	return 1;
}
static int l_key_pressed(lua_State* L)
{
	lua_pushboolean(L, api->input->key_pressed(keyArg(L, 1)));
	return 1;
}
static int l_key(lua_State* L)
{
	lua_pushinteger(L, keyArg(L, 1));
	return 1;
}
static int l_key_name(lua_State* L)
{
	lua_pushstring(L, api->input->key_name((int)luaL_checkinteger(L, 1)));
	return 1;
}
static int l_bind_key(lua_State* L)
{
	lua_pushinteger(L, api->input->bind_key(MOD(L), luaL_checkstring(L, 1), luaL_optstring(L, 2, "")));
	return 1;
}
static int l_capture(lua_State* L)
{
	lua_pushboolean(L, api->input->capture(MOD(L), lua_isnone(L, 1) ? 1 : lua_toboolean(L, 1)));
	return 1;
}
static int l_captured_by(lua_State* L)
{
	const char* id = api->input->captured_by();
	if (id)
		lua_pushstring(L, id);
	else
		lua_pushnil(L);
	return 1;
}

/* ------------------------------------------------------------------ sms.* : settings */

/* setting(key, default): the value, as the default's type (boolean, number or string) */
static int l_setting(lua_State* L)
{
	SmsMod* m       = MOD(L);
	const char* key = luaL_checkstring(L, 1);
	switch (lua_type(L, 2)) {
	case LUA_TBOOLEAN: lua_pushboolean(L, api->config->get_int(m, key, lua_toboolean(L, 2)) != 0); break;
	case LUA_TNUMBER:
		if (lua_isinteger(L, 2))
			lua_pushinteger(L, api->config->get_int(m, key, (int)lua_tointeger(L, 2)));
		else
			lua_pushnumber(L, api->config->get_float(m, key, (float)lua_tonumber(L, 2)));
		break;
	default: {
		const char* v = api->config->get_string(m, key, luaL_optstring(L, 2, NULL));
		if (v)
			lua_pushstring(L, v);
		else
			lua_pushnil(L);
		break;
	}
	}
	return 1;
}

static int l_set_setting(lua_State* L)
{
	SmsMod* m       = MOD(L);
	const char* key = luaL_checkstring(L, 1);
	switch (lua_type(L, 2)) {
	case LUA_TBOOLEAN: api->config->set_int(m, key, lua_toboolean(L, 2)); break;
	case LUA_TNUMBER:
		if (lua_isinteger(L, 2))
			api->config->set_int(m, key, (int)lua_tointeger(L, 2));
		else
			api->config->set_float(m, key, (float)lua_tonumber(L, 2));
		break;
	default: api->config->set_string(m, key, luaL_checkstring(L, 2)); break;
	}
	return 0;
}

static int l_save_settings(lua_State* L)
{
	api->config->save(MOD(L));
	return 0;
}

static void onOption(SmsMod* mod, const char* key, void* user)
{
	(void)mod;
	Ref* r = (Ref*)user;
	lua_rawgeti(r->s->L, LUA_REGISTRYINDEX, r->fn);
	lua_pushstring(r->s->L, key);
	call(r->s, 1, 0);
}

/* option{ key=, label=, type="bool"|"int"|"float"|"choice"|"action", default=, min=, max=, step=, choices={...}, on_change=fn } */
static int l_option(lua_State* L)
{
	static const char* const kTypes[] = {"bool", "int", "float", "choice", "action", NULL};
	luaL_checktype(L, 1, LUA_TTABLE);
	luaL_checkstack(L, 80, "option");
	SmsOption o;
	memset(&o, 0, sizeof o);
	o.size = sizeof o;
	lua_getfield(L, 1, "key");
	o.key = luaL_checkstring(L, -1);
	lua_getfield(L, 1, "label");
	o.label = luaL_optstring(L, -1, o.key);
	lua_getfield(L, 1, "type");
	o.type = luaL_checkoption(L, -1, "bool", kTypes);
	lua_getfield(L, 1, "default");
	o.def = lua_isboolean(L, -1) ? (float)lua_toboolean(L, -1) : (float)luaL_optnumber(L, -1, 0);
	o.min  = fieldnum(L, 1, "min", 0);
	o.max  = fieldnum(L, 1, "max", 0);
	o.step = fieldnum(L, 1, "step", 1);
	const char* choices[64];
	lua_getfield(L, 1, "choices");
	if (lua_istable(L, -1)) {
		const int n = (int)luaL_len(L, -1);
		for (int i = 0; i < n && i < 64; i++) {
			lua_geti(L, -1, i + 1);
			choices[i] = lua_tostring(L, -1); /* stays on the stack until the call returns */
			o.choice_count++;
		}
		o.choices = choices;
	}
	lua_getfield(L, 1, "on_change");
	if (lua_isfunction(L, -1)) {
		o.on_change = onOption;
		o.user      = newRef(L, -1);
	}
	lua_pushboolean(L, api->config->add_option(MOD(L), &o)); /* the game copies the strings */
	return 1;
}

/* ------------------------------------------------------------------ the sms table */

static int l_rgb(lua_State* L)
{
	lua_pushinteger(L, (lua_Integer)SMS_RGBA(luaL_checkinteger(L, 1), luaL_checkinteger(L, 2), luaL_checkinteger(L, 3),
	                                         luaL_optinteger(L, 4, 255)));
	return 1;
}

/* ------------------------------------------------------------------ sms.net (API 1.2) */

static int hasNet(void) { return SMS_API_HAS(api, net); }

static int n_role(lua_State* L)
{
	static const char* const kRole[] = {"off", "host", "client"};
	const int r = hasNet() ? api->net->role() : 0;
	lua_pushstring(L, kRole[r >= 0 && r <= 2 ? r : 0]);
	return 1;
}

static int n_dedicated(lua_State* L)
{
	lua_pushboolean(L, hasNet() && api->net->dedicated());
	return 1;
}

static int n_slot(lua_State* L)
{
	lua_pushinteger(L, hasNet() ? api->net->local_slot() : -1);
	return 1;
}

static int n_session(lua_State* L)
{
	lua_pushinteger(L, hasNet() ? api->net->session() : 0);
	return 1;
}

/* players() -> {slot, ...}: those in the game (a server's slot 0 too) */
static int n_players(lua_State* L)
{
	const uint32_t bits = hasNet() ? api->net->players() : 0;
	int n = 0;
	lua_newtable(L);
	for (int i = 0; i < SMS_NET_MAX_PLAYERS; i++)
		if (bits & SMS_NET_SLOT(i)) {
			lua_pushinteger(L, i);
			lua_rawseti(L, -2, ++n);
		}
	return 1;
}

/* player(slot) -> a table, or nothing when that slot is not in the game */
static int n_player(lua_State* L)
{
	SmsNetPlayer p;
	memset(&p, 0, sizeof p);
	p.size = sizeof p;
	if (!hasNet() || !api->net->player((int)luaL_checkinteger(L, 1), &p))
		return 0;
	lua_createtable(L, 0, 16);
	setint(L, "slot", p.slot);
	setbool(L, "server", p.server);
	setint(L, "character", p.character);
	setstr(L, "name", p.name);
	setint(L, "color", (lua_Integer)p.color);
	setbool(L, "in_stage", p.in_stage);
	setvec(L, "position", &p.position);
	setnum(L, "yaw", p.yaw);
	setbool(L, "visible", (p.flags & SMS_NETP_VISIBLE) != 0);
	setbool(L, "surfing", (p.flags & SMS_NETP_SURFING) != 0);
	setbool(L, "yoshi", (p.flags & SMS_NETP_YOSHI) != 0);
	setbool(L, "spectating", (p.flags & SMS_NETP_SPECTATING) != 0);
	setbool(L, "paused", (p.flags & SMS_NETP_PAUSED) != 0);
	setbool(L, "cutscene", (p.flags & SMS_NETP_CUTSCENE) != 0);
	setint(L, "ping", p.ping);
	if (p.size >= offsetof(SmsNetPlayer, api_version) + sizeof p.api_version) { /* API 1.3 */
		setstr(L, "game_version", p.game_version);
		lua_pushfstring(L, "%d.%d", (int)(p.api_version >> 16), (int)(p.api_version & 0xFFFF));
		lua_setfield(L, -2, "api_version");
	}
	if (p.size >= offsetof(SmsNetPlayer, pad) + sizeof p.pad && !p.pad.err) { /* API 1.4 */
		lua_createtable(L, 0, 7);
		setint(L, "buttons", p.pad.buttons);
		setint(L, "stick_x", p.pad.stick_x);
		setint(L, "stick_y", p.pad.stick_y);
		setint(L, "cstick_x", p.pad.cstick_x);
		setint(L, "cstick_y", p.pad.cstick_y);
		setint(L, "trigger_l", p.pad.trigger_l);
		setint(L, "trigger_r", p.pad.trigger_r);
		lua_setfield(L, -2, "pad");
	}
	return 1;
}

/* set_look(slot, look or nil) -> true; look(slot) -> "hat=...;..." */
static int n_set_look(lua_State* L)
{
	const int slot = (int)luaL_checkinteger(L, 1);
	lua_pushboolean(L, hasNet() && SMS_API_HAS(api->net, set_look) && api->net->set_look(slot, luaL_optstring(L, 2, NULL)));
	return 1;
}

static int n_look(lua_State* L)
{
	char look[512];
	look[0] = 0;
	if (hasNet() && SMS_API_HAS(api->net, look))
		api->net->look((int)luaL_checkinteger(L, 1), look, (int)sizeof look);
	lua_pushstring(L, look);
	return 1;
}

/* set_respawn(slot, "none" | "warp" | "blooper", position, yaw) -> true */
static int n_set_respawn(lua_State* L)
{
	static const char* const kHow[] = {"none", "warp", "blooper", NULL};
	const int slot = (int)luaL_checkinteger(L, 1);
	const int how  = luaL_checkoption(L, 2, "none", kHow);
	SmsVec3 p      = {0, 0, 0};
	if (how)
		p = checkvec(L, 3);
	lua_pushboolean(L, hasNet() && SMS_API_HAS(api->net, set_respawn)
	                       && api->net->set_respawn(slot, how, &p, (float)luaL_optnumber(L, 4, 0)));
	return 1;
}

/* Whom a command goes to: "all", a slot, or a list of slots. */
static uint32_t toMask(lua_State* L, int idx)
{
	if (lua_type(L, idx) == LUA_TSTRING && !strcmp(lua_tostring(L, idx), "all"))
		return SMS_NET_ALL;
	if (lua_isinteger(L, idx)) {
		const lua_Integer slot = lua_tointeger(L, idx);
		return slot >= 0 && slot < SMS_NET_MAX_PLAYERS ? SMS_NET_SLOT(slot) : 0;
	}
	uint32_t bits = 0;
	if (lua_istable(L, idx)) {
		for (lua_Integer i = 1; lua_geti(L, idx, i) == LUA_TNUMBER; i++) {
			const lua_Integer slot = lua_tointeger(L, -1);
			if (slot >= 0 && slot < SMS_NET_MAX_PLAYERS)
				bits |= SMS_NET_SLOT(slot);
			lua_pop(L, 1);
		}
		lua_pop(L, 1); /* the nil (or other) that ended the list */
	}
	return bits;
}

static lua_Integer fieldint(lua_State* L, int t, const char* k, lua_Integer def)
{
	lua_getfield(L, t, k);
	const lua_Integer v = lua_isnumber(L, -1) ? (lua_Integer)lua_tonumber(L, -1) : def;
	lua_pop(L, 1);
	return v;
}

static int fieldbool(lua_State* L, int t, const char* k)
{
	lua_getfield(L, t, k);
	const int v = lua_toboolean(L, -1);
	lua_pop(L, 1);
	return v;
}

/* A name among `names` (its index), or `def`. */
static int fieldchoice(lua_State* L, int t, const char* k, const char* const* names, int count, int def)
{
	lua_getfield(L, t, k);
	int v = def;
	if (lua_type(L, -1) == LUA_TSTRING) {
		for (int i = 0; i < count; i++)
			if (!strcmp(lua_tostring(L, -1), names[i]))
				v = i;
	} else if (lua_isnumber(L, -1)) {
		v = (int)lua_tointeger(L, -1);
	}
	lua_pop(L, 1);
	return v;
}

/* send(to, {type = "banner", text = ..., ...}) -> the slots it went to.
   The fields each type takes are in docs/MODDING.md ("Server mods"). */
static int n_send(lua_State* L)
{
	static const char* const kTypes[] = {"", "banner", "notice", "status", "timer", "ready_go", "sound", "music",
	                                     "warp", "hold", "mount", "watch", "actors", "stop_script", "stay", "object",
	                                     "spawn", "player", "rules", "board", "hud", "marker", "screen", "shake",
	                                     "rumble", "splash", "bounds"};
	static const char* const kTimer[]  = {"hide", "show", "up", "down"};
	static const char* const kActors[] = {"show", "hide", "kill"};
	static const char* const kObject[] = {"place", "speed", "phase", "show", "hide", "scale", "turn", "move", "kill"};
	static const char* const kPlayer[] = {"lose_life", "launch", "dismount", "health", "water", "nozzle", "add_water",
	                                      "water_use", "fludd"};
	static const char* const kScreen[] = {"clear", "fade_out", "fade_in", "flash"};
	static const char* const kRules[]  = {"no_fludd", "no_damage", "no_pause", "no_pickups", "water_hits", "no_refill"};
	static const char* const kHud[]    = {"coins", "shines", "blue_coins", "health", "water", "lives", "red_coins"};
	luaL_checktype(L, 2, LUA_TTABLE);
	const uint32_t to = toMask(L, 1);
	SmsCmd c;
	SmsVec3 at = {0, 0, 0};
	memset(&c, 0, sizeof c);
	c.size = sizeof c;
	c.type = (uint32_t)fieldchoice(L, 2, "type", kTypes, (int)(sizeof kTypes / sizeof kTypes[0]), 0);
	if (!c.type || c.type > SMS_CMD_BOUNDS)
		return luaL_error(L, "sms.net.send: no such command type");
	c.flags    = (fieldbool(L, 2, "sticky") ? SMS_CMD_STICKY : 0) | (fieldbool(L, 2, "unstick") ? SMS_CMD_UNSTICK : 0);
	c.delay_ms = (uint32_t)(fieldnum(L, 2, "delay", 0) * 1000.0f);
	lua_getfield(L, 2, "text");
	c.text = lua_tostring(L, -1); /* kept on the stack until sent */
	lua_getfield(L, 2, "position");
	tovec(L, -1, &at);
	lua_pop(L, 1);
	switch (c.type) {
	case SMS_CMD_BANNER: c.i[0] = (int32_t)fieldint(L, 2, "color", (lua_Integer)SMS_RGB(255, 255, 255)); break;
	case SMS_CMD_TIMER:
		c.i[0] = fieldchoice(L, 2, "mode", kTimer, 4, SMS_TIMER_SHOW);
		c.i[1] = (int32_t)(fieldnum(L, 2, "seconds", 0) * 100.0f);
		break;
	case SMS_CMD_READY_GO: c.i[0] = (int32_t)(fieldnum(L, 2, "go_in", 0) * 1000.0f); break;
	case SMS_CMD_SOUND:
		c.i[0] = (int32_t)(uint32_t)fieldint(L, 2, "id", 0);
		lua_getfield(L, 2, "position"); /* given: in the world there */
		if (!lua_isnil(L, -1)) {
			c.i[1] = 1;
			c.f[0] = at.x, c.f[1] = at.y, c.f[2] = at.z;
		}
		lua_pop(L, 1);
		break;
	case SMS_CMD_MUSIC: c.i[0] = (int32_t)(uint32_t)fieldint(L, 2, "id", 0); break;
	case SMS_CMD_MOUNT:
		c.i[0] = (int32_t)fieldint(L, 2, "from", 0x400000C5);
		c.i[1] = (int32_t)fieldint(L, 2, "to", 0x400000C7);
		/* fall through */ /* (the place) */
	case SMS_CMD_WARP:
		c.f[0] = at.x;
		c.f[1] = at.y;
		c.f[2] = at.z;
		c.f[3] = fieldnum(L, 2, "yaw", 0);
		break;
	case SMS_CMD_HOLD:
	case SMS_CMD_STAY: c.i[0] = fieldbool(L, 2, "on"); break;
	case SMS_CMD_WATCH: c.i[0] = (int32_t)fieldint(L, 2, "slot", -1); break;
	case SMS_CMD_ACTORS:
		c.i[0] = (int32_t)(uint32_t)fieldint(L, 2, "from", 0);
		c.i[1] = (int32_t)(uint32_t)fieldint(L, 2, "to", c.i[0]);
		c.i[2] = fieldchoice(L, 2, "state", kActors, 3, SMS_ACTORS_HIDE);
		break;
	case SMS_CMD_OBJECT:
		c.i[0] = (int32_t)(uint32_t)fieldint(L, 2, "from", 0);
		c.i[1] = (int32_t)(uint32_t)fieldint(L, 2, "to", c.i[0] ? c.i[0] : (lua_Integer)0xFFFFFFFFu);
		c.i[2] = (int32_t)fieldint(L, 2, "index", -1);
		c.i[3] = fieldchoice(L, 2, "what", kObject, 9, SMS_OBJECT_PLACE);
		lua_getfield(L, 2, "kind");
		c.text = lua_tostring(L, -1); /* kept on the stack until sent */
		if (c.i[3] == SMS_OBJECT_PLACE || c.i[3] == SMS_OBJECT_MOVE) {
			c.f[0]  = at.x;
			c.f[1]  = at.y;
			c.f[2]  = at.z;
			c.f[3]  = fieldnum(L, 2, "yaw", 0);
			c.f2[0] = fieldnum(L, 2, "seconds", 0);
		} else if (c.i[3] == SMS_OBJECT_SCALE || c.i[3] == SMS_OBJECT_TURN) {
			SmsVec3 v = {1, 1, 1};
			if (c.i[3] == SMS_OBJECT_TURN)
				v.x = v.y = v.z = 0;
			lua_getfield(L, 2, c.i[3] == SMS_OBJECT_SCALE ? "size" : "turn");
			tovec(L, -1, &v);
			lua_pop(L, 1);
			c.f[0] = v.x, c.f[1] = v.y, c.f[2] = v.z;
		} else {
			c.f[0] = fieldnum(L, 2, c.i[3] == SMS_OBJECT_SPEED ? "times" : "seconds", c.i[3] == SMS_OBJECT_SPEED ? 1 : 0);
		}
		break;
	case SMS_CMD_SPAWN:
		lua_getfield(L, 2, "item"); /* an item by its name, or a copy of the stage's */
		if (lua_isstring(L, -1)) {
			c.text = lua_tostring(L, -1);
		} else {
			c.i[0] = (int32_t)(uint32_t)fieldint(L, 2, "from", 0);
			c.i[1] = (int32_t)(uint32_t)fieldint(L, 2, "to", c.i[0] ? c.i[0] : (lua_Integer)0xFFFFFFFFu);
			c.i[2] = (int32_t)fieldint(L, 2, "index", 0);
			lua_getfield(L, 2, "kind");
			c.text = lua_tostring(L, -1);
		}
		c.i[3] = (int32_t)fieldint(L, 2, "id", 0);
		c.f[0] = at.x, c.f[1] = at.y, c.f[2] = at.z;
		c.f[3] = fieldnum(L, 2, "yaw", 0);
		break;
	case SMS_CMD_PLAYER: {
		SmsVec3 dir = {0, 1, 0};
		c.i[0] = fieldchoice(L, 2, "action", kPlayer, 9, SMS_PLAYER_LOSE_LIFE);
		c.i[1] = (int32_t)fieldint(L, 2, "value", 0);
		lua_getfield(L, 2, "direction");
		tovec(L, -1, &dir);
		lua_pop(L, 1);
		c.f[0] = dir.x, c.f[1] = dir.y, c.f[2] = dir.z;
		c.f[3] = fieldnum(L, 2, "speed", 40);
		break;
	}
	case SMS_CMD_RULES:
		for (int k = 0; k < 6; k++)
			if (fieldbool(L, 2, kRules[k]))
				c.i[0] |= 1 << k;
		break;
	case SMS_CMD_HUD:
		for (int k = 0; k < 7; k++)
			if (fieldbool(L, 2, kHud[k]) || fieldbool(L, 2, "all"))
				c.i[0] |= 1 << k;
		break;
	case SMS_CMD_BOARD:
		c.i[0] = (int32_t)fieldint(L, 2, "row", 0);
		c.i[1] = (int32_t)fieldint(L, 2, "color", 0);
		break;
	case SMS_CMD_MARKER:
		c.i[0] = (int32_t)fieldint(L, 2, "id", 0);
		c.i[1] = (int32_t)fieldint(L, 2, "slot", -1);
		c.i[2] = (int32_t)fieldint(L, 2, "color", 0);
		c.i[3] = fieldbool(L, 2, "remove");
		c.f[0] = at.x, c.f[1] = at.y, c.f[2] = at.z;
		break;
	case SMS_CMD_SCREEN:
		c.i[0] = fieldchoice(L, 2, "effect", kScreen, 4, SMS_SCREEN_CLEAR);
		c.i[1] = (int32_t)fieldint(L, 2, "color", (lua_Integer)SMS_RGB(0, 0, 0));
		c.f[0] = fieldnum(L, 2, "seconds", 0.5f);
		break;
	case SMS_CMD_SHAKE:
		c.f[0] = fieldnum(L, 2, "strength", 1);
		c.f[1] = fieldnum(L, 2, "seconds", 0.5f);
		break;
	case SMS_CMD_RUMBLE:
		c.i[0] = fieldbool(L, 2, "hard");
		c.i[1] = (int32_t)fieldint(L, 2, "times", 1);
		break;
	case SMS_CMD_SPLASH:
		c.i[0] = (int32_t)fieldint(L, 2, "slot", 0);
		c.i[1] = (int32_t)fieldint(L, 2, "meter", 0);
		c.i[2] = (int32_t)fieldint(L, 2, "full", 0);
		c.i[3] = (fieldbool(L, 2, "down") ? (int32_t)SMS_SPLASH_DOWN : 0) | (fieldbool(L, 2, "quiet") ? (int32_t)SMS_SPLASH_QUIET : 0);
		c.f[0] = at.x, c.f[1] = at.y, c.f[2] = at.z;
		c.f[3] = fieldnum(L, 2, "flush", 0);
		break;
	case SMS_CMD_BOUNDS:
		c.i[0]  = fieldbool(L, 2, "ring") ? (int32_t)SMS_BOUNDS_RING : 0;
		c.i[1]  = (int32_t)fieldint(L, 2, "color", 0);
		c.f[0]  = at.x, c.f[1] = at.y, c.f[2] = at.z;
		c.f[3]  = fieldnum(L, 2, "radius", 0);
		c.f2[0] = fieldnum(L, 2, "seconds", 0);
		break;
	default: break;
	}
	lua_pushinteger(L, hasNet() ? (lua_Integer)api->net->send(to, &c) : 0);
	return 1;
}

static int n_load_stage(lua_State* L)
{
	lua_pushboolean(L, hasNet() && api->net->load_stage((int)luaL_checkinteger(L, 1), (int)luaL_optinteger(L, 2, 0)));
	return 1;
}

static int n_set_mode(lua_State* L)
{
	if (hasNet())
		api->net->set_mode(luaL_checkstring(L, 1));
	return 0;
}

static int n_set_status(lua_State* L)
{
	if (hasNet())
		api->net->set_status(luaL_checkstring(L, 1));
	return 0;
}

/* (1.5) set_collision(true | false | nil) -> true: players collide (nil: the host's setting); collision() */
static int n_set_collision(lua_State* L)
{
	const int on = lua_isnoneornil(L, 1) ? -1 : lua_toboolean(L, 1);
	lua_pushboolean(L, hasNet() && SMS_API_HAS(api->net, set_collision) && api->net->set_collision(on));
	return 1;
}

static int n_collision(lua_State* L)
{
	lua_pushboolean(L, hasNet() && SMS_API_HAS(api->net, collision) && api->net->collision());
	return 1;
}

static const luaL_Reg kNet[] = {
	{"role", n_role}, {"dedicated", n_dedicated}, {"slot", n_slot}, {"session", n_session},
	{"players", n_players}, {"player", n_player}, {"send", n_send}, {"load_stage", n_load_stage},
	{"set_mode", n_set_mode}, {"set_status", n_set_status}, {"set_look", n_set_look}, {"look", n_look},
	{"set_respawn", n_set_respawn}, {"set_collision", n_set_collision}, {"collision", n_collision},
	{NULL, NULL},
};

/* The stage's trigger volumes: in_cube(set, index, position) -> true/false (nil: no such cube) */
static const char* const kCubeSets[] = {"camera", "area",   "fast_a", "fast_b", "fast_c",      "mirror",
                                        "wire",   "stream", "shadow", "sound_change", "sound_effect"};
static int cubeSet(lua_State* L, int idx)
{
	if (lua_isinteger(L, idx))
		return (int)lua_tointeger(L, idx);
	const char* name = luaL_checkstring(L, idx);
	for (int i = 0; i < (int)(sizeof kCubeSets / sizeof kCubeSets[0]); i++)
		if (!strcmp(name, kCubeSets[i]))
			return i;
	return luaL_error(L, "no cube set \"%s\"", name);
}

static int l_cube_count(lua_State* L)
{
	lua_pushinteger(L, SMS_API_HAS(api->game, cube_count) ? api->game->cube_count(cubeSet(L, 1)) : 0);
	return 1;
}

static int l_in_cube(lua_State* L)
{
	const int set   = cubeSet(L, 1);
	const int index = (int)luaL_checkinteger(L, 2);
	const SmsVec3 p = checkvec(L, 3);
	const int r     = SMS_API_HAS(api->game, in_cube) ? api->game->in_cube(set, index, &p) : -1;
	if (r < 0)
		return 0;
	lua_pushboolean(L, r);
	return 1;
}

/* The stage's objects (API 1.4): object_count(from, to, kind) -> n;
   object_info(from, to, kind, index) -> {type, kind, position, rotation,
   scale, alive, hidden, spawned} or nil; ground(position) -> {floor,
   floor_type, water, water_y, roof} or nil. Kind "@id": a spawned copy. */
static int l_object_count(lua_State* L)
{
	const uint32_t from = (uint32_t)luaL_optinteger(L, 1, 0), to = (uint32_t)luaL_optinteger(L, 2, 0xFFFFFFFFu);
	lua_pushinteger(L, SMS_API_HAS(api->game, object_count) ? api->game->object_count(from, to, luaL_optstring(L, 3, "")) : 0);
	return 1;
}

static int l_object_info(lua_State* L)
{
	const uint32_t from = (uint32_t)luaL_optinteger(L, 1, 0), to = (uint32_t)luaL_optinteger(L, 2, 0xFFFFFFFFu);
	SmsObjectInfo o;
	memset(&o, 0, sizeof o);
	o.size = sizeof o;
	if (!SMS_API_HAS(api->game, object_info)
	    || !api->game->object_info(from, to, luaL_optstring(L, 3, ""), (int)luaL_optinteger(L, 4, 0), &o))
		return 0;
	lua_createtable(L, 0, 8);
	setint(L, "type", (lua_Integer)o.type);
	setstr(L, "kind", o.kind);
	setvec(L, "position", &o.position);
	setvec(L, "rotation", &o.rotation);
	setvec(L, "scale", &o.scale);
	setbool(L, "alive", o.alive);
	setbool(L, "hidden", o.hidden);
	setint(L, "spawned", o.spawned);
	return 1;
}

static int l_ground(lua_State* L)
{
	const SmsVec3 p = checkvec(L, 1);
	SmsGround g;
	memset(&g, 0, sizeof g);
	g.size = sizeof g;
	if (!SMS_API_HAS(api->game, ground) || !api->game->ground(&p, &g))
		return 0;
	lua_createtable(L, 0, 5);
	setnum(L, "floor", g.floor);
	setint(L, "floor_type", (lua_Integer)g.floor_type);
	setbool(L, "water", g.water);
	if (g.has_water)
		setnum(L, "water_y", g.water_y);
	setnum(L, "roof", g.roof);
	return 1;
}

/* ------------------------------------------------------------------ sms.world (API 1.4) */

static const char* const kAreaKinds[] = {"none", "airstrip", "plaza", "world", "world_part", "secret", "final", "title"};

static int hasWorld(void) { return SMS_API_HAS(api, world) && api->world; }

/* kind(area) -> "world", "plaza", ... ("none": no such area) */
static int w_kind(lua_State* L)
{
	const int k = hasWorld() ? api->world->area_kind((int)luaL_checkinteger(L, 1)) : SMS_AREA_NONE;
	lua_pushstring(L, k >= 0 && k <= 7 ? kAreaKinds[k] : "none");
	return 1;
}

/* of(area) -> the world's area, or nil */
static int w_of(lua_State* L)
{
	const int w = hasWorld() ? api->world->world_of((int)luaL_checkinteger(L, 1)) : -1;
	if (w < 0)
		return 0;
	lua_pushinteger(L, w);
	return 1;
}

static int w_episodes(lua_State* L)
{
	lua_pushinteger(L, hasWorld() ? api->world->episode_count((int)luaL_checkinteger(L, 1)) : 0);
	return 1;
}

/* episode_shine(area, episode) -> the Shine, or nil */
static int w_episode_shine(lua_State* L)
{
	const int n = hasWorld() ? api->world->episode_shine((int)luaL_checkinteger(L, 1), (int)luaL_checkinteger(L, 2)) : -1;
	if (n < 0)
		return 0;
	lua_pushinteger(L, n);
	return 1;
}

/* episode_name(area, episode) -> its name, or nil */
static int w_episode_name(lua_State* L)
{
	char name[128];
	if (!hasWorld() || api->world->episode_name((int)luaL_checkinteger(L, 1), (int)luaL_checkinteger(L, 2), name, sizeof name) <= 0)
		return 0;
	lua_pushstring(L, name);
	return 1;
}

/* next_episode(area) -> the episode its episode select offers first, or nil */
static int w_next_episode(lua_State* L)
{
	const int n = hasWorld() ? api->world->next_episode((int)luaL_checkinteger(L, 1)) : -1;
	if (n < 0)
		return 0;
	lua_pushinteger(L, n);
	return 1;
}

/* shine_episode(shine) -> episode, world (nil: not an episode's) */
static int w_shine_episode(lua_State* L)
{
	int area = -1;
	const int n = hasWorld() ? api->world->shine_episode((int)luaL_checkinteger(L, 1), &area) : -1;
	if (n < 0)
		return 0;
	lua_pushinteger(L, n);
	lua_pushinteger(L, area);
	return 2;
}

static int w_has_cutscene(lua_State* L)
{
	lua_pushboolean(L, hasWorld() && api->world->has_cutscene(luaL_checkstring(L, 1)));
	return 1;
}

/* play_cutscene(name [, focus] [, frames]) -> true when on its way */
static int w_play_cutscene(lua_State* L)
{
	const char* name = luaL_checkstring(L, 1);
	SmsVec3 focus;
	const int has_focus = lua_istable(L, 2);
	if (has_focus)
		focus = checkvec(L, 2);
	const int frames = (int)luaL_optinteger(L, 3, -1);
	lua_pushboolean(L, hasWorld() && api->world->play_cutscene(name, has_focus ? &focus : NULL, frames));
	return 1;
}

static int w_cutscene_playing(lua_State* L)
{
	lua_pushboolean(L, hasWorld() && api->world->cutscene_playing());
	return 1;
}

static const luaL_Reg kWorld[] = {
	{"kind", w_kind}, {"of", w_of}, {"episodes", w_episodes}, {"episode_shine", w_episode_shine},
	{"episode_name", w_episode_name}, {"next_episode", w_next_episode}, {"shine_episode", w_shine_episode},
	{"has_cutscene", w_has_cutscene}, {"play_cutscene", w_play_cutscene}, {"cutscene_playing", w_cutscene_playing},
	{NULL, NULL},
};

static const luaL_Reg kSms[] = {
	{"on", l_on}, {"off", l_off}, {"log", l_log}, {"warn", l_warn}, {"error", l_error}, {"debug", l_debug},
	{"event_id", l_event_id}, {"emit", l_emit}, {"time", l_time}, {"is_loaded", l_is_loaded},
	{"data_dir", l_data_dir}, {"info", l_info},
	{"state", l_state}, {"flag", l_flag}, {"set_flag", l_set_flag}, {"coins", l_coins}, {"lives", l_lives},
	{"shines", l_shines}, {"blue_coins", l_blue_coins}, {"has_shine", l_has_shine}, {"warp", l_warp},
	{"area_name", l_area_name}, {"find_object", l_find_object}, {"object_name", l_object_name},
	{"object_position", l_object_position}, {"set_object_position", l_set_object_position},
	{"mario", l_mario}, {"set_mario", l_set_mario}, {"set_status", l_set_status}, {"camera", l_camera},
	{"set_nozzle", l_set_nozzle}, {"set_fludd", l_set_fludd}, {"set_outfit", l_set_outfit}, {"set_yaw", l_set_yaw}, {"set_aim", l_set_aim}, {"set_hidden_parts", l_set_hidden_parts},
	{"set_hud_hidden", l_set_hud_hidden}, {"hud_hidden", l_hud_hidden},
	{"to_canvas", l_to_canvas}, {"play_sound", l_play_sound},
	{"pad", l_pad}, {"key_down", l_key_down}, {"key_pressed", l_key_pressed}, {"key", l_key},
	{"key_name", l_key_name}, {"bind_key", l_bind_key}, {"capture", l_capture}, {"captured_by", l_captured_by},
	{"setting", l_setting}, {"set_setting", l_set_setting}, {"save_settings", l_save_settings},
	{"option", l_option}, {"rgb", l_rgb}, {"rgba", l_rgb},
	{"cube_count", l_cube_count}, {"in_cube", l_in_cube},
	{"object_count", l_object_count}, {"object_info", l_object_info}, {"ground", l_ground},
	{"object_type", l_object_type}, {"object_health", l_object_health},
	{"files_read", l_files_read}, {"files_write", l_files_write}, {"files_remove", l_files_remove},
	{"files_dir", l_files_dir}, {"set_object_color", l_set_object_color},
	{NULL, NULL},
};

static const luaL_Reg kDraw[] = {
	{"canvas", d_canvas}, {"rect", d_rect}, {"rect_outline", d_rect_outline}, {"gradient", d_gradient},
	{"line", d_line}, {"triangle", d_triangle}, {"circle", d_circle}, {"circle_outline", d_circle_outline},
	{"text", d_text}, {"text_width", d_text_width}, {"image_load", d_image_load}, {"image_create", d_image_create},
	{"image_free", d_image_free}, {"image_size", d_image_size}, {"image", d_image}, {"image_region", d_image_region},
	{"clip_push", d_clip_push}, {"clip_pop", d_clip_pop}, {"world_line", d_world_line}, {"world_box", d_world_box},
	{"world_ring", d_world_ring}, {"world_text", d_world_text}, {"hud_slot", d_hud_slot},
	{NULL, NULL},
};

static void constants(lua_State* L)
{
	static const struct {
		const char* name;
		lua_Integer v;
	} k[] = {
		{"CANCEL", 1}, {"FIRST", SMS_PRIORITY_FIRST}, {"EARLY", SMS_PRIORITY_EARLY}, {"NORMAL", SMS_PRIORITY_NORMAL},
		{"LATE", SMS_PRIORITY_LATE}, {"MONITOR", SMS_PRIORITY_MONITOR},
		{"SHADOW", SMS_TEXT_SHADOW}, {"OUTLINE", SMS_TEXT_OUTLINE}, {"CENTER", SMS_TEXT_CENTER}, {"RIGHT", SMS_TEXT_RIGHT},
		{"PAD_LEFT", SMS_PAD_LEFT}, {"PAD_RIGHT", SMS_PAD_RIGHT}, {"PAD_DOWN", SMS_PAD_DOWN}, {"PAD_UP", SMS_PAD_UP},
		{"PAD_Z", SMS_PAD_Z}, {"PAD_R", SMS_PAD_R}, {"PAD_L", SMS_PAD_L}, {"PAD_A", SMS_PAD_A}, {"PAD_B", SMS_PAD_B},
		{"PAD_X", SMS_PAD_X}, {"PAD_Y", SMS_PAD_Y}, {"PAD_START", SMS_PAD_START},
		{"SOUND_CURSOR", SMS_SOUND_CURSOR}, {"SOUND_DECIDE", SMS_SOUND_DECIDE}, {"SOUND_CANCEL", SMS_SOUND_CANCEL},
		{"SOUND_COIN", SMS_SOUND_COIN}, {"SOUND_NEWS", SMS_SOUND_NEWS}, {"SOUND_SHINE", SMS_SOUND_SHINE},
		{"SOUND_DAMAGE", SMS_SOUND_DAMAGE}, {"SOUND_HEAL", SMS_SOUND_HEAL}, {"SOUND_APPEAR", SMS_SOUND_APPEAR},
		{"MUSIC_EVENT", (lua_Integer)(uint32_t)SMS_MUSIC_EVENT}, {"MUSIC_FANFARE_RACE", (lua_Integer)(uint32_t)SMS_MUSIC_FANFARE_RACE},
	};
	for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
		lua_pushinteger(L, k[i].v);
		lua_setfield(L, -2, k[i].name);
	}
}

static void openSms(lua_State* L, Script* s)
{
	luaL_newlib(L, kSms);
	luaL_newlib(L, kDraw);
	lua_setfield(L, -2, "draw");
	luaL_newlib(L, kNet);
	lua_setfield(L, -2, "net");
	luaL_newlib(L, kWorld);
	lua_setfield(L, -2, "world");
	constants(L);
	setstr(L, "id", api->core->mod_id(s->mod));
	setstr(L, "dir", api->core->mod_dir(s->mod));
	setstr(L, "name", api->core->mod_info(s->mod, "name"));
	setstr(L, "version", api->core->mod_info(s->mod, "version"));
	setstr(L, "game_build", api->core->game_build());
	setstr(L, "lua_version", LUA_RELEASE);
	lua_setglobal(L, "sms");
	/* print() goes to the game's log, prefixed with the script's id */
	lua_pushcfunction(L, l_log);
	lua_setglobal(L, "print");
}

/* ------------------------------------------------------------------ running scripts */

static int startScript(SmsMod* host, SmsMod* mod, void* user)
{
	(void)host;
	(void)user;
	Script* s = (Script*)calloc(1, sizeof *s);
	s->mod    = mod;
	s->L      = luaL_newstate();
	if (!s->L) {
		free(s);
		return 1;
	}
	lua_State* L                    = s->L;
	*(Script**)lua_getextraspace(L) = s;
	luaL_openlibs(L);
	const char* dir  = api->core->mod_dir(mod);
	const char* file = api->core->mod_info(mod, "script");
	/* require() finds modules in the script's folder only, and no C modules */
	lua_getglobal(L, "package");
	lua_pushfstring(L, "%s/?.lua;%s/?/init.lua", dir, dir);
	lua_setfield(L, -2, "path");
	lua_pushstring(L, "");
	lua_setfield(L, -2, "cpath");
	lua_pop(L, 1);
	openSms(L, s);
	lua_pushfstring(L, "%s/%s", dir, file ? file : "main.lua");
	const char* path = lua_tostring(L, -1);
	/* text only: compiled chunks can break Lua's safety */
	if (luaL_loadfilex(L, path, "t") != LUA_OK) {
		api->core->log(mod, SMS_LOG_ERROR, "%s", lua_tostring(L, -1));
		lua_close(L);
		free(s);
		return 1;
	}
	lua_remove(L, -2); /* the path */
	s->next   = s_scripts;
	s_scripts = s;
	if (call(s, 0, 0) != LUA_OK) {
		s_scripts = s->next;
		lua_close(L);
		while (s->refs) {
			Ref* r  = s->refs;
			s->refs = r->next;
			free(r);
		}
		free(s);
		return 1;
	}
	return 0;
}

static void stopScript(SmsMod* host, SmsMod* mod, void* user)
{
	(void)host;
	(void)user;
	Script** at = &s_scripts;
	while (*at && (*at)->mod != mod)
		at = &(*at)->next;
	Script* s = *at;
	if (!s)
		return;
	*at = s->next;
	/* sms.unload, if the script set one */
	lua_getglobal(s->L, "sms");
	lua_getfield(s->L, -1, "unload");
	lua_remove(s->L, -2);
	if (lua_isfunction(s->L, -1))
		call(s, 0, 0);
	else
		lua_pop(s->L, 1);
	lua_close(s->L);
	while (s->refs) {
		Ref* r  = s->refs;
		s->refs = r->next;
		free(r);
	}
	free(s);
}

SMS_MOD_DEFINE()

SMS_MOD_EXTERN_C SMS_MOD_EXPORT int sms_mod_load(SmsMod* mod, const SmsApi* a)
{
	api = a;
	if (!SMS_API_HAS(api->core, host_runtime)) {
		api->core->log(mod, SMS_LOG_ERROR, "this game is too old to run script mods (mod API 1.1 needed)");
		return 1;
	}
	if (!api->core->host_runtime(mod, "lua", startScript, stopScript, NULL))
		return 1;
	api->core->log(mod, SMS_LOG_INFO, "%s ready", LUA_RELEASE);
	return 0;
}
