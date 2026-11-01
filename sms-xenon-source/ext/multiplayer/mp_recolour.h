#ifndef SMS_MP_RECOLOUR_H
#define SMS_MP_RECOLOUR_H

// The characters' colours: Mario's colours changed, one colour at a time.
// game/mp_puppet.cpp changes Mario's own texture's colours with these (its
// CMPR blocks' two colours); sync/mp_look.cpp each texel of his picture.

#include "mp_game_api.h"

namespace mp_recolour {

inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

// Mario's red (cap, shirt), told from his reddish-brown hair as Wario's is,
// and his blue (overalls).
inline bool marioRed(int r, int g, int b) { return r > 80 && r > g * 3 && r > b * 3; }
inline bool marioBlue(int r, int g, int b) { return b > 60 && b * 2 > r * 3 && b * 5 > g * 6; }

// The Marios past the first four: Mario in colours of his own (his red and
// blue swapped for others; skin, hair and the rest stay).
inline void recolourMario(int character, int& r, int& g, int& b)
{
	if (marioRed(r, g, b)) {
		const int v = r; // the red's shade, kept
		switch (character) {
		case MPG_CHAR_FIRE_MARIO: r = clamp255(v + 20), g = clamp255(v + 15), b = clamp255(v + 5); break;
		case MPG_CHAR_PURPLE_MARIO: r = v * 62 / 100, g = v * 20 / 100, b = v * 82 / 100; break;
		case MPG_CHAR_ORANGE_MARIO: r = clamp255(v * 105 / 100), g = v * 55 / 100, b = v / 10; break;
		case MPG_CHAR_ICE_MARIO: r = v * 55 / 100, g = v * 88 / 100, b = clamp255(v * 105 / 100); break;
		}
	} else if (marioBlue(r, g, b)) {
		const int v = b;
		switch (character) {
		case MPG_CHAR_FIRE_MARIO: r = clamp255(v * 13 / 10), g = v / 4, b = v / 5; break;
		case MPG_CHAR_PURPLE_MARIO: r = v * 35 / 100 + 10, g = v * 33 / 100 + 10, b = v * 38 / 100 + 12; break;
		case MPG_CHAR_ORANGE_MARIO: r = v * 60 / 100, g = v * 38 / 100, b = v * 22 / 100; break;
		case MPG_CHAR_ICE_MARIO: r = clamp255(v * 80 / 100 + 60), g = clamp255(v * 82 / 100 + 62), b = clamp255(v * 85 / 100 + 70); break;
		}
	}
}

inline void recolour(int character, int& r, int& g, int& b)
{
	switch (character) {
	case MPG_CHAR_FIRE_MARIO:
	case MPG_CHAR_PURPLE_MARIO:
	case MPG_CHAR_ORANGE_MARIO:
	case MPG_CHAR_ICE_MARIO:
		recolourMario(character, r, g, b);
		break;
	case MPG_CHAR_LUIGI:
		// Mario's red (cap, shirt) becomes Luigi's green; skin, hair and the
		// blue overalls stay
		if (r > 80 && r > g * 2 && r > b * 2) {
			int nr = g / 3, ng = r * 4 / 5, nb = b / 2;
			r = nr;
			g = ng;
			b = nb;
		}
		break;
	case MPG_CHAR_METAL_MARIO: {
		// everything a bright, slightly blue silver
		int l = (r * 77 + g * 150 + b * 29) >> 8;
		r     = 70 + l * 7 / 10;
		g     = 74 + l * 7 / 10;
		b     = 84 + l * 7 / 10;
		break;
	}
	case MPG_CHAR_WARIO:
		// Mario's red (cap, shirt) becomes Wario's yellow; skin and the blue
		// overalls stay, and so does his reddish-brown hair (a stricter test
		// than Luigi's)
		if (r > 80 && r > g * 3 && r > b * 3) {
			int v = r * 9 / 10 + 25;
			r     = v;
			g     = v * 4 / 5;
			b     = v / 8;
		}
		break;
	default:
		break;
	}
}

} // namespace mp_recolour

#endif
