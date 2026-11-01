// Auto save's indicator (ext/autosave, gx_autosave_overlay.h): at the bottom
// left of the game's picture, a Shine that turns while the game saves, then
// faces you beside "Saved" and fades out. Drawn with the debug overlay's
// renderer; the Shine is drawn here, made again each frame it turns.
#include "gx_autosave_overlay.h"
#include "gx_internal.h"

#include "sms_gx/gx_pc.h"

#include <math.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace {

const int kCapH = 7;  // stb_easy_font's capitals are 7 font pixels high
const uint8_t kPanelBg[4] = {15, 15, 15, 220};
const uint8_t kText[4] = {255, 255, 255, 255};
const double kFadeIn = 0.15, kSavedFor = 1.6, kFailedFor = 4.0, kFadeOut = 0.4;
const double kTurn = 1.0;  // seconds a turn takes
const float kPi = 3.14159265f;

std::mutex s_lock;
int s_state = GXPC_AUTOSAVE_OFF;
double s_since;  // when s_state was set

std::vector<uint8_t> s_pixels;

double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool inside(const float* px, const float* py, int n, float x, float y) {
    bool in = false;
    for (int i = 0, j = n - 1; i < n; j = i++)
        if ((py[i] > y) != (py[j] > y) && x < (px[j] - px[i]) * (y - py[i]) / (py[j] - py[i]) + px[i]) in = !in;
    return in;
}

// The Shine, `size` pixels across, its top left at (x0, y0) in the panel;
// `turn` is its width as it turns (1 facing you, below 0 its back).
void drawShine(std::vector<uint8_t>& px, int w, int x0, int y0, int size, float turn) {
    const int kRays = 8, kSub = 4;
    float ox[2 * kRays], oy[2 * kRays];  // the outline
    float fx[2 * kRays], fy[2 * kRays];  // the gold inside it
    const float sx = std::max(fabsf(turn), 0.12f);
    for (int i = 0; i < 2 * kRays; i++) {
        const float a = kPi * i / kRays - kPi / 2;
        const float r = (i & 1) ? 0.62f : 1.0f;
        ox[i] = cosf(a) * r * sx;
        oy[i] = sinf(a) * r;
        fx[i] = ox[i] * 0.84f;
        fy[i] = oy[i] * 0.84f;
    }
    const bool face = turn > 0.2f;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            float sum[4] = {0, 0, 0, 0};
            for (int s = 0; s < kSub * kSub; s++) {
                const float ux = ((x + (s % kSub + 0.5f) / kSub) / size) * 2 - 1;
                const float uy = ((y + (s / kSub + 0.5f) / kSub) / size) * 2 - 1;
                float c[4];
                if (face && (powf((fabsf(ux) - 0.2f * sx) / (0.075f * sx), 2) + powf((uy + 0.08f) / 0.17f, 2) <= 1)) {
                    c[0] = 20, c[1] = 12, c[2] = 0, c[3] = 255;  // an eye
                } else if (inside(fx, fy, 2 * kRays, ux, uy)) {
                    // gold, lighter toward the middle and on the side that faces you
                    const float d = std::min(1.0f, sqrtf(ux * ux / (sx * sx) + uy * uy));
                    const float shade = turn < 0 ? 0.8f : 1.0f;
                    c[0] = 255 * shade, c[1] = (236 - 70 * d) * shade, c[2] = (120 - 110 * d) * shade, c[3] = 255;
                } else if (inside(ox, oy, 2 * kRays, ux, uy)) {
                    c[0] = 110, c[1] = 60, c[2] = 0, c[3] = 255;
                } else {
                    continue;
                }
                for (int k = 0; k < 4; k++) sum[k] += c[k] * (k == 3 ? 1 : c[3] / 255.0f);
            }
            const float a = sum[3] / (kSub * kSub);
            if (a <= 0) continue;
            uint8_t* o = &px[(size_t(y0 + y) * w + x0 + x) * 4];
            const float f = a / 255.0f;
            for (int k = 0; k < 3; k++) o[k] = uint8_t(std::min(255.0f, sum[k] / (kSub * kSub) + o[k] * (1 - f)));
            o[3] = uint8_t(std::min(255.0f, a + o[3] * (1 - f)));
        }
}

}  // namespace

extern "C" void GXPC_AutoSaveIndicator(int state) {
    std::lock_guard<std::mutex> lock(s_lock);
    if (state == s_state && state == GXPC_AUTOSAVE_SAVING) return;  // keeps turning
    s_state = state;
    s_since = now();
}

namespace gx {

void autosaveOverlayRender(int winW, int winH) {
    int state;
    double since;
    {
        std::lock_guard<std::mutex> lock(s_lock);
        state = s_state;
        since = s_since;
    }
    if (state == GXPC_AUTOSAVE_OFF || winW <= 0 || winH <= 0) return;
    const double t = now() - since;
    float alpha = 1;
    float turn = 1;
    const char* text = "Saved";
    if (state == GXPC_AUTOSAVE_SAVING) {
        alpha = float(std::min(1.0, t / kFadeIn));
        turn = cosf(float(2 * kPi * t / kTurn));
        text = "Saving...";
    } else {
        const double hold = state == GXPC_AUTOSAVE_FAILED ? kFailedFor : kSavedFor;
        if (t >= hold + kFadeOut) {
            std::lock_guard<std::mutex> lock(s_lock);
            if (s_state == state && s_since == since) s_state = GXPC_AUTOSAVE_OFF;
            return;
        }
        if (t > hold) alpha = float(1 - (t - hold) / kFadeOut);
        if (state == GXPC_AUTOSAVE_FAILED) text = "Couldn't save: the game will ask next time";
    }

    // sized as the save state messages are
    const int glyph = std::max(2, (winH + 360) / 720);
    const int icon = 13 * glyph, pad = 4 * glyph, gap = 3 * glyph;
    const int w = pad + icon + gap + overlayTextWidth(text) * glyph + 2 * pad;
    const int h = icon + 2 * pad;
    s_pixels.assign(size_t(w) * h * 4, 0);
    // a panel with round corners
    const float radius = 3.0f * glyph;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const float dx = std::max(0.0f, std::max(radius - (x + 0.5f), (x + 0.5f) - (w - radius)));
            const float dy = std::max(0.0f, std::max(radius - (y + 0.5f), (y + 0.5f) - (h - radius)));
            const float cover = std::min(1.0f, std::max(0.0f, radius + 0.5f - sqrtf(dx * dx + dy * dy)));
            uint8_t* o = &s_pixels[(size_t(y) * w + x) * 4];
            memcpy(o, kPanelBg, 3);
            o[3] = uint8_t(kPanelBg[3] * cover);
        }
    drawShine(s_pixels, w, pad, pad, icon, turn);
    overlayText(s_pixels, w, h, pad + icon + gap, (h - kCapH * glyph) / 2, text, kText, glyph);
    if (alpha < 1)
        for (size_t i = 3; i < s_pixels.size(); i += 4) s_pixels[i] = uint8_t(s_pixels[i] * alpha);

    // the bottom left of the game's picture, clear of the HUD's corners
    const PresentRect pic = lastPresentRect();
    const bool havePic = pic.w > 0 && pic.h > 0;
    const int left = havePic ? pic.x : 0, bottom = havePic ? pic.y + pic.h : winH;
    const int margin = (havePic ? pic.h : winH) / 24;
    drawOverlayPanel(OVERLAY_AUTOSAVE, s_pixels.data(), w, h, left + margin, bottom - h - margin, 1, winW, winH);
}

}  // namespace gx
