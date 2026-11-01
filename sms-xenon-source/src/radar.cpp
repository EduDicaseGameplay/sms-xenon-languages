// Port: the radar (SMS_RADAR=1, radar = on in settings.txt; off by default).
// A round map at the HUD's top right of the stage close around Mario, north
// up: the map stays still and scrolls under the radar's border as he moves,
// an arrow at its centre turns the way he faces, and the stage's Shines and
// blue coins are on it as the HUD's own sprites.
//
// The map is a picture of the stage from straight above (tools/radar/
// gen_maps.py makes one per stage from the disc, assets/maps, and from a
// mod's disc a pack in mods/maps, Super Mario Eclipse's installed with it;
// sms_gx loads it, platform/gx/src/gx_radar_maps.cpp), found by a hash of the
// stage's loaded collision, so a stage without one has no radar. GX
// textures are at most 1024 square and the pictures are larger, so the
// radar shows a window of the picture around Mario (Window): the map is a
// round fan whose texture coordinates follow him, so the GPU scrolls and
// filters it, and the CPU only makes a new window now and then. Over
// it go the blue coins and the Shines (a Shine got is the pale blue of the
// game's empty Shine), cut at the map's round edge so they scroll in from
// past it rather than appear; then the border (made once: white, with a dark
// edge and a soft shadow, smooth edges in the alpha), and Mario's arrow, all
// on top of the map whatever their height. In multiplayer the other players
// are smaller white arrows (ext/multiplayer's mp_radar_player); one beyond
// the radar's reach waits at its edge, the way to them.
//
// The HUD draws it last (patch zzzz-radar-01) while a stage plays (not in
// cutscenes, conversations or the pause menu), fading in and out, and with
// SMS_WIDESCREEN_HUD=edges it moves to the right edge with the HUD's pieces.
// The life meter comes down at the same top right when Mario is hurt or under
// water; the HUD gives the radar its place each frame, and SMS_RADAR_PLACE
// says how the radar makes room for it (Place).
//
// D-pad up steps the radar's zoom through kZoomSteps, with the menus' cursor
// sound and its reach shown under it for a moment. MAP (M, or a controller's
// Back; platform/pad) opens the full map: the whole stage's picture over the
// dimmed game, as wide as the frame, with the area's name and the same
// sprites and arrows (the other players' names under theirs), to the game's
// own map screen sounds (the wipe in and out of TGuide).
// The game goes on under it, so Mario can be steered with it open.
#include <Enemy/Conductor.hpp>
#include <GC2D/MessageUtil.hpp>
#include <JSystem/J2D/J2DOrthoGraph.hpp>
#include <JSystem/JDrama/JDRRect.hpp>
#include <JSystem/JKernel/JKRFileLoader.hpp>
#include <JSystem/JUtility/JUTResFont.hpp>
#include <JSystem/JUtility/JUTTexture.hpp>
#include <MSound/MSound.hpp>
#include <MSound/MSoundSE.hpp>
#include <MoveBG/MapObjBase.hpp>
#include <Player/MarioAccess.hpp>
#include <Strategic/LiveActor.hpp>
#include <Strategic/LiveManager.hpp>
#include <System/FlagManager.hpp>
#include <System/Application.hpp>
#include <System/MarDirector.hpp>
#include <System/MarioGamePad.hpp>
#include <System/StageUtil.hpp>
#include <dolphin/pad.h>
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port_framerate.h"
#include "port_radar.h"
#include "sms_modhook.h"

extern "C" void* port_low_alloc(unsigned long size);
extern "C" void GXPC_HudPaneBegin(float x1, float x2);
extern "C" void GXPC_HudPaneEnd(void);
extern "C" void GXPC_SetWide2D(int on);
extern "C" float GXPC_GetWidescreen(void);
// MAP's presses (platform/pad/pad.cpp)
extern "C" int port_radar_map_presses;
// another player's place, facing and name (ext/multiplayer/game/mp_game.cpp)
extern "C" int mp_radar_player(int slot, float* xz, float* facing, char* name, int nameSize) __attribute__((weak));

namespace {

// the map's and the border's textures: kTex square; the map fills radius
// kMapR of its kTex / 2, the border runs from there to kRingR, its dark edge to kEdgeR, and the shadow past it
const int kTex      = 320;
const f32 kMapR     = 143.5f;
const f32 kRingR    = 153.0f;
const f32 kEdgeR    = 155.0f;
const f32 kShadowR  = 160.0f;
const u8 kBack[3]   = { 51, 61, 77 }; // past the picture's edge: its own backdrop colour

const f32 kRadius   = 60.0f; // the texture's radius in the HUD's 2D units (its frame is 600 wide)
const f32 kMargin   = 22.0f; // from the frame's right and top edges

// the sprites' sizes, as parts of the radar's radius
const f32 kShineH   = 0.27f;
const f32 kBlueH    = 0.19f;
const f32 kArrowLen = 0.27f; // tip to back, before its outline
const f32 kOtherLen = 0.72f; // another player's arrow, as a part of Mario's
const u32 kOtherFill = 0xFFFFFFFF, kOtherOutline = 0x1F2433FF; // white around the border's dark edge

const u32 kBlueCoinType = 0x20000010, kShineType = 0x20000013; // MapObjInit's coin_blue, shine

// D-pad up's zoom steps, percent of SMS_RADAR_ZOOM's reach (more is closer)
const int kZoomSteps[] = { 100, 50, 25, 200 };
const int kZoomCount   = sizeof kZoomSteps / sizeof kZoomSteps[0];

f32 sZoomBase = 2500.0f; // SMS_RADAR_ZOOM: world units from the centre to the map's edge at 100%
f32 sZoom     = 2500.0f; // the same, at the zoom step chosen
int sZoomStep;
f32 sZoomShown; // 60 fps frames left to show the zoom under the radar
f32 sRadius   = kRadius;

// SMS_RADAR_PLACE (radar_place): how the radar and the life meter share the
// top right
enum Place {
	kCorner,     // corner: the top right, under the meter when it comes
	kYield,      // yield: the top right, pushed down under the meter while it is there
	kBeside,     // beside: the top right, a little smaller to the meter's left while it is there
	kBelow,      // below: always under the meter's place, so neither moves
	kBottomLeft, // bottom_left: the bottom left, across from the FLUDD gauge
};
Place sPlace = kYield;
// the life meter's place at rest (its back, l_ba, in the HUD's 2D units)
const f32 kLifeX1 = 500.0f, kLifeY1 = 50.0f, kLifeX2 = 574.0f, kLifeY2 = 121.0f;
const f32 kLifeRays    = 1.2f;  // its rays reach past its back: its round width, as a part of the back's
const f32 kPlaceGap    = 6.0f;  // between the radar and the meter
const f32 kBesideScale = 0.8f;  // the radar's size beside the meter
const f32 kPlaceEase  = 0.25f; // the part of the way to its place it goes each 60 fps frame
f32 sPlaceX, sPlaceY, sPlaceR; // where it is now: its centre and radius, 0 before it is placed

// the full map: open, how far faded in, and the overview in GX textures
bool sFullOpen;
f32 sFullAlpha;
int sPresses;
const int kTile = 1024; // a GX texture's most
struct Tile {
	int x, y, w, h, pw, ph; // its place in the overview, its size, padded to 4
	size_t offset;          // in sOverviewTex
};
Tile sTiles[4];
int sTileCount;
u8* sOverviewTex; // GX RGBA8 tiles, below 4 GiB
size_t sOverviewBytes;
unsigned long long sOverviewKey;

// the stage: its collision, the key naming its map, and the map
const void* sCol;
u32 sColProbe[4];
const void* sDirector;
unsigned long long sKey;
int sMapState = -1; // 1 the map is here, 0 it is loading, -1 none
PortRadarMap sMap;

u8* sBorder; // the border over the map: GX RGBA8, kTex x kTex, below 4 GiB

// The map's window: a kWin-square crop of the picture around Mario, a GX
// RGBA8 texture. At level L a texel of it is 2^L x 2^L of the picture's
// pixels (four of them averaged), the level at which the radar's reach is
// about half the window. Two of them: the one shown, and the next, made a
// band of rows a frame (kBandRows) once Mario is halfway to where the shown
// one runs out, so a new one costs no frame much.
const int kWin = 512, kBandRows = 64;
struct Window {
	u8* tex;                 // below 4 GiB
	int ox, oy, level;       // its top left in the picture's pixels, and its level
	unsigned long long key;  // the map's (sKey) it was made from
};
Window sWin[2];
int sShown = -1;          // the window drawn
int sNext = -1, sNextRow; // the one being made, and its rows made so far
bool sWinStale = true;    // the map changed: make one at once
f32 sAlpha; // fades in and out, 0..1

JUTTexture sShineArt, sBlueCoinArt;
bool sHaveArt;

// --- the stage and its map ---------------------------------------------------

inline void feed(unsigned long long& h, u32 v, int bytes)
{
	for (int i = 0; i < bytes; i++) {
		h ^= (v >> (8 * i)) & 0xFF;
		h *= 0x100000001b3ULL;
	}
}

inline u32 swap32(u32 v) { return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24); }
inline u16 swap16(u16 v) { return (u16)((v >> 8) | (v << 8)); }

