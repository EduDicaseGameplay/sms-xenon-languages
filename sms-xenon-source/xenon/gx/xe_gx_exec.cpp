// The render side of the back end (xe_gx.h, "render packets"): the device, the EFB in EDRAM, and
// the executors of the packets the game side emits. Everything here that uses the device's command
// stream runs inside kick(), in packet order.
//
// EDRAM (10 MiB, in tiles of 80x16 32-bit pixels) holds the back buffer (1280x720, 720 tiles,
// base 0), the EFB (colour and depth, after it) and a scratch target for texture copies (640x528;
// over the same EDRAM, a band target for copies at the internal scale, as wide as they are and as
// high as fits: a 16:9 EFB at 1.5x leaves room for 448 rows of 960):
// nothing aliases when all fit, so the EFB keeps its pixels while copies and presenting use the
// others. GPU reads of the EFB go through Resolve into textures.
//
// The device states draws and blits set go through shadows, so a packet that sets what is already
// set costs no device call. Changing the render target forgets them (D3D resets the viewport and
// scissor with it).
#include "xe_gx.h"
#include <xgraphics.h>
#include <math.h>
#include <string>
#include <string.h>
#include <vector>

void WINAPI xe_vblank(D3DVBLANKDATA*);  // xenon/platform/xe_vblank.cpp

