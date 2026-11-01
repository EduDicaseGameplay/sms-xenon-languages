// The mod API's files (api->files, SmsFilesApi; API 1.5 beta 6): each mod's
// own files beside the memory card, in <card folder>-mods/<mod id>/, so they go
// with the player's card wherever it is kept (and a multiplayer guest's copy of
// a host's card, a folder of its own, has its own).
//
// Scope SMS_FILES_MOD is the mod's folder itself, written at once. Scopes 0..2
// are save files A, B and C of the running game, in <mod id>/<game code>-<A|B|C>/
// (Super Mario Sunshine's GMSE01 and Super Mario Eclipse's GMSE04 are kept
// apart, as the card keeps their files), and follow the card: what a mod
// writes to the file being played is held here and written when the game
// saves that file; a new game starts with none (and its old files go at its
// first save); copying or erasing a file at file select copies or erases
// every mod's. The game half says when (modh_files_event, from the save file
// hooks); writes to a file that is not the one played go at once.
#include "mod_host.h"
#include "../modapi_bridge.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

extern "C" const char* port_card_dir(void);

namespace {

const uint32_t kMaxSize = 1u << 20;

// The file being played (0..2), from its load or new game at file select; -1 none.
int s_playing = -1;
// It was started anew: its old files are none of its own (gone at its first save).
bool s_fresh;
// Per mod (by id, so a mod reloaded keeps them): what it wrote to and removed
// from the file being played, until the game saves it.
struct Held {
	std::map<std::string, std::vector<uint8_t>> writes;
	std::set<std::string> removed;
};
std::map<std::string, Held> s_held;

std::string s_root;            // <card folder>-mods
std::map<std::string, std::string> s_dirs; // dir()'s strings, kept per mod

const std::string& root()
{
	if (s_root.empty()) {
		std::string card = port_card_dir() ? port_card_dir() : "card-a";
		while (card.size() > 1 && (card.back() == '/' || card.back() == '\\'))
			card.pop_back();
		s_root = card + "-mods";
	}
	return s_root;
}

void makeDirs(const std::string& p)
{
	for (size_t i = 1; i <= p.size(); i++)
		if (i == p.size() || p[i] == '/' || p[i] == '\\') {
			const std::string part = p.substr(0, i);
#ifdef _WIN32
			_mkdir(part.c_str());
#else
			mkdir(part.c_str(), 0755);
#endif
		}
}

// The files (not folders) in `dir`.
std::vector<std::string> listFiles(const std::string& dir, bool folders = false)
{
	std::vector<std::string> out;
#ifdef _WIN32
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((dir + "/*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE)
		return out;
	do {
		const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		if (fd.cFileName[0] != '.' && isDir == folders)
			out.push_back(fd.cFileName);
	} while (FindNextFileA(h, &fd));
	FindClose(h);
#else
	DIR* d = opendir(dir.c_str());
	if (!d)
		return out;
	while (dirent* e = readdir(d)) {
		if (e->d_name[0] == '.')
			continue;
		struct stat st;
		if (stat((dir + "/" + e->d_name).c_str(), &st) == 0 && S_ISDIR(st.st_mode) == folders)
			out.push_back(e->d_name);
	}
	closedir(d);
#endif
	return out;
}

bool validName(const char* name)
{
	if (!name || !name[0] || name[0] == '.')
		return false;
	size_t n = 0;
	for (const char* c = name; *c; c++, n++) {
		const bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '.'
		                || *c == '_' || *c == '-';
		if (!ok || n >= 64)
			return false;
	}
	return true;
}

// The running game's code (GMSE01, Eclipse's GMSE04), for its files' folders.
std::string gameCode()
{
	char code[8] = {0};
	modg_game_code(code);
	for (char* c = code; *c; c++)
		if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9')))
			*c = '_';
	return code[0] ? code : "GMSE01";
}

std::string modDir(const std::string& id) { return root() + "/" + id; }
std::string fileDir(const std::string& id, int file)
{
	return modDir(id) + "/" + gameCode() + "-" + (char)('A' + file);
}

// SMS_FILES_CURRENT to the file played; -2 when the scope is none.
int scopeOf(int scope)
{
	if (scope == SMS_FILES_CURRENT)
		return s_playing >= 0 ? s_playing : -2;
	return scope == SMS_FILES_MOD || (scope >= 0 && scope <= 2) ? scope : -2;
}

std::string pathOf(const std::string& id, int scope, const char* name)
{
	return (scope == SMS_FILES_MOD ? modDir(id) : fileDir(id, scope)) + "/" + name;
}

bool readDisk(const std::string& path, std::vector<uint8_t>* out)
{
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	out->clear();
	uint8_t buf[16384];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0 && out->size() <= kMaxSize)
		out->insert(out->end(), buf, buf + n);
	fclose(f);
	return true;
}

// Whole, or not at all: a temporary file, then put in place.
bool writeDisk(const std::string& path, const void* data, size_t size)
{
	makeDirs(path.substr(0, path.find_last_of('/')));
	const std::string tmp = path + ".tmp";
	FILE* f               = fopen(tmp.c_str(), "wb");
	if (!f)
		return false;
	const bool ok = (size == 0 || fwrite(data, 1, size, f) == size) && fflush(f) == 0;
	if (fclose(f) != 0 || !ok) {
		remove(tmp.c_str());
		return false;
	}
#ifdef _WIN32
	return MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
	return rename(tmp.c_str(), path.c_str()) == 0;
#endif
}

void clearDir(const std::string& dir)
{
	for (const std::string& f : listFiles(dir))
		remove((dir + "/" + f).c_str());
#ifdef _WIN32
	_rmdir(dir.c_str());
#else
	rmdir(dir.c_str());
#endif
}

// Every mod with a folder here (whether loaded or not: a file's copy or
// erase is every mod's that kept something for it).
std::vector<std::string> modIds() { return listFiles(root(), true); }

// ------------------------------------------------------------------ the table

const char* apiDir(SmsMod* mod)
{
	if (!mod)
		return "";
	std::string& d = s_dirs[mod->id];
	d              = modDir(mod->id);
	makeDirs(d);
	return d.c_str();
}

int apiRead(SmsMod* mod, int scope, const char* name, void* buf, uint32_t cap)
{
	scope = scopeOf(scope);
	if (!mod || scope == -2 || !validName(name))
		return -1;
	std::vector<uint8_t> data;
	bool have = false;
	if (scope >= 0 && scope == s_playing) {
		Held& h = s_held[mod->id];
		auto w  = h.writes.find(name);
		if (w != h.writes.end()) {
			data = w->second;
			have = true;
		} else if (s_fresh || h.removed.count(name)) {
			return -1;
		}
	}
	if (!have && !readDisk(pathOf(mod->id, scope, name), &data))
		return -1;
	if (buf && cap)
		memcpy(buf, data.data(), data.size() < cap ? data.size() : cap);
	return (int)data.size();
}

int apiWrite(SmsMod* mod, int scope, const char* name, const void* data, uint32_t size)
{
	scope = scopeOf(scope);
	if (!mod || scope == -2 || !validName(name) || size > kMaxSize || (size && !data))
		return 0;
	if (scope >= 0 && scope == s_playing) {
		Held& h = s_held[mod->id];
		h.writes[name].assign((const uint8_t*)data, (const uint8_t*)data + size);
		h.removed.erase(name);
		return 1;
	}
	if (!writeDisk(pathOf(mod->id, scope, name), data, size)) {
		modhost::log(mod, SMS_LOG_WARN, "could not write %s", pathOf(mod->id, scope, name).c_str());
		return 0;
	}
	return 1;
}

int apiRemove(SmsMod* mod, int scope, const char* name)
{
	scope = scopeOf(scope);
	if (!mod || scope == -2 || !validName(name))
		return 0;
	if (scope >= 0 && scope == s_playing) {
		const int had = apiRead(mod, scope, name, nullptr, 0) >= 0;
		Held& h       = s_held[mod->id];
		h.writes.erase(name);
		h.removed.insert(name);
		return had;
	}
	return remove(pathOf(mod->id, scope, name).c_str()) == 0;
}

// The file played was saved: what each mod held for it goes to its folder.
void commit(int file)
{
	if (file != s_playing)
		return;
	if (s_fresh)
		for (const std::string& id : modIds())
			clearDir(fileDir(id, file));
	s_fresh = false;
	for (auto& e : s_held) {
		const std::string dir = fileDir(e.first, file);
		for (const std::string& name : e.second.removed)
			remove((dir + "/" + name).c_str());
		for (auto& w : e.second.writes)
			if (!writeDisk(dir + "/" + w.first, w.second.data(), w.second.size()))
				fprintf(stderr, "[mods] %s: could not write %s/%s\n", e.first.c_str(), dir.c_str(), w.first.c_str());
	}
	s_held.clear();
}

} // namespace

namespace modhost {

const SmsFilesApi g_files = {sizeof(SmsFilesApi), apiDir, apiRead, apiWrite, apiRemove};

} // namespace modhost

extern "C" void modh_files_event(int what, int file, int from)
{
	if (file < 0 || file > 2)
		return;
	switch (what) {
	case SMS_SAVE_FILE_LOADED:
	case SMS_SAVE_FILE_NEW:
		// a file chosen: what was held for another (a game not saved) is dropped
		s_playing = file;
		s_fresh   = what == SMS_SAVE_FILE_NEW;
		s_held.clear();
		break;
	case SMS_SAVE_FILE_SAVED:
		commit(file);
		break;
	case SMS_SAVE_FILE_COPIED:
		if (from < 0 || from > 2 || from == file)
			break;
		for (const std::string& id : modIds()) {
			const std::string src = fileDir(id, from), dst = fileDir(id, file);
			clearDir(dst);
			for (const std::string& name : listFiles(src)) {
				std::vector<uint8_t> data;
				if (readDisk(src + "/" + name, &data))
					writeDisk(dst + "/" + name, data.data(), data.size());
			}
		}
		break;
	case SMS_SAVE_FILE_ERASED:
		for (const std::string& id : modIds())
			clearDir(fileDir(id, file));
		if (file == s_playing) {
			s_playing = -1;
			s_held.clear();
		}
		break;
	}
}
