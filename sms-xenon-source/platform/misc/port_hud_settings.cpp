// The settings menu's HUD page (port_settings.h, PORT_SETTINGS_PAGE_HUD): hide the game's
// own HUD, or some of its parts, without a mod. "Hide HUD" hides the parts switched on below
// it; the HUD_TOGGLE key (H by default, port_hud_settings_toggle from the pad) flips it while
// playing. The game leaves the hidden parts out of the HUD's drawing only
// (ext/core/game/port_hud.cpp, which asks port_hud_settings_mask every draw), so the HUD
// carries on underneath: its animations, sounds and FLUDD's messages as ever.
//
// The launcher has no words for these, so they are kept in a file of their own, hud.txt in
// the game's data folder (beside the memory card's, card-a), and last from one game to the
// next. Their ids are null: the menu does not try to save them for the launcher.
#include "port_settings.h"
#include "port_hud_settings.h"
#include "port_ext.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

extern "C" void port_log(const char* fmt, ...);

namespace {

struct Part {
    const char* key;  // in hud.txt
    unsigned bit;     // PORT_HUD_*
};
const Part kParts[] = {
    {"coins", PORT_HUD_COINS},   {"shines", PORT_HUD_SHINES}, {"blue_coins", PORT_HUD_BLUE_COINS},
    {"health", PORT_HUD_HEALTH}, {"water", PORT_HUD_WATER},   {"lives", PORT_HUD_LIVES},
    {"red_coins", PORT_HUD_RED_COINS},
};

bool s_loaded;
int s_hide;                            // Hide HUD
unsigned s_parts = PORT_HUD_COINS | PORT_HUD_SHINES | PORT_HUD_BLUE_COINS | PORT_HUD_HEALTH | PORT_HUD_WATER
                   | PORT_HUD_LIVES | PORT_HUD_RED_COINS;  // the parts it hides: all, until chosen

// ------------------------------------------------------------------ the file
// <data>/sms-port/hud.txt, where <data> is as the memory card's: SMS_SAVE_DIR's parent when set
// (card-a's folder), else XDG_DATA_HOME, %APPDATA%, or ~/.local/share.
#ifdef _WIN32
std::wstring widen(const char* s, UINT page) {
    const int n = MultiByteToWideChar(page, 0, s, -1, nullptr, 0);
    std::wstring out(size_t(n > 0 ? n : 1), L'\0');
    if (n > 0) MultiByteToWideChar(page, 0, s, -1, &out[0], n);
    out.resize(wcslen(out.c_str()));
    return out;
}
std::wstring folder() {
    if (const char* d = getenv("SMS_SAVE_DIR")) {  // .../card-a: its parent
        std::wstring p = widen(d, CP_ACP);
        while (!p.empty() && (p.back() == L'/' || p.back() == L'\\')) p.pop_back();
        const size_t slash = p.find_last_of(L"/\\");
        return slash == std::wstring::npos ? L"." : p.substr(0, slash);
    }
    if (const wchar_t* x = _wgetenv(L"APPDATA")) return std::wstring(x) + L"/sms-port";
    return L".";
}
FILE* openHud(const wchar_t* mode) {
    const std::wstring dir = folder();
    for (size_t i = 1; i <= dir.size(); i++)  // make it, a step at a time
        if (i == dir.size() || dir[i] == L'/' || dir[i] == L'\\') _wmkdir(dir.substr(0, i).c_str());
    return _wfopen((dir + L"/hud.txt").c_str(), mode);
}
#else
std::string folder() {
    if (const char* d = getenv("SMS_SAVE_DIR")) {
        std::string p = d;
        while (!p.empty() && p.back() == '/') p.pop_back();
        const size_t slash = p.find_last_of('/');
        return slash == std::string::npos ? "." : p.substr(0, slash);
    }
    if (const char* x = getenv("XDG_DATA_HOME")) return std::string(x) + "/sms-port";
    return std::string(getenv("HOME") ? getenv("HOME") : ".") + "/.local/share/sms-port";
}
FILE* openHud(const char* mode) {
    const std::string dir = folder();
    for (size_t i = 1; i <= dir.size(); i++)
        if (i == dir.size() || dir[i] == '/') mkdir(dir.substr(0, i).c_str(), 0755);
    return fopen((dir + "/hud.txt").c_str(), mode);
}
#endif

void load() {
    if (s_loaded) return;
    s_loaded = true;
#ifdef _WIN32
    FILE* f = openHud(L"r");
#else
    FILE* f = openHud("r");
#endif
    if (!f) return;  // none yet: the defaults
    char line[128], key[64];
    int value;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || sscanf(line, " %63[a-z_] = %d", key, &value) != 2) continue;
        if (!strcmp(key, "hide")) s_hide = value != 0;
        for (const Part& p : kParts)
            if (!strcmp(key, p.key)) s_parts = value ? s_parts | p.bit : s_parts & ~p.bit;
    }
    fclose(f);
    if (s_hide) port_log("[hud] hidden (the settings' HUD page; HUD_TOGGLE shows it)\n");
}

