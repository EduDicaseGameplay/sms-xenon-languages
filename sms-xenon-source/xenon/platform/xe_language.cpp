// The language menu shown before the game starts (platform/misc/language.cpp asks for it when
// sms_env.txt has no SMS_LANGUAGE, or SMS_LANGUAGE=ask): English and every pack installed in
// game:\mods\languages\<code>\files, in the console's own message box (XShowMessageBoxUI).
// The box holds three buttons at most, so the list goes in pages of two with "More" on the third.
// It runs on the boot thread before the game's renderer exists: the box needs a title that
// presents, so a device of its own clears the screen to black under it and is released before
// xe_gx_exec.cpp creates the game's.
#include "../msvc/include/xe_win.h"
#include <stdio.h>
#include <string.h>

#ifndef INVALID_FILE_ATTRIBUTES
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#endif

namespace gx { void logmsg(const char* fmt, ...); }

namespace {

struct Choice {
	char code[16];
	WCHAR name[40];
};

// The names the menu shows, in its order; other installed packs follow under their code.
struct Known {
	const char* code;
	const WCHAR* name;
};
const Known kKnown[] = {
	{ "en", L"English" },
	{ "pt", L"Portugu\u00eas (Brasil)" },
	{ "es", L"Espa\u00f1ol" },
	{ "fr", L"Fran\u00e7ais" },
	{ "de", L"Deutsch" },
	{ "it", L"Italiano" },
};
const int kMaxChoices = 16;

bool has_pack(const char* code)
{
	char path[MAX_PATH];
	_snprintf(path, sizeof path, "game:\\mods\\languages\\%s\\files", code);
	path[sizeof path - 1] = 0;
	DWORD a = GetFileAttributesA(path);
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

void add(Choice* list, int* n, const char* code, const WCHAR* name)
{
	if (*n >= kMaxChoices)
		return;
	Choice& c = list[(*n)++];
	_snprintf(c.code, sizeof c.code, "%s", code);
	c.code[sizeof c.code - 1] = 0;
	if (name) {
		wcsncpy(c.name, name, 39);
	} else {  // a pack the table does not know: its code, in capitals
		int i = 0;
		for (; code[i] && i < 39; i++)
			c.name[i] = (WCHAR)(code[i] >= 'a' && code[i] <= 'z' ? code[i] - 32 : code[i]);
		c.name[i] = 0;
	}
	c.name[39] = 0;
}

bool known(const char* code)
{
	for (int i = 0; i < (int)(sizeof kKnown / sizeof kKnown[0]); i++)
		if (!_stricmp(kKnown[i].code, code))
			return true;
	return false;
}

int installed(Choice* list)
{
	int n = 0;
	add(list, &n, "en", kKnown[0].name);  // the disc's own text
	for (int i = 1; i < (int)(sizeof kKnown / sizeof kKnown[0]); i++)
		if (has_pack(kKnown[i].code))
			add(list, &n, kKnown[i].code, kKnown[i].name);
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA("game:\\mods\\languages\\*", &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			const char* c = fd.cFileName;
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || c[0] == '.' || known(c) || strlen(c) >= 16)
				continue;
			if (has_pack(c))
				add(list, &n, c, NULL);
		} while (FindNextFileA(h, &fd));
		FindClose(h);
	}
	return n;
}

void black(IDirect3DDevice9* dev)
{
	dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
	dev->Present(NULL, NULL, NULL, NULL);
}

bool any_held()
{
	for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
		XINPUT_STATE st;
		if (XInputGetState(i, &st) != ERROR_SUCCESS)
			continue;
		const XINPUT_GAMEPAD& g = st.Gamepad;
		if (g.wButtons || g.bLeftTrigger > 30 || g.bRightTrigger > 30)
			return true;
	}
	return false;
}

// Until every controller's buttons have been up for a moment (a few seconds at most): the A that
// started the game from the dashboard, or the one that pressed "More", would otherwise be taken
// by the box as soon as it opens and choose its first button.
void wait_released(IDirect3DDevice9* dev)
{
	int up = 0;
	for (int frames = 0; frames < 300 && up < 15; frames++) {
		up = any_held() ? 0 : up + 1;
		black(dev);
	}
}

// The system's notifications so far, into the log (a box the system closes says why there).
void log_notifications(HANDLE listener)
{
	DWORD id;
	ULONG_PTR param;
	while (listener && XNotifyGetNext(listener, 0, &id, &param))
		gx::logmsg("language menu: notification %08lx (%08lx)", id, (unsigned long)param);
}

