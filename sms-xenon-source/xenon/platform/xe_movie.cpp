// .dtm movie playback on the 360: the input half of platform/trace/trace.cpp (whose memory
// trace leans on Linux's link maps), so a run here replays the same Dolphin movie as the
// reference shots (SMS_MOVIE=game:\x.dtm, SMS_TRACE_SYNC=+0, SMS_VI_DETERMINISTIC=1).
// Only a numeric SMS_TRACE_SYNC offset; polls follow 2 * field - 227 (+ SMS_MOVIE_POLL_BIAS).
#include "trace/trace.h"
#include <dolphin/pad.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

namespace {

struct Movie {
	bool inited, on;
	std::vector<uint8_t> recs;  // 8 bytes a poll
	uint32_t polls, cur;
	long long offset;  // retail field = retrace + offset
	int bias;
};
Movie g;

void init()
{
	if (g.inited)
		return;
	g.inited = true;
	const char* m = getenv("SMS_MOVIE");
	if (!m)
		return;
	if (FILE* f = fopen(m, "rb")) {
		uint8_t hdr[256];
		if (fread(hdr, 1, 256, f) == 256 && !memcmp(hdr, "DTM\x1a", 4)) {
			uint8_t rec[8];
			while (fread(rec, 1, 8, f) == 8)
				g.recs.insert(g.recs.end(), rec, rec + 8);
			g.polls = uint32_t(g.recs.size() / 8);
			g.on = g.polls != 0;
		}
		fclose(f);
	}
	const char* sync = getenv("SMS_TRACE_SYNC");
	g.offset = sync ? strtoll(sync, NULL, 0) : 0;
	g.bias = getenv("SMS_MOVIE_POLL_BIAS") ? atoi(getenv("SMS_MOVIE_POLL_BIAS")) : 0;
	fprintf(stderr, g.on ? "[movie] %s: %u polls\n" : "[movie] cannot use %s\n", m, g.polls);
}

}  // namespace

extern "C" int port_trace_movie_active(void)
{
	init();
	return g.on;
}

extern "C" void port_trace_on_retrace(uint32_t retrace)
{
	init();
	// the game reads the pad late in the field, after its two SI polls: the last poll before
	// the next field's
	long long field = (long long)retrace + g.offset;
	long long next = 2 * (field + 1) - 227 + g.bias;
	g.cur = next > 0 ? uint32_t(next - 1) : 0;
}

extern "C" int port_trace_pad_read(PADStatus* status)
{
	init();
	if (!g.on)
		return 0;
	memset(status, 0, 4 * sizeof(PADStatus));
	for (int i = 1; i < 4; i++)
		status[i].err = PAD_ERR_NO_CONTROLLER;
	const uint8_t* rec = &g.recs[8 * (g.cur < g.polls ? g.cur : g.polls - 1)];
	// Dolphin's ControllerState: u16 LE bits, trigger L/R, stick X/Y, c-stick X/Y
	uint16_t bits = uint16_t(rec[0] | rec[1] << 8);
	static const uint16_t map[12] = {PAD_BUTTON_START, PAD_BUTTON_A, PAD_BUTTON_B, PAD_BUTTON_X,
	                                 PAD_BUTTON_Y, PAD_TRIGGER_Z, PAD_BUTTON_UP, PAD_BUTTON_DOWN,
	                                 PAD_BUTTON_LEFT, PAD_BUTTON_RIGHT, PAD_TRIGGER_L, PAD_TRIGGER_R};
	uint16_t b = 0;
	for (int i = 0; i < 12; i++)
		if (bits & (1u << i))
			b |= map[i];
	PADStatus& s = status[0];
	s.button = b;
	s.triggerLeft = rec[2];
	s.triggerRight = rec[3];
	s.stickX = s8(rec[4] - 128);
	s.stickY = s8(rec[5] - 128);
	s.substickX = s8(rec[6] - 128);
	s.substickY = s8(rec[7] - 128);
	s.err = (bits & (1u << 14)) ? PAD_ERR_NONE : PAD_ERR_NO_CONTROLLER;
	return 1;
}
