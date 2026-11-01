// The Mods menu (F1, or SMS_NATIVE_MODS_MENU_KEY): every native mod found,
// whether it loaded (and why not), a switch for each, and the settings mods
// offer (config->add_option). It is drawn with the mods' own drawing, over
// theirs, and has the keyboard and controllers while it is open, so mods need
// no menus or keys of their own for their settings.
#include "mod_host.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>

using namespace modhost;

// The frame rate in force (platform/port_runtime.cpp): outside the unnamed namespace, whose
// extern "C" declaration GCC gives internal linkage.
extern "C" int port_active_frame_rate;

namespace {

bool s_open = false;
int s_sel   = 0;   // the selected row
float s_scroll = 0;
uint16_t s_prevButtons = 0;
int s_stickDir = 0, s_stickHeld = 0; // the stick as a d-pad, with repeat
// menuPad comes once per displayed frame (the pads are read once a frame), so
// the repeat counts 30 fps frames scaled to the frame rate in force: the first
// repeat after 0.7 s, then every 1/6 s, at 30, 60 and 120 alike.
int stickFrames(int frames30) { return frames30 * (port_active_frame_rate > 30 ? port_active_frame_rate / 30 : 1); }

// SDL scancodes (USB HID usages)
enum { kRight = 79, kLeft = 80, kDown = 81, kUp = 82, kReturn = 40, kEscape = 41, kBackspace = 42, kSpace = 44,
       kKpEnter = 88 };

struct Row {
	SmsMod* mod;
	ModOption* opt; // null: the mod's own row
};

std::vector<Row> rows()
{
	std::vector<Row> out;
	for (SmsMod* m : g_mods) {
		out.push_back(Row{m, nullptr});
		if (m->state == SmsMod::LOADED)
			for (ModOption& o : m->options)
				out.push_back(Row{m, &o});
	}
	return out;
}

float optionValue(SmsMod* m, const ModOption& o)
{
	bool found;
	const std::string v = settingGet(m, o.key, "", &found);
	return found && !v.empty() ? (float)strtod(v.c_str(), nullptr) : o.desc.def;
}

std::string optionText(SmsMod* m, const ModOption& o)
{
	const float v = optionValue(m, o);
	char buf[64];
	switch (o.desc.type) {
	case SMS_OPTION_BOOL:
		return v != 0 ? "ON" : "OFF";
	case SMS_OPTION_INT:
		snprintf(buf, sizeof buf, "%d", (int)lroundf(v));
		return buf;
	case SMS_OPTION_FLOAT:
		snprintf(buf, sizeof buf, "%.2f", v);
		return buf;
	case SMS_OPTION_CHOICE: {
		const int i = (int)lroundf(v);
		return i >= 0 && i < (int)o.choices.size() ? o.choices[i] : "?";
	}
	case SMS_OPTION_ACTION:
		return "[ GO ]";
	}
	return "";
}

void changed(SmsMod* m, ModOption& o)
{
	if (!o.desc.on_change)
		return;
	SmsMod* prev = setCurrent(m);
	o.desc.on_change(m, o.key.c_str(), o.desc.user);
	setCurrent(prev);
}

// Left (-1), right (+1) or Enter (0) on a row.
void act(const Row& r, int dir)
{
	SmsMod* m = r.mod;
	if (!r.opt) {
		// the mod's switch
		setModOff(m, m->state == SmsMod::LOADED);
		return;
	}
	ModOption& o = *r.opt;
	float v      = optionValue(m, o);
	switch (o.desc.type) {
	case SMS_OPTION_BOOL:
		v = v != 0 ? 0 : 1;
		break;
	case SMS_OPTION_INT:
	case SMS_OPTION_FLOAT: {
		if (!dir)
			return;
		const float step = o.desc.step > 0 ? o.desc.step : 1;
		v += dir * step;
		if (o.desc.max > o.desc.min)
			v = std::max(o.desc.min, std::min(o.desc.max, v));
		if (o.desc.type == SMS_OPTION_INT)
			v = roundf(v);
		break;
	}
	case SMS_OPTION_CHOICE: {
		const int n = (int)o.choices.size();
		if (n <= 0)
			return;
		int i = ((int)lroundf(v) + (dir < 0 ? n - 1 : 1)) % n;
		v     = (float)i;
		break;
	}
	case SMS_OPTION_ACTION:
		changed(m, o);
		return;
	}
	char buf[32];
	if (o.desc.type == SMS_OPTION_FLOAT)
		snprintf(buf, sizeof buf, "%g", v);
	else
		snprintf(buf, sizeof buf, "%d", (int)lroundf(v));
	settingSet(m, o.key, buf);
	changed(m, o);
}

void move(int dy)
{
	const int n = (int)rows().size();
	if (n)
		s_sel = (s_sel + dy + n) % n;
}

void press(int what) // 0 up, 1 down, 2 left, 3 right, 4 enter, 5 close
{
	std::vector<Row> r = rows();
	if (s_sel >= (int)r.size())
		s_sel = 0;
	switch (what) {
	case 0: move(-1); break;
	case 1: move(1); break;
	case 2: if (!r.empty()) act(r[s_sel], -1); break;
	case 3: if (!r.empty()) act(r[s_sel], 1); break;
	case 4: if (!r.empty()) act(r[s_sel], 0); break;
	case 5: s_open = false; break;
	}
}

const char* stateText(const SmsMod* m)
{
	switch (m->state) {
	case SmsMod::LOADED: return "ON";
	case SmsMod::OFF: return "OFF";
	case SmsMod::FAILED: return "FAILED";
	default: return "...";
	}
}

} // namespace