// One message box, the screen kept black under it; the button pressed, or -1 (B, or no box).
// Right after the dashboard starts the title the system may close a box at once, as cancelled:
// a cancel quicker than anyone presses B opens it again.
int ask(IDirect3DDevice9* dev, HANDLE listener, const WCHAR* text, const WCHAR** buttons, int count)
{
	for (int attempt = 1; attempt <= 5; attempt++) {
		wait_released(dev);
		log_notifications(listener);
		MESSAGEBOX_RESULT result;
		XOVERLAPPED ov;
		DWORD r = ERROR_ACCESS_DENIED;
		// the system's interface may not be ready the moment the title starts: try for a few seconds
		for (int tries = 0; tries < 300; tries++) {
			memset(&ov, 0, sizeof ov);
			memset(&result, 0, sizeof result);
			r = XShowMessageBoxUI(XUSER_INDEX_ANY, L"Super Mario Sunshine", text, count, buttons, 0, XMB_NOICON,
			                      &result, &ov);
			if (r == ERROR_IO_PENDING)
				break;
			black(dev);
		}
		if (r != ERROR_IO_PENDING) {
			gx::logmsg("language menu: XShowMessageBoxUI failed (%lu)", r);
			return -1;
		}
		const DWORD opened = GetTickCount();
		while (!XHasOverlappedIoCompleted(&ov))
			black(dev);
		const DWORD done = XGetOverlappedResult(&ov, NULL, TRUE), ms = GetTickCount() - opened;
		gx::logmsg("language menu: attempt %d, box closed after %lu ms (%lu), button %lu", attempt, ms, done,
		           result.dwButtonPressed);
		log_notifications(listener);
		if (done == ERROR_SUCCESS)
			return (int)result.dwButtonPressed;
		if (done != ERROR_CANCELLED || ms >= 1500)
			return -1;  // B
		for (int frames = 0; frames < 60; frames++)  // a second of black, then the box again
			black(dev);
	}
	return -1;
}

} // namespace

// The chosen language's code, or NULL (only English installed, or no box could be shown).
extern "C" const char* xe_language_pick(void)
{
	static Choice list[kMaxChoices];
	const int n = installed(list);
	if (n < 2)
		return NULL;

	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	D3DPRESENT_PARAMETERS pp;
	memset(&pp, 0, sizeof pp);
	pp.BackBufferWidth = 1280;
	pp.BackBufferHeight = 720;
	pp.BackBufferFormat = D3DFMT_A8R8G8B8;
	pp.BackBufferCount = 1;
	pp.EnableAutoDepthStencil = FALSE;
	pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
	IDirect3DDevice9* dev = NULL;
	if (!d3d || FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev))) {
		gx::logmsg("language menu: no Direct3D device");
		if (d3d)
			d3d->Release();
		return NULL;
	}

	// the system's notifications, for the log; and two seconds of black first, while the dashboard
	// finishes handing the screen over (a box asked for sooner was closed at once on the console)
	HANDLE listener = XNotifyCreateListener(XNOTIFY_SYSTEM);
	for (int frames = 0; frames < 120; frames++)
		black(dev);

	static const WCHAR kText[] = L"Escolha o idioma / Choose your language / Elige el idioma";
	const char* chosen = NULL;
	int first = 0;  // the first choice on the page shown
	for (;;) {
		const WCHAR* buttons[XMB_MAXBUTTONS];
		int count, more;
		if (n - first <= XMB_MAXBUTTONS) {  // the rest fits: no "More"
			count = n - first;
			more = -1;
		} else {
			count = XMB_MAXBUTTONS - 1;
			more = count;
		}
		for (int i = 0; i < count; i++)
			buttons[i] = list[first + i].name;
		int shown = count;
		if (more >= 0)
			buttons[shown++] = L"Mais / More / M\u00e1s  >";
		int b = ask(dev, listener, kText, buttons, shown);
		if (b < 0) {  // B: back to the first page, or English from there
			if (first > 0) {
				first = 0;
				continue;
			}
			break;
		}
		if (b == more) {
			first += count;
			if (first >= n)
				first = 0;
			continue;
		}
		chosen = list[first + b].code;
		break;
	}

	if (listener)
		CloseHandle(listener);
	dev->Release();
	d3d->Release();
	gx::logmsg("language menu: %s", chosen ? chosen : "en (closed)");
	return chosen;
}
