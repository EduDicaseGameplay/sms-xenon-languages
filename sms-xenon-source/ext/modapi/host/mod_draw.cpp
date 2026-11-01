// The native mod API's drawing (SmsDrawApi): the triangles mods ask for in
// SMS_EV_DRAW, built into one list a frame and handed to the window
// (GXPC_ModLayerSubmit, platform/gx/src/gx_mod_layer.cpp), which draws them
// over the game's picture at the window's resolution. Text is stb_easy_font's
// quads; the world's primitives are projected with this frame's camera and
// clipped at its near plane.
#include "mod_host.h"

#include "sms_gx/gx_pc.h"

#include <math.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <vector>

#include "../../../platform/gx/src/third_party/stb_easy_font.h"

using namespace modhost;

namespace {

std::vector<GXPCModVertex> s_v;
std::vector<GXPCModBatch> s_b;
std::vector<SmsRect> s_clips;
bool s_drawing = false;
float s_w = 640, s_h = 480;
float s_used[9]; // each anchor's panels so far this frame
// the panels each column (left, middle, right) gave out this frame, from any
// of its three anchors: a top stack and a bottom stack must not meet
std::vector<SmsRect> s_taken[3];
std::map<SmsMod*, bool> s_warned;

struct ImageInfo {
	SmsMod* owner;
	int w, h;
};
std::map<SmsImage, ImageInfo> s_images;

bool ready()
{
	if (s_drawing)
		return true;
	SmsMod* m = current();
	if (m && !s_warned[m]) {
		s_warned[m] = true;
		log(m, SMS_LOG_WARN, "draws outside SMS_EV_DRAW: ignored");
	}
	return false;
}

void color(uint8_t out[4], SmsColor c)
{
	out[0] = (uint8_t)(c >> 24);
	out[1] = (uint8_t)(c >> 16);
	out[2] = (uint8_t)(c >> 8);
	out[3] = (uint8_t)c;
}

SmsColor withAlpha(SmsColor c, float k)
{
	return (c & 0xFFFFFF00u) | (uint32_t)((float)(c & 0xFF) * k);
}

// The batch new triangles go into: the last one if it samples the same image
// within the same clip.
GXPCModBatch& batch(SmsImage image)
{
	float clip[4] = {0, 0, -1, -1};
	if (!s_clips.empty()) {
		const SmsRect& r = s_clips.back();
		clip[0]          = r.x;
		clip[1]          = r.y;
		clip[2]          = r.w;
		clip[3]          = r.h;
	}
	if (s_b.empty() || s_b.back().image != image || memcmp(s_b.back().clip, clip, sizeof clip)) {
		GXPCModBatch b;
		b.first = (uint32_t)s_v.size();
		b.count = 0;
		b.image = image;
		memcpy(b.clip, clip, sizeof clip);
		s_b.push_back(b);
	}
	return s_b.back();
}

void vertex(float x, float y, float u, float v, SmsColor c)
{
	GXPCModVertex p;
	p.x = x;
	p.y = y;
	p.u = u;
	p.v = v;
	color(p.rgba, c);
	s_v.push_back(p);
}

void tri(float x0, float y0, float x1, float y1, float x2, float y2, SmsColor c)
{
	batch(0).count += 3;
	vertex(x0, y0, 0, 0, c);
	vertex(x1, y1, 0, 0, c);
	vertex(x2, y2, 0, 0, c);
}

void quad(float x, float y, float w, float h, SmsColor top, SmsColor bottom)
{
	batch(0).count += 6;
	vertex(x, y, 0, 0, top);
	vertex(x + w, y, 0, 0, top);
	vertex(x + w, y + h, 0, 0, bottom);
	vertex(x, y, 0, 0, top);
	vertex(x + w, y + h, 0, 0, bottom);
	vertex(x, y + h, 0, 0, bottom);
}

void segment(float x0, float y0, float x1, float y1, float t, SmsColor c)
{
	float dx = x1 - x0, dy = y1 - y0;
	const float len = sqrtf(dx * dx + dy * dy);
	if (len < 1e-4f)
		return;
	const float nx = -dy / len * t * 0.5f, ny = dx / len * t * 0.5f;
	tri(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, c);
	tri(x0 + nx, y0 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, c);
}

// ------------------------------------------------------------------ text

// The font: stb_easy_font's glyphs (lines 12 font pixels apart), drawn once
// into an atlas at kSuper texels a font pixel and mipmapped, then drawn as one
// textured quad a character. Its strokes are a font pixel wide, which drawn as
// quads at a fraction of a window pixel would come and go; sampled from the
// atlas they stay, smoothed.
const int kSuper = 6, kCellW = 10, kCellH = 14, kPad = 2, kCols = 16, kRows = 6;
SmsImage s_font = 0;

bool fontReady()
{
	if (s_font)
		return true;
	const int cw = kCellW * kSuper, chh = kCellH * kSuper, w = cw * kCols, h = chh * kRows;
	std::vector<uint8_t> px(size_t(w) * h * 4, 0);
	std::vector<char> buf(4096);
	for (int c = 32; c < 127; c++) {
		const int cell = c - 32, ox = (cell % kCols) * cw, oy = (cell / kCols) * chh;
		char text[2] = {(char)c, 0};
		const int quads = stb_easy_font_print((float)kPad, (float)kPad, text, nullptr, buf.data(), (int)buf.size());
		struct V {
			float x, y, z;
			unsigned char c[4];
		};
		const V* v = (const V*)buf.data();
		for (int q = 0; q < quads; q++) {
			const V* k   = v + q * 4;
			float x0 = k[0].x, y0 = k[0].y, x1 = k[0].x, y1 = k[0].y;
			for (int i = 1; i < 4; i++) {
				x0 = std::min(x0, k[i].x);
				x1 = std::max(x1, k[i].x);
				y0 = std::min(y0, k[i].y);
				y1 = std::max(y1, k[i].y);
			}
			// a little bolder than a font pixel, to stay legible small
			const int b = kSuper / 6;
			for (int y = std::max(0, (int)(y0 * kSuper) - b); y < (int)(y1 * kSuper) + b && y < chh; y++)
				for (int x = std::max(0, (int)(x0 * kSuper) - b); x < (int)(x1 * kSuper) + b && x < cw; x++) {
					uint8_t* p = &px[(size_t(oy + y) * w + ox + x) * 4];
					p[0] = p[1] = p[2] = p[3] = 255;
				}
		}
	}
	s_font = GXPC_ModImageCreate(px.data(), w, h);
	return s_font != 0;
}

float advance(unsigned char c) { return (float)(stb_easy_font_charinfo[c - 32].advance & 15) + stb_easy_font_spacing_val; }

unsigned char glyph(char c) { return (unsigned char)c >= 32 && (unsigned char)c < 127 ? (unsigned char)c : '?'; }

// One line or more, a line `size` high, from its top left.
void textRun(float x, float y, float size, SmsColor c, const char* text)
{
	if (!fontReady())
		return;
	const float s = size / 12.0f;
	float pen = x;
	const float cu = 1.0f / kCols, cv = 1.0f / kRows;
	for (const char* t = text; *t; t++) {
		if (*t == '\n') {
			pen = x;
			y += size;
			continue;
		}
		const unsigned char g = glyph(*t);
		if (g != ' ') {
			const int cell = g - 32;
			const float u0 = (cell % kCols) * cu, v0 = (cell / kCols) * cv;
			const float qx = pen - kPad * s, qy = y - kPad * s, qw = kCellW * s, qh = kCellH * s;
			batch(s_font).count += 6;
			vertex(qx, qy, u0, v0, c);
			vertex(qx + qw, qy, u0 + cu, v0, c);
			vertex(qx + qw, qy + qh, u0 + cu, v0 + cv, c);
			vertex(qx, qy, u0, v0, c);
			vertex(qx + qw, qy + qh, u0 + cu, v0 + cv, c);
			vertex(qx, qy + qh, u0, v0 + cv, c);
		}
		pen += advance(g) * s;
	}
}

float textWidth(float size, const char* text)
{
	if (!text)
		return 0;
	float line = 0, widest = 0;
	for (const char* t = text; *t; t++) {
		if (*t == '\n') {
			widest = std::max(widest, line);
			line   = 0;
		} else {
			line += advance(glyph(*t));
		}
	}
	return std::max(widest, line) * size / 12.0f;
}

// ------------------------------------------------------------------ the API

void apiCanvas(float* w, float* h)
{
	if (w)
		*w = s_w;
	if (h)
		*h = s_h;
}

void apiRect(float x, float y, float w, float h, SmsColor c)
{
	if (ready())
		quad(x, y, w, h, c, c);
}

void apiRectOutline(float x, float y, float w, float h, float t, SmsColor c)
{
	if (!ready())
		return;
	quad(x, y, w, t, c, c);
	quad(x, y + h - t, w, t, c, c);
	quad(x, y + t, t, h - 2 * t, c, c);
	quad(x + w - t, y + t, t, h - 2 * t, c, c);
}

void apiRectGradient(float x, float y, float w, float h, SmsColor top, SmsColor bottom)
{
	if (ready())
		quad(x, y, w, h, top, bottom);
}

void apiLine(float x0, float y0, float x1, float y1, float t, SmsColor c)
{
	if (ready())
		segment(x0, y0, x1, y1, t, c);
}

void apiTriangle(float x0, float y0, float x1, float y1, float x2, float y2, SmsColor c)
{
	if (ready())
		tri(x0, y0, x1, y1, x2, y2, c);
}

int circleSteps(float r) { return std::max(12, std::min(64, (int)(r * 1.5f))); }

void apiCircle(float cx, float cy, float r, SmsColor c)
{
	if (!ready() || r <= 0)
		return;
	const int n = circleSteps(r);
	for (int i = 0; i < n; i++) {
		const float a0 = 6.2831853f * i / n, a1 = 6.2831853f * (i + 1) / n;
		tri(cx, cy, cx + cosf(a0) * r, cy + sinf(a0) * r, cx + cosf(a1) * r, cy + sinf(a1) * r, c);
	}
}

void apiCircleOutline(float cx, float cy, float r, float t, SmsColor c)
{
	if (!ready() || r <= 0)
		return;
	const int n = circleSteps(r);
	const float ri = std::max(0.0f, r - t * 0.5f), ro = r + t * 0.5f;
	for (int i = 0; i < n; i++) {
		const float a0 = 6.2831853f * i / n, a1 = 6.2831853f * (i + 1) / n;
		const float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
		tri(cx + c0 * ri, cy + s0 * ri, cx + c0 * ro, cy + s0 * ro, cx + c1 * ro, cy + s1 * ro, c);
		tri(cx + c0 * ri, cy + s0 * ri, cx + c1 * ro, cy + s1 * ro, cx + c1 * ri, cy + s1 * ri, c);
	}
}

float apiText(float x, float y, float size, SmsColor c, int flags, const char* text)
{
	if (!ready() || !text || !*text || size <= 0)
		return 0;
	const float w = textWidth(size, text);
	if (flags & SMS_TEXT_CENTER)
		x -= w * 0.5f;
	else if (flags & SMS_TEXT_RIGHT)
		x -= w;
	const float px = size / 12.0f; // one font pixel
	const SmsColor dark = withAlpha(0x000000FFu, (float)(c & 0xFF) / 255.0f * 0.75f);
	if (flags & SMS_TEXT_OUTLINE) {
		static const int d[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}};
		for (const auto& o : d)
			textRun(x + o[0] * px, y + o[1] * px, size, dark, text);
	} else if (flags & SMS_TEXT_SHADOW) {
		textRun(x + px, y + px, size, dark, text);
	}
	textRun(x, y, size, c, text);
	return w;
}