// The whole file, each change: a few lines.
void save() {
#ifdef _WIN32
    FILE* f = openHud(L"w");
#else
    FILE* f = openHud("w");
#endif
    if (!f) {
        static bool told;
        if (!told) port_log("[hud] could not save the HUD settings (hud.txt)\n");
        told = true;
        return;
    }
    fprintf(f, "# The game's HUD (the pause menu's Settings, HUD). 1 on, 0 off.\n");
    fprintf(f, "# hide hides the parts that are 1 below it; the HUD_TOGGLE key (H) flips it.\n");
    fprintf(f, "hide = %d\n", s_hide);
    for (const Part& p : kParts) fprintf(f, "%s = %d\n", p.key, (s_parts & p.bit) ? 1 : 0);
    fclose(f);
}

// ------------------------------------------------------------------ the page
int offOnCount() { return 2; }
const char* offOnName(int i) { return i ? "On" : "Off"; }

int hideGet() {
    load();
    return s_hide;
}
int hideSet(int v) {
    load();
    s_hide = v != 0;
    save();
    return 1;
}

// One get and set per part: the table wants plain functions.
template <int N>
int partGet() {
    load();
    return (s_parts & kParts[N].bit) ? 1 : 0;
}
template <int N>
int partSet(int v) {
    load();
    s_parts = v ? s_parts | kParts[N].bit : s_parts & ~kParts[N].bit;
    save();
    return 1;
}

// Names and hints: plain letters, digits, spaces and . , - ( ) ' only (port_settings.h).
const PortSetting kHudSettings[] = {
    {nullptr, PORT_SETTINGS_PAGE_HUD, 10, "Hide HUD",
     "Hides the parts switched on below. The H key switches it while playing.", 0, nullptr,
     offOnCount, offOnName, hideGet, hideSet},
    {nullptr, PORT_SETTINGS_PAGE_HUD, 20, "Hide Coin Counter",
     "With Hide HUD, the coin counter and its sparkle.", 0, nullptr, offOnCount, offOnName, partGet<0>,
     partSet<0>},
    {nullptr, PORT_SETTINGS_PAGE_HUD, 30, "Hide Shine Counter",
     "With Hide HUD, the Shine counter and its sparkle.", 0, nullptr, offOnCount, offOnName, partGet<1>,
     partSet<1>},
    {nullptr, PORT_SETTINGS_PAGE_HUD, 40, "Hide Blue Coin Counter",
     "With Hide HUD, the blue coin counter and its sparkle.", 0, nullptr, offOnCount, offOnName, partGet<2>,
     partSet<2>},
    {nullptr, PORT_SETTINGS_PAGE_HUD, 50, "Hide Health Meter",
     "With Hide HUD, the sun-shaped health meter.", 0, nullptr, offOnCount, offOnName, partGet<3>, partSet<3>},
    {nullptr, PORT_SETTINGS_PAGE_HUD, 60, "Hide Water Gauge",
     "With Hide HUD, FLUDD's water gauge (and Yoshi's juice). FLUDD works as ever.", 0, nullptr, offOnCount,
     offOnName, partGet<4>, partSet<4>},
    {nullptr, PORT_SETTINGS_PAGE_HUD, 70, "Hide Lives Counter",
     "With Hide HUD, the lives counter.", 0, nullptr, offOnCount, offOnName, partGet<5>, partSet<5>},
    {nullptr, PORT_SETTINGS_PAGE_HUD, 80, "Hide Red Coin Counter",
     "With Hide HUD, the red coin counter.", 0, nullptr, offOnCount, offOnName, partGet<6>, partSet<6>},
};

}  // namespace

extern "C" {

const PortSetting* port_hud_settings(int* count) {
    *count = int(sizeof kHudSettings / sizeof kHudSettings[0]);
    return kHudSettings;
}

unsigned port_hud_settings_mask(void) {
    load();
    return s_hide ? s_parts : 0;
}

void port_hud_settings_toggle(void) {
    load();
    s_hide = !s_hide;
    save();
    port_log("[hud] %s (HUD_TOGGLE)\n", s_hide ? "hidden" : "shown");
}

}  // extern "C"
