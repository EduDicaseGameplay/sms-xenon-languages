// The demo pilot (ext/demo/README.md): plays Mario from a script for trailers
// and footage, as a player would hold the controller. SMS_DEMO_PILOT=<file>
// switches it on; without it the extension does nothing.
//
// The script steers with the analog stick toward points in the stage, closing
// the loop every frame on where Mario is and where the camera looks, so runs
// curve and settle as a player's do, and presses buttons for jumps, dives and
// FLUDD. Times are seconds of play in the current stage.
#include "port_ext.h"
#include "port_framerate.h"

#include <Camera/Camera.hpp>
#include <MarioUtil/MathUtil.hpp>
#include <Player/Mario.hpp>
#include <System/Application.hpp>
#include <System/MarDirector.hpp>
#include <dolphin/pad.h>
#include <dolphin/vi.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern "C" int mp_player_position(int slot, float* xyz) __attribute__((weak));
extern "C" uint32_t GXPC_FrameCount(void) __attribute__((weak)); // as sms_gx declares it (u32 is long on 32-bit)
extern "C" void port_vi_set_pace(double fields) __attribute__((weak));

namespace {

enum Op {
	OP_AREA,      // area A: wait for stage A, then its clock starts at 0
	OP_AT,        // at T: wait for the stage clock to reach T
	OP_WAIT,      // wait S
	OP_GO,        // go X Z [mag]: run to a point and stop there
	OP_VIA,       // via X Z [mag]: run toward a point and keep running
	OP_GOPLAYER,  // goplayer K [dist] [mag]: run to another player
	OP_STOP,      // stop: let go of the stick
	OP_NEAR,      // near R: wait until the point is within R
	OP_ARRIVE,    // arrive: wait until a go has stopped
	OP_FACE,      // face X Z: turn on the spot toward a point
	OP_FACEPLAYER,// faceplayer K
	OP_A,         // a [S]: hold A
	OP_B,         // b [S]: hold B
	OP_X,         // x: tap X (the other nozzle)
	OP_Y,         // y: tap Y
	OP_R,         // r S [level]: hold R (spray, hover)
	OP_RON,       // ron [level]: hold R until roff
	OP_ROFF,
	OP_LAND,      // land: wait until Mario is on the ground
	OP_AIR,       // air: wait until he is in the air
	OP_SPIN,      // spin: a circle of the stick, then A
	OP_CAM,       // cam DX DY S: hold the C-stick
	OP_LOG,       // log: print where Mario is
	OP_PLAYERS,   // players N: wait until N players are in the stage
	OP_WAITNEAR,  // waitnear K R: wait until player K is within R
	OP_WAITFAR,   // waitfar K R: wait until player K is further than R
	OP_WAITAT,    // waitat K X Z R: wait until player K is within R of a point
	OP_STAGE,     // stage A E: leave for area A, episode E (as a pipe would)
	OP_END,
};

struct Step {
	int op;
	float v[4];
};

enum { kMaxSteps = 512 };
Step s_steps[kMaxSteps];
int s_count;
bool s_loaded, s_on, s_log;
float s_pace;  // SMS_DEMO_PACE: frames per wall second while playing (captures)

// the stage's run
int s_stage = -1;
int s_frames;  // frames of play in this stage
int s_pc;      // the step running
int s_stepFrames;
bool s_stepStarted;

// steering: a point (or a player), how hard, and whether to stop there
bool s_steer, s_stopAt, s_player;
float s_tx, s_tz, s_mag, s_stopDist;
int s_playerSlot;
float s_stick;    // the stick's deflection now, eased toward the wanted one
float s_wobble;   // a slow drift of the aim, as a thumb has
bool s_arrived;

// held buttons and sticks for this frame
int s_holdA, s_holdB, s_holdR, s_tapX, s_tapY, s_holdCam;
float s_rLevel, s_camX, s_camY;
bool s_rOn;
int s_spin;  // frames into a spin

float rate() { return (float)port_active_frame_rate; }
int framesOf(float seconds) { return (int)(seconds * rate() + 0.5f); }

int opOf(const char* w)
{
	static const char* names[] = {"area", "at",   "wait", "go",   "via",  "goplayer", "stop", "near",
	                              "arrive", "face", "faceplayer", "a", "b", "x", "y", "r", "ron", "roff",
	                              "land", "air",  "spin", "cam",  "log",  "players", "waitnear", "waitfar",
	                              "waitat", "stage", "end"};
	for (int i = 0; i < (int)(sizeof names / sizeof names[0]); i++)
		if (!strcmp(w, names[i]))
			return i;
	return -1;
}

void load()
{
	s_loaded       = true;
	const char* p  = getenv("SMS_DEMO_PILOT");
	s_log          = getenv("SMS_DEMO_LOG") && strcmp(getenv("SMS_DEMO_LOG"), "0");
	s_pace         = getenv("SMS_DEMO_PACE") ? (float)atof(getenv("SMS_DEMO_PACE")) : 0.0f;
	if (!p || !*p)
		return;
	FILE* f = fopen(p, "r");
	if (!f) {
		fprintf(stderr, "[pilot] can't open %s\n", p);
		return;
	}
	char line[256];
	int n = 0;
	while (fgets(line, sizeof line, f) && s_count < kMaxSteps) {
		n++;
		if (char* hash = strchr(line, '#'))
			*hash = 0;
		char word[32];
		Step st;
		float extra = 0.0f;
		memset(&st, 0, sizeof st);
		const int got = sscanf(line, "%31s %f %f %f %f", word, &st.v[0], &st.v[1], &st.v[2], &extra);
		if (got < 1)
			continue;
		st.op = opOf(word);
		if (st.op < 0) {
			fprintf(stderr, "[pilot] %s:%d: what is \"%s\"?\n", p, n, word);
			continue;
		}
		// defaults for what was left out
		if (st.op == OP_GO || st.op == OP_VIA)
			if (got < 4)
				st.v[2] = 1.0f;
		if (st.op == OP_GOPLAYER) {
			if (got < 3)
				st.v[1] = 120.0f;
			if (got < 4)
				st.v[2] = 1.0f;
		}
		if ((st.op == OP_A || st.op == OP_B) && got < 2)
			st.v[0] = 0.12f;
		if (st.op == OP_R && got < 3)
			st.v[1] = 1.0f;
		if (st.op == OP_RON && got < 2)
			st.v[0] = 1.0f;
		st.v[3] = extra;
		s_steps[s_count++] = st;
	}
	fclose(f);
	s_on = s_count > 0;
	fprintf(stderr, "[pilot] %d steps from %s\n", s_count, p);
}

bool playing()
{
	TMarDirector* dir = gpMarDirector;
	return dir && gpMarioOriginal && gpCamera && dir->mState == TMarDirector::STATE_UNK4
	       && gpApplication.mCurrArea.getStage() != 15;
}

bool airborne() { return (gpMarioOriginal->mStatus & 0x800) != 0; }

bool playerAt(int slot, float* xz)
{
	float p[3];
	if (!mp_player_position || !mp_player_position(slot, p))
		return false;
	xz[0] = p[0];
	xz[1] = p[2];
	return true;
}

// Where the steering goes this frame, if anywhere.
bool target(float* tx, float* tz)
{
	if (s_player) {
		float xz[2];
		if (!playerAt(s_playerSlot, xz))
			return false;
		*tx = xz[0];
		*tz = xz[1];
		return true;
	}
	*tx = s_tx;
	*tz = s_tz;
	return true;
}

float distanceTo(float tx, float tz)
{
	const float dx = tx - gpMarioOriginal->mPosition.x, dz = tz - gpMarioOriginal->mPosition.z;
	return sqrtf(dx * dx + dz * dz);
}

// The stick for moving toward world yaw `yaw` (radians, Mario's convention:
// forward is (sin, cos)) with deflection `mag`: the game adds the camera's
// angle to the stick's (TMario::checkController), so take it off.
void stickToward(float yaw, float mag, s8* x, s8* y)
{
	// matan's handedness, measured once
	static int sign;
	static float base;
	if (!sign) {
		const int right = matan(0.0f, 64.0f), up = matan(-64.0f, 0.0f);
		sign = (s16)(up - right) > 0 ? 1 : -1;
		base = right * (3.14159265f / 32768.0f);
	}
	const float cam   = gpCamera->getUnk258() * (3.14159265f / 32768.0f);
	const float theta = (yaw - cam - base) * sign;
	*x = (s8)(cosf(theta) * mag * 100.0f);
	*y = (s8)(sinf(theta) * mag * 100.0f);
}

float yawTo(float tx, float tz)
{
	return atan2f(tx - gpMarioOriginal->mPosition.x, tz - gpMarioOriginal->mPosition.z);
}

void logPosition(const char* why)
{
	TMario* m = gpMarioOriginal;
	fprintf(stderr, "[pilot] %s t=%.2f stage=%d pos=%.0f,%.0f,%.0f yaw=%.0f cam=%.0f status=%x step=%d gx=%u vi=%u\n", why,
	        s_frames / rate(), (int)gpApplication.mCurrArea.getStage(), m->mPosition.x, m->mPosition.y,
	        m->mPosition.z, m->mFaceAngle.y * (180.0f / 32768.0f), gpCamera->getUnk258() * (180.0f / 32768.0f),
	        (unsigned)m->mStatus, s_pc, GXPC_FrameCount ? GXPC_FrameCount() : 0u,
	        (unsigned)VIGetRetraceCount());
}

// Runs steps until one has to wait.
void runSteps()
{
	for (int guard = 0; guard < 64 && s_pc < s_count; guard++) {
		const Step& st = s_steps[s_pc];
		const bool first = !s_stepStarted;
		s_stepStarted    = true;
		bool done        = true;
		switch (st.op) {
		case OP_AREA:
			done = gpApplication.mCurrArea.getStage() == (int)st.v[0];
			if (done)
				s_frames = 0;
			break;
		case OP_AT: done = s_frames >= framesOf(st.v[0]); break;
		case OP_WAIT: done = s_stepFrames >= framesOf(st.v[0]); break;
		case OP_GO:
		case OP_VIA:
			s_steer = true, s_player = false, s_arrived = false;
			s_tx = st.v[0], s_tz = st.v[1], s_mag = st.v[2];
			s_stopAt   = st.op == OP_GO;
			s_stopDist = 45.0f;
			break;
		case OP_GOPLAYER:
			s_steer = true, s_player = true, s_arrived = false, s_stopAt = true;
			s_playerSlot = (int)st.v[0], s_stopDist = st.v[1], s_mag = st.v[2];
			break;
		case OP_STOP: s_steer = false; break;
		case OP_NEAR: {
			float tx, tz;
			done = !s_steer || s_arrived || (target(&tx, &tz) && distanceTo(tx, tz) <= st.v[0]);
			break;
		}
		case OP_ARRIVE: done = !s_steer || s_arrived; break;
		case OP_FACE:
		case OP_FACEPLAYER: {
			// a light touch of the stick turns Mario where he stands
			float tx = st.v[0], tz = st.v[1];
			if (st.op == OP_FACEPLAYER) {
				float xz[2];
				if (!playerAt((int)st.v[0], xz)) {
					done = true;
					break;
				}
				tx = xz[0], tz = xz[1];
			}
			s_steer = false;
			const float want = yawTo(tx, tz);
			const float have = gpMarioOriginal->mFaceAngle.y * (3.14159265f / 32768.0f);
			float diff       = want - have;
			while (diff > 3.14159265f)
				diff -= 6.2831853f;
			while (diff < -3.14159265f)
				diff += 6.2831853f;
			done = (fabsf(diff) < 0.12f && s_stepFrames > 2) || s_stepFrames > framesOf(1.0f);
			if (!done)
				s_tx = tx, s_tz = tz;  // the stick below turns him
			break;
		}
		case OP_A:
			if (first)
				s_holdA = framesOf(st.v[0]) > 0 ? framesOf(st.v[0]) : 1;
			break;
		case OP_B:
			if (first)
				s_holdB = framesOf(st.v[0]) > 0 ? framesOf(st.v[0]) : 1;
			break;
		case OP_X: s_tapX = 2; break;
		case OP_Y: s_tapY = 2; break;
		case OP_R:
			if (first) {
				s_holdR  = framesOf(st.v[0]);
				s_rLevel = st.v[1];
			}
			break;
		case OP_RON: s_rOn = true, s_rLevel = st.v[0]; break;
		case OP_ROFF: s_rOn = false; break;
		case OP_LAND: done = !airborne() && s_stepFrames > 0; break;
		case OP_AIR: done = airborne(); break;
		case OP_SPIN:
			if (first)
				s_spin = 1;
			break;
		case OP_CAM:
			if (first) {
				s_camX = st.v[0], s_camY = st.v[1];
				s_holdCam = framesOf(st.v[2]);
			}
			break;
		case OP_LOG: logPosition("log"); break;
		case OP_PLAYERS: {
			int n = 0;
			float xz[2];
			for (int slot = 0; slot < 4; slot++)
				n += playerAt(slot, xz) ? 1 : 0;
			done = n >= (int)st.v[0];
			break;
		}
		case OP_WAITNEAR:
		case OP_WAITFAR: {
			float xz[2];
			if (!playerAt((int)st.v[0], xz)) {
				done = false;
				break;
			}
			const float d = distanceTo(xz[0], xz[1]);
			done          = st.op == OP_WAITNEAR ? d <= st.v[1] : d > st.v[1];
			break;
		}
		case OP_WAITAT: {
			float xz[2];
			done = playerAt((int)st.v[0], xz)
			       && sqrtf((xz[0] - st.v[1]) * (xz[0] - st.v[1]) + (xz[1] - st.v[2]) * (xz[1] - st.v[2])) <= st.v[3];
			break;
		}
		case OP_STAGE:
			gpMarDirector->setNextStage((u16)((((int)st.v[0] + 1) << 8) | (int)st.v[1]), nullptr);
			break;
		case OP_END: s_on = false; return;
		}
		if (!done)
			return;
		s_pc++;
		s_stepFrames  = 0;
		s_stepStarted = false;
	}
}

void padRead(void* raw)
{
	if (!s_loaded)
		load();
	const bool play = s_on && playing();
	if (s_pace > 0 && port_vi_set_pace) {
		static bool paced;
		if (play != paced)
			port_vi_set_pace(play ? s_pace : 0);
		paced = play;
	}
	if (!play)
		return;
	PADStatus& pad = ((PADStatus*)raw)[0];
	const int stage = gpApplication.mCurrArea.getStage();
	if (stage != s_stage) {
		s_stage  = stage;
		s_frames = 0;
		s_steer = s_rOn = false;
		s_stick = 0.0f;
		s_holdA = s_holdB = s_holdR = s_spin = 0;
		// the stage changed under the script (the host took everyone along):
		// carry on from this stage's part
		for (int i = s_pc; i < s_count; i++)
			if (s_steps[i].op == OP_AREA && (int)s_steps[i].v[0] == stage) {
				s_pc          = i;
				s_stepFrames  = 0;
				s_stepStarted = false;
				break;
			}
	}

	runSteps();
	if (!s_on)
		return;
	const Step* st = s_pc < s_count ? &s_steps[s_pc] : nullptr;
	const bool facing = st && (st->op == OP_FACE || st->op == OP_FACEPLAYER);

	// the left stick
	float want = 0.0f, yaw = 0.0f;
	if (s_steer) {
		float tx, tz;
		if (target(&tx, &tz)) {
			const float d = distanceTo(tx, tz);
			if (s_stopAt && d <= s_stopDist)
				s_arrived = true;
			if (!(s_stopAt && s_arrived)) {
				want = s_mag;
				// ease off when nearly there, as a thumb does
				if (s_stopAt && d < 220.0f)
					want *= 0.45f + 0.55f * (d / 220.0f);
				yaw = yawTo(tx, tz);
			}
		}
	} else if (facing) {
		want = 0.22f;
		yaw  = yawTo(s_tx, s_tz);
	}
	// a thumb takes a moment to push the stick over, and lets go quickly
	if (want > s_stick)
		s_stick = s_stick + 1.0f / (0.12f * rate()) > want ? want : s_stick + 1.0f / (0.12f * rate());
	else
		s_stick = want;
	s_wobble += 1.0f / rate();
	pad.stickX = pad.stickY = 0;
	if (s_stick > 0.0f)
		stickToward(yaw + 0.05f * sinf(s_wobble * 1.7f), s_stick, &pad.stickX, &pad.stickY);
	if (s_spin > 0) {
		// a full turn of the stick in a quarter second, then the jump
		const int n = framesOf(0.25f);
		const float a = 6.2831853f * s_spin / n;
		pad.stickX = (s8)(cosf(a) * 95.0f);
		pad.stickY = (s8)(sinf(a) * 95.0f);
		if (++s_spin > n) {
			s_spin  = 0;
			s_holdA = framesOf(0.15f);
		}
	}

	// the buttons
	u16 b = pad.button & PAD_BUTTON_START;
	if (s_holdA > 0)
		b |= PAD_BUTTON_A, s_holdA--;
	if (s_holdB > 0)
		b |= PAD_BUTTON_B, s_holdB--;
	if (s_tapX > 0)
		b |= PAD_BUTTON_X, s_tapX--;
	if (s_tapY > 0)
		b |= PAD_BUTTON_Y, s_tapY--;
	u8 r = 0;
	if (s_holdR > 0 || s_rOn) {
		r = (u8)(s_rLevel * 255.0f);
		if (s_holdR > 0)
			s_holdR--;
	}
	if (r >= 250)
		b |= PAD_TRIGGER_R;
	pad.button       = b;
	pad.analogA      = (b & PAD_BUTTON_A) ? 255 : 0;
	pad.analogB      = (b & PAD_BUTTON_B) ? 255 : 0;
	pad.triggerRight = r;
	pad.triggerLeft  = 0;
	pad.substickX = pad.substickY = 0;
	if (s_holdCam > 0) {
		pad.substickX = (s8)(s_camX * 100.0f);
		pad.substickY = (s8)(s_camY * 100.0f);
		s_holdCam--;
	}

	if (s_log && s_frames % framesOf(0.5f) == 0)
		logPosition("at");
	s_frames++;
	s_stepFrames++;
}

PortExtension s_extension = {
	"demo", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, padRead,
};

struct Register {
	Register() { port_ext_register(&s_extension); }
} s_register;

} // namespace