namespace modhost {

bool menuOpen() { return s_open; }

void menuToggle()
{
	s_open = !s_open;
	s_prevButtons = 0xFFFF; // what is held as it opens does nothing
}

void menuKey(int sc, int down, int)
{
	if (!down)
		return;
	switch (sc) {
	case kUp: press(0); break;
	case kDown: press(1); break;
	case kLeft: press(2); break;
	case kRight: press(3); break;
	case kReturn: case kKpEnter: case kSpace: press(4); break;
	case kEscape: case kBackspace: press(5); break;
	}
}

void menuPad(const SmsPadStatus* raw)
{
	const SmsPadStatus& p = raw[0];
	const uint16_t held = p.err == 0 ? p.buttons : 0;
	const uint16_t pressed = (uint16_t)(held & ~s_prevButtons);
	s_prevButtons = held;
	if (pressed & SMS_PAD_UP) press(0);
	if (pressed & SMS_PAD_DOWN) press(1);
	if (pressed & SMS_PAD_LEFT) press(2);
	if (pressed & SMS_PAD_RIGHT) press(3);
	if (pressed & SMS_PAD_A) press(4);
	if (pressed & SMS_PAD_B) press(5);
	// the stick, with a repeat while held
	int dir = 0;
	if (p.err == 0) {
		if (p.stick_y > 60) dir = 1;
		else if (p.stick_y < -60) dir = 2;
		else if (p.stick_x < -60) dir = 3;
		else if (p.stick_x > 60) dir = 4;
	}
	if (dir && (dir != s_stickDir || ++s_stickHeld > stickFrames(20))) {
		press(dir == 1 ? 0 : dir == 2 ? 1 : dir == 3 ? 2 : 3);
		if (dir == s_stickDir)
			s_stickHeld = stickFrames(16);
		else
			s_stickHeld = 0;
	}
	if (!dir)
		s_stickHeld = 0;
	s_stickDir = dir;
}

void menuDraw()
{
	if (!s_open)
		return;
	const SmsDrawApi* d = &g_drawApi;
	float cw, ch;
	d->canvas(&cw, &ch);
	const SmsColor bg = SMS_RGBA(14, 16, 22, 235), title = SMS_RGBA(41, 74, 122, 255), text = SMS_RGB(255, 255, 255),
	               dim = SMS_RGB(140, 140, 150), sel = SMS_RGBA(250, 190, 40, 60), accent = SMS_RGB(250, 190, 40),
	               bad = SMS_RGB(255, 110, 100), good = SMS_RGB(120, 230, 140);
	const float w = std::min(cw - 40, 560.0f), rowH = 16, x = (cw - w) * 0.5f;
	std::vector<Row> r = rows();
	if (s_sel >= (int)r.size())
		s_sel = (int)r.size() - 1;
	if (s_sel < 0)
		s_sel = 0;
	// one line a row, and a second under a mod's own for its error or description
	float listH = 0;
	for (const Row& row : r)
		listH += rowH + (!row.opt ? 11 : 0);
	const float headH = 26, footH = 22, maxH = ch - 60;
	const float h     = std::min(maxH, headH + listH + footH + 8);
	const float y     = (ch - h) * 0.5f;
	d->rect(x, y, w, h, bg);
	d->rect(x, y, w, headH, title);
	d->text(x + 10, y + 6, 16, text, SMS_TEXT_SHADOW, "MODS");
	char count[64];
	int loaded = 0;
	for (SmsMod* m : g_mods)
		loaded += m->state == SmsMod::LOADED;
	snprintf(count, sizeof count, "%d of %d on", loaded, (int)g_mods.size());
	d->text(x + w - 10, y + 8, 12, dim, SMS_TEXT_RIGHT, count);
	// the list, scrolled to keep the selected row in view
	const float top = y + headH + 4, viewH = h - headH - footH - 8;
	float selY = 0, at = 0;
	for (int i = 0; i < (int)r.size(); i++) {
		if (i == s_sel)
			selY = at;
		at += rowH + (!r[i].opt ? 11 : 0);
	}
	if (selY - s_scroll > viewH - 30)
		s_scroll = selY - viewH + 30;
	if (selY < s_scroll)
		s_scroll = selY;
	d->clip_push(x, top, w, viewH);
	float ry = top - s_scroll;
	for (int i = 0; i < (int)r.size(); i++) {
		const Row& row = r[i];
		SmsMod* m      = row.mod;
		const float lh = rowH + (!row.opt ? 11 : 0);
		if (i == s_sel)
			d->rect(x + 2, ry, w - 4, rowH, sel);
		if (!row.opt) {
			char head[160];
			snprintf(head, sizeof head, "%s  %s", m->name.c_str(), m->version.c_str());
			d->text(x + 10, ry + 3, 12, text, 0, head);
			const SmsColor sc = m->state == SmsMod::LOADED ? good : m->state == SmsMod::FAILED ? bad : dim;
			d->text(x + w - 12, ry + 3, 12, sc, SMS_TEXT_RIGHT, stateText(m));
			const std::string& sub = m->state == SmsMod::LOADED || m->error.empty()
			                             ? (m->description.empty() ? m->author : m->description)
			                             : m->error;
			d->text(x + 18, ry + rowH, 9, m->state == SmsMod::FAILED ? bad : dim, 0, sub.c_str());
		} else {
			d->text(x + 26, ry + 3, 12, i == s_sel ? accent : text, 0, row.opt->label.c_str());
			const std::string v = optionText(m, *row.opt);
			d->text(x + w - 12, ry + 3, 12, i == s_sel ? accent : text, SMS_TEXT_RIGHT, v.c_str());
		}
		ry += lh;
	}
	d->clip_pop();
	if (g_mods.empty())
		d->text(x + 10, top + 4, 12, dim, 0, "No native mods in mods/.");
	d->rect(x, y + h - footH, w, 1, SMS_RGBA(80, 80, 92, 240));
	d->text(x + w * 0.5f, y + h - footH + 6, 10, dim, SMS_TEXT_CENTER,
	        "UP/DOWN choose   LEFT/RIGHT change   ENTER switch   ESC close");
}

} // namespace modhost
