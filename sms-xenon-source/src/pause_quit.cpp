// Port: the pause menu's QUIT GAME item (port_pause_quit.h).
//
// The menu's items are pictures of their words (pause_tx_1 to 3 in game_6:
// CONTINUE, SAVE, EXIT AREA), so this one is a picture too, made from the
// disc's own letters. Its lettering is the file select's (option.szs), drawn
// bolder: the same white letters, narrower, in a thicker outline and without
// the drop shadow. Neither has a G or a Q anywhere on the disc, so QUIT GAME
// is built from the letters' white insides: U, I, T, A and E from the menu's
// words, M from the file select's MONO (narrowed to the menu's proportions),
// Q from CONTINUE's O with a tail, and G from its C with a bar and stem. The
// outline is drawn again around them all as the menu's words have it. (The
// file select's Q, narrowed, closed up its hole, and the word looked bold.)
//
// With a texture pack that has those four words in high resolution (the UHD
// pack draws them 8 times as large), the word is made the same way from the
// pack's, at its size and with mipmaps, so it is as sharp as the words above
// it; without one, at the disc's size, as soft as theirs.
//
// The letters are taken from where they are on the US disc; when any of
// those four pictures differs (a language pack, or a mod's), the item is the
// file select's QUIT word as it is in that language, restyled the same way:
// the pack's picture of it when the pack has one (the UHD pack has the
// European languages' at 8 times), else the disc's.

#include <JSystem/JKernel/JKRFileLoader.hpp>
#include <JSystem/ResTIMG.hpp>
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port_pause_quit.h"
#include "port_savestate.h"

extern "C" void* port_low_alloc(unsigned long size);
extern "C" __attribute__((weak)) int GXPC_RequestQuit(void);
extern "C" __attribute__((weak)) int GXPC_TexturePackHas(const void* data, unsigned fmt, unsigned w, unsigned h);
extern "C" __attribute__((weak)) unsigned char* GXPC_TexturePackImage(const void* data, unsigned fmt, unsigned w,
                                                                      unsigned h, int* outW, int* outH);

