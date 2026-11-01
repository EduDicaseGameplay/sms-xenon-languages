// Audio Interface (AI): the DMA engine that plays JAudio's DAC buffers.
//
// JAudio (JASAiCtrl.cpp / JASAudioThread.cpp) programs one DMA block at a time
// with AIInitDMA; the hardware latches the registers when a block starts
// playing and raises the DMA interrupt, whose callback (syncAudio) wakes the
// audio thread to mix and program the next block. Here a block "starts" when
// the host output FIFO has room for it: the latched block (host-order s16
// stereo pairs, written by the CPU) is copied into the FIFO and the callback
// runs in interrupt context on the emulated CPU.
//
// Output: SDL2 audio (loaded with dlopen, so builds without the GX layer still
// link) at 32 kHz; the SDL callback drains the FIFO and kicks the CPU when it
// runs low (three blocks queued, 52 ms, or up to eight after the output ran
// dry). Without a device (headless, SMS_AUDIO_OUT=null, SDL failure, or no
// answer from the sound server within SMS_AUDIO_OPEN_TIMEOUT seconds) a host
// timer paces blocks at the DAC rate. SMS_AUDIO_WAV=path also records
// every block. SMS_AUDIO=0 drops the output only: the DMA engine keeps its
// clock (as with no device), so JAudio's audio thread, its sequencer and the
// DSP run as on the console; stopping them would leave every sequence the game
// stops allocated (only the sequencer frees it), and the eight root sequence
// slots run out after eight music changes (README.md, Environment).
// SMS_NO_AUDIO (an empty sound configuration) leaves the DMA engine idle.
// Pairs are (bus 2, bus 1) = (right, left) in DMA order; they are swapped for
// SDL (SMS_AUDIO_SWAP=0 keeps DMA order).
#include "port_compat.h"
#include "port_os.h"
#include "port_platform.h"
#include <dolphin/ai.h>
#include <dolphin/os/OSThread.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <dlfcn.h>
#include "dsp_mixer.h"
#include "port_setting_list.h"
#include "port_settings.h"
#include "port_host.h"
#include <mutex>
#include <pthread.h>
#include <string>
#include <time.h>
#include <vector>
#ifdef __linux__
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

extern "C" int port_audio_dsp_frame_pending(void) __attribute__((weak));
extern "C" int GXPC_GetSpeed(void) __attribute__((weak));
extern "C" void port_dtk_mix(s16* lr, size_t frames, u32 outRate);