// A value of the .col: in the host's order once the port has converted it
// (platform/endian), or still in the disc's (big-endian) order.
struct ColReader {
	const u8* b;
	u32 size;
	bool swap;
	u32 w32(u32 o) const
	{
		u32 v;
		memcpy(&v, b + o, 4);
		return swap ? swap32(v) : v;
	}
	u16 w16(u32 o) const
	{
		u16 v;
		memcpy(&v, b + o, 2);
		return swap ? swap16(v) : v;
	}
	// the header fits: vertex and group arrays inside the file
	bool fits() const
	{
		const u32 nv = w32(0), vo = w32(4), ng = w32(8), go = w32(12);
		return nv < 0x100000 && ng < 0x10000 && vo <= size && nv * 12 <= size - vo && go <= size
		       && ng * 0x18 <= size - go;
	}
};

// FNV-1a over the collision's values in little-endian order, as gen_maps.py's
// col_key: the vertex count and vertices, the group count, and each group's
// surface type, triangle count and vertex indices. 0 if it is not a .col
// this can read. The game's own copy is in the host's order (platform/
// endian) and its groups' index offsets are addresses by the time it plays.
unsigned long long colKey(const void* col, long size)
{
	if (size < 16)
		return 0;
	ColReader c = { (const u8*)col, (u32)size, false };
	if (!c.fits()) {
		c.swap = true;
		if (!c.fits())
			return 0;
	}
	const u32 nv = c.w32(0), vo = c.w32(4), ng = c.w32(8), go = c.w32(12);
	unsigned long long h = 0xcbf29ce484222325ULL;
	feed(h, nv, 4);
	for (u32 i = 0; i < nv * 3; i++)
		feed(h, c.w32(vo + i * 4), 4);
	feed(h, ng, 4);
	for (u32 g = 0; g < ng; g++) {
		const u32 e    = go + g * 0x18;
		const u16 type = c.w16(e);
		const s16 tris = (s16)c.w16(e + 2);
		u32 idx        = c.w32(e + 8);
		feed(h, type, 2);
		feed(h, (u16)tris, 2);
		if (tris <= 0)
			continue;
		// once the stage is set up, the game has made this offset an address
		// in the file (TMapCollisionData)
		const u32 base = (u32)(uintptr_t)col;
		if (idx >= base && idx - base < c.size)
			idx -= base;
		if (idx > c.size || (u32)tris * 6 > c.size - idx)
			return 0;
		for (int i = 0; i < tris * 3; i++)
			feed(h, c.w16(idx + i * 2), 2);
	}
	return h;
}

bool loadArt(JUTTexture& tex, const char* name)
{
	char path[64];
	snprintf(path, sizeof path, "/game_6/timg/%s.bti", name);
	const ResTIMG* timg = (const ResTIMG*)JKRGetResource(path);
	if (!timg)
		return false;
	tex.storeTIMG(timg);
	return true;
}

// Which stage this is, its map and the HUD's sprites; false without a map.
bool stage()
{
	const void* col = JKRGetResource("/scene/map/map.col");
	if (!col)
		return false;
	// a new stage can reuse the old one's memory: its first values tell
	u32 probe[4];
	memcpy(probe, col, sizeof probe);
	if (col != sCol || gpMarDirector != sDirector || memcmp(probe, sColProbe, sizeof probe)) {
		sCol      = col;
		sDirector = gpMarDirector;
		memcpy(sColProbe, probe, sizeof probe);
		sKey      = colKey(col, JKRFileLoader::getResSize((void*)col, nullptr));
		sMapState = sKey ? 0 : -1;
		sFullOpen = false;
		sFullAlpha = 0.0f;
		sWinStale = true;
		sHaveArt  = loadArt(sShineArt, "shine_icon") && loadArt(sBlueCoinArt, "bluecoin_icon");
	}
	if (sMapState == 0) {
		sMapState = port_radar_map(sKey, &sMap);
		if (sMapState == 1)
			sWinStale = true;
	}
	return sMapState == 1 && sHaveArt;
}

bool playing()
{
	return gpMarDirector && gpMarDirector->mState == TMarDirector::STATE_UNK4
	       && !gpMarDirector->isTalkOrDemoModeNow() && gpMarioPos;
}

// --- the disc's texture ----------------------------------------------------------

inline const u8* texel(int x, int y)
{
	if (x < 0 || y < 0 || x >= sMap.w || y >= sMap.h)
		return kBack;
	return sMap.rgba + ((size_t)y * sMap.w + x) * 4;
}

