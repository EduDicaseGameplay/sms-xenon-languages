// The renderer side of gx_internal.h on the Xbox 360 (platform/gx/src/gx_render.cpp's part
// for GL), its game side (xe_gx.h, "render packets"): draw batches, pipeline state, EFB copies and
// presenting, peeks and pixel metrics, turned into packets for xe_gx_exec.cpp.
//
// Batches go into vertex and index rings (16-bit indices, so at most 65535 vertices a batch).
#include "xe_gx.h"
#include <xgraphics.h>
#include <sms_gx/gx_pc.h>
#include <port_settings.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unordered_map>
#include <vector>

extern double g_dspSeconds;  // port patch 13: the DSP mixer's time (the bench)
extern "C" int port_audio_dsp_reply_ready(void);  // port patch 14
extern "C" void port_irq_check(void);

namespace gx {

uint32_t g_displayFrames = 0;
double g_flushSeconds = 0;  // drawing batches (gx_fifo.cpp leaves it out of vertex decoding)
bool g_gxStats = false;     // gx_fifo.cpp times vertex decoding (g_decodeSeconds): two QPC calls a draw command, off (SMS_XE_CORE_PARTS=1 times them by timebase)
extern double g_decodeSeconds;
// gx_fifo.cpp widens lines on the CPU without geometry shaders: Xenos draws them wide itself
// (D3DRS_LINEWIDTH), so lines go as they are
bool platformGeometryShaders() { return true; }
extern uint64_t g_statTexInvalidates;  // xe_gx_tex.cpp
uint32_t g_traceLastVat = 0;
FILE* traceFile() { return NULL; }
void traceNoteBegin() {}
void traceNoteProjection(const float*) {}

namespace xe {

double g_xeWait, g_xeTex, g_xeTexCreate, g_xeTexDecode, g_xeTexStore, g_xeTexMips;
static double s_copySeconds;
static uint32_t s_efbGen = 1;  // bumped by anything that changes the EFB
// The internal resolution (SMS_XE_SCALE, default 1.5: 720p's 672 of 720 rows): the EFB's surfaces are s_scale times the
// GameCube's, s_efbW x s_efbH. Draws keep EFB coordinates (the shaders map them onto the whole
// surface); the D3D viewport, scissor, clears, the display copy and EFB reads are scaled. Texture
// copies stay at the game's size (each texel the EFB pixel under its centre, as upstream's native
// copies), so the memory they are written back to is as the game expects.
static float s_scale = 1;
// Texture copies the game only samples (not written back: reflections, the screen texture) are made
// at this scale, as the EFB's pixels are: at the GameCube's size, magnified, they came out blurry at
// 720p. They are drawn in bands of the EDRAM left next to the EFB (xe_gx_exec.cpp): up to s_scale
// (SMS_XE_COPY_SCALE=1: the GameCube's size).
static float s_copyScale = 1;
static int s_efbW = EFB_W, s_efbH = EFB_H;
static int scaled(float v) { return int(floorf(v * s_scale + 0.5f)); }

// Widescreen (SMS_WIDESCREEN, read by port_runtime.cpp; 16:9 by default on the 360, see
// xe_posix.cpp): gx_render.cpp's scheme. The EFB is s_wideW = 640 * s_wide game pixels wide
// (before the internal resolution) while the game keeps working in 640-wide coordinates, which
// each draw, copy and peek maps into it (x' = a * x + b, clip x scaled by clip): stretched
// (a = s_stretch) for the game camera (which the widescreen patches widen), 2D across the whole
// width (fades, screen copies drawn back), full-width copies and clears, and peeks; centred
// (b = s_ox) for 2D and 3D over part of the screen (the HUD). A full-screen perspective draw the
// game did not widen (title, file select, cutscene overlays) is stretched with its clip x scaled
// by 1 / s_stretch: its field of view widens.
struct XMap {
	float a, b, clip;
};
static float s_wide = 1.0f, s_stretch = 1.0f;
static int s_wideW = EFB_W, s_ox = 0;
static XMap s_xmap = {1, 0, 1};
static bool s_stretch2D, s_wide2D;  // coreSetStretch2D (the faders), coreSetWide2D (the map)
// SMS_WIDESCREEN_HUD=edges: the HUD's pieces in the left and right thirds go to the wide frame's
// edges (coreSetHud, GXPC_HudPane*, GXPC_HudPiece*); otherwise the HUD stays centred.
static bool s_hudEdges, s_hud;
struct HudPane {
	float x1, x2;
};
static std::vector<HudPane> s_hudPanes;  // the J2D panes being drawn, outermost first
static float s_hudAnchor = -1.0f;        // the piece's centre (game x), -1 for none
static float hudOffset(float at)
{
	if (at < float(EFB_W) / 3) return 0.0f;
	if (at > float(EFB_W) * 2 / 3) return float(2 * s_ox);
	return float(s_ox);
}
static float centredOffset(float at) { return s_hud ? hudOffset(s_hudAnchor >= 0.0f ? s_hudAnchor : at) : float(s_ox); }
// the game camera's aspect (TMarDirector: video width 660 * 0.91346 / 448)
static const float kCamAspect = 660.0f * 0.91346145f / 448.0f;
float internalScale() { return s_scale; }
static bool s_pixActive;       // a pixel metric count is running (see pixMetricClear)
static uint32_t s_pixTris;     // triangles drawn during it
static uint32_t s_statPeeks, s_statMetricReads;
static double s_statDraws, s_statCopies;  // [perf]
static uint32_t s_drawsTotal;  // SMS_XE_SPIKES
static uint32_t s_arenaResets, s_ringWaits;  // SMS_XE_SPIKES: arena generations, ring quarters waited for
// [perf]: the parts of a draw, in timebase ticks (__mftb: no kernel call, unlike monoSeconds)
enum { TB_APPEND, TB_STATE, TB_PROGRAM, TB_TEXTURES, TB_XF, TB_CONSTS, TB_SUBMIT, TB_COUNT };
static unsigned long long s_tb[TB_COUNT];
struct TbClock {
	unsigned long long* acc;
	unsigned long long t0;
	explicit TbClock(int part) : acc(&s_tb[part]), t0(__mftb()) {}
	void next(int part)
	{
		const unsigned long long t = __mftb();
		*acc += t - t0;
		acc = &s_tb[part];
		t0 = t;
	}
	~TbClock() { *acc += __mftb() - t0; }
};
static bool s_ready;
// The XF block's constant rows (VC_XF..VC_PROJ) whose XF memory changed since they were uploaded
// (port patch 15 reports each word; J3D changes a matrix or two before almost every draw, and
// rebuilding and comparing all 200 rows each time was 14 ms of a Delfino Plaza frame on the console)
static uint32_t s_xfRows[(VC_PROJ + 31) / 32];
static void xfRowDirty(int row) { s_xfRows[row >> 5] |= 1u << (row & 31); }
static void xfAllDirty()
{
	for (int r = 0; r < VC_PROJ; r++) xfRowDirty(r);
}

// What the shader constants hold, so a batch uploads only the 16-byte rows that changed: a full
// upload a draw (~3 KB of XF matrices; J3D changes a matrix before almost every draw) filled the
// D3D command buffer and cost most of a draw's ~20 us on the console. Rows set elsewhere (blits,
// copy passes) are marked unknown (all ones, a NaN no upload writes).
static float s_vcShadow[VC_COUNT * 4], s_pcShadow[PC_COUNT * 4];
static bool s_constSrcValid;  // s_constSrc is what the shadows hold (uploadConstants)
static void forgetConsts(bool vs, int first, int count)
{
	memset((vs ? s_vcShadow : s_pcShadow) + first * 4, 0xFF, size_t(count) * 16);
	s_constSrcValid = false;
}
void constsClobbered()  // a blit: vertex c0-c2 (XF matrix rows), and its own pixel constants
{
	forgetConsts(true, 0, 3);
	forgetConsts(false, 0, PC_COUNT);
	for (int r = 0; r < 3; r++) xfRowDirty(r);
}
static void setConsts(bool vs, int first, const float* d, int count)
{
	float* sh = (vs ? s_vcShadow : s_pcShadow) + first * 4;
	for (int i = 0; i < count;) {
		if (sameWords(sh + i * 4, d + i * 4, 16)) {
			i++;
			continue;
		}
		int j = i + 1;
		while (j < count && !sameWords(sh + j * 4, d + j * 4, 16)) j++;
		emitConsts(vs, first + i, d + i * 4, j - i);
		memcpy(sh + i * 4, d + i * 4, size_t(j - i) * 16);
		i = j;
	}
}

// SMS_XE_DEBUG: bits that switch parts of the pipeline off, to find what hides a draw
// (1 culling, 2 depth test, 8 blending, 16 scissor; 32 draws everything white; 16384 encodes I8
// write-backs on the CPU instead of kEncodeI8Ps; 32768 skips line and point batches)
static unsigned debugBits()
{
	static int v = -1;
	if (v < 0) {
		const char* e = getenv("SMS_XE_DEBUG");
		v = e ? atoi(e) : 0;
	}
	return unsigned(v);
}

// ------------------------------------------------------------------ vertex declarations
// The loader stores colours (r, g, b, a) and matrix indices as bytes in memory order. Vertex
// fetch swaps each dword (8IN32), and the XDK's own UBYTE4 types (swizzle ABGR) then give them
// reversed; swizzle RGBA puts them back (as OpenLara found; the sky's blue came out orange).
#define XE_UBYTE4N MAKED3DDECLTYPE(GPUVERTEXFORMAT_8_8_8_8, GPUENDIAN_8IN32, GPUVERTEXSIGN_UNSIGNED, GPUNUMFORMAT_FRACTION, GPUSWIZZLE_RGBA)
#define XE_UBYTE4 MAKED3DDECLTYPE(GPUVERTEXFORMAT_8_8_8_8, GPUENDIAN_8IN32, GPUVERTEXSIGN_UNSIGNED, GPUNUMFORMAT_INTEGER, GPUSWIZZLE_RGBA)

static std::unordered_map<uint32_t, IDirect3DVertexDeclaration9*> s_decls;

static IDirect3DVertexDeclaration9* declFor(uint32_t fmt)
{
	IDirect3DVertexDeclaration9*& d = s_decls[fmt];
	if (d) return d;
	const VtxFmtLayout& l = vtxFmtLayout(fmt);
	D3DVERTEXELEMENT9 e[20];
	int n = 0;
	D3DVERTEXELEMENT9 pos = {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0};
	e[n++] = pos;
	if (fmt & VF_NRM) {
		D3DVERTEXELEMENT9 x = {0, l.nrm, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0};
		e[n++] = x;
		if (fmt & VF_NBT) {
			D3DVERTEXELEMENT9 b = {0, WORD(l.nrm + 12), D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BINORMAL, 0};
			D3DVERTEXELEMENT9 t = {0, WORD(l.nrm + 24), D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TANGENT, 0};
			e[n++] = b;
			e[n++] = t;
		}
	}
	for (int c = 0; c < 2; c++)
		if (fmt & (VF_CLR0 << c)) {
			D3DVERTEXELEMENT9 x = {0, l.clr[c], XE_UBYTE4N, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, BYTE(c)};
			e[n++] = x;
		}
	for (int t = 0; t < 8; t++)
		if (fmt & (VF_TEX0 << t)) {
			D3DVERTEXELEMENT9 x = {0, l.tex[t], D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, BYTE(t)};
			e[n++] = x;
		}
	if (fmt & VF_MTX)
		for (int i = 0; i < 3; i++) {
			D3DVERTEXELEMENT9 x = {0, WORD(l.mtx + 4 * i), XE_UBYTE4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDINDICES, BYTE(i)};
			e[n++] = x;
		}
	D3DVERTEXELEMENT9 end = D3DDECL_END();
	e[n++] = end;
	dev->CreateVertexDeclaration(e, &d);
	return d;
}

// ------------------------------------------------------------------ batching
static PodBuf<uint8_t> s_bdata;
static PodBuf<uint16_t> s_bidx;
static PodBuf<uint32_t> s_tmpIdx;
static uint32_t s_bcount, s_bfmt, s_bstride = 12;
static PrimClass s_bclass = PRIM_TRIS;
enum { kMaxBatchVerts = 65535 };
// A batch is a sequence of segments, each one draw: indices of the streamed vertices (s_bdata,
// s_bidx, put into the rings at the flush) or a display-list run stored in the arena.
struct Seg {
	bool arena;
	uint32_t first, count;  // stream: the range of s_bidx; arena: indices
	UINT voff, firstIndex, verts;  // arena: where its vertices and indices are
	const uint8_t* cpu;            // arena: its vertices in RAM (gx_fifo.cpp's copy)
};
static PodBuf<Seg> s_segs;
static bool batchEmpty() { return s_segs.empty(); }
static void streamIndices(uint32_t at, uint32_t k)  // s_bidx[at, at + k) were added
{
	if (s_segs.empty() || s_segs[s_segs.size() - 1].arena) {
		s_segs.resize(s_segs.size() + 1);
		Seg& g0 = s_segs[s_segs.size() - 1];
		g0.arena = false;
		g0.first = at;
		g0.count = 0;
	}
	s_segs[s_segs.size() - 1].count += k;
}

}  // namespace xe

using namespace xe;

bool rendererReady() { return s_ready; }
float efbScale() { return 1.0f; }

void onStateChange()
{
	if (!batchEmpty()) flushBatch();
}

PrimClass primClass(uint8_t op) { return op >= 0xB8 ? PRIM_POINTS : op >= 0xA8 ? PRIM_LINES : PRIM_TRIS; }

uint8_t* primitiveBegin(uint8_t op, uint32_t n, uint32_t fmt, uint32_t stride)
{
	TbClock tb(TB_APPEND);
	PrimClass cls = primClass(op);
	if (!batchEmpty() && (cls != s_bclass || fmt != s_bfmt || s_bcount + n > kMaxBatchVerts)) flushBatch();
	s_bclass = cls;
	s_bfmt = fmt;
	s_bstride = stride;
	s_bdata.resize(size_t(s_bcount + n) * stride);
	return &s_bdata[size_t(s_bcount) * stride];
}

uint32_t primitiveIndices(uint8_t op, uint32_t n, uint32_t base, uint32_t* w)
{
	uint32_t* const w0 = w;
	switch (op & 0xF8) {
	case 0x80:
	case 0x88:  // quads
		for (uint32_t i = 0; i + 3 < n; i += 4) {
			*w++ = base + i; *w++ = base + i + 1; *w++ = base + i + 2;
			*w++ = base + i; *w++ = base + i + 2; *w++ = base + i + 3;
		}
		break;
	case 0x90:
		for (uint32_t i = 0; i + 2 < n; i += 3) {
			*w++ = base + i; *w++ = base + i + 1; *w++ = base + i + 2;
		}
		break;
	case 0x98:
		for (uint32_t i = 2; i < n; i++) {
			if (i & 1) { *w++ = base + i - 1; *w++ = base + i - 2; }
			else { *w++ = base + i - 2; *w++ = base + i - 1; }
			*w++ = base + i;
		}
		break;
	case 0xA0:
		for (uint32_t i = 2; i < n; i++) { *w++ = base; *w++ = base + i - 1; *w++ = base + i; }
		break;
	case 0xA8:
		for (uint32_t i = 0; i + 1 < n; i += 2) { *w++ = base + i; *w++ = base + i + 1; }
		break;
	case 0xB0:
		for (uint32_t i = 1; i < n; i++) { *w++ = base + i - 1; *w++ = base + i; }
		break;
	default:
		for (uint32_t i = 0; i < n; i++) *w++ = base + i;
		break;
	}
	return uint32_t(w - w0);
}

void primitiveEnd(uint8_t op, uint32_t n)
{
	if (!s_ready || n == 0) return;
	TbClock tb(TB_APPEND);
	uint32_t base = s_bcount;
	s_bcount += n;
	s_tmpIdx.resize(size_t(n) * 3);
	uint32_t k = primitiveIndices(op, n, base, &s_tmpIdx[0]);
	size_t at = s_bidx.size();
	s_bidx.resize(at + k);
	for (uint32_t i = 0; i < k; i++) s_bidx[at + i] = uint16_t(s_tmpIdx[i]);
	streamIndices(uint32_t(at), k);
}

void appendDecoded(PrimClass cls, uint32_t fmt, uint32_t stride, const uint8_t* verts, uint32_t n, const uint32_t* idx,
                   uint32_t nidx)
{
	TbClock tb(TB_APPEND);
	if (!batchEmpty() && (cls != s_bclass || fmt != s_bfmt || s_bcount + n > kMaxBatchVerts)) flushBatch();
	s_bclass = cls;
	s_bfmt = fmt;
	s_bstride = stride;
	if (!s_ready || n == 0 || n > kMaxBatchVerts) return;
	uint32_t base = s_bcount;
	s_bdata.resize(size_t(base + n) * stride);
	memcpy(&s_bdata[size_t(base) * stride], verts, size_t(n) * stride);
	s_bcount += n;
	size_t at = s_bidx.size();
	s_bidx.resize(at + nidx);
	for (uint32_t i = 0; i < nidx; i++) s_bidx[at + i] = uint16_t(idx[i] + base);
	streamIndices(uint32_t(at), nidx);
}

// ------------------------------------------------------------------ display-list arena
// The runs of cached display lists (gx_fifo.cpp) are stored once in a vertex and an index buffer
// of their own and drawn from there every frame: no copy of their vertices a frame. Full, it
// starts over (a new generation: the lists store their runs again) once the GPU is done with it.
// SMS_XE_ARENA=0: off (the runs are copied into each batch).
struct Arena {
	enum { kVerts = 32 << 20, kIdx = 8 << 20 };
	IDirect3DVertexBuffer9* vb;
	IDirect3DIndexBuffer9* ib;
	uint8_t *vmem, *imem;
	UINT vpos, ipos;
	uint32_t gen;
	bool init()
	{
		void* p = NULL;
		if (FAILED(dev->CreateVertexBuffer(kVerts, 0, 0, 0, &vb, NULL))) return false;
		vb->Lock(0, 0, &p, 0);
		vb->Unlock();
		vmem = (uint8_t*)p;
		if (FAILED(dev->CreateIndexBuffer(kIdx, 0, D3DFMT_INDEX16, 0, &ib, NULL))) return false;
		ib->Lock(0, 0, &p, 0);
		ib->Unlock();
		imem = (uint8_t*)p;
		gen = 1;
		return vmem && imem;
	}
};
static Arena s_arena;
static bool s_arenaOn;

bool arenaEnabled() { return s_arenaOn; }
uint32_t arenaGeneration() { return s_arena.gen; }

bool arenaUpload(const uint8_t* verts, uint32_t n, uint32_t stride, const uint32_t* idx, uint32_t nidx, ArenaRef& out)
{
	const UINT vbytes = n * stride, ibytes = nidx * 2;
	if (!s_ready || n > kMaxBatchVerts || vbytes > Arena::kVerts / 8 || ibytes > Arena::kIdx / 8) return false;
	if ((s_arena.vpos + 15) / 16 * 16 + vbytes > Arena::kVerts || s_arena.ipos + ibytes > Arena::kIdx) {
		flushBatch();  // what is queued may draw from it
		waitToken(fenceToken());
		s_arenaResets++;
		s_arena.vpos = s_arena.ipos = 0;
		s_arena.gen++;
	}
	const UINT voff = (s_arena.vpos + 15) / 16 * 16;
	XMemCpyStreaming_WriteCombined(s_arena.vmem + voff, verts, vbytes);
	s_tmpIdx.resize((nidx + 1) / 2);
	uint16_t* i16 = (uint16_t*)&s_tmpIdx[0];
	for (uint32_t i = 0; i < nidx; i++) i16[i] = uint16_t(idx[i]);
	XMemCpyStreaming_WriteCombined(s_arena.imem + s_arena.ipos, i16, ibytes);
	out.gen = s_arena.gen;
	out.baseVertex = int32_t(voff);  // here: the byte offset of its vertices
	out.iOff = s_arena.ipos;
	s_arena.vpos = voff + vbytes;
	s_arena.ipos += (ibytes + 15) / 16 * 16;
	return true;
}

void appendArena(PrimClass cls, uint32_t fmt, uint32_t stride, const ArenaRef& ref, uint32_t n, uint32_t nidx,
                 const uint8_t* verts)
{
	TbClock tb(TB_APPEND);
	if (!batchEmpty() && (cls != s_bclass || fmt != s_bfmt)) flushBatch();
	s_bclass = cls;
	s_bfmt = fmt;
	s_bstride = stride;
	s_segs.resize(s_segs.size() + 1);
	Seg& sg = s_segs[s_segs.size() - 1];
	sg.arena = true;
	sg.first = 0;
	sg.count = nidx;
	sg.voff = UINT(ref.baseVertex);
	sg.firstIndex = UINT(ref.iOff / 2);
	sg.verts = n;
	sg.cpu = verts;
}

bool batchUsesArena()
{
	for (size_t i = 0; i < s_segs.size(); i++)
		if (s_segs[i].arena) return true;
	return false;
}

void markXfMemDirty(uint32_t a)
{
	if (a < 0x100) xfRowDirty(int(a >> 2));                           // matrices
	else if (a >= 0x400 && a < 0x460) xfRowDirty(VC_NRM + int(a - 0x400) / 3);  // normal matrices
	else if (a >= 0x500 && a < 0x600) xfRowDirty(VC_POST + int(a - 0x500) / 4);  // post matrices
	else if (a >= 0x600 && a < 0x680)                                  // lights: all five rows
		for (int r = 0; r < 5; r++) xfRowDirty(VC_LIGHT + 5 * (int(a - 0x600) / 16) + r);
}

// XF constant row `row` from XF memory
static void xfRow(int row, float* o)
{
	if (row < VC_NRM) {
		memcpy(o, &g.xfMem[row * 4], 16);
	} else if (row < VC_POST) {
		memcpy(o, &g.xfMem[0x400 + (row - VC_NRM) * 3], 12);
		o[3] = 0.0f;
	} else if (row < VC_LIGHT) {
		memcpy(o, &g.xfMem[0x500 + (row - VC_POST) * 4], 16);
	} else {  // lights: colour, A, K, position, direction
		const int l = (row - VC_LIGHT) / 5, k = (row - VC_LIGHT) % 5;
		const uint32_t* w = &g.xfMem[0x600 + l * 16];
		if (k == 0) {
			o[0] = float((w[3] >> 24) & 255) / 255.0f;
			o[1] = float((w[3] >> 16) & 255) / 255.0f;
			o[2] = float((w[3] >> 8) & 255) / 255.0f;
			o[3] = float(w[3] & 255) / 255.0f;
		} else {
			memcpy(o, &w[4 + 3 * (k - 1)], 12);
			o[3] = 0.0f;
		}
	}
}

static inline float sext11(uint32_t v) { return float(int32_t(v << 21) >> 21); }
static inline float xff(uint32_t r)
{
	float f;
	memcpy(&f, &g.xfReg[r], 4);
	return f;
}

// gx_render.cpp's applyGlState in Direct3D: the draw's device state into s_ds. Blend states stay
// as they were while blending is off, so they do not change for nothing.
static DrawState s_ds;
static bool applyState()
{
	// scissor (registers hold coordinates + 342)
	uint32_t tl = g.bp[BP_SCISSOR_TL], br = g.bp[BP_SCISSOR_BR];
	int top = int(tl & 0x7FF) - 342, left = int((tl >> 12) & 0x7FF) - 342;
	int bottom = int(br & 0x7FF) - 342, right = int((br >> 12) & 0x7FF) - 342;
	float l = float(left), r = float(right + 1);
	if (s_wideW != EFB_W) {  // into the draw's widescreen mapping; the whole width stays whole
		if (left <= 0 && right + 1 >= EFB_W) {
			l = 0.0f;
			r = float(s_wideW);
		} else {
			l = floorf(l * s_xmap.a + s_xmap.b);
			r = ceilf(r * s_xmap.a + s_xmap.b);
		}
	}
	RECT sc = {scaled(l < 0.0f ? 0.0f : l), scaled(float(top < 0 ? 0 : top)), scaled(r), scaled(float(bottom + 1))};
	if (sc.right > s_efbW) sc.right = s_efbW;
	if (sc.bottom > s_efbH) sc.bottom = s_efbH;
	if (sc.right <= sc.left || sc.bottom <= sc.top) return false;
	s_ds.rs[RS_SCISSOR] = !(debugBits() & 16);
	s_ds.scissor = sc;

	// culling: GX front faces are clockwise on screen (the register's 1 culls back faces)
	uint32_t cull = (g.bp[BP_GENMODE] >> 14) & 3;
	if (s_bclass == PRIM_TRIS && cull == 3) return false;
	s_ds.rs[RS_CULL] = s_bclass != PRIM_TRIS || cull == 0 || (debugBits() & 1) ? D3DCULL_NONE
	                   : cull == 1 ? D3DCULL_CCW : D3DCULL_CW;

	static const DWORD cmp[8] = {D3DCMP_NEVER, D3DCMP_LESS, D3DCMP_EQUAL, D3DCMP_LESSEQUAL,
	                             D3DCMP_GREATER, D3DCMP_NOTEQUAL, D3DCMP_GREATEREQUAL, D3DCMP_ALWAYS};
	uint32_t z = g.bp[BP_ZMODE];
	s_ds.rs[RS_ZENABLE] = (z & 1) && !(debugBits() & 2) ? D3DZB_TRUE : D3DZB_FALSE;
	s_ds.rs[RS_ZFUNC] = cmp[(z >> 1) & 7];
	s_ds.rs[RS_ZWRITE] = (z & 1) && ((z >> 4) & 1);

	uint32_t pix = g.bp[BP_PE_CONTROL] & 7;
	bool hasAlpha = pix == 1;  // GX_PF_RGBA6_Z24
	uint32_t cm = g.bp[BP_CMODE0], cm1 = g.bp[BP_CMODE1];
	bool colorUpd = (cm >> 3) & 1, alphaUpd = ((cm >> 4) & 1) && hasAlpha;
	bool dstAlpha = ((cm1 >> 8) & 1) != 0;
	DWORD mask = (colorUpd ? (D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE) : 0) |
	             (alphaUpd || (dstAlpha && hasAlpha) ? D3DCOLORWRITEENABLE_ALPHA : 0);
	s_ds.rs[RS_COLORWRITE] = mask;

	static const DWORD srcF[8] = {D3DBLEND_ZERO, D3DBLEND_ONE, D3DBLEND_DESTCOLOR, D3DBLEND_INVDESTCOLOR,
	                              D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLEND_DESTALPHA, D3DBLEND_INVDESTALPHA};
	static const DWORD dstF[8] = {D3DBLEND_ZERO, D3DBLEND_ONE, D3DBLEND_SRCCOLOR, D3DBLEND_INVSRCCOLOR,
	                              D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLEND_DESTALPHA, D3DBLEND_INVDESTALPHA};
	struct NoDst {  // without EFB alpha, the destination's alpha reads as 1
		bool a;
		DWORD operator()(DWORD f) const
		{
			if (a) return f;
			return f == D3DBLEND_DESTALPHA ? DWORD(D3DBLEND_ONE) : f == D3DBLEND_INVDESTALPHA ? DWORD(D3DBLEND_ZERO) : f;
		}
	} noDst = {hasAlpha};
	DWORD aSrc = D3DBLEND_ONE, aDst = D3DBLEND_ZERO;
	if (dstAlpha) {  // the constant destination alpha: the alpha channel blends to it
		DWORD a = cm1 & 0xFF;
		s_ds.rs[RS_BLENDFACTOR] = D3DCOLOR_ARGB(a, 0, 0, 0);
		aSrc = D3DBLEND_BLENDFACTOR;
	}
	DWORD blend = FALSE, op = D3DBLENDOP_ADD, s = D3DBLEND_ONE, d = D3DBLEND_ZERO, opA = D3DBLENDOP_ADD;
	if (cm & 1) {
		blend = TRUE;
		if ((cm >> 11) & 1) {  // subtract: destination minus source, the factors ignored (both one)
			op = D3DBLENDOP_REVSUBTRACT;
			s = d = D3DBLEND_ONE;
			opA = dstAlpha ? D3DBLENDOP_ADD : D3DBLENDOP_REVSUBTRACT;
			if (!dstAlpha) aSrc = aDst = D3DBLEND_ONE;
		} else {
			s = noDst(srcF[(cm >> 8) & 7]);
			d = noDst(dstF[(cm >> 5) & 7]);
			if (!dstAlpha) {
				aSrc = s;
				aDst = d;
			}
		}
	} else if (dstAlpha) {
		blend = TRUE;
	}
	// ponytail: logic ops (cm bit 1, without blending) draw as copies; D3D9 has none. Each one
	// seen is logged once, to find out whether any game draw needs them.
	if (!(cm & 1) && ((cm >> 1) & 1)) {
		static uint32_t seen;
		const uint32_t lop = (cm >> 12) & 15;
		if (!(seen & (1u << lop))) {
			seen |= 1u << lop;
			logmsg("logic op %u drawn as a copy (frame %u)", lop, g_displayFrames);
		}
	}
	if (debugBits() & 8) blend = FALSE;
	s_ds.rs[RS_BLEND] = blend;
	if (blend) {
		s_ds.rs[RS_BLENDOP] = op;
		s_ds.rs[RS_SRCBLEND] = s;
		s_ds.rs[RS_DESTBLEND] = d;
		s_ds.rs[RS_SEPARATEALPHA] = TRUE;
		s_ds.rs[RS_BLENDOPALPHA] = opA;
		s_ds.rs[RS_SRCBLENDALPHA] = aSrc;
		s_ds.rs[RS_DESTBLENDALPHA] = aDst;
	}
	uint32_t lp = g.bp[BP_LPSIZE];
	float pt = float((lp >> 8) & 0xFF) / 6.0f * s_scale;
	if (pt < 1.0f) pt = 1.0f;
	memcpy(&s_ds.rs[RS_POINTSIZE], &pt, 4);
	// lines as wide as GXSetLineWidth sets them (sixths of an EFB pixel): the circle wipes draw their
	// black field as a line a row, 6 (one pixel) wide, which one screen pixel left striped above 1x
	float lw = float(lp & 0xFF) / 6.0f * s_scale;
	if (lw < 1.0f) lw = 1.0f;
	memcpy(&s_ds.rs[RS_LINEWIDTH], &lw, 4);
	return true;
}

static void uploadConstants(float texW[8], float texH[8])
{
	float vc[(VC_COUNT - VC_PROJ) * 4];
	for (int wi = 0; wi < (VC_PROJ + 31) / 32; wi++) {  // the changed XF rows, in runs
		uint32_t bits = s_xfRows[wi];
		s_xfRows[wi] = 0;
		while (bits) {
			const int lo = 31 - int(_CountLeadingZeros(bits & (0u - bits)));
			int hi = lo;
			while (hi < 31 && (bits >> (hi + 1) & 1)) hi++;
			const int first = wi * 32 + lo, n = hi - lo + 1;
			float rows[32 * 4];
			for (int r = 0; r < n; r++) xfRow(first + r, &rows[r * 4]);
			emitConsts(true, first, rows, n);
			memcpy(&s_vcShadow[first * 4], rows, size_t(n) * 16);
			bits &= hi == 31 ? 0u : ~0u << (hi + 1);
		}
	}
	TbClock tb(TB_CONSTS);  // inside flushBatch's TB_XF, which [perf] takes it off
	// The registers the other constants come from, in groups of constant rows: a group whose
	// registers are unchanged since the last draw (most draws only change matrices, some a colour
	// or the matrix indices) is neither built nor compared
	struct Src {
		uint32_t proj[7], viewport[6];  // G_VIEW
		XMap xmap;
		uint32_t chan[4];                     // G_CHAN
		uint32_t matIdx[2];                   // G_MTX
		uint32_t tevReg[8], kreg[8];          // G_TEV
		uint32_t genMode, tref[8], ssize[16]; // G_TEX
		bool manual[8];
		float texW[8], texH[8];
		uint32_t alpha, fog[5];               // G_FOG
		uint32_t indMtx[9];                   // G_IND
	};
	enum { G_VIEW, G_CHAN, G_MTX, G_TEV, G_TEX, G_FOG, G_IND, G_COUNT };
	static const size_t groupAt[G_COUNT + 1] = {offsetof(Src, proj), offsetof(Src, chan), offsetof(Src, matIdx),
	                                            offsetof(Src, tevReg), offsetof(Src, genMode), offsetof(Src, alpha),
	                                            offsetof(Src, indMtx), sizeof(Src)};
	static Src s_constSrc;
	Src src;
	memset(&src, 0, sizeof src);
	memcpy(src.proj, &g.xfReg[XFR_PROJ], sizeof src.proj);
	memcpy(src.viewport, &g.xfReg[XFR_VIEWPORT], sizeof src.viewport);
	src.xmap = s_xmap;
	src.chan[0] = g.xfReg[XFR_AMB0];
	src.chan[1] = g.xfReg[XFR_AMB0 + 1];
	src.chan[2] = g.xfReg[XFR_MAT0];
	src.chan[3] = g.xfReg[XFR_MAT0 + 1];
	src.matIdx[0] = g.xfReg[XFR_MATIDX_A];
	src.matIdx[1] = g.xfReg[XFR_MATIDX_B];
	memcpy(src.tevReg, &g.bp[BP_TEV_REG], sizeof src.tevReg);
	memcpy(src.kreg, g.kreg, sizeof src.kreg);
	src.genMode = g.bp[BP_GENMODE];
	memcpy(src.tref, &g.bp[BP_TREF], sizeof src.tref);
	memcpy(src.ssize, &g.bp[BP_SU_SSIZE], sizeof src.ssize);
	memcpy(src.manual, g.tcManual, sizeof src.manual);
	memcpy(src.texW, texW, sizeof src.texW);
	memcpy(src.texH, texH, sizeof src.texH);
	src.alpha = g.bp[BP_ALPHACOMPARE];
	src.fog[0] = g.bp[BP_FOG0];
	src.fog[1] = g.bp[BP_FOG1];
	src.fog[2] = g.bp[BP_FOG2];
	src.fog[3] = g.bp[BP_FOG3];
	src.fog[4] = g.bp[BP_FOG_COLOR];
	memcpy(src.indMtx, &g.bp[BP_IND_MTX], sizeof src.indMtx);
	bool changed[G_COUNT];
	bool any = false;
	for (int i = 0; i < G_COUNT; i++) {
		changed[i] = !s_constSrcValid || !sameWords((const char*)&src + groupAt[i], (const char*)&s_constSrc + groupAt[i],
		                                        groupAt[i + 1] - groupAt[i]);
		any |= changed[i];
	}
	if (!any) return;
	s_constSrc = src;
	if (changed[G_VIEW]) {
		float* p = vc;
		const float proj[8] = {xff(XFR_PROJ), xff(XFR_PROJ + 1), xff(XFR_PROJ + 2), xff(XFR_PROJ + 3),
		                       xff(XFR_PROJ + 4), xff(XFR_PROJ + 5), float(g.xfReg[XFR_PROJ + 6] & 1), 0.0f};
		memcpy(p, proj, sizeof proj);
		p += 8;
		float vp[8] = {xff(XFR_VIEWPORT), xff(XFR_VIEWPORT + 1), xff(XFR_VIEWPORT + 2), 0.0f,
		               xff(XFR_VIEWPORT + 3), xff(XFR_VIEWPORT + 4), xff(XFR_VIEWPORT + 5), 0.0f};
		if (s_wideW != EFB_W) {  // the draw's widescreen mapping (centre and half-width, clip x scale)
			vp[4] = (vp[4] - 342.0f) * s_xmap.a + s_xmap.b + 342.0f;
			vp[0] *= s_xmap.a * s_xmap.clip;
		}
		memcpy(p, vp, sizeof vp);
		p += 8;
		p[0] = float(s_wideW);
		p[1] = float(EFB_H);
		p[2] = p[3] = 0.0f;
		setConsts(true, VC_PROJ, vc, VC_CHAN - VC_PROJ);
	}
	if (changed[G_CHAN]) {
		float* p = vc;
		for (int i = 0; i < 4; i++, p += 4) {
			uint32_t v = src.chan[i];
			p[0] = float((v >> 24) & 255) / 255.0f;
			p[1] = float((v >> 16) & 255) / 255.0f;
			p[2] = float((v >> 8) & 255) / 255.0f;
			p[3] = float(v & 255) / 255.0f;
		}
		setConsts(true, VC_CHAN, vc, VC_DEFMTX - VC_CHAN);
	}
	if (changed[G_MTX]) {
		uint32_t matA = src.matIdx[0], matB = src.matIdx[1];
		const uint32_t def[12] = {matA & 63, (matA >> 6) & 63, (matA >> 12) & 63, (matA >> 18) & 63,
		                          (matA >> 24) & 63, matB & 63, (matB >> 6) & 63, (matB >> 12) & 63,
		                          (matB >> 18) & 63, 0, 0, 0};
		for (int i = 0; i < 12; i++) vc[i] = float(def[i]);
		setConsts(true, VC_DEFMTX, vc, VC_COUNT - VC_DEFMTX);
	}

	float pc[PC_COUNT * 4];
	if (changed[G_TEV]) {
		for (int i = 0; i < 4; i++) {
			uint32_t ra = g.bp[BP_TEV_REG + 2 * i], bg = g.bp[BP_TEV_REG + 2 * i + 1];
			float* o = &pc[(PC_TEVREG + i) * 4];
			o[0] = sext11(ra & 0x7FF);
			o[1] = sext11((bg >> 12) & 0x7FF);
			o[2] = sext11(bg & 0x7FF);
			o[3] = sext11((ra >> 12) & 0x7FF);
			ra = g.kreg[2 * i];
			bg = g.kreg[2 * i + 1];
			o = &pc[(PC_KONST + i) * 4];
			o[0] = float(ra & 0xFF);
			o[1] = float((bg >> 12) & 0xFF);
			o[2] = float(bg & 0xFF);
			o[3] = float((ra >> 12) & 0xFF);
		}
		setConsts(false, PC_TEVREG, &pc[PC_TEVREG * 4], PC_TEXSCALE - PC_TEVREG);
	}
	if (changed[G_TEX]) {
		memset(&pc[PC_TEXSCALE * 4], 0, (PC_ALPHAREF - PC_TEXSCALE) * 16);
		// manual texcoord scaling: coordinates in units of the given size
		for (int c = 0; c < 8; c++) pc[(PC_TEXSCALE + c) * 4] = pc[(PC_TEXSCALE + c) * 4 + 1] = 1.0f;
		uint32_t nst = ((g.bp[BP_GENMODE] >> 10) & 15) + 1;
		for (uint32_t st = 0; st < nst; st++) {
			uint32_t ord = (g.bp[BP_TREF + (st >> 1)] >> ((st & 1) * 12)) & 0x3FF;
			uint32_t map = ord & 7, coord = (ord >> 3) & 7;
			if (g.tcManual[coord] && texW[map] > 0) {
				pc[(PC_TEXSCALE + coord) * 4] = float((g.bp[BP_SU_SSIZE + 2 * coord] & 0xFFFF) + 1) / texW[map];
				pc[(PC_TEXSCALE + coord) * 4 + 1] = float((g.bp[BP_SU_SSIZE + 2 * coord + 1] & 0xFFFF) + 1) / texH[map];
			}
		}
		for (int m = 0; m < 8; m++) {
			pc[(PC_TEXSIZE + m) * 4] = texW[m] > 0 ? texW[m] : 1.0f;
			pc[(PC_TEXSIZE + m) * 4 + 1] = texH[m] > 0 ? texH[m] : 1.0f;
		}
		setConsts(false, PC_TEXSCALE, &pc[PC_TEXSCALE * 4], PC_ALPHAREF - PC_TEXSCALE);
	}
	if (changed[G_FOG]) {
		memset(&pc[PC_ALPHAREF * 4], 0, 16);
		uint32_t ac = g.bp[BP_ALPHACOMPARE];
		pc[PC_ALPHAREF * 4] = float(ac & 0xFF);
		pc[PC_ALPHAREF * 4 + 1] = float((ac >> 8) & 0xFF);
		uint32_t f0 = g.bp[BP_FOG0], f1 = g.bp[BP_FOG1], f2 = g.bp[BP_FOG2], f3 = g.bp[BP_FOG3];
		struct F {
			static float fl(uint32_t r)
			{
				uint32_t bits = ((r >> 19) & 1) << 31 | ((r >> 11) & 0xFF) << 23 | (r & 0x7FF) << 12;
				float f;
				memcpy(&f, &bits, 4);
				return f;
			}
		};
		int bs = int(f2 & 31);
		float* o = &pc[PC_FOG * 4];
		o[0] = ldexpf(F::fl(f0), bs);
		o[1] = ldexpf(float(f1 & 0xFFFFFF) / 8388638.0f, bs - 1);
		o[2] = F::fl(f3);
		o[3] = float((f3 >> 20) & 1);
		uint32_t fc = g.bp[BP_FOG_COLOR];
		o = &pc[PC_FOGCOLOR * 4];
		o[0] = float((fc >> 16) & 255) / 255.0f;
		o[1] = float((fc >> 8) & 255) / 255.0f;
		o[2] = float(fc & 255) / 255.0f;
		o[3] = 1.0f;
		setConsts(false, PC_ALPHAREF, &pc[PC_ALPHAREF * 4], PC_INDMTX - PC_ALPHAREF);
	}
	if (changed[G_IND]) {
		for (int m = 0; m < 3; m++) {
			uint32_t r0 = g.bp[BP_IND_MTX + 3 * m], r1 = g.bp[BP_IND_MTX + 3 * m + 1], r2 = g.bp[BP_IND_MTX + 3 * m + 2];
			int sc = int(((r0 >> 22) & 3) | ((r1 >> 22) & 3) << 2 | ((r2 >> 22) & 3) << 4) - 17;
			float* row0 = &pc[(PC_INDMTX + 2 * m) * 4];
			float* row1 = row0 + 4;
			row0[0] = sext11(r0 & 0x7FF) / 1024.0f;
			row1[0] = sext11((r0 >> 11) & 0x7FF) / 1024.0f;
			row0[1] = sext11(r1 & 0x7FF) / 1024.0f;
			row1[1] = sext11((r1 >> 11) & 0x7FF) / 1024.0f;
			row0[2] = sext11(r2 & 0x7FF) / 1024.0f;
			row1[2] = sext11((r2 >> 11) & 0x7FF) / 1024.0f;
			row0[3] = float(sc);
			row1[3] = 0.0f;
		}
		setConsts(false, PC_INDMTX, &pc[PC_INDMTX * 4], PC_COUNT - PC_INDMTX);
	}
	s_constSrcValid = true;
}

static void resetBatch()
{
	s_bdata.clear();
	s_bidx.clear();
	s_segs.clear();
	s_bcount = 0;
}

// SMS_XE_TRACE_FRAME=n: every batch and copy of display frame n in the log
bool xe::tracing()
{
	static int frame = -2;
	if (frame == -2) {
		const char* e = getenv("SMS_XE_TRACE_FRAME");
		frame = e ? atoi(e) : -1;
	}
	return frame >= 0 && g_displayFrames == uint32_t(frame);
}

// Batches go through two rings, of vertices and of 16-bit indices, in buffers of our own: an UP
// draw copies its data into the command ring buffer, and one of 860 KB (Pinna's beach, 31503
// vertices) left D3D waiting for room there for ever. Each ring is four quarters; entering a
// quarter waits for the fence of the last draw that read it, a lap ago. The console's Lock locks
// a whole buffer (no ranges) and may not while it is set on the device, so the buffer's memory is
// taken once and written directly (write-combined: XMemCpyStreaming_WriteCombined).
struct Ring {
	enum { kSize = 16 << 20, kQuarter = kSize / 4 };
	IDirect3DVertexBuffer9* vb;
	IDirect3DIndexBuffer9* ib;
	uint8_t* mem;
	UINT at;
	uint32_t fence[4];  // fence tokens
	bool touched[4];
	bool init(bool index)
	{
		at = 0;
		memset(fence, 0, sizeof fence);
		memset(touched, 0, sizeof touched);
		void* p = NULL;
		if (index) {
			if (FAILED(dev->CreateIndexBuffer(kSize, 0, D3DFMT_INDEX16, 0, &ib, NULL))) return false;
			ib->Lock(0, 0, &p, 0);
			ib->Unlock();
		} else {
			if (FAILED(dev->CreateVertexBuffer(kSize, 0, 0, 0, &vb, NULL))) return false;
			vb->Lock(0, 0, &p, 0);
			vb->Unlock();
		}
		mem = (uint8_t*)p;
		return mem != NULL;
	}
	// Copies n bytes in, 16-byte aligned; returns their offset. n <= kSize.
	UINT put(const void* data, UINT n)
	{
		UINT off = (at + 15) / 16 * 16;
		if (off + n > kSize) off = 0;
		for (UINT q = off / kQuarter; q <= (off + n - 1) / kQuarter; q++)
			if (q != (at ? (at - 1) / kQuarter : 4) || off == 0) {  // entering it: its last reader is done
				// one fence for the quarters written since the last: every draw that reads them was
				// emitted before this put (a fence a draw made D3D start a command segment for each,
				// and it stalled for free ones: ~6 ms a Delfino Plaza frame on the console)
				uint32_t f = 0;
				for (int t = 0; t < 4; t++)
					if (touched[t]) {
						if (!f) f = fenceToken();
						fence[t] = f;
						touched[t] = false;
					}
				if (fence[q]) {
					s_ringWaits++;
					waitToken(fence[q]);
				}
				fence[q] = 0;
			}
		XMemCpyStreaming_WriteCombined(mem + off, data, n);
		for (UINT q = off / kQuarter; q <= (off + n - 1) / kQuarter; q++) touched[q] = true;
		at = off + n;
		return off;
	}
};
static Ring s_vring, s_iring;

// A batch's horizontal extent on screen (game x), from its positions, position matrices and the
// projection and viewport (gx_render.cpp's).
static void screenExtent(float sx, float cx, float* outLo, float* outHi)
{
	const VtxFmtLayout& l = vtxFmtLayout(s_bfmt);
	const float p0 = xff(XFR_PROJ), p1 = xff(XFR_PROJ + 1);
	const bool ortho = (g.xfReg[XFR_PROJ + 6] & 1) != 0;
	const uint32_t defIdx = g.xfReg[XFR_MATIDX_A] & 63;
	float lo = 1e30f, hi = -1e30f;
	// the streamed vertices, then the arena runs'
	size_t seg = 0;
	const uint8_t* run = s_bcount ? &s_bdata[0] : NULL;
	uint32_t runVerts = s_bcount;
	for (uint32_t v = 0;; v++) {
		while (v >= runVerts) {
			while (seg < s_segs.size() && !s_segs[seg].arena) seg++;
			if (seg == s_segs.size()) break;
			run = s_segs[seg].cpu;
			runVerts = s_segs[seg].verts;
			seg++;
			v = 0;
		}
		if (v >= runVerts) break;
		const uint8_t* vx = run + size_t(v) * s_bstride;
		float pos[3];
		memcpy(pos, vx, 12);
		const uint32_t idx = (s_bfmt & VF_MTX) ? (vx[l.mtx] & 63) : defIdx;
		float m[12];
		memcpy(m, &g.xfMem[idx * 4], 48);
		const float x = m[0] * pos[0] + m[1] * pos[1] + m[2] * pos[2] + m[3];
		float X;
		if (ortho) {
			X = cx + sx * (p0 * x + p1);
		} else {
			const float z = m[8] * pos[0] + m[9] * pos[1] + m[10] * pos[2] + m[11];
			if (z > -1e-6f) continue;  // behind the eye
			X = cx + sx * (p0 * x + p1 * z) / -z;
		}
		lo = X < lo ? X : lo;
		hi = X > hi ? X : hi;
	}
	*outLo = lo;
	*outHi = hi;
}

// A centring offset moved to the nearest whole pixel of the scaled EFB (a centred draw's edges
// on pixel centres made copies of it, Delfino's goop, creep a pixel a frame upstream).
static float pixelAligned(float b)
{
	if (b == 0.0f) return b;
	const float px = float(s_efbW) / float(s_wideW);
	return floorf(b * px + 0.5f) / px;
}

// How the current batch's game coordinates map into the widened EFB (gx_render.cpp's drawXMap).
static XMap drawXMap()
{
	XMap m = {1, 0, 1};
	if (s_wideW == EFB_W) return m;
	const bool ortho = (g.xfReg[XFR_PROJ + 6] & 1) != 0;
	if (s_stretch2D && ortho) {  // a fade or wipe: all of it across the frame
		m.a = s_stretch;
		return m;
	}
	if (s_wide2D && ortho) {  // laid out wide: the 4:3 scale, centred
		m.b = pixelAligned(float(s_ox));
		return m;
	}
	const float sx = xff(XFR_VIEWPORT), cx = xff(XFR_VIEWPORT + 3) - 342.0f;
	const bool fullWidth = fabsf(fabsf(sx) * 2.0f - float(EFB_W)) < 2.0f && fabsf(cx - float(EFB_W) / 2) < 2.0f;
	if (!fullWidth) {
		m.b = pixelAligned(centredOffset(cx));
		return m;
	}
	if (!ortho) {
		if (s_hud) {  // a 3D part of the HUD (the water tank): moved like the rest
			float lo, hi;
			screenExtent(sx, cx, &lo, &hi);
			if (lo <= hi) m.b = pixelAligned(centredOffset((lo + hi) / 2));
			return m;
		}
		m.a = s_stretch;
		const float p00 = xff(XFR_PROJ), p11 = xff(XFR_PROJ + 2);
		const float aspect = p00 != 0.0f ? fabsf(p11 / p00) : 0.0f;
		if (aspect < kCamAspect * sqrtf(s_wide)) m.clip = 1.0f / s_stretch;  // not widened by the game
		return m;
	}
	// 2D across the whole width: artwork (menus, the map, movies: colour from an image) stays
	// centred; fades, masks and passes over the frame (untextured, a tiny utility texture, alpha
	// only, or drawing screen copies back) are stretched
	bool artwork = false;
	const bool colour = (g.bp[BP_CMODE0] >> 3) & 1;
	const uint32_t nst = ((g.bp[BP_GENMODE] >> 10) & 15) + 1;
	for (uint32_t st = 0; colour && st < nst && !artwork; st++) {
		const uint32_t ord = (g.bp[BP_TREF + (st >> 1)] >> ((st & 1) * 12)) & 0x3FF;
		if (!((ord >> 6) & 1)) continue;
		const int map = int(ord & 7);
		const uint32_t img0 = g.bp[bpTexReg(BP_TX_IMAGE0, map)];
		const uint32_t tw = (img0 & 0x3FF) + 1, th = ((img0 >> 10) & 0x3FF) + 1;
		if (g.texImage[map] && tw >= 32 && th >= 32 && !isCopyTexture(g.texImage[map])) artwork = true;
	}
	float lo, hi;
	screenExtent(sx, cx, &lo, &hi);
	const bool spans = lo <= 2.0f && hi >= float(EFB_W) - 2.0f;
	if (spans && !artwork) m.a = s_stretch;
	else m.b = pixelAligned(spans ? float(s_ox) : centredOffset((lo + hi) / 2));
	return m;
}

void flushBatch()
{
	// The DSP's reply, if its subframe is done: the audio thread runs at once (an interrupt on the
	// GameCube), not after the frame's drawing, which calls no OS function (on the render thread the
	// game's thread does it: xe_gx_queue.cpp; the game's OS runs on that thread alone)
	if (!xeGxThreaded() && port_audio_dsp_reply_ready()) port_irq_check();
	OwnClock clock(g_flushSeconds);
	if (batchEmpty() || !s_ready) {
		resetBatch();
		return;
	}
	// SMS_XE_STOP=n: draw only the first n batches of every frame (what each one adds)
	// SMS_XE_ONLY=n: only batch n of every frame
	static int stopAt = -2, only = -1;
	static uint32_t frameDraws, drawsFrame;
	if (stopAt == -2) {
		const char* e = getenv("SMS_XE_STOP");
		stopAt = e ? atoi(e) : -1;
		e = getenv("SMS_XE_ONLY");
		only = e ? atoi(e) : -1;
	}
	if (drawsFrame != g_displayFrames) drawsFrame = g_displayFrames, frameDraws = 0;
	const uint32_t drawNo = frameDraws++;
	if ((stopAt >= 0 && drawNo >= uint32_t(stopAt)) || (only >= 0 && drawNo != uint32_t(only))) {
		resetBatch();
		return;
	}
	TbClock tb(TB_STATE);
	s_xmap = drawXMap();
	tb.next(TB_PROGRAM);
	const Program* prog = programFor(s_bfmt);
	tb.next(TB_STATE);
	bool drawn = prog && applyState();
	if (tracing()) {
		static const uint8_t none[48] = {0};
		const uint8_t* v0 = s_bcount ? &s_bdata[0] : s_segs[0].cpu ? s_segs[0].cpu : none;
		const float* v = (const float*)v0;
		logmsg("draw fmt %04x verts %u idx %u cls %d prog %d %s zmode %x cmode %03x gen %06x cull %u v0 (%g %g %g) proj %g %g %g %g %g %g %u vp %g %g %g %g %g %g mtx %08x",
		       s_bfmt, s_bcount, unsigned(s_bidx.size()), int(s_bclass), prog ? prog->id : -1, drawn ? "" : "SKIPPED",
		       g.bp[BP_ZMODE] & 0xFFFFFF, g.bp[BP_CMODE0] & 0xFFF, g.bp[BP_GENMODE] & 0xFFFFFF, (g.bp[BP_GENMODE] >> 14) & 3,
		       v[0], v[1], v[2], xff(XFR_PROJ), xff(XFR_PROJ + 1), xff(XFR_PROJ + 2), xff(XFR_PROJ + 3), xff(XFR_PROJ + 4),
		       xff(XFR_PROJ + 5), g.xfReg[XFR_PROJ + 6], xff(XFR_VIEWPORT), xff(XFR_VIEWPORT + 1), xff(XFR_VIEWPORT + 2),
		       xff(XFR_VIEWPORT + 3), xff(XFR_VIEWPORT + 4), xff(XFR_VIEWPORT + 5), g.xfReg[XFR_MATIDX_A]);
		{
			char hex[3 * 48 + 1];
			uint32_t n = s_bstride < 48 ? s_bstride : 48;
			for (uint32_t i = 0; i < n; i++) _snprintf(hex + 3 * i, 4, "%02x ", v0[i]);
			hex[3 * n] = 0;
			logmsg("  v0 bytes (stride %u): %s", s_bstride, hex);
		}
		logmsg("  tref %06x %06x cenv0 %06x aenv0 %06x ksel %06x numchans %u chan0 %x", g.bp[BP_TREF] & 0xFFFFFF,
		       g.bp[BP_TREF + 1] & 0xFFFFFF, g.bp[BP_TEV_COLOR_ENV] & 0xFFFFFF, g.bp[BP_TEV_COLOR_ENV + 1] & 0xFFFFFF,
		       g.bp[BP_TEV_KSEL] & 0xFFFFFF, g.xfReg[XFR_NUMCHANS], g.xfReg[XFR_COLOR0CTRL]);
		const uint8_t* a = g.arrayBase[0];
		logmsg("  vcd %05x/%03x vat0 %08x array0 %p stride %u be %d bytes %02x %02x %02x %02x %02x %02x %02x %02x", g.cpVcdLo,
		       g.cpVcdHi, g.cpVatA[0], a, g.arrayStride[0], int(g.arrayBigEndian[0]), a ? a[0] : 0, a ? a[1] : 0,
		       a ? a[2] : 0, a ? a[3] : 0, a ? a[4] : 0, a ? a[5] : 0, a ? a[6] : 0, a ? a[7] : 0);
	}
	if (!drawn) {
		resetBatch();
		return;
	}
	float texW[8] = {0}, texH[8] = {0};
	uint32_t used = 0;
	uint32_t gen = g.bp[BP_GENMODE];
	uint32_t nst = ((gen >> 10) & 15) + 1, nind = (gen >> 16) & 7;
	for (uint32_t st = 0; st < nst; st++) {
		uint32_t ord = (g.bp[BP_TREF + (st >> 1)] >> ((st & 1) * 12)) & 0x3FF;
		if ((ord >> 6) & 1) used |= 1u << (ord & 7);
	}
	for (uint32_t i = 0; i < nind && i < 4; i++) used |= 1u << ((g.bp[BP_RAS1_IREF] >> (6 * i)) & 7);
	tb.next(TB_TEXTURES);
	for (int m = 0; m < 8; m++)
		if (used & (1u << m)) bindTexture(m, &texW[m], &texH[m]);
	tb.next(TB_XF);
	uploadConstants(texW, texH);
	tb.next(TB_SUBMIT);
	emitDrawState(s_ds);
	DrawArgs da = {declFor(s_bfmt), prog->vs, prog->ps, s_vring.vb, s_iring.ib};
	if (debugBits() & 32) {  // geometry only: white
		da.ps = NULL;
		forgetConsts(false, 0, 1);
	}
	da.type = s_bclass == PRIM_TRIS ? D3DPT_TRIANGLELIST : s_bclass == PRIM_LINES ? D3DPT_LINELIST : D3DPT_POINTLIST;
	da.stride = s_bstride;
	const UINT per = s_bclass == PRIM_TRIS ? 3 : s_bclass == PRIM_LINES ? 2 : 1;
	// no lines or points (SMS_XE_DEBUG 32768): are they the stray lines?
	const bool skip = (debugBits() & 32768) && s_bclass != PRIM_TRIS;
	UINT streamVoff = 0, streamIoff = 0;
	if (!s_bidx.empty() && !skip) {
		streamVoff = s_vring.put(&s_bdata[0], s_bcount * s_bstride);
		streamIoff = s_iring.put(&s_bidx[0], UINT(s_bidx.size()) * 2) / 2;
	}
	UINT prims = 0;
	for (size_t i = 0; i < s_segs.size() && !skip; i++) {
		const Seg& sg = s_segs[i];
		da.prims = sg.count / per;
		if (!da.prims) continue;
		if (sg.arena) {
			da.vb = s_arena.vb;
			da.ib = s_arena.ib;
			da.voff = sg.voff;
			da.firstIndex = sg.firstIndex;
			da.verts = sg.verts;
		} else {
			da.vb = s_vring.vb;
			da.ib = s_iring.ib;
			da.voff = streamVoff;
			da.firstIndex = streamIoff + sg.first;
			da.verts = s_bcount;
		}
		emitDraw(da);
		prims += da.prims;
		s_statDraws++;
		s_drawsTotal++;
	}
	s_efbGen++;
	if (s_pixActive && s_bclass == PRIM_TRIS) s_pixTris += prims;
	resetBatch();
}

// ------------------------------------------------------------------ EFB copies
// EFB pixels to clip space
static float clipX(float x) { return x * (2.0f / float(s_wideW)) - 1.0f; }  // widened game x
static float clipY(float y) { return 1.0f - y * (2.0f / EFB_H); }

// A copy's clear: a quad over the rectangle that obeys the PE's write enables, as the
// hardware's does (gx_render.cpp's clearRect: SMS's RGBA6 shadows keep the EFB's alpha).
static void clearRect(float x, int y, float w, int h)  // x, w: widened game x (copyEfb's)
{
	uint32_t ar = g.bp[BP_CLEAR_AR], gb = g.bp[BP_CLEAR_GB], z = g.bp[BP_CLEAR_Z];
	bool hasAlpha = (g.bp[BP_PE_CONTROL] & 7) == 1;
	bool colour = (g.bp[BP_CMODE0] >> 3) & 1;
	bool alpha = hasAlpha && ((g.bp[BP_CMODE0] >> 4) & 1);
	bool depth = (g.bp[BP_ZMODE] >> 4) & 1;
	const ClearArgs a = {clipX(x), clipY(float(y)), clipX(x + w), clipY(float(y + h)), float(z & 0xFFFFFF) / 16777215.0f,
	                     {float(ar & 255) / 255.0f, float((gb >> 8) & 255) / 255.0f, float(gb & 255) / 255.0f,
	                      hasAlpha ? float((ar >> 8) & 255) / 255.0f : 1.0f},
	                     DWORD((colour ? 7 : 0) | (alpha ? 8 : 0)), depth};
	emitClear(a);
	s_efbGen++;
}

// A w x h texture that Resolve wrote (tiled, 0xAARRGGBB texels) as RGBA rows, top first. The
// GPU must be done with it.
static double s_wbFlush;  // [perf]: readTiled's cache flush
static void readTiled(const uint32_t* t, int w, int h, uint8_t* rgba)
{
	const UINT width = UINT(w + 31) & ~31u;
	// the GPU wrote memory the CPU may hold stale lines of: drop them
	const uint32_t size = width * ((UINT(h) + 31) & ~31u) * 4;
	const double t0 = monoSeconds();
	for (uint32_t at = 0; at < size; at += 128) __dcbf(int(at), t);
	__sync();
	s_wbFlush += monoSeconds() - t0;
	// untiled in blocks (XGAddress2DTiledOffset for each texel was most of a write-back's time),
	// then 0xAARRGGBB -> R, G, B, A bytes in place
	const RECT rect = {0, 0, w, h};
	XGUntileSurface(rgba, UINT(w) * 4, NULL, t, width, (UINT(h) + 31) & ~31u, &rect, 4);
	uint32_t* p = (uint32_t*)rgba;
	for (size_t i = 0, n = size_t(w) * h; i < n; i++) p[i] = p[i] << 8 | p[i] >> 24;
}

static bool readTexture(IDirect3DTexture9* tex, int w, int h, uint8_t* rgba)
{
	D3DLOCKED_RECT r;
	if (FAILED(tex->LockRect(0, &r, NULL, D3DLOCK_READONLY))) return false;
	readTiled((const uint32_t*)r.pBits, w, h, rgba);
	tex->UnlockRect(0);
	return true;
}

// ------------------------------------------------------------------ copy write-back
// A texture copy is stored in RAM too, in its GX layout (gx_render.cpp's writeBackCopy): the
// goop's pollution maps are read there. Read back one frame later, when the GPU is long done
// with the Resolve, or before the next copy to the same place; dropped when a newer copy of the
// same frame supersedes it or the CPU changed the bytes meanwhile. SMS_GX_COPY_WRITEBACK=0: off.
struct WriteBack {
	const void* dest;
	IDirect3DTexture9* tex;  // a reference of its own
	struct Readback* rb;  // the same Resolve in CPU-cached memory, read here
	struct Readback* check;  // SMS_XE_WBVERIFY: the copy as the CPU path reads it, to compare
	bool encoded;  // rb holds the GPU's encoding (kEncodeI8Ps), 256 texels a row
	int w, h;
	uint32_t layout, bytes, frame;
	uint64_t hash;
	uint32_t token;  // the fence after its Resolves
};
static std::vector<WriteBack> s_writeBacks;

// Where write-backs are read: textures D3D makes are write-combined (~175 ns a 4-byte read: a
// 320x224 copy took 12 ms on the console), so the copy is resolved a second time into memory of
// our own, CPU-cached, under a texture header. Reused by size.
// ponytail: never freed (a few per distinct copy size); free them if sizes ever churn.
struct Readback {
	IDirect3DTexture9 tex;
	void* mem;
	int w, h;
	bool linear;  // LIN_A8R8G8B8 rows (GX-encoded bytes), else tiled
};
static std::vector<Readback*> s_readbacks;  // free ones

static Readback* takeReadback(int w, int h, bool linear)
{
	for (size_t i = 0; i < s_readbacks.size(); i++)
		if (s_readbacks[i]->w == w && s_readbacks[i]->h == h && s_readbacks[i]->linear == linear) {
			Readback* rb = s_readbacks[i];
			s_readbacks.erase(s_readbacks.begin() + i);
			return rb;
		}
	Readback* rb = new Readback;
	UINT base = 0;
	XGSetTextureHeader(w, h, 1, 0, linear ? D3DFMT_LIN_A8R8G8B8 : D3DFMT_A8R8G8B8, 0, 0, 0, 0, &rb->tex, &base, NULL);
	rb->mem = XPhysicalAlloc(base, MAXULONG_PTR, 4096, PAGE_READWRITE);
	if (!rb->mem) {
		logmsg("copy write-back %dx%d: out of memory", w, h);
		delete rb;
		return NULL;
	}
	XGOffsetBaseTextureAddress(&rb->tex, rb->mem, rb->mem);
	logmsg("copy write-back %dx%d: readback at %p, %u bytes", w, h, rb->mem, base);
	rb->w = w;
	rb->h = h;
	rb->linear = linear;
	return rb;
}

// 0 none, 1 intensity copies only (default), 2 all (SMS_GX_COPY_WRITEBACK=0 / all)
static int writeBackMode()
{
	static int mode = -1;
	if (mode < 0) {
		const char* e = getenv("SMS_GX_COPY_WRITEBACK");
		mode = !e ? 1 : e[0] == '0' ? 0 : !strcmp(e, "all") ? 2 : 1;
	}
	return mode;
}
static bool writeBackOn() { return writeBackMode() != 0; }

static uint32_t s_tsnapToken;  // threaded: the last peek snapshot's fence, waited for at the display copy
static double s_wbHash, s_wbRead, s_wbEncode, s_wbStore, s_wbBytes, s_wbCount;  // [perf]
static double s_wbTotal;  // write-backs since the start (the bench)

// The copy's bytes into RAM, if the CPU has not changed them since (the GPU is done with it). On the
// render thread's behalf the game's thread does this (handed over at the render thread's display
// copy, stored at the game's next one), and the memory-written stamp goes through the queue.
// Threaded, the copy's hash (taken on the render thread) may come from before, or during, the store
// of the frame before's write-back to the same place: the bytes that store left count as unchanged too.
static std::unordered_map<const void*, uint64_t> s_wbStored;  // the hash of the bytes last stored there
static void storeWriteBackBytes(const WriteBack& wb)
{
	double t = monoSeconds();
	const uint64_t now = hashBytes(wb.dest, wb.bytes);
	std::unordered_map<const void*, uint64_t>::const_iterator st = s_wbStored.find(wb.dest);
	const bool same = now == wb.hash || (st != s_wbStored.end() && now == st->second);
	s_wbHash += monoSeconds() - t;
	if (same && wb.rb) {
		static std::vector<uint8_t> px, enc;
		const uint8_t* bytes;
		{
			t = monoSeconds();
			double t1;
			if (wb.encoded && wb.rb->linear) {  // drop stale cache lines, use the bytes as they are
				for (uint32_t at = 0; at < wb.bytes; at += 128) __dcbf(int(at), wb.rb->mem);
				__sync();
				bytes = (const uint8_t*)wb.rb->mem;
				t1 = monoSeconds();
			} else if (wb.encoded) {  // tiled: only untiled (texels are 4 bytes already in order)
				enc.resize(wb.bytes);
				const UINT rows = wb.bytes / 1024;
				const uint32_t size = 1024 * ((rows + 31) & ~31u);
				for (uint32_t at = 0; at < size; at += 128) __dcbf(int(at), wb.rb->mem);
				__sync();
				const RECT rect = {0, 0, 256, LONG(rows)};
				XGUntileSurface(&enc[0], 1024, NULL, wb.rb->mem, 256, (rows + 31) & ~31u, &rect, 4);
				bytes = &enc[0];
				t1 = monoSeconds();
			} else {
				px.resize(size_t(wb.w) * wb.h * 4);
				enc.resize(wb.bytes);
				readTiled((const uint32_t*)wb.rb->mem, wb.w, wb.h, &px[0]);
				t1 = monoSeconds();
				encodeTexture(&px[0], wb.layout, uint32_t(wb.w), uint32_t(wb.h), &enc[0]);
				bytes = &enc[0];
			}
			if (wb.check) {  // the GPU's encoding against the CPU's, of the same copy
				px.resize(size_t(wb.w) * wb.h * 4);
				enc.resize(wb.bytes);
				readTiled((const uint32_t*)wb.check->mem, wb.w, wb.h, &px[0]);
				encodeTexture(&px[0], wb.layout, uint32_t(wb.w), uint32_t(wb.h), &enc[0]);
				uint32_t bad = 0, first = 0;
				for (uint32_t i = 0; i < wb.bytes; i++)
					if (bytes[i] != enc[i] && !bad++) first = i;
				static int lines = 50;
				if (lines > 0 && (bad || lines > 45)) {
					lines--;
					logmsg("write-back check %dx%d: %u of %u bytes differ (first at %u: gpu %u, cpu %u)", wb.w, wb.h,
					       bad, wb.bytes, first, bad ? bytes[first] : 0, bad ? enc[first] : 0);
				}
			}
			const double t2 = monoSeconds();
			s_wbRead += t1 - t;
			s_wbEncode += t2 - t1;
			s_wbBytes += wb.bytes;
			s_wbCount++;
			s_wbTotal++;
			static int logCopies = -1;  // SMS_GX_COPY_LOG=n: the first n write-backs, as upstream's
			if (logCopies < 0) logCopies = getenv("SMS_GX_COPY_LOG") ? atoi(getenv("SMS_GX_COPY_LOG")) : 0;
			if (logCopies > 0) {
				uint32_t changed = 0;
				for (uint32_t i = 0; i < wb.bytes; i++) changed += bytes[i] != ((const uint8_t*)wb.dest)[i];
				logmsg("copy write-back %p: %dx%d layout %u, %u bytes, %u changed", wb.dest, wb.w, wb.h, wb.layout, wb.bytes, changed);
				logCopies--;
			}
			t = monoSeconds();
			memcpy(const_cast<void*>(wb.dest), bytes, wb.bytes);
			s_wbStored[wb.dest] = hashBytes(bytes, wb.bytes);
			if (xeGxThreaded()) xeApiMemoryWritten(wb.dest, wb.bytes);
			else memoryWritten(wb.dest, wb.bytes);
			s_wbStore += monoSeconds() - t;
		}
	}
}

static void releaseWriteBack(const WriteBack& wb)
{
	wb.tex->Release();
	if (wb.rb) s_readbacks.push_back(wb.rb);
	if (wb.check) s_readbacks.push_back(wb.check);
}

static void storeWriteBack(const WriteBack& wb, bool cancelled)
{
	if (!cancelled) {
		waitToken(wb.token);
		storeWriteBackBytes(wb);
	}
	releaseWriteBack(wb);
}

// Threaded (xe_gx_queue.cpp): write-backs the render thread has finished, for the game's thread.
static CRITICAL_SECTION s_handoffLock;
static std::vector<WriteBack> s_handoff;
static void releaseOn(const void* p) { releaseWriteBack(*(const WriteBack*)p); }

// The game's thread, at its display copy: stores the handed-over write-backs of frames before
// `frame` (all of them are here once the render thread has shown `frame` frames).
void xeStoreWriteBacks(uint32_t frame)
{
	static std::vector<WriteBack> mine;
	EnterCriticalSection(&s_handoffLock);
	size_t keep = 0;
	for (size_t i = 0; i < s_handoff.size(); i++) {
		if (s_handoff[i].frame < frame) mine.push_back(s_handoff[i]);
		else s_handoff[keep++] = s_handoff[i];
	}
	s_handoff.resize(keep);
	LeaveCriticalSection(&s_handoffLock);
	for (size_t i = 0; i < mine.size(); i++) {
		storeWriteBackBytes(mine[i]);
		xeCorePostData(releaseOn, &mine[i], sizeof(WriteBack));
	}
	mine.clear();
}

// Before a copy to `dest`: its pending write-back is stored (from an earlier frame: the CPU
// may have read it since) or dropped (this frame's, superseded).
static void settleWriteBack(const void* dest)
{
	size_t keep = 0;
	if (xeGxThreaded()) {  // earlier frames' are handed over already: this frame's is superseded
		for (size_t i = 0; i < s_writeBacks.size(); i++) {
			if (s_writeBacks[i].dest != dest) s_writeBacks[keep++] = s_writeBacks[i];
			else releaseWriteBack(s_writeBacks[i]);
		}
		s_writeBacks.resize(keep);
		return;
	}
	for (size_t i = 0; i < s_writeBacks.size(); i++) {
		if (s_writeBacks[i].dest != dest) s_writeBacks[keep++] = s_writeBacks[i];
		else storeWriteBack(s_writeBacks[i], s_writeBacks[i].frame >= g_displayFrames);
	}
	s_writeBacks.resize(keep);
}

// At a display copy: stores the write-backs of the frames before this one.
static void storeWriteBacks()
{
	size_t keep = 0;
	if (xeGxThreaded()) {  // all of them, this frame's too, once the GPU is done: the game stores them
		if (s_writeBacks.empty()) return;
		for (size_t i = 0; i < s_writeBacks.size(); i++) waitToken(s_writeBacks[i].token);
		EnterCriticalSection(&s_handoffLock);
		s_handoff.insert(s_handoff.end(), s_writeBacks.begin(), s_writeBacks.end());
		LeaveCriticalSection(&s_handoffLock);
		s_writeBacks.clear();
		return;
	}
	for (size_t i = 0; i < s_writeBacks.size(); i++) {
		if (s_writeBacks[i].frame >= g_displayFrames) s_writeBacks[keep++] = s_writeBacks[i];
		else storeWriteBack(s_writeBacks[i], false);
	}
	s_writeBacks.resize(keep);
}

// Whether a copy goes back to RAM at all. The game reads back its goop maps (I8 copies of the
// pollution layers) and the depth of Bowser's bath (a Z24X8 copy); its colour copies (the screen
// texture, the mirror) feed the GPU alone. Writing those back cost ~15 ms a frame in Delfino on the
// console, so by default only intensity layouts (0-3) and depth copies go back.
// ponytail: a rule from the 20 xregress suites, not a proof; SMS_GX_COPY_WRITEBACK=all: every copy.
static bool writesBack(const void* dest, uint32_t layout, bool depth)
{
	return dest && writeBackOn() && !(writeBackMode() == 1 && layout > 3 && !depth);
}

// I8 in whole tiles and 1 KiB rows: encoded by the GPU (kEncodeI8Ps) in the copy pass
static bool gpuEncodes(int w, int h, uint32_t layout)
{
	return layout == 1 && w % 8 == 0 && h % 4 == 0 && (w * h) % 1024 == 0 && !(debugBits() & 16384);
}

// `encoded`: the GPU's encoding of the copy (copyEfb), else the scratch surface still holds the
// copy and is read here; `check` (SMS_XE_WBVERIFY=1): the copy too, to compare the two.
static void writeBack(const void* dest, IDirect3DTexture9* tex, int w, int h, uint32_t layout, bool depth,
                      Readback* encoded, Readback* check)
{
	if (!writesBack(dest, layout, depth)) return;
	const uint32_t bytes = texLevelBytes(layout, uint32_t(w), uint32_t(h));
	if (!bytes) return;
	Readback* rb = encoded;
	if (!rb && (rb = takeReadback(w, h, false)) != NULL) {
		RECT r = {0, 0, w, h};
		emitResolve(false, &r, &rb->tex);
	}
	copySetBytes(dest, bytes);
	tex->AddRef();
	WriteBack wb = {dest, tex, rb, check, encoded != NULL, w, h, layout, bytes, g_displayFrames, hashBytes(dest, bytes), fenceToken()};
	s_writeBacks.push_back(wb);
}

// [perf]: every 300 shown frames, where the time of a frame went, in ms (means over the 300):
// the game (the rest: its code, the GX core's command handling, everything else on its
// thread), GX vertex decoding, drawing (state, shaders, constants, batches), EFB copies,
// textures (checks, decodes, uploads), waits for the GPU (fences, peeks, metrics, the flip),
// idle (no thread had work: waiting for the next retrace). And the frame rate and the longest
// frame. SMS_XE_PERF=0: no line.
extern "C" double port_idle_seconds(void);
extern "C" volatile LONG g_xeCondSignals, g_xeCondWaits;
// port patch 16: the GX core's parts in timebase ticks, and the renderer's seconds inside each
enum { CP_OUT, CP_PARSE, CP_BP, CP_CP, CP_XF, CP_VERTS, CP_DL_CHECK, CP_DL_DRAW, CP_DL_RECORD, CP_COUNT };
extern unsigned long long g_corePartTicks[CP_COUNT];
extern double g_corePartRender[CP_COUNT];
static double tbSeconds()  // a timebase tick (the timebase runs at QueryPerformanceFrequency)
{
	static double s;
	if (!s) {
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		s = 1.0 / double(f.QuadPart);
	}
	return s;
}
static double corePartSeconds(int part) { return double(g_corePartTicks[part]) * tbSeconds() - g_corePartRender[part]; }
static double coreSeconds()  // the GX core's own time so far: all its parts
{
	double t = 0;
	for (int i = CP_OUT + 1; i < CP_COUNT; i++) t += corePartSeconds(i);
	return t;
}
extern "C" void xe_bench_frame(double frame, double game, double core, double render, double wait, double audio,
                               unsigned syncs, unsigned writeBacks);  // xenon/platform/xe_bench.cpp

// The renderer's time so far, for the GX core's clock (port patch 11): drawing, copies, textures,
// packets and GPU waits, which do not overlap.
double xeRenderSeconds() { return g_flushSeconds + s_copySeconds + g_xeTex + g_xeExec + g_xeWait; }

// Each frame's own split, for the bench: the frame, the game's part (the rest), the GX core's,
// the renderer's (drawing, copies, textures, packets), the waits for the GPU, and the DSP mixer's (audio).
void xeBenchFrame()
{
	static double last, idle0, core0, render0, wait0, wb0, dsp0;
	static uint32_t peeks0;
	const double now = monoSeconds(), idle = port_idle_seconds(), wait = g_xeWait;
	const double render = g_flushSeconds + s_copySeconds + g_xeTex + g_xeExec;
	const uint32_t peeks = s_statPeeks + s_statMetricReads;
	if (last) {
		const double frame = now - last, core = coreSeconds() - core0, r = render - render0, w = wait - wait0,
		             a = ::g_dspSeconds - dsp0;
		xe_bench_frame(frame, frame - (idle - idle0) - core - r - w - a, core, r, w, a, peeks - peeks0,
		               unsigned(s_wbTotal - wb0));
	}
	last = now;
	idle0 = idle;
	core0 = coreSeconds();
	render0 = render;
	wait0 = wait;
	peeks0 = peeks;
	wb0 = s_wbTotal;
	dsp0 = ::g_dspSeconds;
}

static void perfFrame()
{
	if (!xeGxThreaded()) xeBenchFrame();  // threaded: the game's thread, at its display copy
	static int on = -1;
	if (on < 0) on = !(getenv("SMS_XE_PERF") && getenv("SMS_XE_PERF")[0] == '0');
	if (!on) return;
	static double t0, last, worst, idle0, dec0, draw0, copy0, tex0, exec0, wait0, core0, part0[CP_COUNT];
	static uint32_t frames;
	const double now = monoSeconds();
	if (frames && now - last > worst) worst = now - last;
	last = now;
	if (frames++ == 0) {
		t0 = now;
		idle0 = port_idle_seconds();
		dec0 = g_decodeSeconds;
		core0 = coreSeconds();
		draw0 = g_flushSeconds;
		copy0 = s_copySeconds;
		tex0 = g_xeTex;
		exec0 = g_xeExec;
		wait0 = g_xeWait;
		worst = 0;
		memset(s_tb, 0, sizeof s_tb);
		for (int i = 0; i < CP_COUNT; i++) part0[i] = corePartSeconds(i);
		return;
	}
	if (frames <= 300) return;
	const double n = double(frames - 1), ms = 1000.0 / n, wall = now - t0;
	const double idle = port_idle_seconds() - idle0, dec = g_decodeSeconds - dec0, draw = g_flushSeconds - draw0,
	             copy = s_copySeconds - copy0, tex = g_xeTex - tex0, exec = g_xeExec - exec0, wait = g_xeWait - wait0;
	const double core = coreSeconds() - core0, game = wall - idle - core - draw - copy - tex - exec - wait;
	logmsg("[perf] %.1f fps, ms/frame: game %.2f, gx core %.2f (of which vertex decode %.2f), draw %.2f, copies %.2f, "
	       "textures %.2f, packets %.2f, gpu wait %.2f, idle %.2f; longest frame %.1f ms",
	       n / wall, game * ms, core * ms, dec * ms, draw * ms, copy * ms, tex * ms, exec * ms, wait * ms, idle * ms,
	       worst * 1000.0);
	{
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		const double tms = 1000.0 / double(f.QuadPart) / n;
		logmsg("[perf] draw parts ms: batching %.2f, state %.2f, program %.2f, textures %.2f, XF constants %.2f, "
		       "other constants %.2f, submit %.2f",
		       double(s_tb[TB_APPEND]) * tms, double(s_tb[TB_STATE]) * tms, double(s_tb[TB_PROGRAM]) * tms,
		       double(s_tb[TB_TEXTURES]) * tms, double(s_tb[TB_XF] - s_tb[TB_CONSTS]) * tms, double(s_tb[TB_CONSTS]) * tms,
		       double(s_tb[TB_SUBMIT]) * tms);
		memset(s_tb, 0, sizeof s_tb);
		double cp[CP_COUNT];
		for (int i = 0; i < CP_COUNT; i++) cp[i] = (corePartSeconds(i) - part0[i]) * ms;
		logmsg("[perf] gx core parts ms: parsing %.2f, BP %.2f, CP %.2f, XF %.2f, vertices %.2f, list checks %.2f, "
		       "cached lists %.2f, list recording %.2f",
		       cp[CP_PARSE], cp[CP_BP], cp[CP_CP], cp[CP_XF], cp[CP_VERTS], cp[CP_DL_CHECK], cp[CP_DL_DRAW], cp[CP_DL_RECORD]);
	}
	if (xeGxThreaded()) {
		static double ridle0, present0;
		const double ridle = xeRenderIdleSeconds();
		static uint64_t inv0;
		logmsg("[perf] render thread: busy %.2f ms a frame (in Present %.2f), idle %.2f; %.1f texture invalidations a frame",
		       (wall - (ridle - ridle0)) * ms, (g_xePresent - present0) * ms, (ridle - ridle0) * ms,
		       double(g_statTexInvalidates - inv0) / n);
		inv0 = g_statTexInvalidates;
		ridle0 = ridle;
		present0 = g_xePresent;
	}
	logmsg("[perf] a frame: %.1f write-backs, %.0f KB (ms: hash %.2f, read %.2f of which flush %.2f, encode %.2f, store %.2f); "
	       "cond signals %.0f, waits %.0f; %.0f draws, %.0f copies",
	       s_wbCount / n, s_wbBytes / n / 1024.0, s_wbHash * ms, s_wbRead * ms, s_wbFlush * ms, s_wbEncode * ms, s_wbStore * ms,
	       double(g_xeCondSignals) / n, double(g_xeCondWaits) / n, s_statDraws / n, s_statCopies / n);
	s_statDraws = s_statCopies = 0;
	s_wbHash = s_wbRead = s_wbFlush = s_wbEncode = s_wbStore = s_wbBytes = s_wbCount = 0;
	g_xeCondSignals = g_xeCondWaits = 0;
	frames = 0;
}

struct Xfb {
	IDirect3DTexture9* tex;
	int w, h;
};
static std::unordered_map<const void*, Xfb> s_xfbs;

// SMS_XE_SPIKES=ms: each frame of the render thread longer than that, and where its time went
// (xe_gx_queue.cpp logs the game thread's)
double xeSpikeMs()
{
	static double v = -1;
	if (v < 0) v = getenv("SMS_XE_SPIKES") ? atof(getenv("SMS_XE_SPIKES")) : 0;
	return v;
}
extern uint32_t g_statTexUploads;
extern uint64_t g_statTexHashBytes;
extern uint32_t g_xeDlHashes, g_xeDlHashBytes;  // port patch 19
static void renderSpike()
{
	static double last, core0, draw0, tex0, exec0, wait0, copy0, part0[CP_COUNT], texPart0[4];
	static uint32_t up0, comp0, draws0, dlh0, dlb0, ar0, rw0;
	static uint64_t texb0;
	if (!xeSpikeMs()) return;
	const double now = monoSeconds(), core = coreSeconds();
	const double frame = (now - last) * 1000.0;
	if (last && frame > xeSpikeMs())
		logmsg("[spike] render frame %u: %.1f ms (core %.1f, draw %.1f, textures %.1f, packets %.1f, copies %.1f, gpu wait %.1f; "
		       "%u texture uploads, %u shaders compiled, %u draws; hashed %u lists, %u KB, textures %u KB; %u arena resets, %u ring waits)",
		       g_displayFrames, frame, (core - core0) * 1e3, (g_flushSeconds - draw0) * 1e3, (g_xeTex - tex0) * 1e3,
		       (g_xeExec - exec0) * 1e3, (s_copySeconds - copy0) * 1e3, (g_xeWait - wait0) * 1e3, g_statTexUploads - up0,
		       g_xeCompiles - comp0, s_drawsTotal - draws0, g_xeDlHashes - dlh0, (g_xeDlHashBytes - dlb0) >> 10,
		       uint32_t((g_statTexHashBytes - texb0) >> 10), s_arenaResets - ar0, s_ringWaits - rw0);
	if (last && frame > xeSpikeMs()) {
		double cp[CP_COUNT];
		for (int i = 0; i < CP_COUNT; i++) cp[i] = (corePartSeconds(i) - part0[i]) * 1e3;
		logmsg("[spike]   core: parsing %.1f, BP/CP/XF/vertices %.1f, list checks %.1f, cached lists %.1f, list recording %.1f; "
		       "textures: create %.1f, decode %.1f, store %.1f, mip test %.1f",
		       cp[CP_PARSE], cp[CP_BP] + cp[CP_CP] + cp[CP_XF] + cp[CP_VERTS], cp[CP_DL_CHECK], cp[CP_DL_DRAW], cp[CP_DL_RECORD],
		       (g_xeTexCreate - texPart0[0]) * 1e3, (g_xeTexDecode - texPart0[1]) * 1e3, (g_xeTexStore - texPart0[2]) * 1e3,
		       (g_xeTexMips - texPart0[3]) * 1e3);
	}
	for (int i = 0; i < CP_COUNT; i++) part0[i] = corePartSeconds(i);
	texPart0[0] = g_xeTexCreate;
	texPart0[1] = g_xeTexDecode;
	texPart0[2] = g_xeTexStore;
	texPart0[3] = g_xeTexMips;
	last = now;
	core0 = core;
	draw0 = g_flushSeconds;
	tex0 = g_xeTex;
	exec0 = g_xeExec;
	wait0 = g_xeWait;
	copy0 = s_copySeconds;
	up0 = g_statTexUploads;
	comp0 = g_xeCompiles;
	draws0 = s_drawsTotal;
	dlh0 = g_xeDlHashes;
	dlb0 = g_xeDlHashBytes;
	texb0 = g_statTexHashBytes;
	ar0 = s_arenaResets;
	rw0 = s_ringWaits;
}

// A display copy's end of the frame: the frame's write-backs handed over, its snapshots done, the
// game's thread told it was shown
static void displayCopyDone()
{
	kick();
	storeWriteBacks();
	if (xeGxThreaded() && s_tsnapToken) waitToken(s_tsnapToken);  // the frame's snapshots, for the game
	textureFrameEnd();
	perfFrame();
	s_efbGen++;
	renderSpike();
	xeRenderShown(++g_displayFrames);
	if (g_displayFrames % 600 == 0)
		logmsg("%u frames (%u peeks, %u metric reads; shaders: %u compiled in %.1f s, %u from disk)", g_displayFrames,
		       s_statPeeks, s_statMetricReads, g_xeCompiles, g_xeCompileSeconds, g_xeCacheHits);
}

static void copyEfb(uint32_t ctrl)
{
	uint32_t src = g.bp[BP_COPY_SRC_TL], size = g.bp[BP_COPY_SRC_WH];
	int x = int(src & 0x3FF), y = int((src >> 10) & 0x3FF);
	int w = int(size & 0x3FF) + 1, h = int((size >> 10) & 0x3FF) + 1;
	const void* dest = g.copyDest;
	bool disp = (ctrl >> 14) & 1;
	bool clear = (ctrl >> 11) & 1;
	if (x + w > EFB_W) w = EFB_W - x;
	if (y + h > EFB_H) h = EFB_H - y;
	if (w < 1 || h < 1) {
		if (disp) displayCopyDone();  // a frame all the same, if empty: the game's thread waits for it
		return;
	}
	// widescreen: a full-width copy takes the whole EFB, a fader's pieces stretch like its
	// drawing, others take the centred 4:3 part (where the centred draws they copy landed)
	float fx = float(x), fw = float(w);
	if (s_wideW != EFB_W) {
		if (x <= 0 && x + w >= EFB_W) {
			fx = 0.0f;
			fw = float(s_wideW);
		} else if (s_stretch2D) {
			fx = float(x) * s_stretch;
			fw = float(w) * s_stretch;
		} else {
			fx = float(x) + pixelAligned(float(s_ox));
		}
	}

	if (disp) {
		Xfb& xfb = s_xfbs[dest];
		// at the internal resolution: the picture keeps the EFB's pixels
		const int sx = scaled(fx), sy = scaled(float(y));
		const int sw = scaled(fx + fw) - sx, sh = scaled(float(y + h)) - sy;
		if (!xfb.tex || xfb.w != sw || xfb.h != sh) {
			if (xfb.tex) releaseLater(xfb.tex);
			xfb.tex = NULL;
			dev->CreateTexture(sw, sh, 1, 0, D3DFMT_A8R8G8B8, 0, &xfb.tex, NULL);
			xfb.w = sw;
			xfb.h = sh;
		}
		// the rectangle's corner must be a multiple of 8 (its far edges meet the texture's)
		RECT r = {sx & ~7, sy & ~7, sx + sw, sy + sh};
		if (xfb.tex) emitResolve(false, &r, xfb.tex);
		// presenting takes all of EDRAM's contents (the XDK: undefined after Present): an EFB
		// the copy does not clear is drawn back afterwards (its colour; depth is lost)
		if (!clear) emitResolve(false, NULL, NULL);
		emitShow(xfb.tex, !clear);
		if (clear) clearRect(fx, y, fw, h);
		displayCopyDone();
		return;
	}

	uint32_t fmt = ((ctrl >> 3) & 1) << 3 | ((ctrl >> 4) & 7);
	bool intensity = ((ctrl >> 15) & 3) == 3;
	bool zcopy = (g.bp[BP_PE_CONTROL] & 7) == 3;
	bool half = (ctrl >> 9) & 1;
	int ow = half ? w / 2 : w, oh = half ? h / 2 : h;
	if (ow < 1) ow = 1;
	if (oh < 1) oh = 1;
	settleWriteBack(dest);
	const uint32_t layout = copyLayout(fmt, zcopy);
	// the texture's size: the GameCube's for a copy that goes back to RAM, else at s_copyScale
	const bool big = s_copyScale > 1.0f && !writesBack(dest, layout, zcopy);
	const int tw = big ? int(floorf(float(ow) * s_copyScale + 0.5f)) : ow;
	const int th = big ? int(floorf(float(oh) * s_copyScale + 0.5f)) : oh;
	{  // each place and size once: what the game copies, and how (SMS_XE_COPY_LOG=0: not)
		static std::unordered_map<const void*, uint32_t> logged;
		static const bool on = !(getenv("SMS_XE_COPY_LOG") && getenv("SMS_XE_COPY_LOG")[0] == '0');
		const uint32_t sig = uint32_t(tw) << 16 | uint32_t(th) ^ fmt << 28 ^ uint32_t(half) << 27;
		if (on && logged.size() < 256 && logged[dest] != sig) {
			logged[dest] = sig;
			logmsg("texture copy to %p: %dx%d at %d,%d%s, format %u (layout %u)%s, texture %dx%d", dest, w, h, x, y,
			       half ? " halved" : "", fmt, layout, writesBack(dest, layout, zcopy) ? ", written back" : "", tw, th);
		}
	}
	IDirect3DTexture9* tex = copyTexture(dest, tw, th);
	if (tex) {
		// a full-size copy: the left and top edges and the step between texels in (scaled) EFB pixels
		const CopyPassArgs cp = {zcopy, (debugBits() & 256) != 0, tw, th,
		                         {float(fmt), intensity ? 1.0f : 0.0f, (g.bp[BP_PE_CONTROL] & 7) != 1 ? 1.0f : 0.0f,
		                          zcopy ? 1.0f : 0.0f, fx * s_scale, float(y) * s_scale, half ? 0.0f : fw * s_scale / float(tw),
		                          float(h) * s_scale / float(th), float(s_efbW), float(s_efbH), 0.0f, 0.0f},
		                         {fx / float(s_wideW), float(y) / EFB_H, (fx + fw) / float(s_wideW), float(y + h) / EFB_H},
		                         big ? tex : NULL};
		emitCopyPass(cp);
		RECT r = {0, 0, tw, th};
		if (!big) emitResolve(false, &r, tex);
		Readback *encoded = NULL, *check = NULL;
		if (writesBack(dest, layout, zcopy) && gpuEncodes(ow, oh, layout) && !(debugBits() & 256)) {
			const int rows = ow * oh / 1024;
			static int verify = -1;
			if (verify < 0) verify = getenv("SMS_XE_WBVERIFY") && getenv("SMS_XE_WBVERIFY")[0] == '1';
			if (verify && (check = takeReadback(ow, oh, false)) != NULL) emitResolve(false, &r, &check->tex);
			// tiled, and untiled by the CPU: a linear Resolve target came back scrambled under Xenia
			// (SMS_XE_WBLINEAR=1 tries it, with SMS_XE_WBVERIFY=1 to see whether the console agrees)
			static const bool linear = getenv("SMS_XE_WBLINEAR") && getenv("SMS_XE_WBLINEAR")[0] == '1';
			if ((encoded = takeReadback(256, rows, linear)) != NULL) {
				// the blit's uv (as above) at the copy's first texel, and its step a texel
				const EncodeArgs ea = {zcopy, rows,
				                       {fx / float(s_wideW), float(y) / EFB_H, (fw / float(s_wideW)) / float(ow),
				                        (float(h) / EFB_H) / float(oh), float(ow / 8), 0.0f, 0.0f, 0.0f}};
				emitEncodeI8(ea);
				RECT er = {0, 0, 256, rows};
				emitResolve(false, &er, &encoded->tex);
			}
		}
		writeBack(dest, tex, ow, oh, layout, zcopy, encoded, check);
		emitEfbTarget();
	}
	if (clear) clearRect(fx, y, fw, h);
}

// ------------------------------------------------------------------ asynchronous reads
// A synchronous read of the GPU's work (a query result, a resolved EFB) makes the CPU wait for
// everything queued. Reads the game makes every frame are answered from the same read one frame
// earlier instead, as gx_render.cpp does (and Dolphin); SMS_GX_SYNC_READS=1 waits for each.
static bool asyncReads()
{
	static int on = -1;
	if (on < 0) on = !(getenv("SMS_GX_SYNC_READS") && getenv("SMS_GX_SYNC_READS")[0] == '1');
	return on != 0;
}

// ------------------------------------------------------------------ pixel metrics
// GXClearPixMetric/GXReadPixMetric as gx_render.cpp counts them: Delfino's goop counters draw
// each layer with an alpha test and read how many pixels passed; the game takes 4 a triangle
// off, so the count is the samples plus 4 per triangle. Occlusion queries count the samples;
// copies in between are left out (they end the running query; the ended ones are summed at the
// read). Each clear/read pair of a frame is a slot: a slot that drew as many triangles a frame
// earlier, in one query, is answered from that query. After a read the count goes on until the
// frame is shown, then stops; a count never read stops once it has run through a whole frame.
// Xenia makes query results up (query_occlusion_sample_*_threshold): right on the console only.
enum { kMetricSlots = 32 };
struct PendingQuery {
	IDirect3DQuery9* q;
	bool pooled;
};
static IDirect3DQuery9* s_mq[2][kMetricSlots];
static uint32_t s_mqTris[2][kMetricSlots], s_mqFrame[2][kMetricSlots];
static bool s_mqUsed[2][kMetricSlots];
static int s_mqSlot = -1;
static uint32_t s_mqSlotFrame = ~0u;
static IDirect3DQuery9* s_pixCur;
static bool s_pixCurPooled, s_pixTaint, s_pixStopped, s_pixReadSince;
static std::vector<PendingQuery> s_pixPending;
static std::vector<IDirect3DQuery9*> s_queryPool;
static uint32_t s_pixClearFrame;
static uint64_t s_pixSamples;

static IDirect3DQuery9* newQuery()
{
	IDirect3DQuery9* q = NULL;
	if (FAILED(dev->CreateQuery(D3DQUERYTYPE_OCCLUSION, &q))) q = NULL;
	return q;
}

static IDirect3DQuery9* poolQuery()
{
	if (s_queryPool.empty()) return newQuery();
	IDirect3DQuery9* q = s_queryPool.back();
	s_queryPool.pop_back();
	return q;
}

static void releaseQuery(IDirect3DQuery9* q, bool pooled)
{
	if (q && pooled) s_queryPool.push_back(q);  // issued again later: D3D drops what it had
}

static uint64_t queryResult(IDirect3DQuery9* q)
{
	DWORD n = 0;
	kick();  // its issues
	XeClock c(g_xeWait);
	if (q)
		while (q->GetData(&n, sizeof n, D3DGETDATA_FLUSH) == S_FALSE) {}
	return n;
}

static void pixBegin(IDirect3DQuery9* q, bool pooled)
{
	s_pixCur = q;
	s_pixCurPooled = pooled;
	emitQuery(q, true);
}

static void pixEndCur()
{
	emitQuery(s_pixCur, false);
}

static uint64_t pixWaitPending()
{
	uint64_t n = 0;
	for (size_t i = 0; i < s_pixPending.size(); i++) n += queryResult(s_pixPending[i].q);
	return n;
}

static void pixReleasePending()
{
	for (size_t i = 0; i < s_pixPending.size(); i++) releaseQuery(s_pixPending[i].q, s_pixPending[i].pooled);
	s_pixPending.clear();
}

static uint32_t pixValue()
{
	uint64_t v = uint64_t(double(s_pixSamples) / (double(s_scale) * double(s_scale))) + uint64_t(s_pixTris) * 4;
	return v > 0xFFFFFFFFu ? 0xFFFFFFFFu : uint32_t(v);
}

void pixMetricClear()
{
	flushBatch();
	if (!s_ready) return;
	if (s_pixActive) {
		pixEndCur();
		releaseQuery(s_pixCur, s_pixCurPooled);
	}
	pixReleasePending();
	s_pixStopped = s_pixReadSince = false;
	s_pixClearFrame = g_displayFrames;
	if (s_mqSlotFrame != g_displayFrames) {
		s_mqSlotFrame = g_displayFrames;
		s_mqSlot = -1;
	}
	s_mqSlot++;
	const int cur = g_displayFrames & 1;
	if (s_mqSlot < kMetricSlots) {
		if (!s_mq[cur][s_mqSlot]) s_mq[cur][s_mqSlot] = newQuery();
		pixBegin(s_mq[cur][s_mqSlot], false);
	} else {
		pixBegin(poolQuery(), true);
	}
	s_pixSamples = 0;
	s_pixTris = 0;
	s_pixTaint = !asyncReads() || s_mqSlot >= kMetricSlots;
	s_pixActive = true;
}

uint32_t pixMetricRead()
{
	flushBatch();
	if (!s_ready || (!s_pixActive && !s_pixStopped)) return 0;
	s_statMetricReads++;
	if (s_pixStopped) {  // read again after the count stopped: what it had counted
		s_pixSamples += pixWaitPending();
		pixReleasePending();
		return pixValue();
	}
	pixEndCur();
	uint64_t samples = 0;
	const int cur = g_displayFrames & 1, slot = s_mqSlot;
	bool fromPrev = false;
	if (!s_pixTaint) {
		s_mqUsed[cur][slot] = true;
		s_mqTris[cur][slot] = s_pixTris;
		s_mqFrame[cur][slot] = g_displayFrames;
		const int prev = cur ^ 1;
		if (s_mqUsed[prev][slot] && s_mqFrame[prev][slot] + 1 == g_displayFrames && s_mqTris[prev][slot] == s_pixTris) {
			samples = queryResult(s_mq[prev][slot]);
			fromPrev = true;
		}
	}
	if (!fromPrev) samples = s_pixSamples + queryResult(s_pixCur) + pixWaitPending();
	pixReleasePending();
	releaseQuery(s_pixCur, s_pixCurPooled);
	// reading does not reset the counter: it goes on (a second read waits for its queries)
	s_pixSamples = samples;
	s_pixTaint = s_pixReadSince = true;
	pixBegin(poolQuery(), true);
	return pixValue();
}

void executeCopy(uint32_t ctrl)
{
	s_statCopies++;
	if (!s_ready) {
		if ((ctrl >> 14) & 1) xeRenderShown(++g_displayFrames);  // no device: still a frame for the game's thread
		return;
	}
	flushBatch();
	OwnClock clock(s_copySeconds);
	if (tracing())
		logmsg("copy ctrl %06x src %06x wh %06x dest %p pe %x", ctrl, g.bp[BP_COPY_SRC_TL], g.bp[BP_COPY_SRC_WH], g.copyDest,
		       g.bp[BP_PE_CONTROL]);
	if (s_pixActive) {  // the count pauses: this query is summed at the read
		pixEndCur();
		PendingQuery p = {s_pixCur, s_pixCurPooled};
		s_pixPending.push_back(p);
		s_pixTaint = true;
	}
	copyEfb(ctrl);
	if (((ctrl >> 14) & 1) && s_pixActive && (s_pixReadSince || g_displayFrames - s_pixClearFrame >= 2)) {
		s_pixActive = false;  // the frame is shown
		s_pixStopped = true;
	}
	if (s_pixActive) pixBegin(poolQuery(), true);
}

// ------------------------------------------------------------------ EFB peeks
// GXPeekARGB/GXPeekZ: the sun's lens flare peeks 17 depths a frame, Mario's occlusion test one
// colour. Peeks with nothing drawn in between form a group; its first peek resolves the EFB into
// the group's texture, and the group is answered from the same group's texture a frame earlier
// (by waiting for the GPU the first time a group appears). Textures are made as groups appear.
enum { kPeekGroups = 8 };
struct PeekSnap {
	IDirect3DTexture9* tex;
	uint32_t token;
	uint32_t frame;
};
static PeekSnap s_peek[2][kPeekGroups][2];  // [frame parity][group][colour, depth]
static uint32_t s_peekGen = ~0u, s_peekFrame = ~0u, s_peekIssued;
static int s_peekGroup = -1;

static uint32_t texel(IDirect3DTexture9* t, int x, int y)
{
	D3DLOCKED_RECT r;
	if (FAILED(t->LockRect(0, &r, NULL, D3DLOCK_READONLY))) return 0;
	uint32_t v = ((const uint32_t*)r.pBits)[XGAddress2DTiledOffset(x, y, (s_efbW + 31) & ~31, 4)];
	t->UnlockRect(0);
	return v;
}

static uint32_t peekSync(int x, int y, bool depth)
{
	static uint32_t resolvedGen[2];
	IDirect3DTexture9* t = efbTexture(depth);
	if (resolvedGen[depth] != s_efbGen) {
		resolvedGen[depth] = s_efbGen;
		emitResolve(depth, NULL, NULL);
		waitToken(fenceToken());
	}
	return texel(t, x, y);
}

static uint32_t peekRaw(int x, int y, bool depth)
{
	if (!s_ready || x < 0 || y < 0 || x >= EFB_W || y >= EFB_H) return depth ? 0xFFFFFF00u : 0;
	flushBatch();
	x = int((float(x) + 0.5f) * s_stretch * s_scale);  // under the game's pixel centre (the camera's mapping)
	y = int((float(y) + 0.5f) * s_scale);
	s_statPeeks++;
	uint32_t v;
	if (!asyncReads()) {
		v = peekSync(x, y, depth);
	} else {
		if (s_peekGen != s_efbGen || s_peekFrame != g_displayFrames) {
			if (s_peekFrame != g_displayFrames) s_peekGroup = -1;
			s_peekGroup++;
			s_peekGen = s_efbGen;
			s_peekFrame = g_displayFrames;
			s_peekIssued = 0;
		}
		const int cur = g_displayFrames & 1, t = depth;
		if (s_peekGroup < kPeekGroups && !(s_peekIssued & (1u << t))) {
			s_peekIssued |= 1u << t;
			PeekSnap& now = s_peek[cur][s_peekGroup][t];
			if (!now.tex && FAILED(dev->CreateTexture(s_efbW, s_efbH, 1, 0, depth ? D3DFMT_D24S8 : D3DFMT_A8R8G8B8, 0, &now.tex, NULL)))
				now.tex = NULL;
			if (now.tex) {
				emitResolve(depth, NULL, now.tex);
				now.token = fenceToken();
				now.frame = g_displayFrames;
			}
		}
		const PeekSnap* prev = s_peekGroup < kPeekGroups ? &s_peek[cur ^ 1][s_peekGroup][t] : NULL;
		if (prev && prev->tex && prev->frame + 1 == g_displayFrames) {
			waitToken(prev->token);  // a frame old: long done
			v = texel(prev->tex, x, y);
		} else {
			v = peekSync(x, y, depth);
		}
	}
	if (tracing()) logmsg("peek %s (%d, %d) = %08x", depth ? "z" : "colour", x, y, v);
	return v;
}

// Threaded (xe_gx_queue.cpp): what draw-sync callbacks read comes from the render thread's
// snapshots of the frame before, taken where the game's commands put them: the EFB resolved for each
// group of peeks (the game numbers the groups), the metric value of each read. The game's thread
// reads a frame's snapshots once the render thread has shown it (its GPU work is waited for there).
struct TSnap {
	IDirect3DTexture9* tex;
	const uint32_t* bits;  // the texture's memory (a lock once: it stays where it is)
	uint32_t token, frame;
};
static TSnap s_tsnap[2][kPeekGroups][2];
enum { kTMetricSlots = 32 };
static uint32_t s_tmetric[2][kTMetricSlots], s_tmetricFrame[2][kTMetricSlots];

void xeSnapPeek(int group, bool depth)
{
	if (!s_ready || group >= kPeekGroups) return;
	flushBatch();
	TSnap& sn = s_tsnap[g_displayFrames & 1][group][depth];
	if (!sn.tex) {
		if (FAILED(dev->CreateTexture(s_efbW, s_efbH, 1, 0, depth ? D3DFMT_D24S8 : D3DFMT_A8R8G8B8, 0, &sn.tex, NULL))) {
			sn.tex = NULL;
			return;
		}
		D3DLOCKED_RECT r;
		if (FAILED(sn.tex->LockRect(0, &r, NULL, D3DLOCK_READONLY))) return;
		sn.bits = (const uint32_t*)r.pBits;
		sn.tex->UnlockRect(0);
	}
	if (!sn.bits) return;
	emitResolve(depth, NULL, sn.tex);
	sn.token = s_tsnapToken = fenceToken();
	sn.frame = g_displayFrames;
}

// The game's thread: the peek from frame `frame`'s snapshot of the group (false: none was taken).
bool xeSnapPeekRead(uint32_t frame, int group, bool depth, int x, int y, uint32_t* v)
{
	if (x < 0 || y < 0 || x >= EFB_W || y >= EFB_H) {
		*v = depth ? 0xFFFFFF00u : 0;
		return true;
	}
	if (group >= kPeekGroups) return false;
	const TSnap& sn = s_tsnap[frame & 1][group][depth];
	if (!sn.bits || sn.frame != frame) return false;
	x = int((float(x) + 0.5f) * s_stretch * s_scale);
	y = int((float(y) + 0.5f) * s_scale);
	*v = sn.bits[XGAddress2DTiledOffset(x, y, (s_efbW + 31) & ~31, 4)];
	return true;
}

static uint32_t s_tmetricLast, s_tmetricLastFrame = ~0u;  // the latest read, any slot
static int s_tmetricLastSlot = -1;
void xeSnapMetric(int slot)
{
	const uint32_t v = pixMetricRead();
	s_tmetricLast = v;
	s_tmetricLastFrame = g_displayFrames;
	s_tmetricLastSlot = slot;
	if (slot >= kTMetricSlots) return;
	s_tmetric[g_displayFrames & 1][slot] = v;
	s_tmetricFrame[g_displayFrames & 1][slot] = g_displayFrames;
}

// The read just made, whatever its slot (past the slots too): only once the render thread is
// drained, as it writes these between the game's reads otherwise
bool xeSnapMetricLast(uint32_t frame, int slot, uint32_t* v)
{
	if (slot != s_tmetricLastSlot || frame != s_tmetricLastFrame) return false;
	*v = s_tmetricLast;
	return true;
}

bool xeSnapMetricRead(uint32_t frame, int slot, uint32_t* v)
{
	if (slot >= kTMetricSlots || s_tmetricFrame[frame & 1][slot] != frame) return false;
	*v = s_tmetric[frame & 1][slot];
	return true;
}

uint32_t peekColor(int x, int y) { return peekRaw(x, y, false); }  // 0xAARRGGBB, as GX's
uint32_t peekZ(int x, int y) { return peekRaw(x, y, true) >> 8; }   // D24S8: depth above stencil

}  // namespace gx