namespace {

// Minimal SDL2 ABI (public SDL_audio.h): only what the output needs.
struct SdlAudioSpec {
	int freq;
	uint16_t format;
	uint8_t channels;
	uint8_t silence;
	uint16_t samples;
	uint16_t padding;
	uint32_t size;
	void (*callback)(void* userdata, uint8_t* stream, int len);
	void* userdata;
};
const uint32_t kSdlInitAudio = 0x10;
const uint16_t kAudioS16Sys  = 0x8010; // AUDIO_S16LSB (little-endian hosts)

typedef int (*PFN_SDL_InitSubSystem)(uint32_t);
typedef uint32_t (*PFN_SDL_OpenAudioDevice)(const char*, int, const SdlAudioSpec*, SdlAudioSpec*, int);
typedef void (*PFN_SDL_PauseAudioDevice)(uint32_t, int);
typedef const char* (*PFN_SDL_GetError)(void);
typedef void (*PFN_SDL_CloseAudioDevice)(uint32_t);
typedef void (*PFN_SDL_GetVersion)(uint8_t*); // SDL_version: major, minor, patch
typedef int (*PFN_SDL_GetNumAudioDrivers)(void);
typedef const char* (*PFN_SDL_GetAudioDriver)(int);
typedef const char* (*PFN_SDL_GetCurrentAudioDriver)(void);

const u32 kRate = 32000;
const u32 kBlockFrames = 560; // JAudio's DAC block (0x460 samples), 17.5 ms
const int kBaseBlocks  = 3;   // blocks the output keeps queued
const int kMaxExtra    = 5;   // more after it ran dry: up to 8 blocks, 140 ms

struct Ai {
	bool enabled; // the DMA engine runs (not SMS_NO_AUDIO)
	bool output;  // SMS_AUDIO != 0: a device or SMS_AUDIO_WAV
	bool inited;
	bool running;
	bool swap;
	// SMS_VOLUME (0..100) as a Q8 output gain; 256 is unity. The settings menu changes it
	// (port_audio_settings) from the game's thread while the SDL callback reads it.
	std::atomic<int> volume;
	u32 start, length;         // registers
	u32 latchedStart, latchedLen;
	AIDCallback cb;
	OSThread* dacThread; // the thread that started the DMA: JAudio's audio thread
	u32 dspRate;
	u32 streamRate;
	// Output FIFO of stereo frames (L, R after swap).
	std::mutex mu;
	std::vector<int16_t> fifo;
	size_t head, count;
	bool sdl;
	bool fed; // a block has reached the FIFO
	s16 lastL, lastR;
	// Blocks queued beyond kBaseBlocks, added when the output ran dry and
	// given back one at a time after kRate * 30 frames without (sdl_callback).
	std::atomic<int> extra;
	std::atomic<int> grew; // for the log: the new target in blocks
	u32 dryFrames;         // device frames with nothing to play, this dry spell
	u32 steadyFrames;      // device frames played since the last dry spell
	// Null clock
	std::atomic<int> due;
	u64 blocks;
	FILE* wav;
	u32 wavBytes;
	std::vector<int16_t> mix; // a block in output order, with the DVD stream (dtk.cpp)
};
// Never destroyed: exit() (Quit, closing the window) runs while SDL's audio
// thread and the game's threads still use it, and a destroyed mutex threw
// std::system_error ("Invalid argument") as the game closed on Windows.
Ai& g = *new Ai();

size_t fifo_cap() { return g.fifo.size() / 2; }

int playback_speed()
{
	int speed = GXPC_GetSpeed ? GXPC_GetSpeed() : 1;
	return speed > 0 ? speed : 1;
}

size_t fifo_target(int speed) { return (kBaseBlocks + g.extra.load()) * kBlockFrames * (size_t)speed; }

void wav_header(FILE* f, u32 bytes)
{
	u32 v32;
	u16 v16;
	fseek(f, 0, SEEK_SET);
	fwrite("RIFF", 1, 4, f);
	v32 = 36 + bytes;
	fwrite(&v32, 4, 1, f);
	fwrite("WAVEfmt ", 1, 8, f);
	v32 = 16;
	fwrite(&v32, 4, 1, f);
	v16 = 1;
	fwrite(&v16, 2, 1, f);
	v16 = 2;
	fwrite(&v16, 2, 1, f);
	v32 = kRate;
	fwrite(&v32, 4, 1, f);
	v32 = kRate * 4;
	fwrite(&v32, 4, 1, f);
	v16 = 4;
	fwrite(&v16, 2, 1, f);
	v16 = 16;
	fwrite(&v16, 2, 1, f);
	fwrite("data", 1, 4, f);
	fwrite(&bytes, 4, 1, f);
	fseek(f, 0, SEEK_END);
}

void wav_close(void)
{
	if (g.wav) {
		wav_header(g.wav, g.wavBytes);
		fclose(g.wav);
		g.wav = NULL;
	}
}

void sdl_callback(void*, uint8_t* stream, int len)
{
	int16_t* out = (int16_t*)stream;
	size_t frames = (size_t)len / 4;
	const int volume = g.volume.load(std::memory_order_relaxed); // one gain for the whole buffer
	int speed = playback_speed();
	bool low;
	{
		std::lock_guard<std::mutex> lk(g.mu);
		size_t cap = fifo_cap();
		size_t dry = 0;
		for (size_t i = 0; i < frames; i++) {
			if (g.count) {
				g.lastL = g.fifo[g.head * 2];
				g.lastR = g.fifo[g.head * 2 + 1];
				size_t consumed = std::min(g.count, (size_t)speed);
				g.head = (g.head + consumed) % cap;
				g.count -= consumed;
			} else {
				dry++;
			}
			out[i * 2]     = (int16_t)((g.lastL * volume) >> 8);
			out[i * 2 + 1] = (int16_t)((g.lastR * volume) >> 8);
		}
		// Ran dry: blocks are made on the emulated CPU, so the game's thread
		// held it (a load, a shader compile, a slow frame) for longer than the
		// queue lasts, and the gap clicks. When the spell ends, queue at least
		// one block more, or enough more to have covered it, so hitches like
		// it play through.
		if (dry && g.fed && g.running) {
			g.dryFrames += (u32)dry;
			g.steadyFrames = 0;
		} else if (g.dryFrames) {
			int cover = (int)((g.dryFrames + kBlockFrames - 1) / kBlockFrames);
			int need  = std::min(kMaxExtra, std::max(g.extra.load() + 1, cover));
			if (need > g.extra.load()) {
				g.extra.store(need);
				g.grew.store(kBaseBlocks + need);
			}
			g.dryFrames = 0;
		} else if (g.extra.load() > 0 && (g.steadyFrames += (u32)frames) >= kRate * 30) {
			g.extra.fetch_sub(1);
			g.steadyFrames = 0;
		}
		low = g.count < fifo_target(speed);
	}
	if (low)
		port_irq_kick();
}

void* null_clock(void*)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	for (;;) {
		u32 len = g.latchedLen ? g.latchedLen : 0x460 * 2;
		long ns = (long)((u64)(len / 4) * 1000000000ull / kRate / playback_speed());
		t.tv_nsec += ns;
		while (t.tv_nsec >= 1000000000) {
			t.tv_nsec -= 1000000000;
			t.tv_sec++;
		}
		port_sleep_until(&t);
		if (g.running) {
			if (g.due.load() < 4) // a stalled game does not get a burst later
				g.due.fetch_add(1);
			port_irq_kick();
		}
	}
	return NULL;
}

