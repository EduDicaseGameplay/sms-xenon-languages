// A stand-in player for testing multiplayer without more games running
// (ext/multiplayer/README.md#testing): joins a host through the same
// controller the game uses, follows it from stage to stage, and stands
// beside the host doing whatever the host's Mario does (its pose, moved
// sideways), so the host's game shows it as a puppet.
//
//   mp_bot ADDRESS[:PORT] [--name NAME] [--offset X,Z] [--seconds N] [--coins N] [--turn]
//
// --turn faces the other way from the host (towards the camera behind them).
// --yoshi N rides Yoshi (N: his colour, 0 green, 1 orange, 2 purple, 3 pink).
// --spray pumps FLUDD (the upper body's pumping pose).
// --follow SLOT stands beside that player instead of the host (slot 0).
// --delay S does what the host did S seconds ago, where it was then (a
// player trailing behind, out of step with the host).
// --yaw DEG faces DEG degrees away from the host's facing.
// --circle R walks round a circle of R units about its place, at an even
// speed in its own game time (a player moving smoothly at the source, to see
// how smoothly the others draw them).
// --coins N reports N coins picked up, one a second from three seconds in
// the host's stage, to check that pickups add up in the host's game.
// --body N wears Eclipse character N's model (MPG_BODY_*: 0 Mario, 1 Luigi,
// 2 Il Piantissimo, 3 Shadow Mario), whatever the host plays.
// --sunglasses wears the sunglasses; --no-sunglasses never does.
#include "../mp_game_api.h"

#include <chrono>
#include <deque>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <vector>