float apiTextWidth(float size, const char* text) { return textWidth(size, text); }

std::string modPath(SmsMod* mod, const char* path)
{
	std::string p = path ? path : "";
	const bool absolute = !p.empty() && (p[0] == '/' || p[0] == '\\' || (p.size() > 1 && p[1] == ':'));
	return absolute || !mod ? p : mod->dir + "/" + p;
}

SmsImage apiImageLoad(SmsMod* mod, const char* path)
{
	if (!mod || !path)
		return 0;
	int w = 0, h = 0;
	const std::string full = modPath(mod, path);
	const SmsImage id      = GXPC_ModImageLoad(full.c_str(), &w, &h);
	if (!id) {
		log(mod, SMS_LOG_WARN, "could not read the image %s", full.c_str());
		return 0;
	}
	s_images[id] = ImageInfo{mod, w, h};
	return id;
}

SmsImage apiImageCreate(SmsMod* mod, const uint8_t* rgba, int w, int h)
{
	if (!mod)
		return 0;
	const SmsImage id = GXPC_ModImageCreate(rgba, w, h);
	if (id)
		s_images[id] = ImageInfo{mod, w, h};
	return id;
}

void apiImageFree(SmsMod* mod, SmsImage image)
{
	auto it = s_images.find(image);
	if (it == s_images.end() || it->second.owner != mod)
		return;
	GXPC_ModImageFree(image);
	s_images.erase(it);
}