// The SDL calls the output makes (loaded with dlopen).
struct Sdl {
	PFN_SDL_InitSubSystem init;
	PFN_SDL_OpenAudioDevice open;
	PFN_SDL_PauseAudioDevice pause;
	PFN_SDL_CloseAudioDevice close;
	PFN_SDL_GetError error;
	// for the log, if opening hangs
	PFN_SDL_GetVersion version;
	PFN_SDL_GetNumAudioDrivers numDrivers;
	PFN_SDL_GetAudioDriver driver;
	PFN_SDL_GetCurrentAudioDriver currentDriver;
} sdl;

bool load_sdl()
{
#ifdef _WIN32
	void* h = dlopen("SDL2.dll", RTLD_NOW | RTLD_GLOBAL);
#elif defined(__APPLE__)
	// Prefer symbols already linked (SDL2.framework), then common dylib names.
	void* h = dlopen(NULL, RTLD_NOW | RTLD_GLOBAL);
	if (!h || !dlsym(h, "SDL_InitSubSystem")) {
		h = dlopen("@rpath/SDL2.framework/SDL2", RTLD_NOW | RTLD_GLOBAL);
	}
	if (!h)
		h = dlopen("SDL2.framework/SDL2", RTLD_NOW | RTLD_GLOBAL);
	if (!h)
		h = dlopen("libSDL2-2.0.0.dylib", RTLD_NOW | RTLD_GLOBAL);
	if (!h)
		h = dlopen("libSDL2.dylib", RTLD_NOW | RTLD_GLOBAL);
#else
	void* h = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
#endif
	if (!h)
		return false;
	sdl.init          = (PFN_SDL_InitSubSystem)dlsym(h, "SDL_InitSubSystem");
	sdl.open          = (PFN_SDL_OpenAudioDevice)dlsym(h, "SDL_OpenAudioDevice");
	sdl.pause         = (PFN_SDL_PauseAudioDevice)dlsym(h, "SDL_PauseAudioDevice");
	sdl.close         = (PFN_SDL_CloseAudioDevice)dlsym(h, "SDL_CloseAudioDevice");
	sdl.error         = (PFN_SDL_GetError)dlsym(h, "SDL_GetError");
	sdl.version       = (PFN_SDL_GetVersion)dlsym(h, "SDL_GetVersion");
	sdl.numDrivers    = (PFN_SDL_GetNumAudioDrivers)dlsym(h, "SDL_GetNumAudioDrivers");
	sdl.driver        = (PFN_SDL_GetAudioDriver)dlsym(h, "SDL_GetAudioDriver");
	sdl.currentDriver = (PFN_SDL_GetCurrentAudioDriver)dlsym(h, "SDL_GetCurrentAudioDriver");
	return sdl.init && sdl.open && sdl.pause && sdl.close && sdl.error;
}

