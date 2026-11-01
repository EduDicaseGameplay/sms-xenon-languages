// THP video frames decode on a host thread of their own.
//
// The player's decode thread (decomp libs/THPPlayer/src/THPVideoDecode.c)
// calls THPVideoDecode once a frame. Decoding is the heaviest work a movie
// does: about 20 ms per frame of an HD cutscene (1920x960) on a 3.5 GHz
// desktop, against 33 ms per frame of playback, and on the emulated CPU no
// other game thread runs meanwhile (platform/os). So the decode runs on a
// host thread, and the player's thread sleeps until it is done, as it would
// wait on hardware: the game's thread (drawing the movie, the HUD over
// it), JAudio and the reader run beside it on another core.
//
// The decoder (port_thp_video_decode, decomp-patches/thp-07) only writes the
// frame's planes and its own work area. The GX layer is told about the planes
// it wrote (port_thp_stored: DCFlushRange) on the game's CPU once the frame
// is back, before the player queues it for drawing.
//
// The decode stays on the game's CPU with a deterministic clock (tests), on
// one core, or with SMS_THP_DECODE_THREAD=0.
#include <dolphin/os.h>
#include "port_os.h"
#include "port_platform.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <time.h>
#include <utility>
#include <vector>

extern "C" s32 port_thp_video_decode(void* file, void* tileY, void* tileU, void* tileV, void* work);

namespace {

enum { kIdle, kDecoding, kDecoded, kCollected };

struct Decoder {
	std::mutex m;
	std::condition_variable cv;
	std::atomic<int> state{kIdle};
	void* args[5];
	s32 result;
	std::vector<std::pair<void*, u32> > stored; // planes written (DCFlushRange once back)
	OSThreadQueue queue;                         // the player's thread, while it waits
	// per movie, for the log
	u32 frames;
	double seconds;
};
// Never destroyed: exit() runs while the decoder thread waits on it (as
// platform/audio/ai.cpp's state, a destroyed mutex throws on Windows).
Decoder& d = *new Decoder();
int g_threaded = -1; // -1 not chosen yet
thread_local bool t_onDecoder;

double mono_seconds()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

s32 decode(void** a)
{
	const double t0 = mono_seconds();
	s32 r = port_thp_video_decode(a[0], a[1], a[2], a[3], a[4]);
	d.seconds += mono_seconds() - t0;
	d.frames++;
	return r;
}

void decoder_main()
{
	t_onDecoder = true;
	std::unique_lock<std::mutex> l(d.m);
	for (;;) {
		d.cv.wait(l, [] { return d.state.load() == kDecoding; });
		l.unlock();
		d.result = decode(d.args);
		d.state.store(kDecoded);
		port_irq_kick();
		l.lock();
	}
}

// Interrupt source: a frame the decoder finished wakes the player's thread.
void poll_decoder()
{
	int done = kDecoded;
	if (d.state.compare_exchange_strong(done, kCollected))
		OSWakeupThread(&d.queue);
}

bool threaded()
{
	if (g_threaded < 0) {
		const char* e = getenv("SMS_THP_DECODE_THREAD");
		g_threaded = !(e && !strcmp(e, "0")) && !port_vi_deterministic() && std::thread::hardware_concurrency() > 1;
		if (g_threaded) {
			OSInitThreadQueue(&d.queue);
			port_irq_add_source(poll_decoder);
			std::thread(decoder_main).detach();
		}
		port_log("[thp] video frames decode %s\n", g_threaded ? "on their own thread" : "on the game's CPU");
	}
	return g_threaded;
}

// Waits for the frame being decoded.
void settle()
{
	while (d.state.load() == kDecoding)
		std::this_thread::yield();
}

} // namespace

extern "C" void port_thp_stored(void* p, u32 n)
{
	if (t_onDecoder)
		d.stored.push_back(std::make_pair(p, n));
	else
		DCFlushRange(p, n);
}

extern "C" s32 THPVideoDecode(void* file, void* tileY, void* tileU, void* tileV, void* work)
{
	void* a[5] = {file, tileY, tileU, tileV, work};
	if (!threaded())
		return decode(a);
	settle();
	d.stored.clear();
	{
		std::lock_guard<std::mutex> l(d.m);
		memcpy(d.args, a, sizeof a);
		d.state.store(kDecoding);
	}
	d.cv.notify_one();
	BOOL enabled = OSDisableInterrupts();
	while (d.state.load() != kCollected)
		OSSleepThread(&d.queue);
	OSRestoreInterrupts(enabled);
	for (const std::pair<void*, u32>& s : d.stored)
		DCFlushRange(s.first, s.second);
	d.state.store(kIdle);
	return d.result;
}

// The player stops (THPPlayerStop cancels its decode thread, then frees the
// buffers): the frame decoding meanwhile is dropped once it ends.
extern "C" void port_thp_video_decode_cancel(void)
{
	settle();
	d.state.store(kIdle);
	if (d.frames)
		port_log("[thp] %u video frames decoded, %.2f ms each\n", (unsigned)d.frames, d.seconds * 1000 / d.frames);
	d.frames  = 0;
	d.seconds = 0;
}

// For save states (platform/savestate): a frame decoding writes game memory.
// Settling waits for it; the player's thread then wakes at the next
// interrupt check.
extern "C" int port_thp_video_decode_busy(void)
{
	const int s = d.state.load();
	return s == kDecoding || s == kDecoded;
}
extern "C" void port_thp_video_decode_settle(void) { settle(); }