using namespace gx;

extern "C" {

static void initOn(void*)
{
	InitializeCriticalSection(&s_handoffLock);
	// Upstream reads vertex arrays not registered as big-endian in the x86 host's order, since the
	// resource converters swapped them there; on the 360 they stay as on disc, and the port's own
	// arrays are native: big-endian either way
	GXPC_SetDefaultArrayBigEndian(1);
	resetState();
	if (const char* e = getenv("SMS_XE_SCALE")) s_scale = float(atof(e));
	else s_scale = 1.5f;
	if (!(s_scale >= 1.0f)) s_scale = 1.0f;
	s_wideW = s_wide > 1.0f ? (int(float(EFB_W) * s_wide + 1.0f) & ~1) : EFB_W;
	s_ox = (s_wideW - EFB_W) / 2;
	s_stretch = float(s_wideW) / float(EFB_W);
	// EDRAM holds 2048 tiles: two EFB surfaces and the scratch target must fit
	while (s_scale > 1.0f && 2 * edramTiles(scaled(float(s_wideW)), scaled(EFB_H)) + edramTiles(EFB_W, EFB_H) > 2048)
		s_scale -= 0.125f;
	s_efbW = scaled(float(s_wideW));
	s_efbH = scaled(EFB_H);
	s_copyScale = getenv("SMS_XE_COPY_SCALE") ? float(atof(getenv("SMS_XE_COPY_SCALE"))) : s_scale;
	if (!(s_copyScale >= 1.0f)) s_copyScale = 1.0f;
	if (s_copyScale > s_scale) s_copyScale = s_scale;
	// bands as wide as a copy at that scale (no more than the EFB's width: 640 GameCube pixels) and
	// as many 32-row steps high as the EDRAM left holds, up to the whole copy
	int bandW = 0, bandH = 0;
	for (; s_copyScale > 1.0f; s_copyScale -= 0.125f) {
		bandW = int(ceilf(EFB_W * s_copyScale));
		const int full = (int(ceilf(EFB_H * s_copyScale)) + 31) & ~31;
		const int left = 2048 - int(2 * edramTiles(s_efbW, s_efbH)), across = (bandW + 79) / 80;
		bandH = left / across * 16 & ~31;
		if (bandH > full) bandH = full;
		if (bandH >= 64) break;
	}
	if (s_copyScale <= 1.0f) bandW = bandH = 0;
	logmsg("texture copies at %.3gx (bands %dx%d)", s_copyScale, bandW, bandH);
	forgetConsts(true, 0, VC_COUNT);
	forgetConsts(false, 0, PC_COUNT);
	xfAllDirty();
	// D3D's defaults, for the blend states draws without blending leave as they are
	s_ds.rs[RS_BLENDOP] = s_ds.rs[RS_BLENDOPALPHA] = D3DBLENDOP_ADD;
	s_ds.rs[RS_SRCBLEND] = s_ds.rs[RS_SRCBLENDALPHA] = D3DBLEND_ONE;
	s_ds.rs[RS_DESTBLEND] = s_ds.rs[RS_DESTBLENDALPHA] = D3DBLEND_ZERO;
	s_ds.rs[RS_BLENDFACTOR] = 0xFFFFFFFF;
	s_ready = execInit(s_efbW, s_efbH, s_wide, bandW, bandH) && s_vring.init(false) && s_iring.init(true);
	if (s_ready && bandW && !execHasBand()) {  // no band target: copies at the GameCube's size
		logmsg("texture copies: no band target, at 1x");
		s_copyScale = 1.0f;
	}
	s_arenaOn = s_ready && !(getenv("SMS_XE_ARENA") && getenv("SMS_XE_ARENA")[0] == '0') && s_arena.init();
	if (s_ready) logmsg("EFB at %gx the GameCube's, %.3f times 4:3 wide", double(s_scale), double(s_wide));
	{  // what the frame-time clocks cost (a monoSeconds call, a timebase read)
		const double t0 = monoSeconds();
		for (int i = 0; i < 10000; i++) monoSeconds();
		const double t1 = monoSeconds();
		volatile unsigned long long tb = 0;
		for (int i = 0; i < 10000; i++) tb += __mftb();
		const double t2 = monoSeconds();
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		logmsg("clocks: monoSeconds %.0f ns, mftb %.0f ns a call; performance counter %lld Hz", (t1 - t0) * 1e5,
		       (t2 - t1) * 1e5, f.QuadPart);
	}
	if (!s_ready) logmsg("Direct3D: no device, or no room for the vertex/index rings");
	else programWarmup();
}

int GXPC_InitAuto(float)
{
	if (s_ready) return 1;
	xeGxStart(initOn, NULL);
	return s_ready;
}

uint32_t GXPC_FrameCount(void) { return xeGxThreaded() ? xeGameFrames() : g_displayFrames; }

// The XFB `xfb` as RGBA rows, top first (SMS_SHOTS); with rgba null only its size. The
// texture Resolve wrote is tiled: XGAddress2DTiledOffset finds each texel (0xAARRGGBB).
struct ReadXfbArgs {
	const void* xfb;
	uint8_t* rgba;
	int *w, *h;
	int ok;
};
static void readXfbOn(void* p);
int GXPC_ReadXFB(const void* xfb, uint8_t* rgba, int* w, int* h)
{
	ReadXfbArgs a = {xfb, rgba, w, h, 0};
	xeCoreCall(readXfbOn, &a);
	return a.ok;
}
static int readXfb(const void* xfb, uint8_t* rgba, int* w, int* h)
{
	std::unordered_map<const void*, Xfb>::iterator it = s_xfbs.find(xfb);
	if (it == s_xfbs.end() || !it->second.tex) return 0;
	*w = it->second.w;
	*h = it->second.h;
	if (!rgba) return 1;
	waitToken(fenceToken());  // the frame's Resolve has landed
	return readTexture(it->second.tex, *w, *h, rgba);
}
static void readXfbOn(void* p)
{
	ReadXfbArgs& a = *(ReadXfbArgs*)p;
	a.ok = readXfb(a.xfb, a.rgba, a.w, a.h);
}
int GXPC_ParseArgs(int*, char**) { return 0; }
void GXPC_SetHeadless(int) {}
int GXPC_IsHeadless(void) { return 0; }
double GXPC_LastPresentSeconds(void) { return 0; }
int GXPC_GameInputBlocked(void) { return 0; }
int GXPC_MouseCaptured(void) { return 0; }
int GXPC_RequestQuit(void) { return 0; }
int GXPC_SettingsMenuIsOpen(void) { return 0; }
void GXPC_SettingsMenuOpen(int) {}
void GXPC_SkipNextPresent(int) {}
// Widescreen (gx_render.cpp's): the width is fixed once the device exists.
void GXPC_SetWidescreen(float widthOver43)
{
	if (s_ready) return;
	s_wide = widthOver43 > 1.0f ? widthOver43 : 1.0f;
	const char* e = getenv("SMS_WIDESCREEN_HUD");
	s_hudEdges = s_wide > 1.0f && e && !strcmp(e, "edges");
}
float GXPC_GetWidescreen(void) { return s_wide; }
float GXPC_ApplyWidescreen(void) { return s_wide; }  // no settings menu changes it here

void coreSetWide2D(int on)
{
	if (s_wideW == EFB_W || s_wide2D == (on != 0)) return;
	flushBatch();
	s_wide2D = on != 0;
}
void coreSetStretch2D(int on)
{
	if (s_wideW == EFB_W || s_stretch2D == (on != 0)) return;
	flushBatch();  // what was queued keeps its own mapping
	s_stretch2D = on != 0;
}
void coreSetHud(int on)
{
	if (!s_hudEdges || s_hud == (on != 0)) return;
	flushBatch();
	s_hud = on != 0;
	s_hudPanes.clear();
	s_hudAnchor = -1.0f;
}
// Anchors on the outermost pane that is a piece, not a container; the batch queued so far keeps
// its mapping when the anchor moves to another side.
static void hudReanchor()
{
	float now = -1.0f;
	for (size_t i = 0; i < s_hudPanes.size(); i++)
		if (s_hudPanes[i].x2 - s_hudPanes[i].x1 < float(EFB_W) * 3 / 4) {
			now = (s_hudPanes[i].x1 + s_hudPanes[i].x2) / 2;
			break;
		}
	if (now == s_hudAnchor) return;
	if (now < 0.0f || s_hudAnchor < 0.0f || hudOffset(now) != hudOffset(s_hudAnchor)) flushBatch();
	s_hudAnchor = now;
}
void coreHudPaneBegin(float x1, float x2)
{
	if (!s_hud) return;
	HudPane p = {x1 < x2 ? x1 : x2, x1 < x2 ? x2 : x1};
	s_hudPanes.push_back(p);
	hudReanchor();
}
void coreHudPaneEnd(void)
{
	if (!s_hud || s_hudPanes.empty()) return;
	s_hudPanes.pop_back();
	hudReanchor();
}
static std::vector<bool> s_hudPieceInHud;  // whether each open piece found the HUD being drawn
void coreHudPieceBegin(float x1, float x2)
{
	if (!s_hudEdges) return;
	s_hudPieceInHud.push_back(s_hud);
	coreSetHud(1);
	coreHudPaneBegin(x1, x2);
}
void coreHudPieceEnd(void)
{
	if (!s_hudEdges || s_hudPieceInHud.empty()) return;
	coreHudPaneEnd();
	if (!s_hudPieceInHud.back()) coreSetHud(0);
	s_hudPieceInHud.pop_back();
}
void coreInvalidateRange(const void* p, uint32_t size)
{
	memoryWritten(p, size);
	if (writeBackOn()) copyCpuWrote(p, size);
}
// The game's entry points: they run where the core does, in order with the GX commands.
struct HudArgs {
	int on;
	float x1, x2;
};
static void wide2DOn(const void* p) { coreSetWide2D(((const HudArgs*)p)->on); }
static void stretch2DOn(const void* p) { coreSetStretch2D(((const HudArgs*)p)->on); }
static void hudOn(const void* p) { coreSetHud(((const HudArgs*)p)->on); }
static void paneBeginOn(const void* p) { coreHudPaneBegin(((const HudArgs*)p)->x1, ((const HudArgs*)p)->x2); }
static void paneEndOn(const void*) { coreHudPaneEnd(); }
static void pieceBeginOn(const void* p) { coreHudPieceBegin(((const HudArgs*)p)->x1, ((const HudArgs*)p)->x2); }
static void pieceEndOn(const void*) { coreHudPieceEnd(); }
static void hudPost(void (*fn)(const void*), int on, float x1, float x2)
{
	const HudArgs a = {on, x1, x2};
	xeCorePostData(fn, &a, sizeof a);
}
void GXPC_SetWide2D(int on) { hudPost(wide2DOn, on, 0, 0); }
void GXPC_SetStretch2D(int on) { hudPost(stretch2DOn, on, 0, 0); }
void GXPC_SetHud(int on) { hudPost(hudOn, on, 0, 0); }
void GXPC_HudPaneBegin(float x1, float x2) { hudPost(paneBeginOn, 0, x1, x2); }
void GXPC_HudPaneEnd(void) { hudPost(paneEndOn, 0, 0, 0); }
void GXPC_HudPieceBegin(float x1, float x2) { hudPost(pieceBeginOn, 0, x1, x2); }
void GXPC_HudPieceEnd(void) { hudPost(pieceEndOn, 0, 0, 0); }
struct RangeArgs {
	const void* p;
	uint32_t size;
};
static void invalidateOn(const void* p) { coreInvalidateRange(((const RangeArgs*)p)->p, ((const RangeArgs*)p)->size); }
void GXPC_InvalidateRange(const void* p, uint32_t size)
{
	const RangeArgs a = {p, size};
	xeCorePostData(invalidateOn, &a, sizeof a);
}
void GXPC_PrefetchResource(const void*, uint32_t, const char*) {}
void GXPC_PrepareStage(int) {}
void GXPC_SetIdleClock(double (*)(void)) {}
void GXPC_SetNativeCopy(const void*, int, int) {}
void GXPC_AutoSaveIndicator(int) {}
int GXPC_GetSpeed(void) { return 1; }

const void* port_button_glyph(int) { return NULL; }
int port_radar_map(unsigned long long, struct PortRadarMap*) { return 0; }

}  // extern "C"

const PortSetting* gx_port_settings(int* count) { *count = 0; return NULL; }
const PortSetting* gx_port_settings_extra(int* count) { *count = 0; return NULL; }
void port_gfx_describe(char* out, int size)
{
	if (size > 0) _snprintf(out, size, "Xenos (Direct3D 9)"), out[size - 1] = 0;
}