// Opening the output can hang: SDL's PulseAudio backend waits for the sound
// server with no time limit, and a PipeWire PulseAudio layer that stopped
// answering new streams (other programs' streams stuck starting too) froze
// the game at boot, with the emulated CPU held so not even Ctrl+C got
// through. So the output opens on a thread of its own, and past
// SMS_AUDIO_OPEN_TIMEOUT seconds (default 5; 0 waits for ever) the game goes
// on without sound, paced by the host clock as with no device, and the log
// says what it was waiting for. Should SDL answer later, the device is closed
// again: this run stays silent.
struct Opening {
	std::mutex mu;
	std::condition_variable cv;
	bool done;      // the thread has an answer
	bool abandoned; // the game stopped waiting for it
	const char* step;
	uint32_t dev;
	SdlAudioSpec have;
	char error[256];
	std::chrono::steady_clock::time_point start;
} s_open;

void opening_step(const char* step)
{
	std::lock_guard<std::mutex> l(s_open.mu);
	s_open.step = step;
}

void* open_thread(void*)
{
	char error[256] = "";
	uint32_t dev    = 0;
	SdlAudioSpec want, have;
	memset(&want, 0, sizeof want);
	memset(&have, 0, sizeof have);
	opening_step("starting SDL's audio (SDL_InitSubSystem)");
	if (sdl.init(kSdlInitAudio) != 0) {
		snprintf(error, sizeof error, "SDL audio init failed: %s", sdl.error());
	} else {
		opening_step("opening the output device (SDL_OpenAudioDevice)");
		want.freq     = kRate;
		want.format   = kAudioS16Sys;
		want.channels = 2;
		want.samples  = 512;
		want.callback = sdl_callback;
		dev           = sdl.open(NULL, 0, &want, &have, 0); // no changes allowed: SDL converts
		if (!dev)
			snprintf(error, sizeof error, "no SDL audio device: %s", sdl.error());
	}
	std::unique_lock<std::mutex> l(s_open.mu);
	if (s_open.abandoned) {
		l.unlock();
		const double late = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_open.start).count();
		if (dev) {
			sdl.close(dev);
			port_log("[audio] the sound output answered after %.1f s, too late: this run stays without sound "
			         "(restart the game for sound)\n",
			         late);
		} else {
			port_log("[audio] the sound output gave up after %.1f s: %s\n", late, error);
		}
		return NULL;
	}
	s_open.done = true;
	s_open.dev  = dev;
	s_open.have = have;
	memcpy(s_open.error, error, sizeof error);
	s_open.cv.notify_all();
	return NULL;
}