namespace gx {
namespace xe {

IDirect3DDevice9* dev;
double g_xeExec;
double g_xePresent;  // in Present (part of the GPU waits)
static IDirect3D9* s_d3d;
static IDirect3DSurface9 *s_backBuffer, *s_efbColor, *s_efbDepth, *s_scratch, *s_band;
static int s_bandH;
static IDirect3DTexture9* s_efbTex;  // the EFB's colour, resolved for copies that sample it
static IDirect3DTexture9* s_depthTex;  // its depth, resolved for peeks and depth copies
static IDirect3DTexture9 s_depthAsColour;  // s_depthTex's memory read as A8R8G8B8 (depth copies)
static int s_efbW, s_efbH;
static float s_wide;

static void waitFence(DWORD f)
{
	XeClock c(g_xeWait);
	dev->BlockOnFence(f);
}

// ------------------------------------------------------------------ the packet buffer
// Each packet is its executor and the size of its arguments, then the arguments (8-byte aligned).
typedef void (*ExecFn)(const void* args);
struct Packet {
	ExecFn fn;
	uint32_t size;
};
enum { kHeader = (sizeof(Packet) + 7) & ~7 };
static PodBuf<uint8_t> s_packets;

static size_t kickBytes()
{
	static int v = -2;
	if (v == -2) {
		const char* e = getenv("SMS_XE_KICK");
		v = e ? atoi(e) : 64 * 1024;
	}
	return size_t(v);
}

static void* emitRaw(ExecFn fn, uint32_t bytes)
{
	if (s_packets.size() >= kickBytes()) kick();
	bytes = (bytes + 7) & ~7u;
	const size_t at = s_packets.size();
	s_packets.resize(at + kHeader + bytes);
	Packet* p = (Packet*)&s_packets[at];
	p->fn = fn;
	p->size = bytes;
	return &s_packets[at + kHeader];
}

template <class A, void (*F)(const A&)> static void run(const void* p) { F(*(const A*)p); }
template <class A, void (*F)(const A&)> static void emit(const A& a)
{
	memcpy(emitRaw(&run<A, F>, sizeof(A)), &a, sizeof(A));
}

void kick()
{
	if (s_packets.empty()) return;
	OwnClock clock(g_xeExec);
	for (size_t at = 0; at < s_packets.size();) {
		const Packet* p = (const Packet*)&s_packets[at];
		p->fn(&s_packets[at + kHeader]);
		at += kHeader + p->size;
	}
	s_packets.clear();
}

// ------------------------------------------------------------------ fences
// A token's D3D fence, by token: older tokens' slots are reused (a fence 4096 tokens old, frames
// ago, is long passed; waiting for a newer one in its slot would only wait longer).
enum { kFenceSlots = 4096 };
static DWORD s_fences[kFenceSlots];
static uint32_t s_tokens, s_tokensRun;

static void execFence(const uint32_t& t)
{
	s_fences[t % kFenceSlots] = dev->InsertFence();
	s_tokensRun = t;
}

uint32_t fenceToken()
{
	emit<uint32_t, execFence>(++s_tokens);
	return s_tokens;
}

void waitToken(uint32_t t)
{
	if (!t) return;
	if (int32_t(t - s_tokensRun) > 0) kick();
	if (s_tokens - t >= kFenceSlots) return;
	waitFence(s_fences[t % kFenceSlots]);
}

// ------------------------------------------------------------------ state shadows
static const D3DRENDERSTATETYPE kRs[RS_COUNT] = {
    D3DRS_SCISSORTESTENABLE, D3DRS_CULLMODE,     D3DRS_ZENABLE,     D3DRS_ZFUNC,
    D3DRS_ZWRITEENABLE,      D3DRS_COLORWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_BLENDOP,
    D3DRS_SRCBLEND,          D3DRS_DESTBLEND,    D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_BLENDOPALPHA,
    D3DRS_SRCBLENDALPHA,     D3DRS_DESTBLENDALPHA, D3DRS_BLENDFACTOR, D3DRS_POINTSIZE, D3DRS_LINEWIDTH};
static const D3DSAMPLERSTATETYPE kSs[SS_COUNT] = {D3DSAMP_ADDRESSU,  D3DSAMP_ADDRESSV,      D3DSAMP_MAGFILTER,
                                                 D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER,     D3DSAMP_MIPMAPLODBIAS,
                                                 D3DSAMP_MAXMIPLEVEL};
static DWORD s_rs[RS_COUNT], s_ss[8][SS_COUNT];
static uint32_t s_rsKnown, s_ssKnown[8];
static RECT s_scissor;
static bool s_scissorKnown;
static IDirect3DBaseTexture9* s_tex[8];  // what each sampler has bound

static void setRS(int i, DWORD v)
{
	if ((s_rsKnown >> i & 1) && s_rs[i] == v) return;
	dev->SetRenderState(kRs[i], v);
	s_rs[i] = v;
	s_rsKnown |= 1u << i;
}

static void setSS(int u, int i, DWORD v)
{
	if ((s_ssKnown[u] >> i & 1) && s_ss[u][i] == v) return;
	dev->SetSamplerState(DWORD(u), kSs[i], v);
	s_ss[u][i] = v;
	s_ssKnown[u] |= 1u << i;
}

static void setTexture(int u, IDirect3DBaseTexture9* t)
{
	if (s_tex[u] == t) return;
	dev->SetTexture(DWORD(u), t);
	s_tex[u] = t;
}

static void forgetStates()
{
	s_rsKnown = 0;
	memset(s_ssKnown, 0, sizeof s_ssKnown);
	s_scissorKnown = false;
}

static void target(IDirect3DSurface9* colour, IDirect3DSurface9* depth, int w, int h)
{
	dev->SetRenderTarget(0, colour);
	dev->SetDepthStencilSurface(depth);
	D3DVIEWPORT9 vp = {0, 0, DWORD(w), DWORD(h), 0.0f, 1.0f};
	dev->SetViewport(&vp);
	forgetStates();
}

static void efbTarget() { target(s_efbColor, s_efbDepth, s_efbW, s_efbH); }

static void plainState()
{
	setRS(RS_BLEND, FALSE);
	setRS(RS_ZENABLE, FALSE);
	setRS(RS_ZWRITE, FALSE);
	setRS(RS_CULL, D3DCULL_NONE);
	setRS(RS_SCISSOR, FALSE);
	setRS(RS_COLORWRITE, 0xF);
}

static void blitSampler(DWORD filter)
{
	setSS(0, SS_MINFILTER, filter);
	setSS(0, SS_MAGFILTER, filter);
	setSS(0, SS_MIPFILTER, D3DTEXF_NONE);
	setSS(0, SS_ADDRESSU, D3DTADDRESS_CLAMP);
	setSS(0, SS_ADDRESSV, D3DTADDRESS_CLAMP);
}

// ------------------------------------------------------------------ the render side's own shaders
static const char* kBlitVs =
    "float4 u_rect : register(c0); float4 u_uv : register(c1); float4 u_z : register(c2);\n"
    "struct O { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
    "O main(float2 p : POSITION) { O o; o.pos = float4(lerp(u_rect.xy, u_rect.zw, p), u_z.x, 1.0);\n"
    "  o.uv = lerp(u_uv.xy, u_uv.zw, p); return o; }\n";
static const char* kTexPs =
    "sampler2D s0 : register(s0);\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR0 { return tex2D(s0, uv); }\n";
static const char* kColorPs =
    "float4 u_color : register(c0);\n"
    "float4 main() : COLOR0 { return u_color; }\n";
// EFB -> texture conversion (gx_render.cpp's kCopyFs): the output is what sampling the GX texture
// of the copy's format would return. u_mode bit 4: intensity (YUV), bits 0-3: format.
// the copy's texel at px (texels from its top left) and uv (the blit's interpolation there)
static const char* kCopyFn =
    "sampler2D s0 : register(s0);\n"
    "float4 u_mode : register(c0);  // x format, y intensity, z alpha one, w depth copy\n"
    "float4 u_src : register(c1);   // a full-size copy (z > 0): left, top, EFB pixels per texel\n"
    "float4 u_efb : register(c2);   // the EFB texture's size\n"
    "float q(float v, float bits) { float m = exp2(bits) - 1.0; return floor(v * m + 0.5) / m; }\n"
    "float4 copyTexel(float2 px, float2 uv) {\n"
    "  float4 c;\n"
    "  if (u_src.z > 0.0) {\n"
    "    float2 at = clamp(floor(u_src.xy + (px + 0.5) * u_src.zw), 0.0, u_efb.xy - 1.0);\n"
    "    c = tex2Dlod(s0, float4((at + 0.5) / u_efb.xy, 0.0, 0.0));\n"
    "  } else c = tex2Dlod(s0, float4(uv, 0.0, 0.0));\n"
    "  float f = u_mode.x;\n"
    // depth copies sample the resolved D24S8 depth as A8R8G8B8: its bytes, high to low, in a, r, g
    "  if (u_mode.w != 0.0) {\n"
    "    float hi = c.a, mid = c.r, lo = c.g;\n"
    "    if (f == 1.0) return hi.xxxx;\n"                        // Z8
    "    if (f == 11.0 || f == 3.0) return float4(mid.xxx, hi);\n" // Z16 (IA8 layout)
    "    if (f == 6.0) return float4(hi, mid, lo, 1.0);\n"       // Z24X8
    "    if (f == 0.0) return q(hi, 4.0).xxxx;\n"                // Z4
    "    if (f == 9.0) return mid.xxxx;\n"                       // Z8M
    "    if (f == 10.0) return lo.xxxx;\n"                       // Z8L
    "    if (f == 12.0) return float4(lo.xxx, mid);\n"           // Z16L
    "    return hi.xxxx;\n"
    "  }\n"
    "  if (u_mode.z != 0.0) c.a = 1.0;\n"
    "  if (u_mode.y != 0.0) {\n"
    "    float y = saturate(0.257 * c.r + 0.504 * c.g + 0.098 * c.b + 16.0 / 255.0);\n"
    "    if (f == 0.0) return q(y, 4.0).xxxx;\n"
    "    if (f == 1.0) return y.xxxx;\n"
    "    if (f == 2.0) return float4(q(y, 4.0).xxx, q(c.a, 4.0));\n"
    "    if (f == 3.0) return float4(y.xxx, c.a);\n"
    "    return c;\n"
    "  }\n"
    "  if (f == 0.0) return q(c.r, 4.0).xxxx;\n"
    "  if (f == 2.0) return float4(q(c.r, 4.0).xxx, q(c.a, 4.0));\n"
    "  if (f == 3.0) return float4(c.rrr, c.a);\n"
    "  if (f == 4.0) return float4(q(c.r, 5.0), q(c.g, 6.0), q(c.b, 5.0), 1.0);\n"
    "  if (f == 5.0) return c.a > 0.99 ? float4(q(c.r, 5.0), q(c.g, 5.0), q(c.b, 5.0), 1.0)\n"
    "                                  : float4(q(c.r, 4.0), q(c.g, 4.0), q(c.b, 4.0), q(c.a, 3.0));\n"
    "  if (f == 7.0) return c.aaaa;\n"
    "  if (f == 8.0) return c.rrrr;\n"
    "  if (f == 9.0) return c.gggg;\n"
    "  if (f == 10.0) return c.bbbb;\n"
    "  if (f == 11.0) return float4(c.ggg, c.r);\n"
    "  if (f == 12.0) return float4(c.bbb, c.g);\n"
    "  return c;\n"
    "}\n";
static const char* kCopyPs =
    "float4 main(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR0 { return copyTexel(floor(vpos), uv); }\n";
// An I8 copy (the goop maps) encoded as GX stores it, for its write-back: the bytes of its 8x4 tiles
// in order, four to a pixel (a, r, g, b: memory order) of a 256-pixel-wide target, so the CPU only
// copies them (untiling and encoding a 512x512 map on the CPU took ~10 ms a frame on the console).
// It converts from the EFB as the copy pass does (sampling the just-resolved copy texture instead
// read stale texels under Xenia).
static const char* kEncodeI8Ps =
    "float4 u_uv : register(c3);    // the blit's uv at texel 0's corner, and its step a texel\n"
    "float4 u_size : register(c4);  // tiles a row\n"
    "float t8(float2 p) { return copyTexel(p, u_uv.xy + (p + 0.5) * u_uv.zw).r; }\n"
    "float4 main(float2 vpos : VPOS) : COLOR0 {\n"
    "  float b = (floor(vpos.y) * 256.0 + floor(vpos.x)) * 4.0;\n"
    "  float t = floor(b / 32.0), o = b - t * 32.0;\n"
    "  float r = floor(o / 8.0), c = o - r * 8.0;\n"
    "  float ty = floor((t + 0.5) / u_size.x), tx = t - ty * u_size.x;\n"
    "  float2 p = float2(tx * 8.0 + c, ty * 4.0 + r);\n"
    "  return float4(t8(p + float2(1, 0)), t8(p + float2(2, 0)), t8(p + float2(3, 0)), t8(p));\n"
    "}\n";
static IDirect3DVertexShader9* s_blitVs;
static IDirect3DPixelShader9 *s_texPs, *s_colorPs, *s_copyPs, *s_encodeI8Ps;
static IDirect3DVertexDeclaration9* s_blitDecl;

// The unit square as a strip; the blit shader maps it to rect (clip space) and uv. It overwrites
// vertex constants c0-c2 (the game side's constsClobbered).
static void blit(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, float z = 0.0f)
{
	static const float quad[8] = {0, 0, 1, 0, 0, 1, 1, 1};
	const float c[12] = {x0, y0, x1, y1, u0, v0, u1, v1, z, 0, 0, 0};
	dev->SetVertexDeclaration(s_blitDecl);
	dev->SetVertexShader(s_blitVs);
	dev->SetVertexShaderConstantF(0, c, 3);
	dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, 8);
}

// ------------------------------------------------------------------ device
bool execInit(int efbW, int efbH, float wide, int bandW, int bandH)
{
	s_efbW = efbW;
	s_efbH = efbH;
	s_wide = wide;
	s_d3d = Direct3DCreate9(D3D_SDK_VERSION);
	D3DPRESENT_PARAMETERS pp;
	memset(&pp, 0, sizeof(pp));
	pp.BackBufferWidth = 1280;
	pp.BackBufferHeight = 720;
	pp.BackBufferFormat = D3DFMT_A8R8G8B8;
	pp.BackBufferCount = 1;
	pp.EnableAutoDepthStencil = FALSE;
	pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	// flips at the vblank, which also paces VI (xe_vblank.cpp); a deterministic run (no real clock)
	// presents at once, or it would run no faster than the display
	const char* det = getenv("SMS_VI_DETERMINISTIC");
	pp.PresentationInterval = det && *det && strcmp(det, "0") != 0 ? D3DPRESENT_INTERVAL_IMMEDIATE : D3DPRESENT_INTERVAL_ONE;
	// a bigger command ring than the default: with GX's many small draws D3D stalled starting new
	// segments (BlockOnFence in KickOff, ~7% of a Delfino Plaza frame on the console)
	pp.RingBufferParameters.PrimarySize = 64 * 1024;
	pp.RingBufferParameters.SecondarySize = 8 * 1024 * 1024;
	pp.RingBufferParameters.SegmentCount = 64;
	// Two frames buffered (D3DCREATE_BUFFER_2_FRAMES): Present no longer waits for the GPU to finish
	// the frame before (~5 ms of the render thread's frame on the console; Gelato 35% -> 3% of frames
	// over 33 ms). SMS_XE_D3D_FLAGS=hex sets the Xbox 360 behaviour flags instead (d3d9.h; 4,
	// asynchronous swaps, hung the GPU in Present; D3D's threads on hardware thread 5 did
	// nothing).
	const DWORD flags = getenv("SMS_XE_D3D_FLAGS") ? DWORD(strtoul(getenv("SMS_XE_D3D_FLAGS"), NULL, 16))
	                                                : D3DCREATE_BUFFER_2_FRAMES;
	if (flags) logmsg("Direct3D behaviour flags %08lx", flags);
	if (!s_d3d || FAILED(s_d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING | flags, &pp, &dev))) {
		logmsg("Direct3D device: CreateDevice failed");
		return false;
	}
	dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &s_backBuffer);
	dev->SetVerticalBlankCallback(xe_vblank);
	D3DSURFACE_PARAMETERS sp;
	memset(&sp, 0, sizeof(sp));
	// after the back buffer when all fits, else over it: presenting leaves EDRAM undefined anyway
	// (copyEfb draws the EFB's colour back after it)
	const DWORD efb = edramTiles(s_efbW, s_efbH), scratch = edramTiles(EFB_W, EFB_H) > edramTiles(bandW, bandH) ? edramTiles(EFB_W, EFB_H) : edramTiles(bandW, bandH);
	const DWORD back = edramTiles(1280, 720) + 2 * efb + scratch <= 2048 ? edramTiles(1280, 720) : 0;
	sp.Base = back;
	sp.HierarchicalZBase = 0xFFFFFFFF;
	HRESULT h1 = dev->CreateRenderTarget(s_efbW, s_efbH, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &s_efbColor, &sp);
	sp.Base = back + efb;
	sp.HierarchicalZBase = 0;
	HRESULT h2 = dev->CreateDepthStencilSurface(s_efbW, s_efbH, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &s_efbDepth, &sp);
	sp.Base = back + 2 * efb;
	sp.HierarchicalZBase = 0xFFFFFFFF;
	HRESULT h3 = dev->CreateRenderTarget(EFB_W, EFB_H, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &s_scratch, &sp);
	if (SUCCEEDED(h3) && bandW > 0 &&
	    FAILED(dev->CreateRenderTarget(bandW, bandH, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &s_band, &sp)))
		s_band = NULL;
	s_bandH = bandH;
	HRESULT h4 = dev->CreateTexture(s_efbW, s_efbH, 1, 0, D3DFMT_A8R8G8B8, 0, &s_efbTex, NULL);
	HRESULT h5 = dev->CreateTexture(s_efbW, s_efbH, 1, 0, D3DFMT_D24S8, 0, &s_depthTex, NULL);
	if (FAILED(h1) || FAILED(h2) || FAILED(h3) || FAILED(h4) || FAILED(h5)) {
		logmsg("EFB surfaces: %08x %08x %08x %08x %08x", unsigned(h1), unsigned(h2), unsigned(h3), unsigned(h4), unsigned(h5));
		return false;
	}
	{  // the resolved depth's 32-bit texels (depth << 8 | stencil), tiled as any 32-bit texture's
		D3DLOCKED_RECT r;
		s_depthTex->LockRect(0, &r, NULL, D3DLOCK_READONLY);
		s_depthTex->UnlockRect(0);
		XGSetTextureHeader(s_efbW, s_efbH, 1, 0, D3DFMT_A8R8G8B8, 0, 0, 0, 0, &s_depthAsColour, NULL, NULL);
		XGOffsetBaseTextureAddress(&s_depthAsColour, r.pBits, r.pBits);
	}
	static const D3DVERTEXELEMENT9 blitDecl[] = {
	    {0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
	    D3DDECL_END()};
	dev->CreateVertexDeclaration(blitDecl, &s_blitDecl);
	s_blitVs = compileVS(kBlitVs);
	s_texPs = compilePS(kTexPs);
	s_colorPs = compilePS(kColorPs);
	s_copyPs = compilePS((std::string(kCopyFn) + kCopyPs).c_str());
	s_encodeI8Ps = compilePS((std::string(kCopyFn) + kEncodeI8Ps).c_str());
	if (!s_blitVs || !s_texPs || !s_colorPs || !s_copyPs || !s_encodeI8Ps) return false;
	efbTarget();
	plainState();
	// pixel centres at +0.5, as GL and the GameCube have them: without it every frame sat half a
	// pixel down and right of the PC port's (the field shots' best match was at dx, dy = +0.5)
	dev->SetRenderState(D3DRS_HALFPIXELOFFSET, TRUE);
	dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);
	logmsg("Xenos: EFB %dx%d in EDRAM, back buffer 1280x720%s", s_efbW, s_efbH, back ? "" : " over it");
	return true;
}

