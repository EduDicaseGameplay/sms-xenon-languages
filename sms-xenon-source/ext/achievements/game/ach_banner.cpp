// Achievements (ext/achievements/README.md): the banner an unlock shows.
// At the bottom of the screen, a blue ribbon with a white rim holds the
// achievement's name and what was done, in the game's font; its medal (the
// icon, ach_icon.cpp) overlaps the ribbon's left end, and "BADGE GET!" in the
// game's own big letters (game_6's big_tx_*, as "SHINE GET!") drops in above.
// It lasts about five seconds: the medal pops, the ribbon opens, the letters
// fall in one by one and bob, then all of it leaves. Unlocks that come close
// together wait their turn, and none starts during a talk or a cutscene (a
// Shine's is the usual one), so the banner never covers the game's own.
//
// Sizes are the 640 x 448 screen's; the design they come from was drawn on a
// 1600 x 840 frame (kU converts).
#include "achievements.h"

#include "port_ext.h"
#include "port_menu_draw.h"

#include <JSystem/J2D/J2DOrthoGraph.hpp>
#include <JSystem/JDrama/JDRRect.hpp>
#include <JSystem/JKernel/JKRFileLoader.hpp>
#include <JSystem/JUtility/JUTTexture.hpp>
#include <MSound/SoundEffects.hpp>
#include <System/MarDirector.hpp>
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

extern "C" const void* sms_ach_icon(int id, int* size);