// What is known about the output that did not answer, for the log.
void log_stuck(const char* step, int seconds)
{
	port_log("[audio] no answer from the sound output after %d s, while %s: the game goes on without sound\n",
	         seconds, step ? step : "starting it");
	char line[512];
	int n = 0;
	if (sdl.version) {
		uint8_t v[3] = { 0, 0, 0 }; // SDL_version
		sdl.version(v);
		n += snprintf(line + n, sizeof line - n, "SDL %u.%u.%u", v[0], v[1], v[2]);
	}
	const char* current = sdl.currentDriver ? sdl.currentDriver() : NULL;
	n += snprintf(line + n, sizeof line - n, "%sdriver %s", n ? ", " : "",
	              current ? current : "not chosen yet (its first choice was still starting)");
	if (sdl.numDrivers && sdl.driver) {
		n += snprintf(line + n, sizeof line - n, ", built with");
		for (int i = 0, count = sdl.numDrivers(); i < count && n < (int)sizeof line - 32; i++)
			n += snprintf(line + n, sizeof line - n, "%s%s", i ? ", " : " ", sdl.driver(i));
	}
	port_log("[audio]   %s\n", line);
	const char* names[] = { "SDL_AUDIODRIVER", "SDL_AUDIO_DEVICE_NAME", "AUDIODEV", "PULSE_SERVER", "PIPEWIRE_REMOTE",
		                    "XDG_RUNTIME_DIR" };
	n       = 0;
	line[0] = 0;
	for (const char* name : names)
		if (const char* v = getenv(name))
			n += snprintf(line + n, sizeof line - n, "%s%s=%s", n ? " " : "", name, v);
	port_log("[audio]   environment: %s\n", n ? line : "no audio variables set");
#ifdef __linux__
	// The sound servers' sockets: one that takes a connection but does not
	// answer is a stuck server, not a missing one.
	const char* run    = getenv("XDG_RUNTIME_DIR");
	const char* pulse  = getenv("PULSE_SERVER");
	std::string pulsePath = pulse && !strncmp(pulse, "unix:", 5) ? std::string(pulse + 5)
	                        : run                               ? std::string(run) + "/pulse/native"
	                                                            : std::string();
	std::string pwPath = run ? std::string(run) + "/pipewire-0" : std::string();
	const std::string* paths[] = { &pulsePath, &pwPath };
	const char* labels[]       = { "PulseAudio", "PipeWire" };
	for (int i = 0; i < 2; i++) {
		const std::string& path = *paths[i];
		if (path.empty())
			continue;
		const char* state = "no socket";
		char why[128]     = "";
		if (access(path.c_str(), F_OK) == 0) {
			int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
			sockaddr_un a;
			memset(&a, 0, sizeof a);
			a.sun_family = AF_UNIX;
			snprintf(a.sun_path, sizeof a.sun_path, "%s", path.c_str());
			if (fd >= 0 && connect(fd, (sockaddr*)&a, sizeof a) == 0) {
				state = "takes connections";
			} else {
				state = "refuses connections";
				snprintf(why, sizeof why, " (%s)", strerror(errno));
			}
			if (fd >= 0)
				close(fd);
		}
		port_log("[audio]   %s socket %s: %s%s\n", labels[i], path.c_str(), state, why);
	}
	port_log("[audio]   for sound, restart the sound server (systemctl --user restart pipewire pipewire-pulse "
	         "wireplumber, or pulseaudio -k), or choose another SDL driver (SDL_AUDIODRIVER=pipewire or alsa)\n");
#else
	port_log("[audio]   for sound, check the system's sound output, or restart the computer's audio service\n");
#endif
}

bool open_sdl()
{
	if (!load_sdl())
		return false;
	// SDL turns SIGINT/SIGTERM into an SDL_QUIT event; nothing pumps events in
	// a headless run, so the process would ignore them. (No effect if the GX
	// layer initialised SDL first; the user's own setting wins.)
	port_setenv("SDL_NO_SIGNAL_HANDLERS", "1", 0);
	const char* e = getenv("SMS_AUDIO_OPEN_TIMEOUT");
	const int seconds = e && *e ? std::max(0, atoi(e)) : 5;
	s_open.start      = std::chrono::steady_clock::now();
	pthread_t th;
	if (pthread_create(&th, NULL, open_thread, NULL) != 0)
		open_thread(NULL);
	else
		pthread_detach(th);
	std::unique_lock<std::mutex> l(s_open.mu);
	if (seconds)
		s_open.cv.wait_until(l, s_open.start + std::chrono::seconds(seconds), [] { return s_open.done; });
	else
		s_open.cv.wait(l, [] { return s_open.done; });
	if (!s_open.done) {
		s_open.abandoned = true;
		const char* step = s_open.step;
		l.unlock();
		log_stuck(step, seconds);
		return false;
	}
	l.unlock();
	if (!s_open.dev) {
		port_log("[audio] %s\n", s_open.error);
		return false;
	}
	sdl.pause(s_open.dev, 0);
	port_log("[audio] SDL output: %d Hz, %u-frame device buffer\n", s_open.have.freq, s_open.have.samples);
	return true;
}

// SMS_MUSIC_VOLUME / SMS_SFX_VOLUME (0..100): where a volume class starts. The mixer applies it
// whatever the output (a device, SMS_AUDIO_WAV, none), unlike SMS_VOLUME, which scales the device's stream.
void init_class_volume(const char* env, int cls, const char* name)
{
	const char* e = getenv(env);
	if (!e || !*e)
		return;
	const int percent = std::max(0, std::min(100, atoi(e)));
	port_dspmix_set_class_percent(cls, percent);
	if (percent != 100)
		port_log("[audio] %s volume %d%%\n", name, percent);
}