namespace {

// The menu's words are IA4 pictures 26 pixels high: white letters in a black
// outline, anti-aliased, that the item's tint colours. The outline's width is
// EXIT AREA's, measured (outlineWidth): 3.5 pixels on the disc, the UHD
// pack's 2 (16 of its 8x pixels); kOutline when it cannot be.
// Sizes here are the disc's; a pack's pictures are `scale` times as large.
const int kH = 26, kMaxW = 256;
const float kOutline = 3.5f;
// Margins: the outline's room at the ends, between letters (whose outlines
// overlap) and between the two words.
const int kPad = 4, kGap = 1, kSpace = 5;
// The file select's letters are about 14.5 pixels wide and 16 high, the
// menu's 11 by 15, centred on row 13. An M is a little wider (12.5), which
// keeps its notches open.
const float kNarrowX = 11.0f / 14.5f, kNarrowM = 12.5f / 14.5f, kNarrowY = 15.0f / 16.0f, kMiddleRow = 13.0f;
// The largest pack scale used (the UHD pack's).
const int kMaxScale = 8;

// How white each pixel of a picture is, 0 to 1: its letters' insides, the
// outline and any drop shadow (black) left out; and its alpha.
struct Ink {
	int w, h;
	float *v, *a;
	float at(int x, int y) const { return x >= 0 && x < w && y >= 0 && y < h ? v[y * w + x] : 0.0f; }
	float& operator()(int x, int y) { return v[y * w + x]; }
};

bool inkNew(Ink& ink, int w, int h)
{
	ink.w = w;
	ink.h = h;
	ink.v = (float*)calloc((size_t)w * h, sizeof(float));
	ink.a = (float*)calloc((size_t)w * h, sizeof(float));
	return ink.v && ink.a;
}

void inkFree(Ink& ink)
{
	free(ink.v);
	free(ink.a);
	ink.v = ink.a = nullptr;
}

struct Source {
	const u8* img; // IA4, GX tiles of 8 x 4
	int w, h;
};

u32 ia4Bytes(int w, int h) { return (u32)((w + 7) / 8) * ((h + 3) / 4) * 32; }
u32 ia8Bytes(int w, int h) { return (u32)((w + 3) / 4) * ((h + 3) / 4) * 32; }

u32 fnv1a(const u8* p, u32 n)
{
	u32 h = 0x811C9DC5u;
	while (n--)
		h = (h ^ *p++) * 0x01000193u;
	return h;
}

bool decodeIA4(const Source& s, Ink& ink)
{
	if (!s.img || s.w <= 0 || s.w > kMaxW || s.h <= 0 || s.h > 2 * kH || !inkNew(ink, s.w, s.h))
		return false;
	const u8* p = s.img;
	for (int by = 0; by < s.h; by += 4)
		for (int bx = 0; bx < s.w; bx += 8)
			for (int y = 0; y < 4; y++)
				for (int x = 0; x < 8; x++, p++)
					if (bx + x < s.w && by + y < s.h) {
						ink(bx + x, by + y)               = (*p & 15) * (*p >> 4) / 225.0f; // intensity x alpha
						ink.a[(by + y) * s.w + bx + x] = (*p >> 4) / 15.0f;
					}
	return true;
}

// A pack's picture of one (RGBA8, `scale` times its size): white x alpha.
bool decodePack(const Source& s, int scale, Ink& ink)
{
	int w = 0, h = 0;
	u8* rgba = GXPC_TexturePackImage ? GXPC_TexturePackImage(s.img, GX_TF_IA4, s.w, s.h, &w, &h) : nullptr;
	const bool ok = rgba && w == s.w * scale && h == s.h * scale && inkNew(ink, w, h);
	for (int i = 0; ok && i < w * h; i++) {
		const u8* c = rgba + i * 4;
		ink.v[i] = (c[0] + c[1] + c[2]) / (3.0f * 255.0f) * (c[3] / 255.0f);
		ink.a[i] = c[3] / 255.0f;
	}
	free(rgba);
	return ok;
}

// game_6's pictures are loaded with every stage (headers already native).
Source gamePicture(const char* name, const ResTIMG** header)
{
	Source s = { nullptr, 0, 0 };
	char path[64];
	snprintf(path, sizeof path, "/game_6/timg/%s.bti", name);
	const ResTIMG* t = (const ResTIMG*)JKRGetResource(path);
	if (!t || t->format != GX_TF_IA4)
		return s;
	s.img = (const u8*)t + t->imageDataOffset;
	s.w   = t->width;
	s.h   = t->height;
	if (header)
		*header = t;
	return s;
}

// The file select's (option.szs) are read from the disc, big-endian.
Source discPicture(const u8* file, unsigned size)
{
	Source s = { nullptr, 0, 0 };
	if (!file || size < 0x20 || file[0] != GX_TF_IA4)
		return s;
	const int w = file[2] << 8 | file[3], h = file[4] << 8 | file[5];
	const u32 off = (u32)file[0x1C] << 24 | file[0x1D] << 16 | file[0x1E] << 8 | file[0x1F];
	if (off < 0x20 || off + ia4Bytes(w, h) > size)
		return s;
	s.img = file + off;
	s.w   = w;
	s.h   = h;
	return s;
}

bool isUS(const Source& s, int w, u32 hash)
{
	return s.img && s.w == w && s.h == kH && fnv1a(s.img, ia4Bytes(s.w, s.h)) == hash;
}

// Columns x0 to x1 - 1 of a picture's letters put in the word at x (kept
// whiter where two overlap); narrowed ones (`narrow` across, 0 for none) are
// scaled to the menu's letters and centred on its middle row. Their width.
int place(Ink& word, int x, const Ink& src, int x0, int x1, float narrow, int scale)
{
	const int w = x1 - x0;
	if (!narrow) {
		for (int y = 0; y < word.h; y++)
			for (int i = 0; i < w && x + i < word.w; i++)
				word(x + i, y) = fmaxf(word(x + i, y), src.at(x0 + i, y));
		return w;
	}
	int top = src.h, bottom = -1;
	for (int y = 0; y < src.h; y++)
		for (int i = x0; i < x1; i++)
			if (src.at(i, y) >= 0.5f) {
				top    = y < top ? y : top;
				bottom = y;
			}
	if (bottom < 0)
		return 0;
	const float middle = (top + bottom) * 0.5f, row = kMiddleRow * scale + (scale - 1) * 0.5f;
	const int nw       = (int)lroundf(w * narrow);
	for (int y = 0; y < word.h; y++)
		for (int i = 0; i < nw && x + i < word.w; i++) {
			// bilinear, from where the pixel's centre falls in the letter
			const float fx = (i + 0.5f) / narrow - 0.5f;
			const float fy = (y - row) / kNarrowY + middle;
			const int sx = (int)floorf(fx), sy = (int)floorf(fy);
			const float ax = fx - sx, ay = fy - sy;
			float v = 0.0f;
			for (int dy = 0; dy < 2; dy++)
				for (int dx = 0; dx < 2; dx++)
					if (sx + dx >= 0 && sx + dx < w)
						v += src.at(x0 + sx + dx, sy + dy) * (dx ? ax : 1.0f - ax) * (dy ? ay : 1.0f - ay);
			word(x + i, y) = fmaxf(word(x + i, y), v);
		}
	return nw;
}

// A letter's edge at a larger scale: the column near the disc's where its
// neighbour's letters show least.
int cut(const Ink& src, int x, int scale)
{
	if (scale == 1)
		return x;
	int best = x * scale;
	float least = 1e9f;
	for (int c = x * scale - scale / 2; c <= x * scale + scale / 2; c++) {
		float sum = 0.0f;
		for (int y = 0; y < src.h; y++)
			sum += src.at(c, y);
		if (sum < least - 0.01f || (sum < least + 0.01f && abs(c - x * scale) < abs(best - x * scale))) {
			least = fminf(least, sum);
			best  = c;
		}
	}
	return best;
}

// The four US pictures' letters, at one scale.
struct Letters {
	Ink tx1, tx3, quit, mono;
	int scale;
};

// White over rows y0 to y1 - 1 and columns x0 to x1 - 1 (in the disc's
// pixels) of a letter at x in the word.
void fill(Ink& word, int s, int x, int y0, int y1, int x0, int x1)
{
	for (int y = y0 * s; y < y1 * s; y++)
		for (int i = x0 * s; i < x1 * s && x + i < word.w; i++)
			word(x + i, y) = 1.0f;
}

// QUIT GAME from them (where its letters are in the disc's pictures).
int composeQuitGame(Ink& word, const Letters& in)
{
	struct Letter {
		const Ink* src;
		int x0, x1;
		float narrow;
	};
	const Letter kQ = { &in.tx1, 14, 26, 0 }, kU = { &in.tx1, 70, 81, 0 }, kI = { &in.tx1, 50, 57, 0 },
	             kT = { &in.tx1, 38, 49, 0 }, kG = { &in.tx1, 3, 14, 0 }, kA = { &in.tx3, 51, 61, 0 },
	             kM = { &in.mono, 3, 18, kNarrowM }, kE = { &in.tx3, 74, 85, 0 };
	const Letter* const kLetters[] = { &kQ, &kU, &kI, &kT, nullptr, &kG, &kA, &kM, &kE };
	const int s = in.scale;
	int x       = kPad * s;

	for (const Letter* l : kLetters) {
		if (!l) {
			x += (kSpace - kGap) * s;
			continue;
		}
		const int x0 = cut(*l->src, l->x0, s), x1 = cut(*l->src, l->x1, s);
		int w = place(word, x, *l->src, x0, x1, l->narrow, s);
		if (l == &kG) {
			// G: CONTINUE's C with a bar in to its middle and the stem under the bar's end
			fill(word, s, x, 12, 14, 6, 11);
			fill(word, s, x, 12, 17, 8, 11);
		}
		if (l == &kQ) {
			// Q: CONTINUE's O with a tail out of its lower right, stepping down and out
			static const u8 kTail[5][2] = { { 7, 10 }, { 8, 11 }, { 9, 12 }, { 10, 13 }, { 10, 13 } };
			for (int r = 0; r < 5; r++)
				fill(word, s, x, 16 + r, 17 + r, kTail[r][0], kTail[r][1]);
			w += 2 * s;
		}
		x += w + kGap * s;
	}
	return x - kGap * s + kPad * s;
}

// The file select's QUIT, whatever its language: all its letters, narrowed.
int composeQuit(Ink& word, const Ink& quit, int scale)
{
	// a pack's upscale leaves a faint haze around the letters
	const float faint = scale > 1 ? 0.02f : 0.0f;
	int x0 = quit.w, x1 = 0;
	for (int y = 0; y < quit.h; y++)
		for (int x = 0; x < quit.w; x++)
			if (quit.at(x, y) > faint) {
				x0 = x < x0 ? x : x0;
				x1 = x + 1 > x1 ? x + 1 : x1;
			}
	if (x1 <= x0)
		return 0;
	return kPad * scale + place(word, kPad * scale, quit, x0, x1, kNarrowX, scale) + kPad * scale;
}

// Squared distances along a line to the nearest zero of f (Felzenszwalb and
// Huttenlocher's lower envelope of parabolas).
void distance1(const float* f, float* d, int n, int* v, float* z)
{
	int k = 0;
	v[0]  = 0;
	z[0]  = -1e20f;
	z[1]  = 1e20f;
	for (int q = 1; q < n; q++) {
		float s;
		while ((s = ((f[q] + q * q) - (f[v[k]] + v[k] * v[k])) / (2.0f * (q - v[k]))) <= z[k])
			k--;
		k++;
		v[k]     = q;
		z[k]     = s;
		z[k + 1] = 1e20f;
	}
	k = 0;
	for (int q = 0; q < n; q++) {
		while (z[k + 1] < q)
			k++;
		d[q] = (q - v[k]) * (q - v[k]) + f[v[k]];
	}
}

// Each pixel's distance to the nearest inside a letter; null if out of memory.
float* distances(const Ink& word)
{
	const int n = word.w > word.h ? word.w : word.h;
	float* d    = (float*)malloc(sizeof(float) * word.w * word.h);
	float* f    = (float*)malloc(sizeof(float) * n * 2);
	float* z    = (float*)malloc(sizeof(float) * (n + 1));
	int* v      = (int*)malloc(sizeof(int) * n);
	if (d && f && z && v) {
		for (int x = 0; x < word.w; x++) {
			for (int y = 0; y < word.h; y++)
				f[y] = word.at(x, y) >= 0.5f ? 0.0f : 1e20f;
			distance1(f, f + n, word.h, v, z);
			for (int y = 0; y < word.h; y++)
				d[y * word.w + x] = f[n + y];
		}
		for (int y = 0; y < word.h; y++) {
			distance1(d + y * word.w, f, word.w, v, z);
			for (int x = 0; x < word.w; x++)
				d[y * word.w + x] = sqrtf(f[x]);
		}
	} else {
		free(d);
		d = nullptr;
	}
	free(f);
	free(z);
	free(v);
	return d;
}

// A picture's outline width: the one that draws its alpha back best around its
// letters (to an eighth of a pixel at the disc's size); kOutline x scale when
// it has none to measure.
float outlineWidth(const Ink& ink, int scale)
{
	float* dist = ink.v ? distances(ink) : nullptr;
	if (!dist)
		return kOutline * scale;
	float best = kOutline * scale, least = 1e30f;
	for (float r = 1.0f * scale; r <= 6.0f * scale; r += scale / 8.0f) {
		float err = 0.0f;
		for (int i = 0; i < ink.w * ink.h; i++)
			err += fabsf(fmaxf(ink.v[i], fminf(1.0f, fmaxf(0.0f, r + 0.5f - dist[i]))) - ink.a[i]);
		if (err < least) {
			least = err;
			best  = r;
		}
	}
	free(dist);
	return best;
}

// The word's letters white in a black outline (anti-aliased), as a picture
// with a native ResTIMG header like the menu's own: IA4 at the disc's size,
// or IA8 with mipmaps down to the disc's size at a pack's.
const ResTIMG* render(const Ink& word, int scale, float outline, const ResTIMG& like)
{
	float* dist = distances(word);
	if (!dist)
		return nullptr;
	// level 0's intensity and alpha
	const int w = word.w, h = word.h;
	float* ia   = (float*)malloc(sizeof(float) * w * h * 2);
	if (!ia) {
		free(dist);
		return nullptr;
	}
	const float reach = outline + 0.5f;
	for (int i = 0; i < w * h; i++) {
		const float white = word.v[i], outline = fminf(1.0f, fmaxf(0.0f, reach - dist[i]));
		ia[i * 2]         = white;
		ia[i * 2 + 1]     = fmaxf(white, outline);
	}
	free(dist);

	int levels = 1;
	while ((1 << (levels - 1)) < scale)
		levels++;
	u32 bytes = 0;
	for (int l = 0; l < levels; l++)
		bytes += scale == 1 ? ia4Bytes(w, h) : ia8Bytes(w >> l > 0 ? w >> l : 1, h >> l > 0 ? h >> l : 1);
	u8* buf = (u8*)port_low_alloc(0x20 + bytes);
	if (!buf) {
		free(ia);
		return nullptr;
	}
	memcpy(buf, &like, 0x20);
	ResTIMG* t         = (ResTIMG*)buf;
	t->format          = scale == 1 ? GX_TF_IA4 : GX_TF_IA8;
	t->width           = (u16)w;
	t->height          = (u16)h;
	t->imageDataOffset = 0x20;
	if (levels > 1) {
		t->mipmapEnabled = 1;
		t->mipmapCount   = (u8)levels;
		t->minFilter     = GX_LIN_MIP_LIN;
		t->magFilter     = GX_LINEAR;
		t->minLod        = 0;
		t->maxLod        = (s8)((levels - 1) * 8);
	}

	u8* p  = buf + 0x20;
	int lw = w, lh = h;
	for (int l = 0; l < levels; l++) {
		if (l > 0) {
			// a 2 x 2 box down, the white weighted by alpha
			const int nw = lw / 2 > 0 ? lw / 2 : 1, nh = lh / 2 > 0 ? lh / 2 : 1;
			for (int y = 0; y < nh; y++)
				for (int x = 0; x < nw; x++) {
					float wa = 0.0f, a = 0.0f;
					for (int dy = 0; dy < 2; dy++)
						for (int dx = 0; dx < 2; dx++) {
							const int sx = x * 2 + dx < lw ? x * 2 + dx : lw - 1, sy = y * 2 + dy < lh ? y * 2 + dy : lh - 1;
							wa += ia[(sy * lw + sx) * 2] * ia[(sy * lw + sx) * 2 + 1];
							a += ia[(sy * lw + sx) * 2 + 1];
						}
					ia[(y * nw + x) * 2]     = a > 0.0f ? wa / a : 0.0f;
					ia[(y * nw + x) * 2 + 1] = a / 4.0f;
				}
			lw = nw;
			lh = nh;
		}
		const int tw = scale == 1 ? 8 : 4;
		for (int by = 0; by < lh; by += 4)
			for (int bx = 0; bx < lw; bx += tw)
				for (int y = 0; y < 4; y++)
					for (int x = 0; x < tw; x++) {
						const int px = bx + x, py = by + y;
						const bool in = px < lw && py < lh;
						const float i = in ? ia[(py * lw + px) * 2] : 0.0f, a = in ? ia[(py * lw + px) * 2 + 1] : 0.0f;
						if (scale == 1) {
							*p++ = (u8)((int)(a * 15.0f + 0.5f) << 4 | (int)(i * 15.0f + 0.5f));
						} else {
							*p++ = (u8)(a * 255.0f + 0.5f);
							*p++ = (u8)(i * 255.0f + 0.5f);
						}
					}
	}
	free(ia);
	return t;
}

struct Art {
	bool tried;
	const ResTIMG* timg;
	int layoutW;
};
Art sArt[2]; // at the disc's size, and at a pack's

const ResTIMG* make(bool pack, int* layoutW)
{
	const ResTIMG* like = nullptr;
	const Source tx1 = gamePicture("pause_tx_1", nullptr), tx3 = gamePicture("pause_tx_3", &like);
	unsigned quitSize = 0, monoSize = 0;
	u8* quitFile = (u8*)port_disc_archive_file("/data/option.szs", "select_quit.bti", &quitSize);
	u8* monoFile = (u8*)port_disc_archive_file("/data/option.szs", "select_mono.bti", &monoSize);
	const Source quit = discPicture(quitFile, quitSize), mono = discPicture(monoFile, monoSize);
	const bool us = isUS(tx1, 96, 0x64F46869u) && isUS(tx3, 100, 0x7BAC9E8Bu) && isUS(quit, 68, 0x932BE423u)
	                && isUS(mono, 72, 0xC51053BEu);

	Letters in;
	memset(&in, 0, sizeof in);
	in.scale = 1;
	bool ok  = false;
	if (pack) {
		// the pack's pictures, all at the same scale as its EXIT AREA's (in
		// another language, only its QUIT, and EXIT AREA for the outline)
		int w = 0, h = 0;
		u8* probe = GXPC_TexturePackImage ? GXPC_TexturePackImage(tx3.img, GX_TF_IA4, tx3.w, tx3.h, &w, &h) : nullptr;
		free(probe);
		in.scale = w % tx3.w == 0 && h == tx3.h * (w / tx3.w) ? w / tx3.w : 0;
		ok = in.scale >= 2 && in.scale <= kMaxScale && decodePack(tx3, in.scale, in.tx3)
		     && decodePack(quit, in.scale, in.quit)
		     && (!us || (decodePack(tx1, in.scale, in.tx1) && decodePack(mono, in.scale, in.mono)));
	} else if (us) {
		ok = decodeIA4(tx1, in.tx1) && decodeIA4(tx3, in.tx3) && decodeIA4(quit, in.quit) && decodeIA4(mono, in.mono);
	} else {
		ok = decodeIA4(quit, in.quit);
		decodeIA4(tx3, in.tx3); // only for its outline
	}

	const ResTIMG* art = nullptr;
	Ink word;
	int w         = 0;
	float outline = 0.0f;
	if (ok && like && inkNew(word, kMaxW * in.scale, kH * in.scale)) {
		w = us ? composeQuitGame(word, in) : composeQuit(word, in.quit, in.scale);
		if (w > 0 && w <= word.w) {
			// as wide as the word
			Ink tight;
			if (inkNew(tight, w, word.h)) {
				for (int y = 0; y < word.h; y++)
					memcpy(tight.v + y * w, word.v + y * word.w, sizeof(float) * w);
				outline = outlineWidth(in.tx3, in.scale);
				art     = render(tight, in.scale, outline, *like);
				inkFree(tight);
			}
		}
		inkFree(word);
	}
	inkFree(in.tx1);
	inkFree(in.tx3);
	inkFree(in.quit);
	inkFree(in.mono);
	free(quitFile);
	free(monoFile);
	*layoutW = art ? w / in.scale : 0;
	OSReport("[pause] QUIT GAME item: %s, %d x %d, outline %.2f%s\n",
	         !art             ? "no picture"
	         : us             ? "QUIT GAME"
	                          : "the file select's QUIT",
	         w, kH * in.scale, outline, art && in.scale > 1 ? " (from the texture pack's)" : "");
	return art;
}

// The texture packs (when on) have high-resolution pictures of EXIT AREA.
bool packHasLetters()
{
	const Source tx3 = gamePicture("pause_tx_3", nullptr);
	return tx3.img && GXPC_TexturePackHas && GXPC_TexturePackHas(tx3.img, GX_TF_IA4, tx3.w, tx3.h);
}

} // namespace

const ResTIMG* PauseQuitArt(int* layoutWidth)
{
	// the pack's when it has the letters (and made from them), else the disc's
	bool pack = packHasLetters();
	for (int i = 0; i < 2; i++, pack = false) {
		Art& a = sArt[pack ? 1 : 0];
		if (!a.tried) {
			a.tried = true;
			a.timg  = make(pack, &a.layoutW);
		}
		if (a.timg) {
			*layoutWidth = a.layoutW;
			return a.timg;
		}
		if (!pack)
			break;
	}
	return nullptr;
}

void PauseQuitRequest()
{
	OSReport("[pause] QUIT GAME picked\n");
	if (!(GXPC_RequestQuit && GXPC_RequestQuit()))
		exit(0);
}