bool execHasBand() { return s_band != NULL; }

IDirect3DTexture9* efbTexture(bool depth) { return depth ? s_depthTex : s_efbTex; }

// ------------------------------------------------------------------ draws
static void execDrawState(const DrawState& s)
{
	if (!s_scissorKnown || memcmp(&s.scissor, &s_scissor, sizeof s_scissor)) {
		dev->SetScissorRect(&s.scissor);
		s_scissor = s.scissor;
		s_scissorKnown = true;
	}
	for (int i = 0; i < RS_COUNT; i++) setRS(i, s.rs[i]);
}
void emitDrawState(const DrawState& s) { emit<DrawState, execDrawState>(s); }

struct TexArgs {
	int map;
	IDirect3DBaseTexture9* tex;
	DWORD ss[SS_COUNT];
};
static void execTexture(const TexArgs& a)
{
	setTexture(a.map, a.tex);
	for (int i = 0; i < SS_COUNT; i++) setSS(a.map, i, a.ss[i]);
}
void emitTexture(int map, IDirect3DBaseTexture9* tex, const DWORD sampler[SS_COUNT])
{
	TexArgs a = {map, tex};
	memcpy(a.ss, sampler, sizeof a.ss);
	emit<TexArgs, execTexture>(a);
}

struct ConstsHead {
	BOOL vs;
	int first, count;
};
static void execConsts(const void* p)
{
	const ConstsHead* h = (const ConstsHead*)p;
	const float* rows = (const float*)(h + 1);
	if (h->vs) dev->SetVertexShaderConstantF(UINT(h->first), rows, UINT(h->count));
	else dev->SetPixelShaderConstantF(UINT(h->first), rows, UINT(h->count));
}
void emitConsts(bool vs, int first, const float* rows, int count)
{
	ConstsHead* h = (ConstsHead*)emitRaw(execConsts, uint32_t(sizeof(ConstsHead) + size_t(count) * 16));
	h->vs = vs;
	h->first = first;
	h->count = count;
	memcpy(h + 1, rows, size_t(count) * 16);
}