void init_output()
{
	const char* e = getenv("SMS_AUDIO");
	g.output      = !(e && strcmp(e, "0") == 0);
	g.enabled     = !port_no_audio;
	e             = getenv("SMS_AUDIO_SWAP");
	g.swap        = !(e && strcmp(e, "0") == 0);
	e             = getenv("SMS_VOLUME");
	g.volume      = e && *e ? std::max(0, std::min(100, atoi(e))) * 256 / 100 : 256;
	if (g.volume != 256)
		port_log("[audio] master volume %d%%\n", (g.volume * 100 + 128) / 256);
	init_class_volume("SMS_MUSIC_VOLUME", PORT_DSPMIX_CLASS_MUSIC, "music");
	init_class_volume("SMS_SFX_VOLUME", PORT_DSPMIX_CLASS_SFX, "SFX");
	g.fifo.assign(32768 * 2, 0);
	g.head = g.count = 0;
	if (!g.enabled) {
		port_log("[audio] SMS_NO_AUDIO: AI DMA idle (no mixing, no output)\n");
		return;
	}
	if (!g.output)
		port_log("[audio] SMS_AUDIO=0: no output; the game's audio still runs\n");
	if (g.output && (e = getenv("SMS_AUDIO_WAV")) && *e) {
		g.wav = fopen(e, "wb");
		if (g.wav) {
			wav_header(g.wav, 0);
			atexit(wav_close);
		}
	}
	e              = getenv("SMS_AUDIO_OUT");
	bool headless  = getenv("SMS_HEADLESS") && strcmp(getenv("SMS_HEADLESS"), "0") != 0;
	bool wantSdl   = e ? strcmp(e, "sdl") == 0 : !headless;
	// SMS_VI_DETERMINISTIC: no device and no host clock; AI blocks are paced by
	// VI retraces instead (port_audio_on_retrace), so runs stay repeatable.
	if (port_vi_deterministic()) {
		port_log("[audio] deterministic VI clock: AI DMA paced by retraces, no output device\n");
		return;
	}
	g.sdl          = g.output && wantSdl && open_sdl();
	if (!g.sdl) {
		if (g.output)
			port_log("[audio] no audio device: pacing AI DMA from the host clock\n");
		pthread_t th;
		pthread_create(&th, NULL, null_clock, NULL);
		pthread_detach(th);
	}
}

// One DMA block starts: latch the registers, queue the samples, interrupt.
void dma_block()
{
	g.latchedStart = g.start;
	g.latchedLen   = g.length;
	const int16_t* src = (const int16_t*)(uintptr_t)g.latchedStart;
	size_t frames      = g.latchedLen / 4;
	if (src && frames) {
		g.mix.resize(frames * 2);
		for (size_t i = 0; i < frames; i++) {
			g.mix[i * 2]     = g.swap ? src[i * 2 + 1] : src[i * 2];
			g.mix[i * 2 + 1] = g.swap ? src[i * 2] : src[i * 2 + 1];
		}
		port_dtk_mix(g.mix.data(), frames, kRate);
		if (g.wav) {
			fwrite(g.mix.data(), 4, frames, g.wav);
			g.wavBytes += (u32)frames * 4;
			if ((++g.blocks & 63) == 0) {
				wav_header(g.wav, g.wavBytes);
				fflush(g.wav);
			}
		}
		if (g.sdl) {
			std::lock_guard<std::mutex> lk(g.mu);
			g.fed      = true;
			size_t cap = fifo_cap();
			for (size_t i = 0; i < frames && g.count < cap; i++) {
				size_t p         = (g.head + g.count) % cap;
				g.fifo[p * 2]     = g.mix[i * 2];
				g.fifo[p * 2 + 1] = g.mix[i * 2 + 1];
				g.count++;
			}
		}
	}
	if (g.cb)
		g.cb();
}

u64 g_det_acc; // deterministic pacing: samples owed, in 1/60000 units

// The DMA callback wakes JAudio's audio thread, which programs the next block
// (AIInitDMA) and mixes the one after it. On hardware a block plays for
// 17.5 ms, so that work is long done when the next block starts. Here blocks
// start back to back while the output catches up after the game's thread held
// the CPU (a slow frame, a hitch), and one started before the audio thread
// has run plays the same DAC buffer again; the buffer it mixes meanwhile is
// then overwritten unplayed. Each catch-up repeated and dropped 17.5 ms
// pieces, heard as crackling, and at a steady low frame rate (the output
// catching up every frame) as distortion that lasted as long as the low rate.
bool dac_thread_busy()
{
	return g.dacThread && (g.dacThread->state == OS_THREAD_STATE_READY || g.dacThread->state == OS_THREAD_STATE_RUNNING);
}

