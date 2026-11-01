// Bench runs (phase 0 of xenon/ARCHITECTURE.md): one launch measures several scenes.
// sms_env.txt names a list, SMS_XE_BENCH=game:\bench.txt, of runs as sections:
//
//   [plaza]
//   SMS_MOVIE=game:\plaza.dtm
//   SMS_WARP=1,0,1
//   SMS_XE_BENCH_FIELDS=3000-6600
//
// A run's lines are added to the environment over sms_env.txt's. Each frame shown inside its field
// range is measured (xe_gx_render.cpp's perfFrame). At the range's end the run's figures go to
// game:\bench_results.txt, and the title starts itself again for the next run: game:\bench.state
// holds its index. Every run starts from an empty card of its own (game:\bench\card-a). After the
// last run the title goes back to the dashboard. Frame times only mean
// something with SMS_VI_DETERMINISTIC=1 (no vsync, no audio clock: a frame takes its work), or at the real clock
// (sms_env.bench-rt.txt) where idle time and the wait for the flip are left out of the parts.
#include "../msvc/include/xe_win.h"
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

extern "C" unsigned long port_vi_game_field(void);  // port patch 12
extern "C" volatile long g_xeVblanks;               // xe_vblank.cpp
extern "C" int port_active_frame_rate;            // platform/port_runtime.cpp
namespace gx { void logmsg(const char* fmt, ...); }

namespace {

const char kState[] = "game:\\bench.state";
const char kResults[] = "game:\\bench_results.txt";
const char kCard[] = "game:\\bench\\card-a";  // SMS_SAVE_DIR of the runs

int s_run = -1;  // this launch's run (-1: no bench)
int s_runs;
char s_name[64];
unsigned s_from, s_to;
bool s_done;
// Vertical blanks while the run counted frames (xe_vblank.cpp): beyond fields-per-frame each, a frame
// shown late. Frame times alone also count a frame that ends later than the one before, though shown on time.
long s_vblank0, s_vblanks;

int readState()
{
	FILE* f = fopen(kState, "r");
	int n = 0;
	if (f) {
		if (fscanf(f, "%d", &n) != 1) n = 0;
		fclose(f);
	}
	return n;
}

// One measured quantity over the run's frames, in ms.
struct Series {
	std::vector<float> v;
	void add(double s) { v.push_back(float(s * 1000.0)); }
	void print(FILE* f, const char* name)
	{
		if (v.empty()) return;
		std::vector<float> s(v);
		std::sort(s.begin(), s.end());
		double sum = 0;
		for (size_t i = 0; i < s.size(); i++) sum += s[i];
		const size_t n = s.size();
		fprintf(f, "  %-8s mean %6.2f  p50 %6.2f  p95 %6.2f  p99 %6.2f  max %6.2f\n", name, sum / n, s[n / 2],
		        s[std::min(n - 1, n * 95 / 100)], s[std::min(n - 1, n * 99 / 100)], s[n - 1]);
	}
};
Series s_frame, s_game, s_core, s_render, s_wait, s_audio;
double s_syncs, s_writeBacks;

void finish()
{
	s_done = true;
	FILE* f = fopen(kResults, "a");
	if (f) {
		size_t over = 0;  // frames over a 30 fps frame (two fields)
		for (size_t i = 0; i < s_frame.v.size(); i++)
			if (s_frame.v[i] > 2 * 16.6834f) over++;
		const double n = double(s_frame.v.size() ? s_frame.v.size() : 1);
		const long late = s_vblanks - long(s_frame.v.size()) * (60 / port_active_frame_rate);
		fprintf(f, "[%s] fields %u-%u, %u frames, %ld vblanks late (%.1f%%), %u over 33.4 ms (%.1f%%); per frame: "
		           "%.2f peeks/metric reads, %.2f write-backs\n",
		        s_name, s_from, s_to, unsigned(s_frame.v.size()), late, 100.0 * late / n, unsigned(over),
		        100.0 * over / n, s_syncs / n, s_writeBacks / n);
		s_frame.print(f, "frame");
		s_game.print(f, "game");
		s_core.print(f, "gx core");
		s_render.print(f, "render");
		s_wait.print(f, "gpu wait");
		s_audio.print(f, "audio");
		fclose(f);
	}
	gx::logmsg("[bench] run %d of %d (%s) done", s_run + 1, s_runs, s_name);
	if (s_run + 1 < s_runs) {
		f = fopen(kState, "w");
		if (f) {
			fprintf(f, "%d\n", s_run + 1);
			fclose(f);
		}
		XLaunchNewImage("game:\\default.xex", 0);
	} else {
		DeleteFileA(kState);
		XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP, 0);
	}
}

} // namespace

