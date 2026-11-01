// Independent, exact impulse expectations for the Zelda DSP reverb protocol
// and its oscillator sources.
// No disc assets or game headers are needed.
#include "../dsp_mixer.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint8_t voices[0x180], fx[4][0x20], pcm[4096];
static int16_t delay[4][160], left[80], right[80];
static uint32_t resampler[256]; // 64 x 4 taps, then four 64-sample patterns
template<class T> static void put(uint8_t* p, int offset, T value)
{
	memcpy(p + offset, &value, sizeof value);
}
template<class T> static T get(const uint8_t* p, int offset)
{
	T value;
	memcpy(&value, p + offset, sizeof value);
	return value;
}
static const uint8_t* sample_memory(uint32_t addr) { return addr < sizeof pcm ? pcm + addr : NULL; }
static int16_t* effect_memory(uint32_t addr, uint32_t size)
{
	return addr >= 1 && addr <= 4 && size <= 160 ? delay[addr - 1] : NULL;
}
static void reset()
{
	memset(voices, 0, sizeof voices);
	memset(fx, 0, sizeof fx);
	memset(delay, 0, sizeof delay);
	memset(pcm, 64, sizeof pcm);
	// Exact 1:1 resampling, with one Q15 rounding step.
	for (int i = 0; i < 64; i++) {
		resampler[i * 2] = 32767;
		resampler[i * 2 + 1] = 0;
	}
	// Pattern p, sample i = p * 1000 + i * 10 (two s16 per u32, high first).
	for (int i = 0; i < 128; i++) {
		int p = i / 32, k = (i % 32) * 2;
		resampler[128 + i] = (uint32_t)(p * 1000 + k * 10) << 16 | (uint32_t)(p * 1000 + (k + 1) * 10);
	}
	port_dspmix_setup(1, voices, resampler, NULL, fx);
	port_dspmix_set_aram(sample_memory);
	port_dspmix_set_mram(effect_memory);
}
static void line(int i, int mode, uint16_t bus, int16_t gain)
{
	put<uint16_t>(fx[i], 0, mode);
	put<uint16_t>(fx[i], 2, 1);
	put<uint32_t>(fx[i], 4, i + 1);
	put<uint16_t>(fx[i], 8, bus);
	put<int16_t>(fx[i], 10, gain);
}
static void render() { port_dspmix_render(left, right, 80, 0x8000); }
static void voice(bool automatic)
{
	put<uint16_t>(voices, 0, 1);
	put<uint16_t>(voices, 4, 0x1000);
	put<uint16_t>(voices, 8, 1);
	put<uint16_t>(voices, 0x100, 8);
	put<uint16_t>(voices, 0x102, 1);
	put<uint32_t>(voices, 0x114, sizeof pcm);
	if (automatic) {
		put<uint16_t>(voices, 0x52, 127 << 8);
		put<uint16_t>(voices, 0x54, 0x4000);
		put<uint16_t>(voices, 0x56, 0x4000);
		put<uint16_t>(voices, 0x58, 1);
	} else {
		put<uint16_t>(voices, 0x10, 0xD00);
		put<uint16_t>(voices, 0x12, 0x2000); // 1.0 (Q13)
		put<uint16_t>(voices, 0x14, 0x2000);
	}
}
int main()
{
	unsetenv("SMS_AUDIO_FX");
	unsetenv("SMS_AUDIO_MASTER_SHIFT");
	unsetenv("SMS_AUDIO_SLOT_SHIFT");
	reset();
	line(0, 2, 0xD00, 4096);
	delay[0][0] = 1024;
	put<int16_t>(fx[0], 0x10 + 7 * 2, 0x4000);
	render();
	assert(left[8] == 128 && left[1] == 0); // Q15 return precedes the post-filter.
	assert(delay[0][1] == 512 && delay[0][8] == 0); // Forward FIR, written after mixing.
	reset();
	line(0, 1, 0xD60, 4096);
	delay[0][0] = 1024;
	put<int16_t>(fx[0], 0x10 + 7 * 2, 0x4000);
	render();
	assert(right[1] == 64 && right[8] == 0 && left[1] == 0);
	reset();
	line(0, 1, 0xD00, 4096);
	delay[0][79] = 2000;
	put<int16_t>(fx[0], 0x10, 0x4000);
	render();
	render();
	assert(left[7] == 125); // Last eight raw samples cross subframes.
	reset();
	line(0, 2, 0xD00, 4096);
	delay[0][0] = 1024;
	render();
	assert(left[8] == 128);
	memset(delay, 0, sizeof delay);
	render();
	for (int i = 0; i < 80; i++)
		assert(left[i] == 0); // CPU clear must remove old echo.
	reset();
	line(0, 2, 0, 0);
	line(2, 2, 0, 0);
	delay[2][0] = 1024;
	put<uint16_t>(fx[2], 0xC, 0xDC0);
	put<int16_t>(fx[2], 0xE, 8192);
	render();
	assert(delay[0][8] == 256); // Second return may feed another reverb bus.
	reset();
	for (int i = 0; i < 4; i++)
		line(i, 2, 0, 0);
	voice(true);
	render();
	assert(left[79] == 8191 && right[79] == 0);
	assert(delay[2][79] == 4063 && delay[3][79] == 0 && delay[0][79] == 0 && delay[1][79] == 0);
	// The largest game-generated factor (127 << 8) is just under 0.5,
	// not a full-strength wet send. Changing it must leave dry levels alone.
	put<uint16_t>(voices, 0x52, 0x4000);
	render();
	assert(left[79] == 8191 && delay[2][79] == 2047);
	put<uint16_t>(voices, 0x52, 1);
	render();
	assert(left[79] == 8191 && delay[2][79] == 0);
	for (int automatic = 0; automatic < 2; automatic++) {
		reset();
		voice(automatic);
		render();
		assert(left[79] == (automatic ? 8191 : 16383)); // Preserve dry path levels.
		put<uint16_t>(voices, 0x10A, 1);
		int frames = 0;
		do {
			render();
			frames++;
			if (frames == 1) {
				assert(get<uint16_t>(voices, 2) == 0);
				assert(left[79] > 0);
			}
			assert(frames <= 16);
		} while (!get<uint16_t>(voices, 2));
		assert(frames == (automatic ? 15 : 14)); // Halve the current volume (0x4000 or 0x2000) until zero, then finish.
		render();
		for (int i = 0; i < 80; i++)
			assert(left[i] == 0 && right[i] == 0);
	}
	// Oscillators (JAudio noteOnOsc) have no wave: the fields a previous wave
	// left behind (end position 0 here) must not end them. One period is 32
	// output samples at ratio 0x1000, without the resampling filter.
	const int pattern_of[5][2] = { { 7, 0 }, { 10, 0 }, { 4, 1 }, { 11, 2 }, { 12, 3 } };
	for (int t = 0; t < 5; t++) {
		reset();
		voice(false);
		put<uint16_t>(voices, 0x100, pattern_of[t][0]);
		put<uint32_t>(voices, 0x114, 0);
		for (int f = 0; f < 3; f++) {
			render();
			for (int i = 0; i < 80; i++)
				assert(left[i] == pattern_of[t][1] * 1000 + (2 * (f * 80 + i) % 64) * 10);
			assert(get<uint16_t>(voices, 2) == 0 && get<uint16_t>(voices, 0xA) == 0);
		}
	}
	reset();
	voice(false);
	put<uint16_t>(voices, 0x100, 7);
	put<uint16_t>(voices, 4, 0x800); // an octave down: one pattern sample per output sample
	render();
	for (int i = 0; i < 80; i++)
		assert(left[i] == i % 64 * 10);
	reset();
	voice(false);
	put<uint16_t>(voices, 0x100, 0);
	render();
	for (int i = 0; i < 80; i++)
		assert(left[i] == (i % 32 < 16 ? 0x4000 : -0x4000));
	put<uint16_t>(voices, 0x8, 1); // restart: 25 % duty from phase 0
	put<uint16_t>(voices, 0x100, 3);
	render();
	for (int i = 0; i < 80; i++)
		assert(left[i] == (i % 32 < 8 ? 0x4000 : -0x4000));
	put<uint16_t>(voices, 0x8, 1);
	put<uint16_t>(voices, 0x100, 1);
	render();
	assert(left[0] == 0 && left[1] == 0x800 && left[16] == -0x8000 && left[31] == -0x800);
	put<uint16_t>(voices, 0x10A, 1);
	int frames = 0;
	do {
		render();
		assert(++frames <= 16);
	} while (!get<uint16_t>(voices, 2));
	assert(get<uint16_t>(voices, 0xA) == 0); // stopped, not run out
	// Music and SFX volume (dsp_mixer.h): the class byte at 0x006 picks the gain that scales a
	// voice's samples before its sends; a class at 100 percent leaves them alone.
	port_dspmix_set_class_percent(PORT_DSPMIX_CLASS_MUSIC, 50);
	reset();
	voice(false);
	render(); // the music gain ramps down in this subframe; this voice is SFX (byte 0)
	for (int i = 0; i < 80; i++)
		assert(left[i] == 16383);
	reset();
	voice(false);
	put<uint8_t>(voices, 6, PORT_DSPMIX_CLASS_MUSIC);
	render(); // steady at 50 percent: 16383 * 0.5, rounded down
	for (int i = 0; i < 80; i++)
		assert(left[i] == 8191 && right[i] == 0);
	port_dspmix_set_class_percent(PORT_DSPMIX_CLASS_MUSIC, 100);
	reset();
	voice(false);
	put<uint8_t>(voices, 6, PORT_DSPMIX_CLASS_MUSIC);
	render(); // a change ramps up within the subframe, with no step at its start
	assert(left[0] > 8191 && left[0] < left[1] && left[78] < left[79] && left[79] == 16383);
	port_dspmix_set_class_percent(PORT_DSPMIX_CLASS_SFX, 0);
	reset();
	voice(false);
	render();
	render(); // SFX at 0 percent: silent after the ramp
	for (int i = 0; i < 80; i++)
		assert(left[i] == 0 && right[i] == 0);
	reset();
	voice(false);
	put<uint8_t>(voices, 6, PORT_DSPMIX_CLASS_MUSIC);
	render(); // the music voice is untouched by the SFX setting
	assert(left[79] == 16383);
	assert(port_dspmix_class_percent(PORT_DSPMIX_CLASS_SFX) == 0 && port_dspmix_class_gain(PORT_DSPMIX_CLASS_SFX) == 0);
	assert(port_dspmix_class_gain(PORT_DSPMIX_CLASS_MUSIC) == 32768);
	port_dspmix_set_class_percent(PORT_DSPMIX_CLASS_SFX, 250); // clamped
	assert(port_dspmix_class_percent(PORT_DSPMIX_CLASS_SFX) == 100);
	// Voice filters (Dolphin's Zelda HLE): the biquad runs on the resampled samples before the
	// sends, y[n] = (b1 x[n-1] + b2 x[n-2] + a1 y[n-1] + a2 y[n-2]) >> 15, saturated to s16,
	// only with filterMode bit 5 and coefficients other than the identity 0x7FFF, 0, 0, 0.
	port_dspmix_set_class_percent(PORT_DSPMIX_CLASS_SFX, 100);
	reset();
	voice(false);
	render(); // let the SFX gain finish its ramp back to 100 percent
	reset();
	voice(false);
	put<int16_t>(voices, 0x148, 0x7FFF);
	put<uint16_t>(voices, 0x108, 0x20);
	render(); // identity: untouched
	for (int i = 0; i < 80; i++)
		assert(left[i] == 16383);
	reset();
	voice(false);
	const int16_t coefs[4] = { 0x1591, 0x09AC, 0x4284, 0x366C }; // CUTOFF_TO_IIR_TABLE[7], the under-water row
	for (int i = 0; i < 4; i++)
		put<int16_t>(voices, 0x148 + 2 * i, coefs[i]);
	render(); // bit 5 clear: off
	for (int i = 0; i < 80; i++)
		assert(left[i] == 16383);
	reset();
	voice(false);
	for (int i = 0; i < 4; i++)
		put<int16_t>(voices, 0x148 + 2 * i, coefs[i]);
	put<uint16_t>(voices, 0x108, 0x20);
	{
		int32_t x1 = 0, x2 = 0, y1 = 0, y2 = 0;
		for (int f = 0; f < 4; f++) {
			render(); // the history carries across subframes
			for (int i = 0; i < 80; i++) {
				int64_t acc = (int64_t)coefs[0] * x1 + (int64_t)coefs[1] * x2 + (int64_t)coefs[2] * y1
				            + (int64_t)coefs[3] * y2;
				int32_t y = (int32_t)(acc >> 15);
				y = y > 32767 ? 32767 : y < -32768 ? -32768 : y;
				x2 = x1, x1 = 16383, y2 = y1, y1 = y;
				assert(left[i] == y); // slot volume 1.0 (Q13)
			}
		}
		assert(left[0] > 16383 && left[79] == 32767); // this row's bass boost saturates a DC input
		assert(get<int16_t>(voices, 0xA8) == 16383 && get<int16_t>(voices, 0xAC) == 32767);
	}
	put<uint16_t>(voices, 0x108, 0); // and back off mid-voice
	render();
	for (int i = 0; i < 80; i++)
		assert(left[i] == 16383);
	// The distance low-pass: y[n] = y[n-1] + ((x[n] - x[n-1]) * c >> 7), history cleared on restart.
	reset();
	voice(false);
	put<uint16_t>(voices, 0x150, 0x40);
	render();
	for (int i = 0; i < 80; i++)
		assert(left[i] == 8191);
	assert(get<int16_t>(voices, 0xD0) == 8191 && get<int16_t>(voices, 0xD2) == 16383);
	put<uint16_t>(voices, 0x8, 1);
	render();
	assert(left[0] == 8191); // reset: from zero history again, not 8191 + 0
	setenv("SMS_AUDIO_FILTERS", "0", 1); // the debug bypass
	reset();
	voice(false);
	put<uint16_t>(voices, 0x150, 0x40);
	render();
	assert(left[79] == 16383);
	unsetenv("SMS_AUDIO_FILTERS");
	puts("PASS: Q15 returns, filter modes/history, RAM clears, cross-bus feedback, "
	     "Q16 footstep sends, unchanged dry levels, smooth release and silence, oscillators, music/SFX class gains, "
	     "voice biquad and low-pass filters");
}
