// The Xenos back end of sms_gx: the renderer side of platform/gx/src/gx_internal.h on the
// Xbox 360's Direct3D 9. The GX core (gx_api.cpp, gx_fifo.cpp) turns every GX call, display
// list and pipe write into register writes and decoded vertex batches; this draws them.
//
//   xe_gx_render.cpp  device, EFB in EDRAM, batches, pipeline state, EFB copies, present
//   xe_gx_hlsl.cpp    HLSL generated from the XF/BP registers (as gx_shader.cpp's GLSL)
//   xe_gx_tex.cpp     GX textures decoded into D3D textures, samplers, EFB copy textures
//   xe_gx_codec.cpp   the decoders and encoders of gx_texture.cpp (carved by xenon/tools/xcodec.py)
#ifndef XE_GX_H
#define XE_GX_H

#include "../msvc/include/xe_win.h"
#include "../../platform/gx/src/gx_internal.h"
#include <stdlib.h>

namespace gx {
namespace xe {

enum { EFB_W = 640, EFB_H = 528 };

extern IDirect3DDevice9* dev;

// Shaders (xe_gx_hlsl.cpp). The program for the current register state and a batch of
// vertex format `fmt` (VtxFmtBits); null when it does not compile (the batch is skipped).
struct Program {
	IDirect3DVertexShader9* vs;
	IDirect3DPixelShader9* ps;
	int id;
};
const Program* programFor(uint32_t fmt);
void programWarmup();  // the programs made in earlier runs, made now (at the start: no hitch in play)
// A shader of the back end's own (blits, clears, copies), from HLSL; null on error.
IDirect3DVertexShader9* compileVS(const char* src);
IDirect3DPixelShader9* compilePS(const char* src);
extern uint32_t g_xeCompiles, g_xeCacheHits;  // shaders compiled, and read from the disk cache
extern double g_xeCompileSeconds;

// Constant registers the generated shaders read (xe_gx_hlsl.cpp writes them into the code).
enum {
	VC_XF = 0,        // c0-63 matrices, c64-95 normal matrix rows, c96-159 post matrices
	VC_NRM = 64,
	VC_POST = 96,
	VC_LIGHT = 160,   // 8 lights x 5: colour, A, K, position, direction
	VC_PROJ = 200,    // 2
	VC_VP = 202,      // 2
	VC_EFB = 204,
	VC_CHAN = 205,    // amb0, amb1, mat0, mat1
	VC_DEFMTX = 209,  // 3: the batch's default matrix indices, bytes 0-11
	VC_COUNT = 212,
	PC_TEVREG = 0,    // 4
	PC_KONST = 4,     // 4
	PC_TEXSCALE = 8,  // 8 (.xy)
	PC_TEXSIZE = 16,  // 8 (.xy)
	PC_ALPHAREF = 24,
	PC_FOG = 25,
	PC_FOGCOLOR = 26,
	PC_INDMTX = 27,   // 6
	PC_COUNT = 33,
};

// ------------------------------------------------------------------ render packets
// The back end is split in two (xenon/ARCHITECTURE.md, phase 1). The game side (xe_gx_render.cpp,
// xe_gx_tex.cpp, xe_gx_hlsl.cpp) reads the GX registers and game RAM, keeps every cache, and emits
// packets: plain values, nothing that points into state the game goes on changing. The render side
// (xe_gx_exec.cpp) executes them in order and is alone in using the device's command stream (Set*,
// Draw*, Resolve, Clear, Present, fences, query issues). The game side still creates resources
// (textures, shaders, declarations, queries), fills textures nothing has drawn with yet, and reads
// what the GPU wrote (query results, resolved textures) once a kick or a token says it is there.
// Packets are executed at kick(): before any read of the GPU's results, at each present, and
// whenever 64 KiB are queued (SMS_XE_KICK=bytes).
// the device and the EFB (false: none); bandW x bandH: the scratch target of copies at the internal scale
bool execInit(int efbW, int efbH, float wide, int bandW, int bandH);
bool execHasBand();  // the band target was made (copies at the internal scale need it)
IDirect3DTexture9* efbTexture(bool depth);  // where emitResolve(depth, NULL, NULL) puts the EFB
// EDRAM tiles of a 32-bit surface (d3d9gpu.h: 80x16 pixels, 5120 bytes a tile); EDRAM has 2048
inline DWORD edramTiles(int w, int h) { return DWORD((w + 79) / 80) * DWORD((h + 15) / 16); }
void kick();
// Fences: a token marks the point in the packet stream where it was taken; waitToken returns once
// the GPU has done everything before it.
uint32_t fenceToken();
void waitToken(uint32_t token);

// The device states draws set, in DrawState's order.
enum {
	RS_SCISSOR, RS_CULL, RS_ZENABLE, RS_ZFUNC, RS_ZWRITE, RS_COLORWRITE, RS_BLEND, RS_BLENDOP, RS_SRCBLEND,
	RS_DESTBLEND, RS_SEPARATEALPHA, RS_BLENDOPALPHA, RS_SRCBLENDALPHA, RS_DESTBLENDALPHA, RS_BLENDFACTOR,
	RS_POINTSIZE, RS_LINEWIDTH, RS_COUNT
};
struct DrawState {
	RECT scissor;
	DWORD rs[RS_COUNT];
};
enum { SS_ADDRESSU, SS_ADDRESSV, SS_MAGFILTER, SS_MINFILTER, SS_MIPFILTER, SS_LODBIAS, SS_MAXMIPLEVEL, SS_COUNT };
struct DrawArgs {
	IDirect3DVertexDeclaration9* decl;
	IDirect3DVertexShader9* vs;
	IDirect3DPixelShader9* ps;  // null: white (SMS_XE_DEBUG 32)
	IDirect3DVertexBuffer9* vb;
	IDirect3DIndexBuffer9* ib;
	D3DPRIMITIVETYPE type;
	UINT voff, stride, verts, firstIndex, prims;
};
struct ClearArgs {  // a quad over clip rectangle x0..y1 at depth z, with these write enables
	float x0, y0, x1, y1, z;
	float colour[4];
	DWORD colourMask;
	BOOL depth;
};
struct CopyPassArgs {  // an EFB copy's conversion into the scratch target (kCopyPs)
	BOOL depth, red;
	int w, h;
	float pc[12];  // kCopyPs's c0-c2
	float uv[4];   // the copied rectangle in the EFB texture
	IDirect3DTexture9* tex;  // null: left in the scratch target; else drawn in bands and resolved into it
};
struct EncodeArgs {  // an I8 copy's GX encoding into the scratch target (kEncodeI8Ps)
	BOOL depth;
	int rows;
	float pc[8];  // c3-c4
};
void emitDrawState(const DrawState& s);
void emitTexture(int map, IDirect3DBaseTexture9* tex, const DWORD sampler[SS_COUNT]);
void emitConsts(bool vs, int first, const float* rows, int count);
void emitDraw(const DrawArgs& a);
void emitClear(const ClearArgs& a);
// The EFB's colour (or depth) into `tex` (null: the EFB's own texture, which copies and peeks read),
// all of it, or rectangle r of the current target to tex's corner.
void emitResolve(bool depth, const RECT* r, IDirect3DBaseTexture9* tex);
void emitCopyPass(const CopyPassArgs& a);
void emitEncodeI8(const EncodeArgs& a);
void emitEfbTarget();  // the EFB as the target again, after copy passes
void emitShow(IDirect3DTexture9* xfb, bool drawBack);  // presents xfb; drawBack: the EFB's colour put back
void emitQuery(IDirect3DQuery9* q, bool begin);
void releaseLater(IDirect3DBaseTexture9* t);  // once the GPU no longer reads it
void releaseFrameEnd();  // once per display copy: frees what the GPU is done with
void constsClobbered();  // xe_gx_render.cpp: a blit packet overwrote shader constants

// Textures (xe_gx_tex.cpp): binds map `map` to sampler `map` with its GX sampler state, and
// gives its size in texels as the game sees it.
void bindTexture(int map, float* w, float* h);
void textureFrameEnd();  // once per display copy: frees what the GPU is done with
// EFB copies made into textures, by destination address (the copy is RGBA: what sampling
// the copy's GX format would give).
IDirect3DTexture9* copyTexture(const void* dest, int w, int h);  // made or remade for this size
bool isCopyTexture(const void* addr);  // an EFB copy is registered there
void copySetBytes(const void* dest, uint32_t bytes);  // the copy was written back: its bytes in RAM
void copyCpuWrote(const void* p, uint32_t size);      // drops copies written back there
bool tracing();  // SMS_XE_TRACE_FRAME's frame is being drawn
float internalScale();  // the EFB's size over the GameCube's (xe_gx_render.cpp)
// Frame-time accounting (xe_gx_render.cpp's [perf] line): seconds spent waiting for the GPU, in
// texture work and executing packets, and a clock that adds the time of its scope to one of them.
extern double g_xeWait, g_xeTex, g_xeExec, g_xePresent;
extern double g_xeTexCreate, g_xeTexDecode, g_xeTexStore, g_xeTexMips;  // parts of uploads (spike lines)
struct XeClock {
	double* acc;
	double t0;
	explicit XeClock(double& a) : acc(&a), t0(monoSeconds()) {}
	~XeClock() { *acc += monoSeconds() - t0; }
};
// Adds its scope's time to `acc`, less what went to GPU waits, textures and packets meanwhile.
struct OwnClock {
	double* acc;
	double t0, w0, x0, e0;
	explicit OwnClock(double& a) : acc(&a), t0(monoSeconds()), w0(g_xeWait), x0(g_xeTex), e0(g_xeExec) {}
	~OwnClock() { *acc += monoSeconds() - t0 - (g_xeWait - w0) - (g_xeTex - x0) - (g_xeExec - e0); }
};
// Whether n bytes (4-byte aligned, a multiple of 4) are the same: the XDK's memcmp took 1.6 us for
// a 460-byte shader key on the console, a third of a draw's state work.
inline bool sameWords(const void* a, const void* b, size_t n)
{
	const uint32_t* x = (const uint32_t*)a;
	const uint32_t* y = (const uint32_t*)b;
	for (size_t i = 0; i < n / 4; i++)
		if (x[i] != y[i]) return false;
	return true;
}
// A growing buffer of plain data: std::vector's resize writes each new element one at a time on
// this compiler (12% of a Delfino frame on the console in the batch buffers).
template <class T> struct PodBuf {
	T* p;
	size_t n, cap;
	PodBuf() : p(NULL), n(0), cap(0) {}
	void resize(size_t want)
	{
		if (want > cap) {
			cap = want > 2 * cap ? want : 2 * cap;
			p = (T*)realloc(p, cap * sizeof(T));
		}
		n = want;
	}
	size_t size() const { return n; }
	bool empty() const { return n == 0; }
	void clear() { n = 0; }
	T& operator[](size_t i) { return p[i]; }
};
}  // namespace xe
// The render thread (xe_gx_queue.cpp; SMS_XE_GX_THREAD=0: none): where the GX core and everything below
// it runs. Without it, the game's thread.
bool xeGxThreaded();
extern double g_xeGameWait;  // seconds the game's thread waited for the render thread
double xeRenderIdleSeconds();  // the render thread's time waiting for commands
bool xeOnRenderThread();
uint32_t xeGameFrames();  // display copies the game has made
void xeGxStart(void (*init)(void*), void* arg);  // init runs where the core will
void xeCoreCall(void (*fn)(void*), void* arg);  // runs it there, after what came before; waits
void xeCorePostData(void (*fn)(const void*), const void* args, uint32_t n);  // the same, not waiting
void xeRenderShown(uint32_t frames);
void xeBenchFrame();
double xeSpikeMs();  // SMS_XE_SPIKES: frames longer than this are logged (0: none)
void xeApiMemoryWritten(const void* p, size_t n);  // the game wrote memory GX may have cached
// Threaded: what draw-sync callbacks read (xe_gx_render.cpp's snapshots, a frame old) and the
// write-backs the game's thread stores at its display copy.
void xeSnapPeek(int group, bool depth);
bool xeSnapPeekRead(uint32_t frame, int group, bool depth, int x, int y, uint32_t* v);
void xeSnapMetric(int slot);
bool xeSnapMetricRead(uint32_t frame, int slot, uint32_t* v);
bool xeSnapMetricLast(uint32_t frame, int slot, uint32_t* v);  // the render thread drained
void xeStoreWriteBacks(uint32_t frame);  // the bench's frame (xe_gx_render.cpp), on the game's thread  // the render thread showed a display copy
// gx_texture.cpp's arbitrary-mip test (carved into xe_gx_codec.cpp): how far a level is from a
// reduction of the one above it; a mean above kArbitraryMipThreshold marks levels drawn as an effect
float mipLevelDiff(const uint8_t* prev, uint32_t pw, uint32_t ph, const uint8_t* cur, uint32_t w, uint32_t h);
extern const float kArbitraryMipThreshold;
namespace xe {

}  // namespace xe
}  // namespace gx

#endif