// 1 while a run's frames are measured: profiles (build-xenon/prof3.py) read it over xbdm to keep only
// those samples
extern "C" volatile long g_xeBenchActive;
volatile long g_xeBenchActive;

// From xe_posix.cpp's load_env, after sms_env.txt: this launch's run's lines.
extern "C" void xe_bench_env(const char* list, void (*put)(const char*, const char*))
{
	FILE* f = fopen(list, "r");
	if (!f) return;
	const int want = readState();
	int index = -1;
	char line[512];
	while (fgets(line, sizeof line, f)) {
		char* e = line + strlen(line);
		while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
			*--e = 0;
		char* s = line;
		while (*s == ' ' || *s == '\t')
			s++;
		if (!*s || *s == '#') continue;
		if (*s == '[') {
			index++;
			if (index == want) {
				char* close = strchr(s, ']');
				if (close) *close = 0;
				_snprintf(s_name, sizeof s_name - 1, "%s", s + 1);
			}
			continue;
		}
		char* eq = strchr(s, '=');
		if (index != want || !eq) continue;
		*eq = 0;
		put(s, eq + 1);
	}
	fclose(f);
	s_runs = index + 1;
	if (want >= s_runs) return;
	s_run = want;
	// The movies start from a fresh card: the bench's own, emptied, never the player's
	put("SMS_SAVE_DIR", kCard);
	CreateDirectoryA("game:\\bench", NULL);
	CreateDirectoryA(kCard, NULL);
	WIN32_FIND_DATAA d;
	HANDLE h = FindFirstFileA("game:\\bench\\card-a\\*", &d);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
			char path[MAX_PATH];
			_snprintf(path, sizeof path - 1, "%s\\%s", kCard, d.cFileName);
			path[sizeof path - 1] = 0;
			DeleteFileA(path);
		} while (FindNextFileA(h, &d));
		FindClose(h);
	}
}

// Each frame shown (xe_gx_render.cpp): its time and where it went, in seconds (audio: the DSP mixer).
extern "C" void xe_bench_frame(double frame, double game, double core, double render, double wait, double audio,
                               unsigned syncs, unsigned writeBacks)
{
	if (s_run < 0 || s_done) return;
	if (!s_to) {
		const char* r = getenv("SMS_XE_BENCH_FIELDS");
		if (!r || sscanf(r, "%u-%u", &s_from, &s_to) != 2 || s_to <= s_from) {
			gx::logmsg("[bench] %s: no SMS_XE_BENCH_FIELDS=from-to", s_name);
			s_run = -1;
			return;
		}
		gx::logmsg("[bench] run %d of %d: %s, fields %u-%u", s_run + 1, s_runs, s_name, s_from, s_to);
	}
	const unsigned field = unsigned(port_vi_game_field());
	if (field < s_from) return;
	if (field >= s_to) {
		s_vblanks = g_xeVblanks - s_vblank0;
		g_xeBenchActive = 0;
		finish();
		return;
	}
	if (!g_xeBenchActive) s_vblank0 = g_xeVblanks - long(60 / port_active_frame_rate);  // this frame's own
	g_xeBenchActive = 1;
	s_frame.add(frame);
	s_game.add(game);
	s_core.add(core);
	s_render.add(render);
	s_wait.add(wait);
	s_audio.add(audio);
	s_syncs += syncs;
	s_writeBacks += writeBacks;
}
