// Controller 1 on the 360: XInput pad 0 turned into the SDL game controller events that
// platform/pad/pad.cpp reads on the PC (its sms_gx_* hooks), so its layout, bindings, combos
// and rumble work unchanged. Polled once a PADRead (sms_gx_pump_events).
#include "../msvc/include/xe_win.h"
#include <string.h>

union SDL_Event;

namespace {

// pad.cpp's mirror of SDL2's event ABI
enum { CAXIS = 0x650, CBUTTONDOWN = 0x651, CBUTTONUP = 0x652 };
struct AxisEvent {
	unsigned type, timestamp;
	int which;
	unsigned char axis, p1, p2, p3;
	short value;
	unsigned short p4;
};
struct ButtonEvent {
	unsigned type, timestamp;
	int which;
	unsigned char button, state, p1, p2;
};

void (*g_cb)(const SDL_Event*);
WORD g_buttons;
short g_axes[6];

// SDL_CONTROLLER_BUTTON_* for XInput's button bits (bit n: kButton[n]; -1 none)
const signed char kButton[16] = {11, 12, 13, 14, 6, 4, 7, 8, 9, 10, -1, -1, 0, 1, 2, 3};

short flip(short v) { return v == -32768 ? 32767 : short(-v); }  // XInput's Y is up, SDL's down

void axis(int n, short v)
{
	if (v == g_axes[n]) return;
	g_axes[n] = v;
	AxisEvent e;
	memset(&e, 0, sizeof e);
	e.type = CAXIS;
	e.axis = (unsigned char)n;
	e.value = v;
	g_cb((const SDL_Event*)&e);
}

}  // namespace

extern "C" {

void sms_gx_set_event_callback(void (*cb)(const SDL_Event*)) { g_cb = cb; }

void sms_gx_pump_events(void)
{
	if (!g_cb) return;
	XINPUT_STATE st;
	memset(&st, 0, sizeof st);
	if (XInputGetState(0, &st) != ERROR_SUCCESS) memset(&st, 0, sizeof st);  // unplugged: all released
	const XINPUT_GAMEPAD& p = st.Gamepad;
	const WORD changed = WORD(p.wButtons ^ g_buttons);
	for (int i = 0; i < 16; i++)
		if ((changed >> i) & 1 && kButton[i] >= 0) {
			ButtonEvent e;
			memset(&e, 0, sizeof e);
			e.type = (p.wButtons >> i) & 1 ? CBUTTONDOWN : CBUTTONUP;
			e.button = (unsigned char)kButton[i];
			e.state = (p.wButtons >> i) & 1;
			g_cb((const SDL_Event*)&e);
		}
	g_buttons = p.wButtons;
	axis(0, p.sThumbLX);
	axis(1, flip(p.sThumbLY));
	axis(2, p.sThumbRX);
	axis(3, flip(p.sThumbRY));
	axis(4, short(p.bLeftTrigger * 32767 / 255));
	axis(5, short(p.bRightTrigger * 32767 / 255));
}

int sms_gx_rumble(unsigned short strength)
{
	XINPUT_VIBRATION v = {strength, strength};
	return XInputSetState(0, &v) == ERROR_SUCCESS;
}

void sms_gx_rumble_shutdown(void) { sms_gx_rumble(0); }

}  // extern "C"