int apiImageSize(SmsImage image, int* w, int* h)
{
	auto it = s_images.find(image);
	if (it == s_images.end())
		return 0;
	if (w)
		*w = it->second.w;
	if (h)
		*h = it->second.h;
	return 1;
}

void apiImageRegion(SmsImage image, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                    SmsColor tint)
{
	if (!ready() || !s_images.count(image))
		return;
	batch(image).count += 6;
	vertex(x, y, u0, v0, tint);
	vertex(x + w, y, u1, v0, tint);
	vertex(x + w, y + h, u1, v1, tint);
	vertex(x, y, u0, v0, tint);
	vertex(x + w, y + h, u1, v1, tint);
	vertex(x, y + h, u0, v1, tint);
}

void apiImage(SmsImage image, float x, float y, float w, float h, SmsColor tint)
{
	apiImageRegion(image, x, y, w, h, 0, 0, 1, 1, tint);
}

void apiClipPush(float x, float y, float w, float h)
{
	if (!ready())
		return;
	SmsRect r = {x, y, std::max(0.0f, w), std::max(0.0f, h)};
	if (!s_clips.empty()) {
		// inside the clip it is pushed in
		const SmsRect& o = s_clips.back();
		const float x0 = std::max(r.x, o.x), y0 = std::max(r.y, o.y);
		const float x1 = std::min(r.x + r.w, o.x + o.w), y1 = std::min(r.y + r.h, o.y + o.h);
		r = SmsRect{x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
	}
	s_clips.push_back(r);
}

void apiClipPop(void)
{
	if (ready() && !s_clips.empty())
		s_clips.pop_back();
}

// ------------------------------------------------------------------ the world

SmsCamera s_cam;
uint32_t s_camFrame = ~0u;

bool cam()
{
	if (s_camFrame != frame()) {
		s_camFrame = frame();
		s_cam.size = sizeof s_cam;
		g_api.camera->get(&s_cam);
	}
	return s_cam.valid != 0;
}

void toView(const SmsVec3* p, float v[3])
{
	for (int r = 0; r < 3; r++)
		v[r] = s_cam.view[r][0] * p->x + s_cam.view[r][1] * p->y + s_cam.view[r][2] * p->z + s_cam.view[r][3];
}

bool viewToCanvas(const float v[3], float* x, float* y)
{
	float clip[4];
	for (int r = 0; r < 4; r++)
		clip[r] = s_cam.proj[r][0] * v[0] + s_cam.proj[r][1] * v[1] + s_cam.proj[r][2] * v[2] + s_cam.proj[r][3];
	if (clip[3] <= 1e-4f)
		return false;
	*x = (clip[0] / clip[3] + 1.0f) * 0.5f * s_w;
	*y = (1.0f - clip[1] / clip[3]) * 0.5f * s_h;
	return true;
}

void apiWorldLine(const SmsVec3* a, const SmsVec3* b, float t, SmsColor c)
{
	if (!ready() || !a || !b || !cam())
		return;
	float va[3], vb[3];
	toView(a, va);
	toView(b, vb);
	// the camera looks down -z: keep the part in front of its near plane
	const float zn = -std::max(1.0f, s_cam.near_z);
	if (va[2] > zn && vb[2] > zn)
		return;
	if (va[2] > zn || vb[2] > zn) {
		float* behind = va[2] > zn ? va : vb;
		const float* front = va[2] > zn ? vb : va;
		const float k = (zn - front[2]) / (behind[2] - front[2]);
		for (int i = 0; i < 3; i++)
			behind[i] = front[i] + (behind[i] - front[i]) * k;
	}
	float x0, y0, x1, y1;
	if (viewToCanvas(va, &x0, &y0) && viewToCanvas(vb, &x1, &y1))
		segment(x0, y0, x1, y1, t, c);
}

void apiWorldBox(const SmsVec3* c, const SmsVec3* hs, float t, SmsColor col)
{
	if (!ready() || !c || !hs)
		return;
	SmsVec3 p[8];
	for (int i = 0; i < 8; i++)
		p[i] = SmsVec3{c->x + (i & 1 ? hs->x : -hs->x), c->y + (i & 2 ? hs->y : -hs->y), c->z + (i & 4 ? hs->z : -hs->z)};
	static const int e[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
	for (const auto& k : e)
		apiWorldLine(&p[k[0]], &p[k[1]], t, col);
}

void apiWorldRing(const SmsVec3* c, float r, float t, SmsColor col)
{
	if (!ready() || !c || r <= 0)
		return;
	const int n = 32;
	for (int i = 0; i < n; i++) {
		const float a0 = 6.2831853f * i / n, a1 = 6.2831853f * (i + 1) / n;
		SmsVec3 p0 = {c->x + cosf(a0) * r, c->y, c->z + sinf(a0) * r};
		SmsVec3 p1 = {c->x + cosf(a1) * r, c->y, c->z + sinf(a1) * r};
		apiWorldLine(&p0, &p1, t, col);
	}
}

int apiWorldText(const SmsVec3* p, float size, int scaled, SmsColor c, int flags, const char* text)
{
	if (!ready() || !p || !text || !cam())
		return 0;
	float v[3], x, y;
	toView(p, v);
	if (v[2] > -std::max(1.0f, s_cam.near_z) || !viewToCanvas(v, &x, &y))
		return 0;
	if (scaled)
		size *= std::max(0.35f, std::min(2.0f, 800.0f / -v[2]));
	if (x < -s_w * 0.5f || x > s_w * 1.5f || y < -s_h * 0.5f || y > s_h * 1.5f)
		return 0;
	apiText(x, y - size, size, c, (flags & ~SMS_TEXT_RIGHT) | SMS_TEXT_CENTER, text);
	return 1;
}

// ------------------------------------------------------------------ layout

// Where each column's stacks start, clear of the game's HUD: its counters
// down the top left, the lives at the top, the health meter at the top right,
// the water gauge at the bottom right, and talk boxes at the bottom middle.
const float kMarginX = 16, kGap = 6;
const float kTop[3] = {216, 84, 170}, kBottom[3] = {24, 96, 176};

int apiHudSlot(SmsMod*, int anchor, float w, float h, SmsRect* out)
{
	if (!out || anchor < 0 || anchor > 8 || w <= 0 || h <= 0)
		return 0;
	const int col = anchor % 3, row = anchor / 3;
	float x = col == 0 ? kMarginX : col == 1 ? (s_w - w) * 0.5f : s_w - kMarginX - w;
	float y;
	if (row == 0)
		y = kTop[col] + s_used[anchor];
	else if (row == 2)
		y = s_h - kBottom[col] - s_used[anchor] - h;
	else
		y = s_h * 0.32f + s_used[anchor];
	if (y < 0 || y + h > s_h)
		return 0;
	for (const SmsRect& r : s_taken[col])
		if (y < r.y + r.h + kGap && y + h + kGap > r.y)
			return 0; // into another anchor's panels on this side
	s_used[anchor] += h + kGap;
	*out = SmsRect{x, y, w, h};
	s_taken[col].push_back(*out);
	return 1;
}

} // namespace

namespace modhost {

const SmsDrawApi g_drawApi = {
	sizeof(SmsDrawApi), apiCanvas,      apiRect,         apiRectOutline, apiRectGradient, apiLine,
	apiTriangle,        apiCircle,      apiCircleOutline, apiText,       apiTextWidth,    apiImageLoad,
	apiImageCreate,     apiImageFree,   apiImageSize,    apiImage,       apiImageRegion,  apiClipPush,
	apiClipPop,         apiWorldLine,   apiWorldBox,     apiWorldRing,   apiWorldText,    apiHudSlot,
};

void drawBegin()
{
	GXPC_ModLayerCanvas(&s_w, &s_h);
	s_v.clear();
	s_b.clear();
	s_clips.clear();
	memset(s_used, 0, sizeof s_used);
	for (auto& t : s_taken)
		t.clear();
	s_drawing = true;
}

void drawEnd()
{
	s_drawing = false;
	s_clips.clear();
	// drop empty batches
	s_b.erase(std::remove_if(s_b.begin(), s_b.end(), [](const GXPCModBatch& b) { return b.count == 0; }), s_b.end());
	static bool s_lastEmpty = true;
	if (s_b.empty() && s_lastEmpty)
		return; // nothing then, nothing now
	s_lastEmpty = s_b.empty();
	GXPC_ModLayerSubmit(s_v.data(), (int)s_v.size(), s_b.data(), (int)s_b.size(), s_w, s_h);
}

bool drawing() { return s_drawing; }

void drawCanvas(float* w, float* h)
{
	if (!s_drawing)
		GXPC_ModLayerCanvas(&s_w, &s_h);
	if (w)
		*w = s_w;
	if (h)
		*h = s_h;
}

void drawDropMod(SmsMod* mod)
{
	for (auto it = s_images.begin(); it != s_images.end();) {
		if (it->second.owner == mod) {
			GXPC_ModImageFree(it->first);
			it = s_images.erase(it);
		} else {
			++it;
		}
	}
	s_warned.erase(mod);
}

} // namespace modhost
