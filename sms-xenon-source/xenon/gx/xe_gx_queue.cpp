// The GX API's way into the GX core (xenon/ARCHITECTURE.md, phase 4). gx_api.cpp (port patch 17)
// keeps the registers it reads back in g_api, as the SDK keeps __GXData: its own writes change it,
// display lists do not. Everything that reaches the core (g, the vertex loader, the display-list
// cache, the renderer) goes through the functions here.
//
// By default the core runs on a render thread of its own (HW thread 2), which owns the
// device. The game thread writes commands into a ring (one producer: the game's threads are fibers
// of one hardware thread) and goes on; the render thread executes them in order. The pipe's bytes
// (GXBegin data, J3D's direct writes) collect in a buffer of the game thread's (port patch 18) and
// go into the ring before any other command. What the game reads back (peeks, metrics, XFBs, draw
// done) waits until the render thread has run everything before it. SMS_XE_GX_THREAD=0: each command
// runs at once on the game thread (the exact mode: xregress's references; with the render thread
// stages 55 and 60, where gameplay reads pixel metrics, drift from them, deterministically).
#include "xe_gx.h"
#include <vector>
#include <ppcintrinsics.h>

extern "C" {
extern uint8_t* GXPC_PipeBuf;
extern uint32_t GXPC_PipeLen, GXPC_PipeFast;
void xeCoreMemoryWritten(const void* p, uint32_t size);  // gx_fifo.cpp's GXPC_* (port patch 18)
void xeCoreAddBigEndianRange(const void* p, uint32_t size);
void xeCoreRemoveBigEndianRange(const void* p);
void xeCoreSetArrayBigEndian(int attr, int bigEndian);
void xeCoreSetDefaultArrayBigEndian(int bigEndian);
int port_audio_dsp_reply_ready(void);  // port patch 14
void port_irq_check(void);
}