static void execDraw(const DrawArgs& a)
{
	dev->SetVertexDeclaration(a.decl);
	dev->SetVertexShader(a.vs);
	if (a.ps) {
		dev->SetPixelShader(a.ps);
	} else {  // geometry only: white
		static const float white[4] = {1, 1, 1, 1};
		dev->SetPixelShader(s_colorPs);
		dev->SetPixelShaderConstantF(0, white, 1);
	}
	dev->SetStreamSource(0, a.vb, a.voff, a.stride);
	dev->SetIndices(a.ib);
	dev->DrawIndexedPrimitive(a.type, 0, 0, a.verts, a.firstIndex, a.prims);
}
void emitDraw(const DrawArgs& a) { emit<DrawArgs, execDraw>(a); }

// ------------------------------------------------------------------ copies, clears, presenting
static void execClear(const ClearArgs& a)
{
	plainState();
	setRS(RS_COLORWRITE, a.colourMask);
	setRS(RS_ZENABLE, a.depth ? D3DZB_TRUE : D3DZB_FALSE);
	setRS(RS_ZFUNC, D3DCMP_ALWAYS);
	setRS(RS_ZWRITE, a.depth);
	dev->SetPixelShader(s_colorPs);
	dev->SetPixelShaderConstantF(0, a.colour, 1);
	// the depth through the viewport's range: a quad at z = w = 1 can fall to the far clip plane
	D3DVIEWPORT9 vp = {0, 0, DWORD(s_efbW), DWORD(s_efbH), a.z, a.z};
	dev->SetViewport(&vp);
	blit(a.x0, a.y0, a.x1, a.y1, 0, 0, 0, 0, 0.5f);
	efbTarget();
}
void emitClear(const ClearArgs& a)
{
	emit<ClearArgs, execClear>(a);
	constsClobbered();
}