inline f32 clamp01(f32 v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

// "over": colour c at alpha a onto (rgb, alpha)
inline void over(f32* rgb, f32& alpha, f32 r, f32 g, f32 b, f32 a)
{
	const f32 out = a + alpha * (1.0f - a);
	if (out <= 0.0f)
		return;
	rgb[0] = (r * a + rgb[0] * alpha * (1.0f - a)) / out;
	rgb[1] = (g * a + rgb[1] * alpha * (1.0f - a)) / out;
	rgb[2] = (b * a + rgb[2] * alpha * (1.0f - a)) / out;
	alpha  = out;
}

// Writes the 4x4 block px (RGBA) to a GX RGBA8 texture at o: AR, then GB.
inline void putBlock(u8* tex, size_t& o, const u8 (*px)[4])
{
	for (int i = 0; i < 16; i++) {
		tex[o++] = px[i][3];
		tex[o++] = px[i][0];
	}
	for (int i = 0; i < 16; i++) {
		tex[o++] = px[i][1];
		tex[o++] = px[i][2];
	}
}

// The window's level for the radar's reach: its width, in the picture's
// pixels, at most about half the window's.
int windowLevel()
{
	const f32 reach = 2.0f * sZoom * (kMapR + 3.0f) / kMapR * sMap.density;
	int level = 0;
	while (level < 6 && reach / (1 << level) > kWin * 0.55f)
		level++;
	return level;
}

// Rows y0 to y1 (multiples of 4) of a window: each texel the average of four
// of the picture's pixels across its 2^level square (all of them at level 1).
void windowRows(const Window& w, int y0, int y1)
{
	const int s = 1 << w.level, a = s / 4, b = 3 * s / 4;
	size_t o = (size_t)(y0 / 4) * (kWin / 4) * 64;
	for (int by = y0; by < y1; by += 4) {
		for (int bx = 0; bx < kWin; bx += 4) {
			u8 px[16][4];
			for (int i = 0; i < 16; i++) {
				const int x = w.ox + (bx + (i & 3)) * s, y = w.oy + (by + (i >> 2)) * s;
				const u8 *p = texel(x + a, y + a), *q = texel(x + b, y + a), *r = texel(x + a, y + b),
				         *e = texel(x + b, y + b);
				for (int k = 0; k < 3; k++)
					px[i][k] = (u8)((p[k] + q[k] + r[k] + e[k] + 2) >> 2);
				px[i][3] = 255;
			}
			putBlock(w.tex, o, px);
		}
	}
}

// A window centred on the picture's pixel (px, pz), not made yet.
void windowStart(Window& w, f32 px, f32 pz, int level)
{
	const int s = 1 << level;
	w.ox    = (int)floorf(px) - kWin / 2 * s;
	w.oy    = (int)floorf(pz) - kWin / 2 * s;
	w.level = level;
	w.key   = sKey;
}

// How far the picture's pixel (px, pz) is off a window's centre, in its
// texels (along x or z, the farther); far for a window of another map or level.
f32 offCentre(const Window& w, f32 px, f32 pz, int level)
{
	if (w.key != sKey || w.level != level)
		return 1e30f;
	const f32 s = (f32)(1 << level);
	return fmaxf(fabsf((px - w.ox) / s - kWin / 2), fabsf((pz - w.oy) / s - kWin / 2));
}

// Keeps a window under the radar around Mario at (mx, mz): the next one is
// made a band a frame while he is past halfway to the shown one's edge, and
// shown when done; one is made at once when none will do (a new stage, a
// zoom step to another level, a warp).
void updateWindow(f32 mx, f32 mz)
{
	const int level = windowLevel(), s = 1 << level;
	const f32 px = (mx - sMap.x0) * sMap.density, pz = (mz - sMap.z0) * sMap.density;
	// how far it may be off the window's centre, in its texels, for the radar to fit
	const f32 room = kWin / 2 - sZoom * (kMapR + 3.0f) / kMapR * sMap.density / s - 2.0f;
	if (sWinStale || sShown < 0 || offCentre(sWin[sShown], px, pz, level) > room) {
		sWinStale = false;
		sShown    = sShown < 0 ? 0 : sShown ^ 1;
		sNext     = -1;
		windowStart(sWin[sShown], px, pz, level);
		windowRows(sWin[sShown], 0, kWin);
		DCFlushRange(sWin[sShown].tex, kWin * kWin * 4); // the texture cache sees the new picture
		return;
	}
	if (sNext < 0) {
		if (offCentre(sWin[sShown], px, pz, level) > room * 0.5f) {
			sNext    = sShown ^ 1;
			sNextRow = 0;
			windowStart(sWin[sNext], px, pz, level);
		}
		return;
	}
	const int y1 = sNextRow + kBandRows < kWin ? sNextRow + kBandRows : kWin;
	windowRows(sWin[sNext], sNextRow, y1);
	sNextRow = y1;
	if (sNextRow == kWin) {
		DCFlushRange(sWin[sNext].tex, kWin * kWin * 4);
		if (offCentre(sWin[sNext], px, pz, level) <= room)
			sShown = sNext;
		sNext = -1;
	}
}

// The border, once, into sBorder: the ring, lighter at the top, its dark
// edge and a soft shadow a little low, clear inside kMapR. It goes over the
// map and the sprites, so those slide in under it.
void composeBorder()
{
	const f32 half = kTex * 0.5f;
	size_t o       = 0;
	for (int by = 0; by < kTex; by += 4) {
		for (int bx = 0; bx < kTex; bx += 4) {
			u8 px[16][4];
			for (int i = 0; i < 16; i++) {
				const f32 sx = bx + (i & 3) + 0.5f - half, sy = by + (i >> 2) + 0.5f - half;
				const f32 r = sqrtf(sx * sx + sy * sy);
				f32 rgb[3]  = { 0, 0, 0 }, alpha = 0;
				const f32 rs = sqrtf(sx * sx + (sy - 3.0f) * (sy - 3.0f));
				if (rs > kMapR)
					over(rgb, alpha, 0, 0, 0, 0.35f * clamp01((kShadowR - rs) / (kShadowR - kEdgeR + 2.0f)));
				over(rgb, alpha, 0.12f, 0.14f, 0.2f, clamp01(kEdgeR + 0.5f - r));
				const f32 light = 1.0f - 0.16f * clamp01((sy / kRingR + 1.0f) * 0.5f);
				over(rgb, alpha, light, light, light * 1.02f, clamp01(kRingR + 0.5f - r));
				alpha *= 1.0f - clamp01(kMapR + 0.5f - r); // the hole the map shows through
				for (int k = 0; k < 3; k++)
					px[i][k] = (u8)(clamp01(rgb[k]) * 255.0f + 0.5f);
				px[i][3] = (u8)(clamp01(alpha) * 255.0f + 0.5f);
			}
			putBlock(sBorder, o, px);
		}
	}
	DCFlushRange(sBorder, kTex * kTex * 4);
}

// --- drawing -----------------------------------------------------------------------

void setupTextured()
{
	GXSetNumChans(1);
	GXSetNumTevStages(1);
	GXSetNumTexGens(1);
	GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
	GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	GXClearVtxDesc();
	GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
	GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
}

void setupColour()
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

// A w x h quad of the texture in GX_TEXMAP0, centred on (x, y).
void quad(f32 x, f32 y, f32 w, f32 h, u32 colour, f32 alpha)
{
	static const f32 kU[4] = { 0, 1, 1, 0 }, kV[4] = { 0, 0, 1, 1 };
	const u8 a = (u8)((colour & 0xFF) * alpha);
	setupTextured();
	GXBegin(GX_QUADS, GX_VTXFMT0, 4);
	for (int i = 0; i < 4; i++) {
		GXPosition3f32(x + (kU[i] - 0.5f) * w, y + (kV[i] - 0.5f) * h, 0.0f);
		GXColor4u8(colour >> 24, colour >> 16, colour >> 8, a);
		GXTexCoord2f32(kU[i], kV[i]);
	}
	GXEnd();
}

const int kSides = 64; // the round map's and its shade's

// The map: the shown window as a round fan of radius r around (cx, cy), each
// corner's texture coordinates where its place in the world falls in the
// window (mx, mz at the centre, `scale` 2D units per world unit).
void mapFan(f32 cx, f32 cy, f32 r, f32 mx, f32 mz, f32 scale, f32 alpha)
{
	const Window& w = sWin[sShown];
	const f32 perU = sMap.density / ((f32)(1 << w.level) * kWin); // texture coordinate per world unit
	GXTexObj tex;
	GXInitTexObj(&tex, w.tex, kWin, kWin, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GXInitTexObjLOD(&tex, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
	GXLoadTexObj(&tex, GX_TEXMAP0);
	setupTextured();
	const f32 u0 = (mx - sMap.x0) * perU - (f32)w.ox / ((1 << w.level) * kWin);
	const f32 v0 = (mz - sMap.z0) * perU - (f32)w.oy / ((1 << w.level) * kWin);
	const u8 a = (u8)(255.0f * alpha);
	GXBegin(GX_TRIANGLEFAN, GX_VTXFMT0, kSides + 2);
	for (int i = -1; i <= kSides; i++) {
		const f32 ang = i * (6.2831853f / kSides);
		const f32 dx = i < 0 ? 0.0f : r * cosf(ang), dy = i < 0 ? 0.0f : r * sinf(ang);
		GXPosition3f32(cx + dx, cy + dy, 0.0f);
		GXColor4u8(255, 255, 255, a);
		GXTexCoord2f32(u0 + dx / scale * perU, v0 + dy / scale * perU);
	}
	GXEnd();
}

// A faint shade just inside the border, so the map sits in it: black, from
// nothing 12 texels inside kMapR to 0.22 there and on under the border (k:
// 2D units per texel of the border's texture).
void mapShade(f32 cx, f32 cy, f32 k, f32 alpha)
{
	static const f32 kR[3] = { kMapR - 12.0f, kMapR, kMapR + 2.5f }, kA[3] = { 0.0f, 0.22f, 0.22f };
	setupColour();
	for (int band = 0; band < 2; band++) {
		GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, 2 * (kSides + 1));
		for (int i = 0; i <= kSides; i++) {
			const f32 ang = i * (6.2831853f / kSides), c = cosf(ang), sn = sinf(ang);
			for (int j = band; j <= band + 1; j++) {
				GXPosition3f32(cx + kR[j] * k * c, cy + kR[j] * k * sn, 0.0f);
				GXColor4u8(0, 0, 0, (u8)(255.0f * kA[j] * alpha + 0.5f));
			}
		}
		GXEnd();
	}
}

// A sprite h tall at its own proportions, centred on (x, y).
void sprite(JUTTexture& tex, f32 x, f32 y, f32 h, u32 colour, f32 alpha)
{
	const ResTIMG* t = tex.getTexInfo();
	tex.load(GX_TEXMAP0);
	quad(x, y, t ? h * t->width / t->height : h, h, colour, alpha);
}

// The same, but only its part within clipR of (cx, cy): a sprite at the
// radar's edge is cut by its round border rather than left out, so it slides
// in under the border as the map scrolls.
void clippedSprite(JUTTexture& tex, f32 x, f32 y, f32 h, u32 colour, f32 alpha, f32 cx, f32 cy, f32 clipR)
{
	const ResTIMG* t = tex.getTexInfo();
	const f32 w = t ? h * t->width / t->height : h;
	const f32 dx = x - cx, dy = y - cy, dist = sqrtf(dx * dx + dy * dy);
	const f32 corner = 0.5f * sqrtf(w * w + h * h); // its centre to a corner
	if (dist >= clipR + corner)
		return; // wholly outside
	// the quad as (x, y, u, v), cut by each side of a 64-gon just outside
	// the circle (Sutherland-Hodgman); at most 4 + 64 corners
	const int kSides = 64;
	static f32 a[4 * 70], b[4 * 70];
	f32 *in = a, *out = b;
	static const f32 kU[4] = { 0, 1, 1, 0 }, kV[4] = { 0, 0, 1, 1 };
	int n = 4;
	for (int i = 0; i < 4; i++) {
		in[i * 4 + 0] = x + (kU[i] - 0.5f) * w;
		in[i * 4 + 1] = y + (kV[i] - 0.5f) * h;
		in[i * 4 + 2] = kU[i];
		in[i * 4 + 3] = kV[i];
	}
	if (dist + corner > clipR) { // not wholly inside
		for (int s = 0; s < kSides && n >= 3; s++) {
			const f32 ang = s * (6.2831853f / kSides), nx = cosf(ang), ny = sinf(ang);
			// inside the side touching the circle at angle ang: (p - c) . n <= clipR
			int m = 0;
			for (int i = 0; i < n; i++) {
				const f32* p = in + i * 4;
				const f32* q = in + ((i + 1) % n) * 4;
				const f32 fp = (p[0] - cx) * nx + (p[1] - cy) * ny - clipR;
				const f32 fq = (q[0] - cx) * nx + (q[1] - cy) * ny - clipR;
				if (fp <= 0.0f) {
					for (int k = 0; k < 4; k++)
						out[m * 4 + k] = p[k];
					m++;
				}
				if ((fp <= 0.0f) != (fq <= 0.0f)) {
					const f32 tt = fp / (fp - fq);
					for (int k = 0; k < 4; k++)
						out[m * 4 + k] = p[k] + (q[k] - p[k]) * tt;
					m++;
				}
			}
			f32* sw = in;
			in      = out;
			out     = sw;
			n       = m;
		}
	}
	if (n < 3)
		return;
	const u8 al = (u8)((colour & 0xFF) * alpha);
	setupTextured();
	tex.load(GX_TEXMAP0);
	GXBegin(GX_TRIANGLEFAN, GX_VTXFMT0, (u16)n);
	for (int i = 0; i < n; i++) {
		GXPosition3f32(in[i * 4], in[i * 4 + 1], 0.0f);
		GXColor4u8(colour >> 24, colour >> 16, colour >> 8, al);
		GXTexCoord2f32(in[i * 4 + 2], in[i * 4 + 3]);
	}
	GXEnd();
}

// An arrow at (x, y) pointing along (dx, dy): an arrowhead with a notch at
// its back, outline around fill (Mario's: white around red).
void arrow(f32 x, f32 y, f32 dx, f32 dy, f32 len, f32 alpha, u32 fill = 0xE6231EFF, u32 outline = 0xFFFFFFFF)
{
	setupColour();
	const f32 px = -dy, py = dx;
	for (int pass = 0; pass < 2; pass++) {
		const f32 s = pass ? 1.0f : 1.45f; // the white outline: the same shape, larger
		const u32 c = pass ? fill : outline;
		const u8 a  = (u8)(255 * alpha);
		// tip, the wings and the notch, about the arrow's middle
		const f32 tx = x + dx * len * 0.62f * s, ty = y + dy * len * 0.62f * s;
		const f32 nx = x - dx * len * 0.18f * s, ny = y - dy * len * 0.18f * s;
		const f32 lx = x - dx * len * 0.5f * s + px * len * 0.52f * s, ly = y - dy * len * 0.5f * s + py * len * 0.52f * s;
		const f32 rx = x - dx * len * 0.5f * s - px * len * 0.52f * s, ry = y - dy * len * 0.5f * s - py * len * 0.52f * s;
		GXBegin(GX_TRIANGLES, GX_VTXFMT0, 6);
		GXPosition3f32(tx, ty, 0.0f);
		GXColor4u8(c >> 24, c >> 16, c >> 8, a);
		GXPosition3f32(lx, ly, 0.0f);
		GXColor4u8(c >> 24, c >> 16, c >> 8, a);
		GXPosition3f32(nx, ny, 0.0f);
		GXColor4u8(c >> 24, c >> 16, c >> 8, a);
		GXPosition3f32(tx, ty, 0.0f);
		GXColor4u8(c >> 24, c >> 16, c >> 8, a);
		GXPosition3f32(nx, ny, 0.0f);
		GXColor4u8(c >> 24, c >> 16, c >> 8, a);
		GXPosition3f32(rx, ry, 0.0f);
		GXColor4u8(c >> 24, c >> 16, c >> 8, a);
		GXEnd();
	}
}

struct Marker {
	f32 x, z;
	bool got;
};

// How many Shines have flags: 120, or as many as a code mod gives the game
// (Super Mario Eclipse's 240, through BetterSunshineEngine's limit in
// TFlagManager::getShineFlag, retail 0x80294734's cmplwi; patch
// modhook-31-FlagManager).
u32 shineLimit()
{
	unsigned int w;
	return sms_mod_word(0x80294734, &w) && (w >> 16) == 0x2800 ? (w & 0xFFFF) : 120;
}

// The stage's Shines and blue coins, in every manager the conductor has (a
// placed Shine's is the "star" characters' manager, a boss's another), by
// their actor types.
int scan(Marker* shines, int maxShines, Marker* coins, int maxCoins, int* nCoins)
{
	int ns = 0, nc = 0;
	TFlagManager* flags = TFlagManager::getInstance();
	const u32 limit     = shineLimit();
	if (gpConductor) {
		for (JGadget::TList<TLiveManager*>::iterator it = gpConductor->unk10.begin();
		     it != gpConductor->unk10.end(); ++it) {
			TLiveManager* m = *it;
			if (!m)
				continue;
			for (int i = 0; i < m->getObjNum(); i++) {
				TLiveActor* a = m->getObj(i);
				if (!a)
					continue;
				const u32 type = a->getActorType();
				if (type == kBlueCoinType) {
					// a blue coin got, now or before, is dead
					if (!a->checkLiveFlag(LIVE_FLAG_DEAD) && nc < maxCoins) {
						coins[nc].x   = a->mPosition.x;
						coins[nc].z   = a->mPosition.z;
						coins[nc].got = false;
						nc++;
					}
				} else if (type == kShineType) {
					// a Shine still to come (a boss's, a race's) is dead until it
					// appears, so its flag says whether it is got
					const u32 id   = static_cast<TMapObjBase*>(a)->getEventId();
					const bool got = id < limit && id < 0x100 && flags->getShineFlag(id);
					if ((got || id < limit || !a->checkLiveFlag(LIVE_FLAG_DEAD)) && ns < maxShines) {
						shines[ns].x   = a->mPosition.x;
						shines[ns].z   = a->mPosition.z;
						shines[ns].got = got;
						ns++;
					}
				}
			}
		}
	}
	*nCoins = nc;
	return ns;
}

// The other players in a multiplayer game (ext/multiplayer; none without
// it), drawn as smaller arrows in white.
struct Other {
	f32 x, z;   // where they are
	f32 dx, dz; // the way they face, a unit vector
	char name[32];
};
const int kMaxOthers = 8;
int scanOthers(Other* out)
{
	if (!mp_radar_player)
		return 0;
	int n = 0;
	for (int slot = 0; slot < kMaxOthers; slot++) {
		f32 xz[2], facing[2];
		Other& o = out[n];
		if (!mp_radar_player(slot, xz, facing, o.name, sizeof o.name))
			continue;
		o.x  = xz[0];
		o.z  = xz[1];
		o.dx = facing[0];
		o.dz = facing[1];
		// the system font has ASCII only
		for (char* c = o.name; *c; c++)
			if ((u8)*c < 0x20 || (u8)*c >= 0x7F)
				*c = '?';
		n++;
	}
	return n;
}


// A w x h rectangle in a colour, its top left at (x, y).
void rect(f32 x, f32 y, f32 w, f32 h, u32 colour, f32 alpha)
{
	const u8 a = (u8)((colour & 0xFF) * alpha);
	setupColour();
	GXBegin(GX_QUADS, GX_VTXFMT0, 4);
	GXPosition3f32(x, y, 0.0f);
	GXColor4u8(colour >> 24, colour >> 16, colour >> 8, a);
	GXPosition3f32(x + w, y, 0.0f);
	GXColor4u8(colour >> 24, colour >> 16, colour >> 8, a);
	GXPosition3f32(x + w, y + h, 0.0f);
	GXColor4u8(colour >> 24, colour >> 16, colour >> 8, a);
	GXPosition3f32(x, y + h, 0.0f);
	GXColor4u8(colour >> 24, colour >> 16, colour >> 8, a);
	GXEnd();
}

// Text in the system font, h tall, centred on x with its top at y, over a
// soft shadow.
// How wide text() draws s at height h.
f32 textWidth(const char* s, f32 h)
{
	JUTFont* font = gpSystemFont;
	f32 w         = 0.0f;
	if (font && s)
		for (const char* p = s; *p; p++)
			w += font->getWidth((u8)*p) * (h / font->getWidth());
	return w;
}

void text(const char* s, f32 x, f32 y, f32 h, f32 alpha)
{
	JUTFont* font = gpSystemFont;
	if (!font || !s)
		return;
	const f32 w = textWidth(s, h);
	font->setGX();
	font->setCharColor(JUtility::TColor(0, 0, 0, (u8)(160 * alpha)));
	font->drawString_size_scale(x - w / 2 + 1.5f, y + h * 0.82f + 1.5f, h, h, s, strlen(s), false);
	font->setCharColor(JUtility::TColor(255, 255, 255, (u8)(255 * alpha)));
	font->drawString_size_scale(x - w / 2, y + h * 0.82f, h, h, s, strlen(s), false);
}

// The overview into GX RGBA8 tiles (at most kTile square), once a stage.
bool overviewTiles()
{
	if (sOverviewKey == sKey && sTileCount)
		return true;
	if (!sMap.overview || sMap.ow <= 0 || sMap.oh <= 0 || sMap.ow > 2 * kTile || sMap.oh > 2 * kTile)
		return false;
	sTileCount   = 0;
	size_t bytes = 0;
	for (int y = 0; y < sMap.oh; y += kTile) {
		for (int x = 0; x < sMap.ow; x += kTile) {
			Tile& t = sTiles[sTileCount++];
			t.x = x;
			t.y = y;
			t.w = sMap.ow - x < kTile ? sMap.ow - x : kTile;
			t.h = sMap.oh - y < kTile ? sMap.oh - y : kTile;
			t.pw = (t.w + 3) & ~3;
			t.ph = (t.h + 3) & ~3;
			t.offset = bytes;
			bytes += (size_t)t.pw * t.ph * 4;
		}
	}
	if (bytes > sOverviewBytes) { // the largest a stage needs is kept for the next
		sOverviewTex   = (u8*)port_low_alloc(bytes);
		sOverviewBytes = sOverviewTex ? bytes : 0;
	}
	if (!sOverviewTex)
		return false;
	for (int i = 0; i < sTileCount; i++) {
		const Tile& t = sTiles[i];
		u8* out       = sOverviewTex + t.offset;
		size_t o      = 0;
		for (int by = 0; by < t.ph; by += 4) {
			for (int bx = 0; bx < t.pw; bx += 4) {
				const u8* px[16];
				for (int k = 0; k < 16; k++) { // past the edge: the edge's pixel
					int x = t.x + bx + (k & 3), y = t.y + by + (k >> 2);
					x = x < t.x + t.w ? x : t.x + t.w - 1;
					y = y < t.y + t.h ? y : t.y + t.h - 1;
					px[k] = sMap.overview + ((size_t)y * sMap.ow + x) * 4;
				}
				for (int k = 0; k < 16; k++) { // AR, then GB
					out[o++] = 255;
					out[o++] = px[k][0];
				}
				for (int k = 0; k < 16; k++) {
					out[o++] = px[k][1];
					out[o++] = px[k][2];
				}
			}
		}
	}
	DCFlushRange(sOverviewTex, (u32)bytes);
	sOverviewKey = sKey;
	return true;
}

// The full map over the dimmed frame: viewport is the HUD's.
void drawFullMap(const JDrama::TRect& viewport, f32 mx, f32 mz)
{
	const f32 a = sFullAlpha, ease = a * a * (3.0f - 2.0f * a);
	const f32 vw = (f32)(viewport.x2 - viewport.x1), vh = (f32)(viewport.y2 - viewport.y1);
	J2DOrthoGraph graph(viewport);
	graph.setPort();
	graph.setup2D();

	// at the 2D's scale, and with widescreen past the 4:3 edges: the dimming
	// over all the frame, then the map as large as the frame takes, under the
	// area's name
	const f32 wide = GXPC_GetWidescreen() > 1.0f ? GXPC_GetWidescreen() : 1.0f;
	GXPC_SetWide2D(1);
	rect(vw / 2 - vw * wide / 2 - 4.0f, -4.0f, vw * wide + 8.0f, vh + 8.0f, 0x05080DFF, 0.62f * ease);
	const f32 roomW = vw * wide - 64.0f, roomH = vh - 92.0f;
	const f32 fit   = roomW / sMap.ow < roomH / sMap.oh ? roomW / sMap.ow : roomH / sMap.oh;
	const f32 grow  = 0.94f + 0.06f * ease;
	const f32 w = sMap.ow * fit * grow, h = sMap.oh * fit * grow;
	const f32 cx = vw / 2, cy = vh / 2 + 14.0f;
	const f32 left = cx - w / 2, top = cy - h / 2;

	rect(left - 6.0f, top - 6.0f + 3.0f, w + 12.0f, h + 12.0f, 0x000000FF, 0.35f * ease); // shadow
	rect(left - 5.0f, top - 5.0f, w + 10.0f, h + 10.0f, 0x1F2433FF, ease);              // dark edge
	rect(left - 3.5f, top - 3.5f, w + 7.0f, h + 7.0f, 0xF2F2F5FF, ease);                // the border
	for (int i = 0; i < sTileCount; i++) {
		const Tile& t = sTiles[i];
		GXTexObj obj;
		GXInitTexObj(&obj, sOverviewTex + t.offset, (u16)t.pw, (u16)t.ph, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
		GXInitTexObjLOD(&obj, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
		GXLoadTexObj(&obj, GX_TEXMAP0);
		// the tile's padding stays off the screen: its quad shows only t.w x t.h
		const f32 tx = left + t.x * w / sMap.ow, ty = top + t.y * h / sMap.oh;
		const f32 tw = t.w * w / sMap.ow, th = t.h * h / sMap.oh;
		const f32 u = (f32)t.w / t.pw, v = (f32)t.h / t.ph;
		const u8 al = (u8)(255 * ease);
		setupTextured();
		GXBegin(GX_QUADS, GX_VTXFMT0, 4);
		GXPosition3f32(tx, ty, 0.0f);
		GXColor4u8(255, 255, 255, al);
		GXTexCoord2f32(0.0f, 0.0f);
		GXPosition3f32(tx + tw, ty, 0.0f);
		GXColor4u8(255, 255, 255, al);
		GXTexCoord2f32(u, 0.0f);
		GXPosition3f32(tx + tw, ty + th, 0.0f);
		GXColor4u8(255, 255, 255, al);
		GXTexCoord2f32(u, v);
		GXPosition3f32(tx, ty + th, 0.0f);
		GXColor4u8(255, 255, 255, al);
		GXTexCoord2f32(0.0f, v);
		GXEnd();
	}

	// the sprites: world (x, z) to the picture, then the screen
	const f32 worldW = sMap.w / sMap.density, worldH = sMap.h / sMap.density;
	static Marker shines[64], coins[128];
	int nCoins        = 0;
	const int nShines = scan(shines, 64, coins, 128, &nCoins);
	const f32 blueH = 13.0f, shineH = 19.0f;
	for (int i = 0; i < nCoins; i++) {
		const f32 sx = left + (coins[i].x - sMap.x0) / worldW * w, sy = top + (coins[i].z - sMap.z0) / worldH * h;
		if (sx > left && sx < left + w && sy > top && sy < top + h)
			sprite(sBlueCoinArt, sx, sy, blueH, 0xFFFFFFFF, ease);
	}
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < nShines; i++) {
			if (shines[i].got != (pass == 0))
				continue;
			const f32 sx = left + (shines[i].x - sMap.x0) / worldW * w, sy = top + (shines[i].z - sMap.z0) / worldH * h;
			if (!(sx > left && sx < left + w && sy > top && sy < top + h))
				continue;
			if (shines[i].got)
				sprite(sShineArt, sx, sy, shineH * 0.85f, 0x9CC8FFFF, 0.65f * ease);
			else
				sprite(sShineArt, sx, sy, shineH, 0xFFFFFFFF, ease);
		}
	}
	// the other players, then Mario over them, then the others' names
	// under their arrows over all: a name that would cover one already
	// placed (players close together) moves down a line
	static Other others[kMaxOthers];
	const int nOthers = scanOthers(others);
	f32 ox[kMaxOthers], oy[kMaxOthers];
	bool shown[kMaxOthers];
	for (int i = 0; i < nOthers; i++) {
		ox[i]    = left + (others[i].x - sMap.x0) / worldW * w;
		oy[i]    = top + (others[i].z - sMap.z0) / worldH * h;
		shown[i] = ox[i] > left && ox[i] < left + w && oy[i] > top && oy[i] < top + h;
		if (shown[i])
			arrow(ox[i], oy[i], others[i].dx, others[i].dz, 17.0f * kOtherLen, ease, kOtherFill, kOtherOutline);
	}
	const f32 angle = (gpMarioAngleY ? *gpMarioAngleY : 0) * (6.2831853f / 65536.0f);
	const f32 px = left + (mx - sMap.x0) / worldW * w, py = top + (mz - sMap.z0) / worldH * h;
	arrow(px, py, sinf(angle), cosf(angle), 17.0f, ease);
	const f32 nameH = 12.0f;
	f32 placed[kMaxOthers][4]; // x0, y0, x1, y1
	int nPlaced = 0;
	for (int i = 0; i < nOthers; i++) {
		if (!shown[i])
			continue;
		const f32 nw = textWidth(others[i].name, nameH);
		f32 ny       = oy[i] + 10.0f;
		for (int tries = 0; tries < kMaxOthers; tries++) {
			bool clear = true;
			for (int k = 0; k < nPlaced && clear; k++)
				clear = ox[i] + nw / 2 < placed[k][0] || ox[i] - nw / 2 > placed[k][2] || ny + nameH < placed[k][1]
				        || ny > placed[k][3];
			if (clear)
				break;
			ny += nameH + 1.0f;
		}
		placed[nPlaced][0] = ox[i] - nw / 2;
		placed[nPlaced][1] = ny;
		placed[nPlaced][2] = ox[i] + nw / 2;
		placed[nPlaced][3] = ny + nameH;
		nPlaced++;
		text(others[i].name, ox[i], ny, nameH, ease);
	}

	// the area's name, as the pause menu names it: one of the game's ten
	// areas, or any a code mod names (BetterSunshineEngine's SMS_getShineStage,
	// retail 0x802A8AC8, gives Super Mario Eclipse's areas past them, which
	// its stagename.bmg names)
	if (gpMarDirector) {
		const u8 world = SMS_getShineStage(gpMarDirector->getCurrentMap());
		if (world < 10 || (world != 0xFF && SMS_MOD_SITE(0x802A8AC8)))
			if (const char* name = SMSGetMessageData(JKRFileLoader::getGlbResource("/common/2d/stagename.bmg"), world))
				text(name, cx, top - 34.0f, 22.0f, ease);
	}
	GXPC_SetWide2D(0);
}

} // namespace

void RadarDraw(const JDrama::TRect& viewport, f32 lifeX1, f32 lifeY1, f32 lifeX2, f32 lifeY2)
{
	if (!port_radar)
		return;
	static bool sInit;
	if (!sInit) {
		sInit = true;
		if (const char* z = getenv("SMS_RADAR_ZOOM"))
			if (atof(z) >= 500.0f)
				sZoomBase = (f32)atof(z);
		sZoom    = sZoomBase;
		sPresses = port_radar_map_presses;
		if (const char* s = getenv("SMS_RADAR_SIZE"))
			if (atof(s) >= 40.0f && atof(s) <= 250.0f)
				sRadius = kRadius * (f32)atof(s) / 100.0f;
		if (const char* p = getenv("SMS_RADAR_PLACE")) {
			static const char* const names[] = { "corner", "yield", "beside", "below", "bottom_left" };
			for (int i = 0; i < (int)(sizeof names / sizeof names[0]); i++)
				if (!strcmp(p, names[i]))
					sPlace = (Place)i;
		}
		sWin[0].tex = (u8*)port_low_alloc(kWin * kWin * 4);
		sWin[1].tex = (u8*)port_low_alloc(kWin * kWin * 4);
		sBorder     = (u8*)port_low_alloc(kTex * kTex * 4);
		if (sBorder)
			composeBorder();
	}
	const bool play = playing();
	const bool have = play && gpMarioPos && stage();

	// MAP: the full map opens or closes, as the map screen does, with its sounds
	if (port_radar_map_presses != sPresses) {
		sPresses = port_radar_map_presses;
		if (have && (sFullOpen || overviewTiles())) {
			sFullOpen = !sFullOpen;
			SMSGetMSound()->startSoundSystemSE(sFullOpen ? MSD_SE_SY_WIPE_IN : MSD_SE_SY_WIPE_OUT, 0, nullptr, 0);
		}
	}
	// The radar is drawn once per displayed frame; its fades, ease and zoom
	// label are given per 60 fps frame and scaled to the frame rate in force.
	const f32 frames60 = 60.0f / port_active_frame_rate;
	sFullAlpha = clamp01(sFullAlpha + (sFullOpen && play ? 0.15f : -0.15f) * frames60);

	// D-pad up: the next zoom step, with the menus' cursor sound
	if (have && !sFullOpen) {
		TMarioGamePad* pad = gpMarDirector->getGamePad();
		if (pad && pad->testTrigger(PAD_BUTTON_UP)) {
			sZoomStep  = (sZoomStep + 1) % kZoomCount;
			sZoom      = sZoomBase * 100.0f / kZoomSteps[sZoomStep];
			sZoomShown = 75;
			SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_CURSOR, 0, nullptr, 0);
		}
	}

	const bool show = play && !sFullOpen;
	sAlpha += (show ? 0.125f : -0.125f) * frames60;
	sAlpha = clamp01(sAlpha);
	if (sFullAlpha > 0.0f && gpMarioPos && sMapState == 1 && sOverviewKey == sKey)
		drawFullMap(viewport, gpMarioPos->x, gpMarioPos->z);
	if (sAlpha <= 0.0f || !sWin[0].tex || !sWin[1].tex || !sBorder || !gpMarioPos || !stage())
		return;

	const f32 mx = gpMarioPos->x, mz = gpMarioPos->z;
	updateWindow(mx, mz);

	// its place (Place): at the top right of the frame, clear of the HUD's
	// lives counter, unless the life meter needs it
	f32 R = sRadius, cx = viewport.x2 - kMargin - R, cy = viewport.y1 + kMargin + 6.0f + R;
	// the meter in the radar's way: on screen and over its place (not in the
	// middle of the frame, where it first comes in)
	const bool life = lifeY2 > 0.0f && lifeY2 > lifeY1 && lifeX2 > cx - R && lifeX1 < cx + R && lifeY1 < cy + R;
	switch (sPlace) {
	case kCorner:
		break;
	case kYield: // riding down and up under it as it comes and goes
		if (life && lifeY2 + kPlaceGap + R > cy)
			cy = lifeY2 + kPlaceGap + R;
		break;
	case kBeside: // beside its place at rest, the rays clear
		if (life) {
			R *= kBesideScale;
			cx = (kLifeX1 + kLifeX2) * 0.5f - (kLifeX2 - kLifeX1) * 0.5f * kLifeRays - kPlaceGap - R;
			cy = (kLifeY1 + kLifeY2) * 0.5f;
		}
		break;
	case kBelow:
		cy = kLifeY2 + kPlaceGap + R;
		break;
	case kBottomLeft:
		cx = viewport.x1 + kMargin + R;
		cy = viewport.y2 - kMargin - 10.0f - R;
		break;
	}
	if (sPlaceR <= 0.0f || sAlpha <= 0.125f * frames60) { // shown again: straight there
		sPlaceX = cx;
		sPlaceY = cy;
		sPlaceR = R;
	} else {
		const f32 ease = 1.0f - powf(1.0f - kPlaceEase, frames60);
		sPlaceX += (cx - sPlaceX) * ease;
		sPlaceY += (cy - sPlaceY) * ease;
		sPlaceR += (R - sPlaceR) * ease;
	}
	R  = sPlaceR;
	cx = sPlaceX;
	cy = sPlaceY;
	const f32 mapR  = R * kMapR / (kTex * 0.5f); // the map's radius on screen
	const f32 scale = mapR / sZoom;              // 2D units per world unit (x right, z down)

	// a 2D of its own on the viewport (the HUD's has its own projection by now)
	J2DOrthoGraph graph(viewport);
	graph.setPort();
	graph.setup2D();
	GXPC_HudPaneBegin(cx - R, cx + R);

	// the map, a little past kMapR so the border's inner edge covers its own
	const f32 perTexel = R / (kTex * 0.5f); // 2D units per texel of the border's texture
	mapFan(cx, cy, (kMapR + 2.5f) * perTexel, mx, mz, scale, sAlpha);
	mapShade(cx, cy, perTexel, sAlpha);

	// the sprites, cut at the border's inner edge (a little under it), so
	// one coming in from past the edge slides in under the border
	static Marker shines[64], coins[128];
	int nCoins         = 0;
	const int nShines  = scan(shines, 64, coins, 128, &nCoins);
	const f32 blueH    = R * kBlueH, shineH = R * kShineH;
	const f32 clipR    = R * (kMapR + 1.0f) / (kTex * 0.5f);
	for (int i = 0; i < nCoins; i++) {
		const f32 sx = (coins[i].x - mx) * scale, sy = (coins[i].z - mz) * scale;
		clippedSprite(sBlueCoinArt, cx + sx, cy + sy, blueH, 0xFFFFFFFF, sAlpha, cx, cy, clipR);
	}
	for (int pass = 0; pass < 2; pass++) { // Shines got under those still to get
		for (int i = 0; i < nShines; i++) {
			if (shines[i].got != (pass == 0))
				continue;
			const f32 sx = (shines[i].x - mx) * scale, sy = (shines[i].z - mz) * scale;
			if (shines[i].got)
				clippedSprite(sShineArt, cx + sx, cy + sy, shineH * 0.85f, 0x9CC8FFFF, sAlpha * 0.65f, cx, cy, clipR);
			else
				clippedSprite(sShineArt, cx + sx, cy + sy, shineH, 0xFFFFFFFF, sAlpha, cx, cy, clipR);
		}
	}

	// the border over them
	GXTexObj border;
	GXInitTexObj(&border, sBorder, kTex, kTex, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GXInitTexObjLOD(&border, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
	GXLoadTexObj(&border, GX_TEXMAP0);
	quad(cx, cy, 2 * R, 2 * R, 0xFFFFFFFF, sAlpha);

	// the other players: one past the radar's reach waits at its edge,
	// fainter, the way to them
	static Other others[kMaxOthers];
	const int nOthers = scanOthers(others);
	const f32 otherLen = R * kArrowLen * kOtherLen, reach = mapR - otherLen;
	for (int i = 0; i < nOthers; i++) {
		f32 sx = (others[i].x - mx) * scale, sy = (others[i].z - mz) * scale, fade = 1.0f;
		const f32 d = sqrtf(sx * sx + sy * sy);
		if (d > reach) {
			sx *= reach / d;
			sy *= reach / d;
			fade = 0.7f;
		}
		arrow(cx + sx, cy + sy, others[i].dx, others[i].dz, otherLen, sAlpha * fade, kOtherFill, kOtherOutline);
	}

	// Mario: the way he faces (his angle, 0 facing +z: down the map)
	const f32 angle = (gpMarioAngleY ? *gpMarioAngleY : 0) * (6.2831853f / 65536.0f);
	arrow(cx, cy, sinf(angle), cosf(angle), R * kArrowLen, sAlpha);

	// the zoom, for a moment after D-pad up
	if (sZoomShown > 0.0f) {
		sZoomShown -= frames60;
		// as the reach in metres (the system font has no %)
		char label[24];
		snprintf(label, sizeof label, "%d m", (int)(sZoom / 100.0f + 0.5f));
		text(label, cx, cy + R + 2.0f, 13.0f, sAlpha * clamp01(sZoomShown / 15.0f));
	}

	GXPC_HudPaneEnd();
}