namespace {

using namespace PortMenu;

struct Name {
	int id;
	const char* title;
	const char* description;
};
const Name kNames[] = {
#include "ach_names.inc"
};
// Super Mario Eclipse's, in an Eclipse build
const Name kEclipseNames[] = {
#include "ach_eclipse_names.inc"
};

const f32 kU = 448.0f / 840.0f; // the design's pixels to the screen's

// The ribbon, 920 design pixels wide (the longest description, #89's, in two lines), its medal hanging 22 over its left end; the two centred.
const f32 kBarW = 920 * kU, kBarMinH = 112 * kU, kRadius = 18 * kU, kRim = 4 * kU;
const f32 kMedal = 156 * kU, kMedalOut = 22 * kU;
const f32 kPadLeft = 150 * kU, kPadRight = 26 * kU, kPadV = 14 * kU;
// The game's font draws its letters smaller than their size, so the text is sized to look as the design's does.
const f32 kTitle = 38 * kU, kText = 20 * kU, kTextMax = kBarW - kPadLeft - kPadRight, kLineGap = 3 * kU;
const f32 kBottom = 34 * kU;
// BADGE GET!: letters 78 tall, 4 over each other, a 22 gap between the words, 24 above the ribbon.
const f32 kLetter = 78 * kU, kOverlap = 4 * kU, kWordGap = 22 * kU, kAbove = 24 * kU;
const int kTilt[10] = { -7, 5, -4, 6, -5, 0, 6, -5, 4, 8 };

// Seconds: in until 1.5, held, out from kOut, gone at kEnd.
const f32 kOut = 4.4f, kEnd = kOut + 0.7f;

int s_queue[16], s_queued;
int s_id;       // the banner showing, or 0
OSTime s_start; // when it started

JUTTexture s_big[7]; // b a d g e t !
unsigned s_visit; // the stage visit it started in: its letters are that stage's game_6

f32 clampf(f32 v, f32 a, f32 b) { return v < a ? a : v > b ? b : v; }
f32 seg(f32 t, f32 a, f32 b) { return clampf((t - a) / (b - a), 0.0f, 1.0f); }
f32 outBack(f32 p)
{
	const f32 s = 1.9f;
	return 1 + (s + 1) * powf(p - 1, 3) + s * powf(p - 1, 2);
}
f32 outCubic(f32 p) { return 1 - powf(1 - p, 3); }
f32 inCubic(f32 p) { return p * p * p; }
f32 bounce(f32 p)
{
	const f32 n = 7.5625f, d = 2.75f;
	if (p < 1 / d)
		return n * p * p;
	if (p < 2 / d) {
		p -= 1.5f / d;
		return n * p * p + 0.75f;
	}
	if (p < 2.5f / d) {
		p -= 2.25f / d;
		return n * p * p + 0.9375f;
	}
	p -= 2.625f / d;
	return n * p * p + 0.984375f;
}

const Name* nameOf(int id)
{
	const Name* names = ach::eclipse() ? kEclipseNames : kNames;
	const unsigned n  = ach::eclipse() ? sizeof kEclipseNames / sizeof kEclipseNames[0] : sizeof kNames / sizeof kNames[0];
	for (unsigned i = 0; i < n; i++)
		if (names[i].id == id)
			return &names[i];
	return nullptr;
}

bool loadBig()
{
	static const char* const kFiles[7] = { "b", "a", "d", "g", "e", "t", "ex" };
	for (int i = 0; i < 7; i++) {
		char path[48];
		snprintf(path, sizeof path, "/game_6/timg/big_tx_%s.bti", kFiles[i]);
		const ResTIMG* timg = (const ResTIMG*)JKRGetResource(path);
		if (!timg)
			return false;
		s_big[i].storeTIMG(timg);
	}
	return true;
}

u32 mix(u32 a, u32 b, f32 t)
{
	u32 out = 0;
	for (int s = 24; s >= 0; s -= 8) {
		const f32 v = ((a >> s) & 0xFF) * (1 - t) + ((b >> s) & 0xFF) * t;
		out |= (u32)(v + 0.5f) << s;
	}
	return out;
}

void vertex(f32 x, f32 y, u32 colour, u8 alpha)
{
	GXColor c;
	colourOf(colour, alpha, &c);
	GXPosition3f32(x, y, 0.0f);
	GXColor4u8(c.r, c.g, c.b, c.a);
}

void setupFlat()
{
	GXSetNumChans(1);
	GXSetNumTevStages(1);
	GXSetNumTexGens(0);
	GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GXClearVtxDesc();
	GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
	GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
}

// The ribbon's colour at `f` of its height: light blue at the top, deeper below.
u32 ribbonColour(f32 f)
{
	return f < 0.7f ? mix(0x49A6F0FF, 0x1D63B4FF, f / 0.7f) : mix(0x1D63B4FF, 0x18519AFF, (f - 0.7f) / 0.3f);
}

// A box with round corners, (x, y) its top left, in horizontal bands, each
// coloured by `shade` at its height (null: `colour` throughout).
void roundBox(f32 x, f32 y, f32 w, f32 h, f32 r, u32 colour, u8 alpha, u32 (*shade)(f32))
{
	if (w <= 0 || h <= 0)
		return;
	r = r > w / 2 ? w / 2 : r > h / 2 ? h / 2 : r;
	const int kCorner = 8;
	f32 ys[2 * kCorner + 4];
	int n = 0;
	for (int i = 0; i <= kCorner; i++)
		ys[n++] = y + r - r * cosf(1.5707963f * i / kCorner);
	if (shade)
		ys[n++] = y + h * 0.7f;
	for (int i = kCorner; i >= 0; i--)
		ys[n++] = y + h - r + r * cosf(1.5707963f * i / kCorner);
	setupFlat();
	for (int i = 0; i + 1 < n; i++) {
		const f32 y0 = ys[i], y1 = ys[i + 1];
		if (y1 <= y0)
			continue;
		f32 in[2];
		const f32 at[2] = { y0, y1 };
		for (int k = 0; k < 2; k++) {
			const f32 d = at[k] < y + r ? y + r - at[k] : at[k] > y + h - r ? at[k] - (y + h - r) : 0.0f;
			in[k]       = r - sqrtf(r * r - d * d > 0 ? r * r - d * d : 0.0f);
		}
		const u32 c0 = shade ? shade((y0 - y) / h) : colour, c1 = shade ? shade((y1 - y) / h) : colour;
		GXBegin(GX_QUADS, GX_VTXFMT0, 4);
		vertex(x + in[0], y0, c0, alpha);
		vertex(x + w - in[0], y0, c0, alpha);
		vertex(x + w - in[1], y1, c1, alpha);
		vertex(x + in[1], y1, c1, alpha);
		GXEnd();
	}
}

// Texture `tex` in a w x h box whose bottom middle is (cx, by), turned
// `degrees` and squashed by (sx, sy) about that point. `colours`: top to
// bottom at 0, .12, .45, .9 and 1 of the height (the letters' gold), or all
// one colour.
void piece(JUTTexture& tex, f32 cx, f32 by, f32 w, f32 h, f32 degrees, f32 sx, f32 sy, const u32* colours, int bands,
           u8 alpha)
{
	static const f32 kStops[5] = { 0.0f, 0.12f, 0.45f, 0.9f, 1.0f };
	const f32 r = degrees * 3.14159265f / 180.0f, c = cosf(r), s = sinf(r);
	tex.load(GX_TEXMAP0);
	setupTextured();
	for (int b = 0; b < bands; b++) {
		const f32 v0 = bands == 1 ? 0.0f : kStops[b], v1 = bands == 1 ? 1.0f : kStops[b + 1];
		const f32 u[4] = { 0, 1, 1, 0 }, v[4] = { v0, v0, v1, v1 };
		GXBegin(GX_QUADS, GX_VTXFMT0, 4);
		for (int i = 0; i < 4; i++) {
			// from the bottom middle, up is -y
			const f32 dx = (u[i] - 0.5f) * w * sx, dy = -(1 - v[i]) * h * sy;
			GXColor col;
			colourOf(colours[bands == 1 ? 0 : (i < 2 ? b : b + 1)], alpha, &col);
			GXPosition3f32(cx + dx * c + dy * s, by - dx * s + dy * c, 0.0f);
			GXColor4u8(col.r, col.g, col.b, col.a);
			GXTexCoord2f32(u[i], v[i]);
		}
		GXEnd();
	}
}

// The description's lines, wrapped at spaces to kTextMax.
int wrap(const char* text, char lines[4][160])
{
	int n = 0;
	char line[160] = "";
	const char* p = text;
	while (*p && n < 4) {
		const char* end = strchr(p, ' ');
		const size_t len = end ? (size_t)(end - p) : strlen(p);
		char next[160];
		snprintf(next, sizeof next, "%s%s%.*s", line, *line ? " " : "", (int)len, p);
		if (*line && textWidth(next, kText) > kTextMax) {
			snprintf(lines[n++], 160, "%s", line);
			snprintf(line, sizeof line, "%.*s", (int)len, p);
		} else {
			snprintf(line, sizeof line, "%s", next);
		}
		p += len;
		while (*p == ' ')
			p++;
	}
	if (*line && n < 4)
		snprintf(lines[n++], 160, "%s", line);
	return n;
}

void drawBanner(f32 width, f32 height, f32 t)
{
	const Name* name = nameOf(s_id);
	if (!name)
		return;
	char lines[4][160];
	const int count = wrap(name->description, lines);
	const f32 textH = kTitle + count * (kLineGap + kText);
	const f32 barH  = textH + 2 * kPadV > kBarMinH ? textH + 2 * kPadV : kBarMinH;
	const f32 left  = (width - (kBarW + kMedalOut)) / 2 + kMedalOut;
	const f32 top   = height - kBottom - barH;

	// the ribbon opens to the right and shuts to the left
	const f32 open = outBack(seg(t, 0.12f, 0.5f)) * (1 - outCubic(seg(t, kOut + 0.15f, kOut + 0.45f)));
	const f32 shown = kBarW * clampf(open, 0.0f, 1.04f);
	if (shown > 2 * kRadius) {
		for (int i = 3; i >= 1; i--) // its shadow, soft
			roundBox(left - i * 2.0f, top + 5.0f - i * 1.5f, shown + i * 4.0f, barH + i * 3.0f, kRadius + i * 2.0f,
			         0x00000014, 255, nullptr);
		roundBox(left - kRim, top - kRim, shown + 2 * kRim, barH + 2 * kRim, kRadius + kRim, 0x0B2346FF, 255, nullptr);
		roundBox(left, top, shown, barH, kRadius, 0xFFFFFFFF, 255, nullptr);
		roundBox(left + kRim, top + kRim, shown - 2 * kRim, barH - 2 * kRim, kRadius - kRim, 0, 255, ribbonColour);
	}

	// its text slides in from the left
	const f32 textA = seg(t, 0.4f, 0.6f) * (1 - seg(t, kOut + 0.05f, kOut + 0.2f));
	if (textA > 0) {
		const u8 a = (u8)(255 * textA);
		const f32 x = left + kPadLeft + (1 - outCubic(seg(t, 0.4f, 0.6f))) * 24 * kU;
		f32 y = top + (barH - textH) / 2;
		text(name->title, x, y, kTitle, 0xFFFFFFFF, LEFT, a);
		y += kTitle;
		for (int i = 0; i < count; i++, y += kLineGap + kText)
			text(lines[i], x, y + kLineGap, kText, 0xDCEFFFFF, LEFT, a);
	}

	// the medal pops, overshooting, and shrinks away last
	const f32 pop = outBack(seg(t, 0, 0.3f)) * (1 - inCubic(seg(t, kOut + 0.35f, kOut + 0.65f)));
	int size = 0;
	const void* icon = sms_ach_icon(s_id, &size);
	if (icon && pop > 0.01f) {
		GXTexObj obj;
		GXInitTexObj(&obj, (void*)icon, size, size, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
		GXInitTexObjLOD(&obj, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
		GXLoadTexObj(&obj, GX_TEXMAP0);
		setupTextured();
		const f32 d = kMedal * pop, cx = left - kMedalOut + kMedal / 2, cy = top + barH / 2;
		const f32 turn = (1 - seg(t, 0, 0.3f)) * 25.0f;
		quad(cx, cy + 5 * kU, d, d, turn, 0x00000059, 255, true); // its shadow
		GXLoadTexObj(&obj, GX_TEXMAP0);
		quad(cx, cy, d, d, turn, 0xFFFFFFFF, 255, true);
	}

	// BADGE GET! falls in letter by letter, centred over the blue the medal leaves
	static const int kLetters[9] = { 0, 1, 2, 3, 4, -1, 3, 4, 5 }; // B A D G E _ G E T, then !
	f32 lineW = 0;
	for (int i = 0; i < 10; i++)
		lineW += i == 5 ? kWordGap : kLetter - 2 * kOverlap;
	// the ink: B's starts 4/64 in, !'s ends 47/64 in
	const f32 inkL = kLetter * 4 / 64 - kOverlap, inkR = lineW - (kLetter - kOverlap) + kLetter * 47 / 64;
	const f32 blueL = left - kMedalOut + kMedal, blueR = left + kBarW;
	f32 x = (blueL + blueR) / 2 - (inkL + inkR) / 2 - kOverlap;
	const f32 base = top - kRim - kAbove;
	static const u32 kGold[5] = { 0xFFF7A0FF, 0xFFF7A0FF, 0xFFC81EFF, 0xFF6A00FF, 0xFF6A00FF };
	static const u32 kShadow[1] = { 0x3C140073 };
	int k = 0; // letters so far
	for (int i = 0; i < 10; i++) {
		if (i == 5) {
			x += kWordGap;
			continue;
		}
		const int which = i == 9 ? 6 : kLetters[i];
		const f32 cx = x + kLetter / 2;
		x += kLetter - 2 * kOverlap;
		const f32 a = 0.45f + k * 0.06f, p = seg(t, a, a + 0.42f);
		const f32 drop = (1 - bounce(p)) * -230 * kU;
		const f32 land = seg(t, a + 0.16f, a + 0.32f), squash = sinf(land * 3.14159265f) * 0.16f;
		const f32 wave = sinf((t - 1.2f) * 4 - k * 0.55f) * 4 * kU * seg(t, 1.2f, 1.5f);
		const f32 hop  = seg(t, kOut + k * 0.03f, kOut + k * 0.03f + 0.3f);
		const f32 rest = sinf(k * 0.9f) * 5 * kU;
		const f32 y    = base + drop + rest + wave - outCubic(hop) * 60 * kU;
		const f32 tilt = kTilt[k % 10] * (0.4f + 0.6f * p);
		const f32 alpha = (p > 0 ? 1.0f : 0.0f) * (1 - hop);
		k++;
		if (alpha <= 0)
			continue;
		const u8 a8 = (u8)(255 * alpha);
		piece(s_big[which], cx, y + 3 * kU, kLetter, kLetter, tilt, 1 + squash * 0.6f, 1 - squash, kShadow, 1, a8);
		piece(s_big[which], cx, y, kLetter, kLetter, tilt, 1 + squash * 0.6f, 1 - squash, kGold, 4, a8);
	}
}

// Whether a banner may start: in a stage with its HUD's art (game_6) loaded, playing (the director's state
// the warp menu waits for too: not a stage's opening title, the pause menu or a Shine's end), not in a talk or a cutscene.
bool clear()
{
	return gpMarDirector && ach::playing() && gpMarDirector->mState == TMarDirector::STATE_UNK4
	    && !gpMarDirector->isTalkOrDemoModeNow() && loadBig();
}

void draw(float width, float height)
{
	// a second of play first: a stage's opening talk, or a talk's next message, comes sooner
	static OSTime s_clearSince;
	const OSTime now = OSGetTime();
	if (!s_id && s_queued && !clear())
		s_clearSince = now;
	if (!s_id) {
		if (!s_queued || OSTicksToMilliseconds(now - s_clearSince) < 1000)
			return;
		s_id = s_queue[0];
		memmove(s_queue, s_queue + 1, --s_queued * sizeof s_queue[0]);
		s_start   = OSGetTime();
		s_visit   = ach::visit();
		sound(MSD_SE_SY_PROBLEM_SOLVED);
		if (ach::debug())
			ach::log("banner %d", s_id);
	}
	const f32 t = OSTicksToMilliseconds(OSGetTime() - s_start) / 1000.0f;
	// A new stage let go of the old one's letters: the banner ends with it.
	if (t >= kEnd || !gpMarDirector || ach::visit() != s_visit) {
		if (ach::debug())
			ach::log("banner %d done after %.2f s", s_id, t);
		s_id = 0;
		return;
	}
	J2DOrthoGraph graph(JDrama::TRect(0, 0, (int)width, (int)height));
	graph.setPort();
	graph.setup2D();
	wholeScreen(width, height);
	drawBanner(width, height, t);
}

PortExtension s_extension;

struct Register {
	Register()
	{
		s_extension.name = "achievements-banner";
		s_extension.draw = draw;
		port_ext_register(&s_extension);
	}
} s_register;

} // namespace

namespace ach {

void banner(int id)
{
	if (s_queued < (int)(sizeof s_queue / sizeof s_queue[0]))
		s_queue[s_queued++] = id;
}

} // namespace ach