struct ResolveArgs {
	BOOL depth, rect;
	RECT r;
	IDirect3DBaseTexture9* tex;
};
static void execResolve(const ResolveArgs& a)
{
	D3DPOINT at = {0, 0};
	dev->Resolve(a.depth ? D3DRESOLVE_DEPTHSTENCIL : D3DRESOLVE_RENDERTARGET0, a.rect ? (D3DRECT*)&a.r : NULL,
	             a.tex ? a.tex : efbTexture(a.depth != 0), a.rect ? &at : NULL, 0, 0, NULL, 0.0f, 0, NULL);
}
void emitResolve(bool depth, const RECT* r, IDirect3DBaseTexture9* tex)
{
	ResolveArgs a = {depth, r != NULL};
	if (r) a.r = *r;
	a.tex = tex;
	emit<ResolveArgs, execResolve>(a);
}

static void execCopyPass(const CopyPassArgs& a)
{
	const ResolveArgs efb = {a.depth};  // the EFB's colour, or depth, into its texture
	execResolve(efb);
	if (a.tex) target(s_band, NULL, a.w, a.h < s_bandH ? a.h : s_bandH);
	else target(s_scratch, NULL, a.w, a.h);
	plainState();
	// ponytail: a half-size depth copy takes one pixel of each 2x2 (bytes cannot be filtered)
	setTexture(0, a.depth ? &s_depthAsColour : s_efbTex);
	blitSampler(a.depth ? D3DTEXF_POINT : D3DTEXF_LINEAR);
	dev->SetPixelShader(s_copyPs);
	dev->SetPixelShaderConstantF(0, a.pc, 3);
	if (a.red) {  // texture copies come out red: does the copy reach its sampler?
		static const float red[4] = {1, 0, 0, 1};
		dev->SetPixelShader(s_colorPs);
		dev->SetPixelShaderConstantF(0, red, 1);
	}
	if (!a.tex) {
		// the viewport is the copy: the quad covers it, sampling the copied rectangle
		blit(-1, 1, 1, -1, a.uv[0], a.uv[1], a.uv[2], a.uv[3]);
		setTexture(0, NULL);
		return;
	}
	// in bands of the band target, each resolved to its rows of the texture
	for (int r0 = 0; r0 < a.h; r0 += s_bandH) {
		const int bh = a.h - r0 < s_bandH ? a.h - r0 : s_bandH;
		if (bh != s_bandH) {
			D3DVIEWPORT9 vp = {0, 0, DWORD(a.w), DWORD(bh), 0.0f, 1.0f};
			dev->SetViewport(&vp);
		}
		float pc[12];
		memcpy(pc, a.pc, sizeof pc);
		pc[5] += float(r0) * pc[7];  // a full-size copy's top: the band's first row
		if (!a.red) dev->SetPixelShaderConstantF(0, pc, 3);
		const float v0 = a.uv[1] + (a.uv[3] - a.uv[1]) * float(r0) / float(a.h);
		const float v1 = a.uv[1] + (a.uv[3] - a.uv[1]) * float(r0 + bh) / float(a.h);
		blit(-1, 1, 1, -1, a.uv[0], v0, a.uv[2], v1);
		D3DRECT r = {0, 0, a.w, bh};
		D3DPOINT at = {0, r0};
		dev->Resolve(D3DRESOLVE_RENDERTARGET0, &r, a.tex, &at, 0, 0, NULL, 0.0f, 0, NULL);
	}
	setTexture(0, NULL);
}
void emitCopyPass(const CopyPassArgs& a)
{
	emit<CopyPassArgs, execCopyPass>(a);
	constsClobbered();
}