int main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: mp_bot ADDRESS[:PORT] [--name NAME] [--offset X,Z] [--seconds N] [--coins N]\n");
		return 2;
	}
	const char* name = "Bot";
	float offX = 150.0f, offZ = 0.0f;
	double seconds = 60;
	int coins      = 0;
	bool turn      = false;
	int yoshi      = -1;
	bool spray     = false;
	int follow     = 0;
	float delay    = 0.0f, yawDeg = 0.0f;
	float circle   = 0.0f;
	int body       = -1;
	int sunglasses = -1;
	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--name") && i + 1 < argc)
			name = argv[++i];
		else if (!strcmp(argv[i], "--offset") && i + 1 < argc)
			sscanf(argv[++i], "%f,%f", &offX, &offZ);
		else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
			seconds = atof(argv[++i]);
		else if (!strcmp(argv[i], "--coins") && i + 1 < argc)
			coins = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--turn"))
			turn = true;
		else if (!strcmp(argv[i], "--yoshi") && i + 1 < argc)
			yoshi = atoi(argv[++i]) & 3;
		else if (!strcmp(argv[i], "--spray"))
			spray = true;
		else if (!strcmp(argv[i], "--follow") && i + 1 < argc)
			follow = atoi(argv[++i]) % MPG_MAX_PLAYERS;
		else if (!strcmp(argv[i], "--delay") && i + 1 < argc)
			delay = (float)atof(argv[++i]);
		else if (!strcmp(argv[i], "--yaw") && i + 1 < argc)
			yawDeg = (float)atof(argv[++i]);
		else if (!strcmp(argv[i], "--circle") && i + 1 < argc)
			circle = (float)atof(argv[++i]);
		else if (!strcmp(argv[i], "--body") && i + 1 < argc)
			body = atoi(argv[++i]) % MPG_BODIES;
		else if (!strcmp(argv[i], "--sunglasses"))
			sunglasses = 1;
		else if (!strcmp(argv[i], "--no-sunglasses"))
			sunglasses = 0;
	}
	setenv("SMS_MULTIPLAYER", "join", 1);
	setenv("SMS_MULTIPLAYER_ADDRESS", argv[1], 1);
	setenv("SMS_MULTIPLAYER_NAME", name, 1);
	// the game's build when it differs from this checkout's (mp_shots.py)
	if (const char* build = getenv("MP_BOT_BUILD"))
		mpg_set_build(build);
	if (mpg_start() != MPG_ROLE_CLIENT)
		return 1;

	std::vector<int32_t> flags(mpg_flag_count(), 0);
	int coinIndex = -1;
	for (int i = 0; i < mpg_flag_count(); i++)
		if (mpg_flag_id(i) == 0x40002)
			coinIndex = i;
	const auto start = std::chrono::steady_clock::now();
	int frame = 0, given = 0, inStage = 0;
	bool wasPlaying = false;
	for (;;) {
		const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		if (t > seconds)
			break;
		MpgGame g;
		memset(&g, 0, sizeof g);
		g.frameMs = 33.0f; // the bot's loop sleeps 33 ms a frame
		int area, episode;
		// "in" the host's stage as soon as it is known
		if (mpg_target(&area, &episode) && mpg_flags_ready()) {
			g.area    = area;
			g.episode = episode;
			// the host moved on: a frame "loading", then in their new stage
			g.playing = mpg_in_target() || !wasPlaying;
		}
		static unsigned loads;
		if (g.playing && !wasPlaying)
			loads++;
		g.loads    = loads;
		wasPlaying = g.playing != 0;
		mpg_frame(&g);
		if (g.playing) {
			mpg_flags_sync(flags.data());
			inStage++;
			if (coins && coinIndex >= 0 && given < coins && inStage > 90 + given * 30) {
				flags[coinIndex]++;
				given++;
				fprintf(stderr, "[bot %s] picked up a coin (%d)\n", name, given);
			}
		}
		MpgPose host;
		float age;
		MpgPose mine;
		memset(&mine, 0, sizeof mine);
		mine.session = mpg_session();
		bool haveHost = mpg_pose(follow, &host, &age);
		if (haveHost && delay > 0.0f) {
			// the host's pose from `delay` seconds ago
			static std::deque<std::pair<double, MpgPose>> past;
			past.emplace_back(t, host);
			while (past.size() > 1 && past[1].first <= t - delay)
				past.pop_front();
			if (past.front().first > t - delay)
				haveHost = false; // not that far back yet
			else
				host = past.front().second;
		}
		if (haveHost) {
			mine = host;
			if (yawDeg != 0.0f) {
				// turned about its own up axis
				const float c = cosf(yawDeg * 3.14159265f / 180.0f), s = sinf(yawDeg * 3.14159265f / 180.0f);
				for (int r = 0; r < 3; r++) {
					const float x = mine.mtx[r][0], z = mine.mtx[r][2];
					mine.mtx[r][0] = x * c - z * s;
					mine.mtx[r][2] = x * s + z * c;
				}
			}
			// beside the host, in the host's own frame (to its right)
			mine.mtx[0][3] += host.mtx[0][0] * offX + host.mtx[0][2] * offZ;
			mine.mtx[1][3] += host.mtx[1][0] * offX + host.mtx[1][2] * offZ;
			mine.mtx[2][3] += host.mtx[2][0] * offX + host.mtx[2][2] * offZ;
			if (circle > 0.0f) {
				// 12 units a frame round the circle, by frames (the poses' own clock)
				const float angle = frame * 12.0f / circle;
				mine.mtx[0][3] += circle * cosf(angle);
				mine.mtx[2][3] += circle * sinf(angle);
			}
			// the host's water, from where we stand
			for (int e = 0; e < 2; e++)
				for (int c = 0; c < 3; c++)
					mine.emitPos[e][c] += host.mtx[c][0] * offX + host.mtx[c][2] * offZ;
			if (body >= 0)
				mine.body = (uint8_t)body;
			if (sunglasses >= 0)
				mine.more = (uint8_t)(sunglasses ? mine.more | MPG_POSE2_SUNGLASSES : mine.more & ~MPG_POSE2_SUNGLASSES);
			if (turn)
				for (int r = 0; r < 3; r++) {
					mine.mtx[r][0] = -mine.mtx[r][0];
					mine.mtx[r][2] = -mine.mtx[r][2];
				}
			if (yoshi >= 0) {
				// sitting on Yoshi as he stands (Mario's yo_wait, Yoshi's idle)
				static const uint8_t colours[4][3] = {{0x40, 0xA1, 0x24}, {0xFF, 0x8C, 0x1C}, {0xAA, 0x4C, 0xFF}, {0xFF, 0xA0, 0xBE}};
				mine.flags |= MPG_POSE_YOSHI;
				mine.bodyBck   = 0xB6;
				mine.bodyFrame = (float)(frame % 60);
				mine.bodyBlend = 0;
				mine.yoshiBck  = 22;
				mine.yoshiFrame = (float)(frame % 60);
				mine.nozzle    = 3; // Yoshi's nozzle
				memcpy(mine.yoshiColor, colours[yoshi], 3);
			}
			if (spray) {
				mine.flags |= MPG_POSE_UPPER | MPG_POSE_SPRAYING;
				mine.upperBck   = 0x44; // pump
				mine.upperFrame = (float)(frame % 20);
				// water from in front of the chest, forwards and a little down
				mine.emitters = 1;
				for (int c = 0; c < 3; c++) {
					mine.emitPos[0][c] = mine.mtx[c][3] + mine.mtx[c][1] * 70.0f + mine.mtx[c][2] * 40.0f;
					mine.emitDir[0][c] = mine.mtx[c][2] - mine.mtx[c][1] * 0.15f;
					mine.emitSpeed[c]  = 0.0f;
				}
				mine.nozzle   = 0;
				mine.pressure = 1.0f;
			}
		}
		mpg_set_pose(&mine);
		char line[256];
		while (mpg_take_notice(line, sizeof line))
			fprintf(stderr, "[bot %s] %s\n", name, line);
		if (getenv("MP_BOT_DEBUG") && frame % 60 == 0 && mpg_pose(0, &host, &age))
			fprintf(stderr, "[bot %s] host pose: flags %02x hand %d nozzle %d btp %d body %d@%.1f upper %d@%.1f fludd %d@%.1f yoshi %d@%.1f\n",
			        name, host.flags, host.hand, host.nozzle, host.btp, host.bodyBck, host.bodyFrame, host.upperBck,
			        host.upperFrame, host.fluddBck, host.fluddFrame, host.yoshiBck, host.yoshiFrame);
		if (++frame % 300 == 0) {
			char status[512];
			mpg_status(status, sizeof status);
			fprintf(stderr, "[bot %s] session %u\n%s\n", name, mpg_session(), status);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(33));
	}
	return 0;
}