// Interrupt source, polled on the emulated CPU when kicked.
void ai_poll()
{
	if (!g.running)
		return;
	if (port_audio_dsp_frame_pending && port_audio_dsp_frame_pending()) {
		port_irq_kick(); // the audio thread is mid-frame: retry at the next check point
		return;
	}
	if (dac_thread_busy())
		return; // polled again when it blocks (the scheduler delivers interrupts then)
	if (g.sdl) {
		if (int blocks = g.grew.exchange(0))
			port_log("[audio] the output ran dry; queueing %u ms from now on\n", blocks * kBlockFrames * 1000 / kRate);
		size_t level;
		size_t target = fifo_target(playback_speed());
		{
			std::lock_guard<std::mutex> lk(g.mu);
			level = g.count;
		}
		if (level < target) {
			dma_block(); // one block per delivery: the audio thread must run
			std::lock_guard<std::mutex> lk(g.mu);
			if (g.count < target)
				port_irq_kick();
		}
	} else if (g.due.load() > 0) {
		g.due.fetch_sub(1);
		dma_block();
		if (g.due.load() > 0)
			port_irq_kick(); // catch up one block per delivery
	}
}

} // namespace

// Called by platform/vi once per retrace. In deterministic mode each retrace
// owes 32000 * 1001 / 60000 samples; whole DMA blocks become due.
extern "C" void port_audio_on_retrace(void)
{
	if (!port_vi_deterministic() || !g.enabled || !g.running)
		return;
	g_det_acc += (u64)kRate * 1001;
	u32 len      = g.latchedLen ? g.latchedLen : 0x460 * 2;
	u64 perBlock = (u64)(len / 4) * 60000;
	while (g_det_acc >= perBlock) {
		g_det_acc -= perBlock;
		g.due.fetch_add(1);
	}
	if (g.due.load() > 0)
		port_irq_kick();
}

extern "C" void AIInit(u8*)
{
	if (g.inited)
		return;
	g.inited = true;
	init_output();
	port_irq_add_source(ai_poll);
}
extern "C" BOOL AICheckInit(void) { return g.inited; }
extern "C" void AIReset(void) {}
extern "C" AIDCallback AIRegisterDMACallback(AIDCallback cb)
{
	AIDCallback old = g.cb;
	g.cb            = cb;
	return old;
}
extern "C" void AIInitDMA(u32 start_addr, u32 length)
{
	g.start  = start_addr;
	g.length = length;
}
extern "C" BOOL AIGetDMAEnableFlag(void) { return g.running; }
extern "C" void AIStartDMA(void)
{
	if (!g.enabled)
		return;
	g.dacThread = OSGetCurrentThread();
	g.running = true;
	port_irq_kick();
}
extern "C" void AIStopDMA(void) { g.running = false; }
extern "C" u32 AIGetDMABytesLeft(void) { return 0; }
extern "C" u32 AIGetDMAStartAddr(void) { return g.latchedStart; }
extern "C" u32 AIGetDMALength(void) { return g.latchedLen; }
extern "C" void AISetDSPSampleRate(u32 rate) { g.dspRate = rate; }
extern "C" u32 AIGetDSPSampleRate(void) { return g.dspRate; }
// DVD audio streaming (DTK, dtk.cpp): retail SMS never streams; code mods do.
extern "C" void AISetStreamSampleRate(u32 rate) { g.streamRate = rate; }
extern "C" u32 AIGetStreamSampleRate(void) { return g.streamRate; }