static void execEncodeI8(const EncodeArgs& a)
{
	D3DVIEWPORT9 ev = {0, 0, 256, DWORD(a.rows), 0.0f, 1.0f};
	dev->SetViewport(&ev);
	setTexture(0, a.depth ? &s_depthAsColour : s_efbTex);
	dev->SetPixelShader(s_encodeI8Ps);
	// c0-c2 are the copy pass's
	dev->SetPixelShaderConstantF(3, a.pc, 2);
	blit(-1, 1, 1, -1, 0, 0, 1, 1);
	setTexture(0, NULL);
}
void emitEncodeI8(const EncodeArgs& a)
{
	emit<EncodeArgs, execEncodeI8>(a);
	constsClobbered();
}

static void execEfbTarget(const int&) { efbTarget(); }
void emitEfbTarget() { emit<int, execEfbTarget>(0); }

struct ShowArgs {
	IDirect3DTexture9* xfb;
	BOOL drawBack;
};
static void execShow(const ShowArgs& a)
{
	target(s_backBuffer, NULL, 1280, 720);
	plainState();
	dev->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
	if (a.xfb) {  // at the full height: 4:3, or as wide as the EFB (16:9 fills the screen)
		setTexture(0, a.xfb);
		blitSampler(D3DTEXF_LINEAR);
		dev->SetPixelShader(s_texPs);
		const float pw = fminf(960.0f * s_wide, 1280.0f);
		const float x0 = (1280.0f - pw) / 2.0f / 640.0f - 1.0f, x1 = -x0;
		blit(x0, 1.0f, x1, -1.0f, 0, 0, 1, 1);
		setTexture(0, NULL);
	}
	{
		static int gpuSync = -1;  // SMS_XE_GPUSYNC=1: wait for the GPU to finish the frame (GPU-bound?)
		if (gpuSync < 0) gpuSync = getenv("SMS_XE_GPUSYNC") && getenv("SMS_XE_GPUSYNC")[0] == '1';
		XeClock c(g_xeWait);  // with vsync, the wait for the flip
		XeClock p(g_xePresent);
		if (gpuSync) dev->BlockUntilIdle();
		dev->Present(NULL, NULL, NULL, NULL);
	}
	efbTarget();
	if (a.drawBack) {  // presenting took EDRAM's contents: the EFB's colour (resolved before) back
		plainState();
		setTexture(0, s_efbTex);
		setSS(0, SS_MINFILTER, D3DTEXF_POINT);
		setSS(0, SS_MAGFILTER, D3DTEXF_POINT);
		dev->SetPixelShader(s_texPs);
		blit(-1, 1, 1, -1, 0, 0, 1, 1);
		setTexture(0, NULL);
	}
}
void emitShow(IDirect3DTexture9* xfb, bool drawBack)
{
	ShowArgs a = {xfb, drawBack};
	emit<ShowArgs, execShow>(a);
	constsClobbered();
}

