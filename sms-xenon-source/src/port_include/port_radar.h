#ifndef PORT_RADAR_H
#define PORT_RADAR_H

// The radar (src/radar.cpp): a round map at the HUD's top right of the stage
// around Mario, north up, with an arrow for Mario and the stage's Shines and
// blue coins. The HUD draws it (patch zzzz-radar-01); the maps are pictures
// of each stage made from the disc (tools/radar/gen_maps.py, assets/maps),
// which sms_gx loads (platform/gx/src/gx_radar_maps.cpp).

namespace JDrama {
struct TRect;
}

// SMS_RADAR (radar = on in settings.txt, platform): 1 shows the radar; off
// by default. SMS_RADAR_ZOOM: world units from its centre to its edge
// (default 2500, 25 m). SMS_RADAR_SIZE: its size, percent (default 100).
extern "C" int port_radar;

// SMS_RADAR_PLACE: how it makes room for the life meter (corner, yield,
// beside, below, bottom_left; src/radar.cpp's Place).
// With the HUD, after its own pieces; viewport is the HUD's, and life the
// life meter's place this frame in its 2D units (all 0 while it is hidden).
void RadarDraw(const JDrama::TRect& viewport, float lifeX1, float lifeY1, float lifeX2, float lifeY2);

// sms_gx's side (gx_radar_maps.cpp)
struct PortRadarMap {
	const unsigned char* rgba; // row-major RGBA, w x h
	int w, h;
	float x0, z0;  // world x and z of the picture's top left corner
	float density; // pixels per world unit
	const unsigned char* overview; // the whole picture smaller, for the full map: row-major RGBA
	int ow, oh;
};
extern "C" int port_radar_map(unsigned long long colKey, PortRadarMap* out);

#endif
