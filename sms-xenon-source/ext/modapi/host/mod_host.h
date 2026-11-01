// The native mod API's host half (ext/modapi/README.md): what its files share.
// mod_runtime.cpp finds, orders, loads and unloads the mods and keeps their
// events, services, settings and input; mod_draw.cpp builds each frame's
// drawing; mod_menu.cpp is the Mods menu (F1).
#ifndef MOD_HOST_H
#define MOD_HOST_H

#include "../modapi_bridge.h"

#include <map>
#include <string>
#include <vector>

struct ModOption {
	SmsOption desc;
	std::string key, label;
	std::vector<std::string> choices;
	std::vector<const char*> choicePtrs;
};

struct SmsMod {
	// mod.ini
	std::string id, name, version, author, description, library;
	std::vector<std::string> after, before, requires, conflicts;
	int priority = 0;
	bool gameplay = false; // changes how the game plays (multiplayer needs it on every side)
	std::string runtime, script; // a script mod: run by the native mod hosting `runtime`
	bool server = false;  // side = server: runs only in a game hosting multiplayer (api->net's commands reach the players)
	int clientApi = 0;    // client_api = 1.2: the mod API players' games need for a server mod ((major << 8) | minor)
	std::map<std::string, std::string> ini; // every key of mod.ini
	// where it is
	std::string dir, libPath, loadedPath, dataDir;
	long long libStamp = 0; // the library's modification time, for hot reload
	// state
	enum State { FOUND, LOADED, FAILED, OFF } state = FOUND;
	std::string error;
	int order = -1; // load order, the tie-break for priorities
	void* handle = nullptr;
	SmsModUnloadFn unload = nullptr;
	// settings
	bool settingsRead = false, settingsDirty = false;
	std::map<std::string, std::string> settings;
	std::vector<ModOption> options;
	uint32_t hudHidden = 0; // game->set_hud_hidden: the HUD's parts this mod hides (SMS_HUD_*)
	bool aims = false;      // player->set_aim: it turns Mario's body
	uint32_t hiddenParts = 0; // player->set_hidden_parts: the parts of Mario it hides (SMS_PLAYER_PART_*)
};

namespace modhost {

extern const SmsFilesApi g_files; // mod_files.cpp

extern std::vector<SmsMod*> g_mods; // every mod found, in load order
extern const SmsApi g_api;
extern const SmsDrawApi g_drawApi;

void log(SmsMod* mod, int level, const char* fmt, ...) SMS_MOD_PRINTF(3, 4);
SmsMod* current();                  // the mod whose code is running, or null
SmsMod* setCurrent(SmsMod* mod);    // returns the one before (around calls into a mod)
uint32_t frame();
int mode();
bool loadMod(SmsMod* mod);          // false: mod->error says why
void unloadMod(SmsMod* mod);
void setModOff(SmsMod* mod, bool off); // the Mods menu: unload/load and remember it

// settings (mod_runtime.cpp)
std::string settingGet(SmsMod* mod, const std::string& key, const std::string& def, bool* found = nullptr);
void settingSet(SmsMod* mod, const std::string& key, const std::string& value);

// drawing (mod_draw.cpp)
void drawBegin();                   // before SMS_EV_DRAW
void drawEnd();                     // after it (and the menu): submit
bool drawing();
void drawDropMod(SmsMod* mod);      // its images
void drawCanvas(float* w, float* h);

// the Mods menu (mod_menu.cpp)
bool menuOpen();
void menuToggle();
void menuKey(int scancode, int down, int repeat); // while it is open
void menuPad(const SmsPadStatus* raw);           // the controllers while it is open
void menuDraw();

// input (mod_runtime.cpp)
const SmsPadStatus* rawPads();

} // namespace modhost

#endif