struct QueryArgs {
	IDirect3DQuery9* q;
	BOOL begin;
};
static void execQuery(const QueryArgs& a) { a.q->Issue(a.begin ? D3DISSUE_BEGIN : D3DISSUE_END); }
void emitQuery(IDirect3DQuery9* q, bool begin)
{
	if (!q) return;
	QueryArgs a = {q, begin};
	emit<QueryArgs, execQuery>(a);
}

// ------------------------------------------------------------------ releases
// Released once the GPU has passed the fence inserted after their last use.
struct Dead {
	IDirect3DBaseTexture9* tex;
	DWORD fence;
};
static std::vector<Dead> s_dead;

static void execRelease(IDirect3DBaseTexture9* const& t)
{
	for (int i = 0; i < 8; i++)  // D3D keeps no reference: nothing may stay bound to it
		if (s_tex[i] == t) setTexture(i, NULL);
	Dead d = {t, dev->InsertFence()};
	s_dead.push_back(d);
}
void releaseLater(IDirect3DBaseTexture9* t)
{
	if (t) emit<IDirect3DBaseTexture9*, execRelease>(t);
}

static void execFrameEnd(const int&)
{
	size_t keep = 0;
	for (size_t i = 0; i < s_dead.size(); i++) {
		if (dev->IsFencePending(s_dead[i].fence)) s_dead[keep++] = s_dead[i];
		else s_dead[i].tex->Release();
	}
	s_dead.resize(keep);
}
void releaseFrameEnd() { emit<int, execFrameEnd>(0); }

}  // namespace xe
}  // namespace gx