// --- Settings menu (port_settings.h) ---
// The volume changes while the game is paused: set() stores the new Q8 gain in the atomic
// the SDL callback reads, and the callback picks it up at its next buffer (about 10 ms).
// Music and SFX volume are the mixer's per-class gains (dsp_mixer.h): the mixer reads them at
// its next subframe (2.5 ms) and ramps to the new value within it.
namespace {

PortValueList g_volumeList; // percent
PortValueList g_classList[PORT_DSPMIX_CLASSES];

const int kVolumeSteps[] = { 0, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 65, 70, 75, 80, 85, 90, 95, 100 };

const char* percent_name(const PortValueList& l, int i)
{
	static char text[16];
	snprintf(text, sizeof text, "%d", l.v[i < 0 ? 0 : i >= l.n ? l.n - 1 : i]);
	return text;
}

int volume_percent() { return (g.volume.load() * 100 + 128) / 256; }

const PortValueList& volume_list()
{
	if (!g_volumeList.built)
		g_volumeList.build(kVolumeSteps, (int)(sizeof kVolumeSteps / sizeof kVolumeSteps[0]), volume_percent());
	return g_volumeList;
}

// Only a run with a real output device has a volume to change; with no device (headless, SMS_AUDIO=0,
// SMS_AUDIO_OUT=null, a failed open) or SMS_NO_AUDIO the gain is never applied. The music and SFX
// gains are the mixer's, which applies them whatever the output (a WAV recording too).
int volume_available() { return g.inited && g.enabled && g.sdl; }
int class_available() { return g.inited && g.enabled; }
int volume_count() { return volume_list().n; }
const char* volume_name(int i) { return percent_name(volume_list(), i); }
int volume_get() { return volume_list().indexOf(volume_percent()); }
int volume_set(int i)
{
	const PortValueList& l = volume_list();
	if (i < 0 || i >= l.n)
		return 0;
	g.volume.store(l.v[i] * 256 / 100);
	return 1;
}

const PortValueList& class_list(int cls)
{
	if (!g_classList[cls].built)
		g_classList[cls].build(kVolumeSteps, (int)(sizeof kVolumeSteps / sizeof kVolumeSteps[0]),
		                       port_dspmix_class_percent(cls));
	return g_classList[cls];
}
int class_get(int cls) { return class_list(cls).indexOf(port_dspmix_class_percent(cls)); }
int class_set(int cls, int i)
{
	const PortValueList& l = class_list(cls);
	if (i < 0 || i >= l.n)
		return 0;
	port_dspmix_set_class_percent(cls, l.v[i]);
	return 1;
}

int music_count() { return class_list(PORT_DSPMIX_CLASS_MUSIC).n; }
const char* music_name(int i) { return percent_name(class_list(PORT_DSPMIX_CLASS_MUSIC), i); }
int music_get() { return class_get(PORT_DSPMIX_CLASS_MUSIC); }
int music_set(int i) { return class_set(PORT_DSPMIX_CLASS_MUSIC, i); }

int sfx_count() { return class_list(PORT_DSPMIX_CLASS_SFX).n; }
const char* sfx_name(int i) { return percent_name(class_list(PORT_DSPMIX_CLASS_SFX), i); }
int sfx_get() { return class_get(PORT_DSPMIX_CLASS_SFX); }
int sfx_set(int i) { return class_set(PORT_DSPMIX_CLASS_SFX, i); }

const PortSetting kAudioSettings[] = {
	{ "volume", PORT_SETTINGS_PAGE_GAMEPLAY, 30, "Volume", "How loud the game is. 0 is silent, 100 is full.", 0, volume_available, volume_count,
	  volume_name, volume_get, volume_set },
	{ "musicVolume", PORT_SETTINGS_PAGE_GAMEPLAY, 32, "Music Volume",
	  "How loud the music is, cutscene movies included. 0 is silent, 100 is full.", 0, class_available, music_count,
	  music_name, music_get, music_set },
	{ "sfxVolume", PORT_SETTINGS_PAGE_GAMEPLAY, 34, "SFX Volume",
	  "How loud sound effects are, Mario's voice included. 0 is silent, 100 is full.", 0, class_available, sfx_count,
	  sfx_name, sfx_get, sfx_set },
};

} // namespace

extern "C" const PortSetting* port_audio_settings(int* count)
{
	*count = (int)(sizeof kAudioSettings / sizeof kAudioSettings[0]);
	return kAudioSettings;
}

// THP movie audio is mixed on the emulated CPU (JASystem::Kernel mixInterleaveTrack, patch audio-05):
// the music gain as a Q15 factor.
extern "C" int port_audio_music_gain_q15(void) { return port_dspmix_class_gain(PORT_DSPMIX_CLASS_MUSIC); }