namespace gx {

State g_api;
double g_xeGameWait;  // the game's thread waiting for the render thread (g_xeWait is the render side's)
extern bool g_xePipeOwn;  // port patch 18
extern void (*drawSyncCallback)(uint16_t);

namespace {

bool s_threaded;

// ------------------------------------------------------------------ the ring
// Commands: a header (the function that runs it, its payload's size, its size in the ring), then
// the payload; 16-byte aligned. Positions count bytes from the start and wrap by the mask; a
// command never wraps (the rest of the ring is skipped with a header of no function).
enum { kRingBytes = 2 << 20, kPublishBytes = 4096, kGamePipeBytes = 32 << 10 };
typedef void (*CmdFn)(const uint8_t* p, uint32_t n);
struct Hdr {
	CmdFn fn;
	uint32_t n, total, pad;
};
uint8_t* s_ring;
volatile uint32_t s_pub;   // published by the game thread
volatile uint32_t s_done;  // run by the render thread
uint32_t s_write;          // written by the game thread (published at s_pub)
volatile LONG s_renderIdle, s_gameWaiting;
HANDLE s_workEvent, s_spaceEvent, s_syncEvent, s_frameEvent;
volatile uint32_t s_renderFrames;
double s_renderIdleSeconds;  // the render thread waiting for commands  // display copies the render thread has shown
DWORD s_renderThreadId, s_gameThreadId;
uint8_t s_gamePipe[kGamePipeBytes];

void publish()
{
	if (s_pub == s_write) return;
	__lwsync();  // the commands before the position
	s_pub = s_write;
	__sync();  // against the render thread's going idle
	if (s_renderIdle) SetEvent(s_workEvent);
}

void waitSpace(uint32_t need)
{
	while (kRingBytes - (s_write - s_done) < need) {
		publish();
		s_gameWaiting = 1;
		__sync();
		if (kRingBytes - (s_write - s_done) < need) WaitForSingleObject(s_spaceEvent, INFINITE);
		s_gameWaiting = 0;
	}
}

uint8_t* reserveRaw(CmdFn fn, uint32_t n)
{
	if (GetCurrentThreadId() != s_gameThreadId) fatal("GX called from a second host thread: the ring has one writer");
	const uint32_t total = (uint32_t(sizeof(Hdr)) + n + 15) & ~15u;
	uint32_t at = s_write & (kRingBytes - 1);
	if (at + total > kRingBytes) {
		const uint32_t skip = kRingBytes - at;
		waitSpace(skip + total);
		Hdr* h = (Hdr*)(s_ring + at);
		h->fn = NULL;
		h->n = 0;
		h->total = skip;
		s_write += skip;
		at = 0;
	}
	waitSpace(total);
	Hdr* h = (Hdr*)(s_ring + at);
	h->fn = fn;
	h->n = n;
	h->total = total;
	return (uint8_t*)(h + 1);
}

void commit()
{
	const Hdr* h = (const Hdr*)(s_ring + (s_write & (kRingBytes - 1)));
	s_write += h->total;
	if (s_write - s_pub >= kPublishBytes) publish();
}

void runPipe(const uint8_t* p, uint32_t n) { pipeWrite(p, n); }

void flushGamePipe()
{
	if (!GXPC_PipeLen) return;
	const uint32_t n = GXPC_PipeLen;
	GXPC_PipeLen = 0;
	memcpy(reserveRaw(runPipe, n), s_gamePipe, n);
	commit();
}

// Memory written by another host thread (THP's decoder zeroes its own state with DCZeroRange): the
// GX layer has one writer, the game's thread, which takes these at its next GX call
struct ForeignWrite {
	const void* p;
	uint32_t n;
};
CRITICAL_SECTION s_foreignLock;
std::vector<ForeignWrite> s_foreign, s_foreignTaken;
volatile LONG s_foreignPending;
void applyForeignWrites();

// A command of `n` payload bytes, after the pipe's bytes so far. The DSP's reply, when its subframe
// is done, is delivered here, as the inline flushBatch does (port patch 14).
uint8_t* reserve(CmdFn fn, uint32_t n)
{
	if (s_foreignPending) applyForeignWrites();
	if (port_audio_dsp_reply_ready()) port_irq_check();
	flushGamePipe();
	return reserveRaw(fn, n);
}

template <class T> void post(CmdFn fn, const T& args)
{
	memcpy(reserve(fn, sizeof(T)), &args, sizeof(T));
	commit();
}

void signalSync(const uint8_t*, uint32_t) { SetEvent(s_syncEvent); }

struct RunArgs {
	void (*fn)(void*);
	void* arg;
};
void cRunData(const uint8_t* p, uint32_t)
{
	void (*fn)(const void*);
	memcpy(&fn, p, sizeof fn);
	fn(p + sizeof fn);
}
void cRun(const uint8_t* p, uint32_t)
{
	const RunArgs& a = *(const RunArgs*)p;
	a.fn(a.arg);
}

// The game's thread waiting for the render thread, between GX commands: a DSP reply that is ready
// meanwhile is delivered, as reserve() and the inline flushBatch deliver it, or the mixer misses it.
void gameWait(HANDLE h)
{
	while (WaitForSingleObject(h, 1) == WAIT_TIMEOUT)
		if (port_audio_dsp_reply_ready()) port_irq_check();
}

// Returns once the render thread has run everything posted so far.
void drain()
{
	reserve(signalSync, 0);
	commit();
	publish();
	xe::XeClock c(g_xeGameWait);
	gameWait(s_syncEvent);
}

DWORD WINAPI renderThread(void*)
{
	for (;;) {
		const uint32_t pub = s_pub;
		uint32_t r = s_done;
		if (r == pub) {
			s_renderIdle = 1;
			__sync();
			if (s_pub == s_done) {
				const double t0 = monoSeconds();
				WaitForSingleObject(s_workEvent, INFINITE);
				s_renderIdleSeconds += monoSeconds() - t0;
			}
			s_renderIdle = 0;
			continue;
		}
		__lwsync();  // the commands, after their position
		while (r != pub) {
			const Hdr* h = (const Hdr*)(s_ring + (r & (kRingBytes - 1)));
			if (h->fn) h->fn((const uint8_t*)(h + 1), h->n);
			r += h->total;
			if (r - s_done >= 256 * 1024) {  // room for a waiting game thread
				__lwsync();
				s_done = r;
				__sync();
				if (s_gameWaiting) SetEvent(s_spaceEvent);
			}
		}
		__lwsync();
		s_done = r;
		__sync();
		if (s_gameWaiting) SetEvent(s_spaceEvent);
	}
}

// ------------------------------------------------------------------ the API's registers
void shadowBP(uint32_t v)
{
	const uint32_t reg = v >> 24;
	uint32_t data = v & 0xFFFFFF;
	if (reg == BP_MASK) {
		g_api.bpMask = data;
		return;
	}
	if (g_api.bpMask != 0xFFFFFF) {
		data = (g_api.bp[reg] & ~g_api.bpMask) | (data & g_api.bpMask);
		g_api.bpMask = 0xFFFFFF;
	}
	if (reg >= BP_TEV_REG && reg < BP_TEV_REG + 8 && (data & 0x800000)) {
		g_api.kreg[reg & 7] = data;
		return;
	}
	g_api.bp[reg] = data;
	if (reg == BP_PE_TOKEN || reg == BP_PE_TOKEN_INT) g_api.drawSyncToken = uint16_t(data & 0xFFFF);
}

void shadowCP(uint8_t reg, uint32_t v)
{
	switch (reg & 0xF0) {
	case CP_MATIDX_A: g_api.cpMatA = v; break;
	case CP_MATIDX_B: g_api.cpMatB = v; break;
	case CP_VCD_LO: g_api.cpVcdLo = v; break;
	case CP_VCD_HI: g_api.cpVcdHi = v; break;
	case CP_VAT_A: g_api.cpVatA[reg & 7] = v; break;
	case CP_VAT_B: g_api.cpVatB[reg & 7] = v; break;
	case CP_VAT_C: g_api.cpVatC[reg & 7] = v; break;
	default: break;
	}
}

void shadowXF(uint16_t addr, uint32_t n, const uint32_t* w)
{
	for (uint32_t i = 0; i < n; i++, addr++) {
		if (addr < XF_MEM_WORDS) g_api.xfMem[addr] = w[i];
		else if (addr >= XF_REG_BASE && addr < XF_REG_BASE + XF_REG_COUNT) g_api.xfReg[addr - XF_REG_BASE] = w[i];
	}
}

// ------------------------------------------------------------------ the commands
struct CPArgs {
	uint32_t reg, v;
};
struct XFArgs {
	uint16_t addr;
	uint16_t n;
};
struct PtrArgs {
	const void* p;
	uint32_t n;
};
struct ArrayArgs {
	const void* base;
	int slot;
	uint32_t stride;
};
struct TlutArgs {
	uint32_t off, bytes;
};
struct IntArgs {
	int a, b;
};
void cBP(const uint8_t* p, uint32_t) { writeBP(*(const uint32_t*)p); }
void cCP(const uint8_t* p, uint32_t)
{
	const CPArgs& a = *(const CPArgs*)p;
	writeCP(uint8_t(a.reg), a.v);
}
void cXF(const uint8_t* p, uint32_t)
{
	const XFArgs& a = *(const XFArgs*)p;
	writeXF(a.addr, a.n, (const uint32_t*)(p + sizeof(XFArgs)));
}
void cFlush(const uint8_t*, uint32_t) { flushBatch(); }
void cCallDisplayList(const uint8_t* p, uint32_t)
{
	const PtrArgs& a = *(const PtrArgs*)p;
	callDisplayList((const uint8_t*)a.p, a.n);
}
void cTextureInvalidateAll(const uint8_t*, uint32_t) { textureInvalidateAll(); }
void cResetState(const uint8_t*, uint32_t) { resetState(); }
void setArray(int slot, const void* base, uint32_t stride)
{
	g.arrayBase[slot] = static_cast<const uint8_t*>(base);
	g.arrayStride[slot] = stride;
	g.arrayBigEndian[slot] = g_defaultArrayBE || isBigEndianData(base);
	g_arrayGen++;
}
void cSetArray(const uint8_t* p, uint32_t)
{
	const ArrayArgs& a = *(const ArrayArgs*)p;
	setArray(a.slot, a.base, a.stride);
}
void loadTlut(uint32_t off, const void* data, uint32_t bytes)
{
	onStateChange();
	memcpy(g.tlutMem + off, data, bytes);
}
void cLoadTlut(const uint8_t* p, uint32_t)
{
	const TlutArgs& a = *(const TlutArgs*)p;
	loadTlut(a.off, p + sizeof(TlutArgs), a.bytes);
}
void cTexCoordManual(const uint8_t* p, uint32_t)
{
	const IntArgs& a = *(const IntArgs*)p;
	g.tcManual[a.a] = a.b != 0;
}
void cMemoryWritten(const uint8_t* p, uint32_t)
{
	const PtrArgs& a = *(const PtrArgs*)p;
	xeCoreMemoryWritten(a.p, a.n);
}
void cAddBigEndianRange(const uint8_t* p, uint32_t)
{
	const PtrArgs& a = *(const PtrArgs*)p;
	xeCoreAddBigEndianRange(a.p, a.n);
}
void cRemoveBigEndianRange(const uint8_t* p, uint32_t)
{
	const PtrArgs& a = *(const PtrArgs*)p;
	xeCoreRemoveBigEndianRange(a.p);
}
void cSetArrayBigEndian(const uint8_t* p, uint32_t)
{
	const IntArgs& a = *(const IntArgs*)p;
	xeCoreSetArrayBigEndian(a.a, a.b);
}
void cSetDefaultArrayBigEndian(const uint8_t* p, uint32_t) { xeCoreSetDefaultArrayBigEndian(*(const int*)p); }

void (*s_apiDrawSync)(uint16_t);  // the game's draw-sync callback (threaded: called here)
uint32_t s_gameFrames;            // display copies the game has made (threaded: GXPC_FrameCount)

}  // namespace

void waitShown(uint32_t frames);
bool xeGxThreaded() { return s_threaded; }
double xeRenderIdleSeconds() { return s_renderIdleSeconds; }
bool xeOnRenderThread() { return !s_threaded || GetCurrentThreadId() == s_renderThreadId; }
uint32_t xeGameFrames() { return s_gameFrames; }

// Runs fn(arg) where the core runs and returns once it has (the render thread: after everything
// posted before it).
void xeCoreCall(void (*fn)(void*), void* arg)
{
	if (xeOnRenderThread()) {
		fn(arg);
		return;
	}
	const RunArgs a = {fn, arg};
	post(cRun, a);
	drain();
}

// Runs fn(arg) where the core runs, after everything posted before it, without waiting.
void xeCorePost(void (*fn)(void*), void* arg)
{
	if (xeOnRenderThread()) {
		fn(arg);
		return;
	}
	const RunArgs a = {fn, arg};
	post(cRun, a);
}

// Runs fn(args) where the core runs, on a copy of the n bytes at args, without waiting.
void xeCorePostData(void (*fn)(const void*), const void* args, uint32_t n)
{
	if (xeOnRenderThread()) {
		fn(args);
		return;
	}
	uint8_t* p = reserve(cRunData, uint32_t(sizeof(fn)) + n);
	memcpy(p, &fn, sizeof fn);
	memcpy(p + sizeof fn, args, n);
	commit();
}

// Waits until the render thread has shown `frames` display copies.
void waitShown(uint32_t frames)
{
	if (int32_t(s_renderFrames - frames) >= 0) {
		__lwsync();
		return;
	}
	publish();
	xe::XeClock c(g_xeGameWait);
	while (int32_t(s_renderFrames - frames) < 0) gameWait(s_frameEvent);
	__lwsync();  // what the render thread wrote before showing them
}

// The render thread has shown display copy `frames` (xe_gx_render.cpp).
void xeRenderShown(uint32_t frames)
{
	__lwsync();  // the frame's snapshots and hand-overs first
	s_renderFrames = frames;
	if (s_threaded) SetEvent(s_frameEvent);
}

// Starts the render thread (unless SMS_XE_GX_THREAD=0); the device is made there, by init.
void xeGxStart(void (*init)(void*), void* arg)
{
	{
		MEMORYSTATUS m;
		GlobalMemoryStatus(&m);
		logmsg("memory: %u KB free of %u KB", unsigned(m.dwAvailPhys >> 10), unsigned(m.dwTotalPhys >> 10));
	}
	s_gameThreadId = GetCurrentThreadId();
	InitializeCriticalSection(&s_foreignLock);
	const char* e = getenv("SMS_XE_GX_THREAD");
	s_threaded = !(e && e[0] == '0');
	if (s_threaded) {
		s_ring = (uint8_t*)XPhysicalAlloc(kRingBytes, MAXULONG_PTR, 4096, PAGE_READWRITE);
		s_workEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
		s_spaceEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
		s_syncEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
		s_frameEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
		HANDLE h = s_ring ? CreateThread(NULL, 256 * 1024, renderThread, NULL, CREATE_SUSPENDED, &s_renderThreadId) : NULL;
		if (!h) {
			logmsg("render thread: not started (no memory for the ring)");
			s_threaded = false;
		} else {
			XSetThreadProcessor(h, 2);
			ResumeThread(h);
			g_xePipeOwn = true;
			GXPC_PipeBuf = s_gamePipe;
			GXPC_PipeLen = 0;
			GXPC_PipeFast = kGamePipeBytes;
			logmsg("render thread: the GX core runs on hardware thread 2");
		}
	}
	xeCoreCall(init, arg);
}

// The game thread's pipe (port patch 18): bytes for the ring; GXPC_PipeFast lets gx_pc.h's inline
// writers fill it up, except while the pipe writes a display list into memory.
void xeGamePipePut(const uint8_t* bytes, uint32_t n)
{
	if (GXPC_PipeLen + n > kGamePipeBytes) flushGamePipe();
	memcpy(s_gamePipe + GXPC_PipeLen, bytes, n);
	GXPC_PipeLen += n;
}
void xeGamePipeRedirect(bool on) { GXPC_PipeFast = on ? 0 : kGamePipeBytes; }

// ------------------------------------------------------------------ gx_api.cpp's calls
void xeApiBP(uint32_t v)
{
	shadowBP(v);
	if (!s_threaded) {
		writeBP(v);
		return;
	}
	post(cBP, v);
	const uint32_t reg = v >> 24;
	if (reg == BP_COPY_EXEC && ((v >> 14) & 1)) {  // a display copy
		// the render thread may be a frame behind, no more; the write-backs of the frames before
		// are stored now, as the inline display copy stores them
		const uint32_t frame = s_gameFrames++;
		publish();
		const double t0 = monoSeconds();
		waitShown(frame);
		const double t1 = monoSeconds();
		xeStoreWriteBacks(frame);
		const double t2 = monoSeconds();
		xeBenchFrame();
		static double last, wait0;
		if (xeSpikeMs() && last && (t2 - last) * 1e3 > xeSpikeMs())  // SMS_XE_SPIKES (xe_gx_render.cpp)
			logmsg("[spike] game frame %u: %.1f ms (waited for the render thread %.1f at the display copy, %.1f in all; "
			       "write-backs %.1f)", frame, (t2 - last) * 1e3, (t1 - t0) * 1e3, (g_xeGameWait - wait0) * 1e3, (t2 - t1) * 1e3);
		last = t2;
		wait0 = g_xeGameWait;
	} else if (reg == BP_PE_TOKEN_INT && s_apiDrawSync) {
		// at once, as on the GameCube; what it reads comes from the frame before (readPeek)
		s_apiDrawSync(uint16_t(v & 0xFFFF));
	}
}

void xeApiCP(uint8_t reg, uint32_t v)
{
	shadowCP(reg, v);
	if (!s_threaded) {
		writeCP(reg, v);
		return;
	}
	const CPArgs a = {reg, v};
	post(cCP, a);
}

void xeApiXF(uint16_t addr, uint32_t n, const uint32_t* w)
{
	shadowXF(addr, n, w);
	if (!s_threaded) {
		writeXF(addr, n, w);
		return;
	}
	uint8_t* p = reserve(cXF, uint32_t(sizeof(XFArgs) + n * 4));
	const XFArgs a = {addr, uint16_t(n)};
	memcpy(p, &a, sizeof a);
	memcpy(p + sizeof a, w, n * 4);
	commit();
}

void xeApiFlush()
{
	if (!s_threaded) flushBatch();
	else post(cFlush, 0);
}

void xeApiDrawDone()
{
	if (!s_threaded) {
		flushBatch();
		return;
	}
	post(cFlush, 0);
	drain();
}

void xeApiCallDisplayList(const uint8_t* data, uint32_t size)
{
	if (!s_threaded) {
		callDisplayList(data, size);
		return;
	}
	const PtrArgs a = {data, size};
	post(cCallDisplayList, a);
}

namespace {
struct ReadArgs {
	int x, y;
	uint32_t v;
};
void peekColorOn(void* p)
{
	ReadArgs& a = *(ReadArgs*)p;
	a.v = peekColor(a.x, a.y);
}
void peekZOn(void* p)
{
	ReadArgs& a = *(ReadArgs*)p;
	a.v = peekZ(a.x, a.y);
}
void metricReadOn(void* p) { ((ReadArgs*)p)->v = pixMetricRead(); }
void metricClearOn(void*) { pixMetricClear(); }
}  // namespace

// Threaded peeks: peeks with no other command between them form a group (numbered in each frame by
// the game, so the numbers are the same each frame); the render thread resolves the EFB for the
// group where the game's commands are, and a peek answers from the same group a frame earlier, or,
// the first time a group appears, from the EFB now (after everything before it has run).
namespace {
uint32_t s_peekFrame = ~0u, s_peekMark, s_peekIssued, s_metricFrame = ~0u;
int s_peekGroup, s_metricSlot;
struct SnapArgs {
	int group, depth;
};
void cSnapPeek(const uint8_t* p, uint32_t)
{
	const SnapArgs& a = *(const SnapArgs*)p;
	xeSnapPeek(a.group, a.depth != 0);
}
void cSnapMetric(const uint8_t* p, uint32_t) { xeSnapMetric(*(const int*)p); }

uint32_t readPeek(int x, int y, bool depth)
{
	const uint32_t frame = s_gameFrames;
	if (s_peekFrame != frame || s_peekMark != s_write) {
		s_peekGroup = s_peekFrame == frame ? s_peekGroup + 1 : 0;
		s_peekFrame = frame;
		s_peekIssued = 0;
	}
	if (!(s_peekIssued & (1u << depth))) {
		s_peekIssued |= 1u << depth;
		const SnapArgs a = {s_peekGroup, depth};
		post(cSnapPeek, a);
	}
	s_peekMark = s_write;
	uint32_t v;
	if (frame) {
		waitShown(frame);
		if (xeSnapPeekRead(frame - 1, s_peekGroup, depth, x, y, &v)) return depth ? v >> 8 : v;  // D24S8: as peekZ
	}
	ReadArgs a = {x, y, 0};
	xeCoreCall(depth ? peekZOn : peekColorOn, &a);
	s_peekMark = s_write;  // the call's own commands: the next peek is still of this group
	return a.v;
}
}  // namespace

uint32_t xeApiPeekColor(int x, int y)
{
	if (!s_threaded) return peekColor(x, y);
	return readPeek(x, y, false);
}
uint32_t xeApiPeekZ(int x, int y)
{
	if (!s_threaded) return peekZ(x, y);
	return readPeek(x, y, true);
}
void xeApiPixMetricClear()
{
	if (!s_threaded) pixMetricClear();
	else xeCorePost(metricClearOn, NULL);
}
// Threaded metric reads: the n-th read of a frame answers with the n-th read's value a frame earlier
// (the render thread reads it in order), or, the first time, with the value now.
uint32_t xeApiPixMetricRead()
{
	if (!s_threaded) return pixMetricRead();
	const uint32_t frame = s_gameFrames;
	if (s_metricFrame != frame) {
		s_metricFrame = frame;
		s_metricSlot = 0;
	}
	const int slot = s_metricSlot++;
	post(cSnapMetric, slot);
	uint32_t v = 0;
	if (frame) {
		waitShown(frame);
		if (xeSnapMetricRead(frame - 1, slot, &v)) return v;
	}
	drain();
	if (!xeSnapMetricRead(frame, slot, &v)) xeSnapMetricLast(frame, slot, &v);
	return v;
}

void xeApiTextureInvalidateAll()
{
	if (!s_threaded) textureInvalidateAll();
	else post(cTextureInvalidateAll, 0);
}

void xeApiResetState()  // as resetState, for both
{
	memset(&g_api, 0, sizeof g_api);
	g_api.bpMask = 0xFFFFFF;
	const float ident[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
	for (int i = 0; i < 12; i++) {
		memcpy(&g_api.xfMem[60 * 4 + i], &ident[i], 4);
		memcpy(&g_api.xfMem[0x500 + 61 * 4 + i], &ident[i], 4);
	}
	if (!s_threaded) resetState();
	else post(cResetState, 0);
}

void xeApiSetArray(int slot, const void* base, uint32_t stride)
{
	if (!s_threaded) {
		setArray(slot, base, stride);
		return;
	}
	const ArrayArgs a = {base, slot, stride};
	post(cSetArray, a);
}

void xeApiLoadTlut(uint32_t off, const void* data, uint32_t bytes)
{
	if (!data) return;
	if (!s_threaded) {
		loadTlut(off, data, bytes);
		return;
	}
	uint8_t* p = reserve(cLoadTlut, uint32_t(sizeof(TlutArgs) + bytes));
	const TlutArgs a = {off, bytes};
	memcpy(p, &a, sizeof a);
	memcpy(p + sizeof a, data, bytes);  // as it is now: the SDK's load reads it when the GPU gets there
	commit();
}

void xeApiTexCoordManual(int coord, bool enable)
{
	if (!s_threaded) {
		g.tcManual[coord] = enable;
		return;
	}
	const IntArgs a = {coord, enable};
	post(cTexCoordManual, a);
}

void (*xeApiSetDrawSyncCallback(void (*cb)(uint16_t)))(uint16_t)
{
	void (*old)(uint16_t) = s_apiDrawSync;
	s_apiDrawSync = cb;
	if (!s_threaded) drawSyncCallback = cb;  // the core calls it at the token
	return old;
}

namespace {
void applyForeignWrites()
{
	EnterCriticalSection(&s_foreignLock);
	s_foreignTaken.swap(s_foreign);
	s_foreignPending = 0;
	LeaveCriticalSection(&s_foreignLock);
	for (size_t i = 0; i < s_foreignTaken.size(); i++) xeApiMemoryWritten(s_foreignTaken[i].p, s_foreignTaken[i].n);
	s_foreignTaken.clear();
}
}  // namespace

void xeApiMemoryWritten(const void* p, size_t n)
{
	if (GetCurrentThreadId() != s_gameThreadId && s_gameThreadId) {  // another host thread (THP's decoder)
		EnterCriticalSection(&s_foreignLock);
		const ForeignWrite w = {p, uint32_t(n)};
		s_foreign.push_back(w);
		s_foreignPending = 1;
		LeaveCriticalSection(&s_foreignLock);
		return;
	}
	if (s_foreignPending) applyForeignWrites();
	if (!s_threaded) {
		xeCoreMemoryWritten(p, uint32_t(n));
		return;
	}
	const PtrArgs a = {p, uint32_t(n)};
	post(cMemoryWritten, a);
}

}  // namespace gx

using namespace gx;

extern "C" {

void GXPC_MemoryWritten(const void* p, uint32_t size) { xeApiMemoryWritten(p, size); }
void GXPC_AddBigEndianRange(const void* p, uint32_t size)
{
	if (!s_threaded) {
		xeCoreAddBigEndianRange(p, size);
		return;
	}
	const PtrArgs a = {p, size};
	post(cAddBigEndianRange, a);
}
void GXPC_RemoveBigEndianRange(const void* p)
{
	if (!s_threaded) {
		xeCoreRemoveBigEndianRange(p);
		return;
	}
	const PtrArgs a = {p, 0};
	post(cRemoveBigEndianRange, a);
}
void GXPC_SetArrayBigEndian(int attr, int bigEndian)
{
	if (!s_threaded) {
		xeCoreSetArrayBigEndian(attr, bigEndian);
		return;
	}
	const IntArgs a = {attr, bigEndian};
	post(cSetArrayBigEndian, a);
}
void GXPC_SetDefaultArrayBigEndian(int bigEndian)
{
	if (xeOnRenderThread()) {
		xeCoreSetDefaultArrayBigEndian(bigEndian);
		return;
	}
	post(cSetDefaultArrayBigEndian, bigEndian);
}

}  // extern "C"
