#include "gx_quit_prompt.h"
#include "gx_internal.h"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace {

// Layout units, except kCapH: stb_easy_font's capitals are 7 font pixels high.
const int kCapH = 7;
const int kPad = 8, kTitleH = 17, kBodyH = 26, kButtonH = 17, kFooterH = 18;
const int kButtonW = 60, kButtonGap = 12;
const float kSizeScale = 1.20f;  // as the cheat menu's

const char* const kTitle = "Quit the game?";
const char* const kBody = "Progress since your last save will be lost.";
const char* const kHint = "Y Quit   N/Esc/B Cancel";
const char* const kButtons[2] = {"Quit", "Cancel"};

const uint8_t kPanelBg[4] = {15, 15, 15, 240};
const uint8_t kLine[4] = {80, 80, 92, 240};
const uint8_t kTitleBg[4] = {41, 74, 122, 255};
const uint8_t kText[4] = {255, 255, 255, 255};
const uint8_t kTextDim[4] = {128, 128, 128, 255};
const uint8_t kButton[4] = {29, 47, 73, 240};
const uint8_t kButtonSelected[4] = {52, 96, 150, 255};
const uint8_t kAccent[4] = {66, 150, 250, 255};

enum { QUIT, CANCEL };

bool s_visible = false;
bool s_confirmed = false;
int s_selected = CANCEL;  // so the game's Start (Enter) pressed right after a stray Escape does not quit
bool s_heldKey[SDL_NUM_SCANCODES];
bool s_heldButton[SDL_CONTROLLER_BUTTON_MAX];

// GL thread: the bitmap last uploaded and what it shows
typedef std::array<int, 3> Shown;
std::vector<uint8_t> s_pixels;
Shown s_shown;
bool s_shownValid = false;

struct Rect {
    int x0, y0, x1, y1;
    bool contains(int x, int y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};

int buttonsTop() { return kTitleH + kBodyH; }
int footerTop() { return buttonsTop() + kButtonH + kPad; }

// The panel's rectangles, in window pixels from its top-left.
struct Layout {
    int glyph;         // window pixels per font pixel
    float unit;        // window pixels per layout unit
    int logicalWidth;  // layout units
    int width, height;
    int x, y;  // the panel's top-left in the window

    int at(float units) const { return int(std::lround(units * unit)); }
    int textY(const Rect& band) const { return band.y0 + (band.y1 - band.y0 - kCapH * glyph) / 2; }
    Rect title() const { return {0, 0, width, at(kTitleH)}; }
    Rect body() const { return {0, at(kTitleH), width, at(buttonsTop())}; }
    Rect button(int i) const {
        const float x0 = (logicalWidth - 2 * kButtonW - kButtonGap) / 2.0f + i * (kButtonW + kButtonGap);
        return {at(x0), at(buttonsTop()), at(x0 + kButtonW), at(buttonsTop() + kButtonH)};
    }
};

Layout placedAt(int glyph, int logicalWidth, int winW, int winH) {
    Layout l;
    l.glyph = glyph;
    l.unit = glyph * kSizeScale;
    l.logicalWidth = logicalWidth;
    l.width = l.at(logicalWidth);
    l.height = l.at(footerTop() + kFooterH);
    l.x = (winW - l.width) / 2;
    l.y = std::max(0, (winH - l.height) / 2);
    return l;
}

Layout layoutFor(int winW, int winH) {
    const int textW = std::max({gx::overlayTextWidth(kTitle), gx::overlayTextWidth(kBody), gx::overlayTextWidth(kHint)});
    const int logicalWidth = std::max(kPad + int(std::ceil(textW / kSizeScale)) + kPad, 2 * kButtonW + kButtonGap + 2 * kPad);
    int glyph = std::max(2, (winH + 360) / 720);  // a font pixel is about a 720th of the height
    Layout l = placedAt(glyph, logicalWidth, winW, winH);
    while (glyph > 1 && (l.x < 0 || l.height > winH)) l = placedAt(--glyph, logicalWidth, winW, winH);
    return l;
}

void rasterize(const Layout& l) {
    const int w = l.width, h = l.height, g = l.glyph;
    s_pixels.resize(size_t(w) * h * 4);
    auto fill = [&](const Rect& r, const uint8_t c[4]) { gx::overlayFillRect(s_pixels, w, h, r.x0, r.y0, r.x1, r.y1, c); };
    auto text = [&](int x, int y, const char* str, const uint8_t c[4]) { gx::overlayText(s_pixels, w, h, x, y, str, c, g); };
    auto textWidth = [&](const char* str) { return gx::overlayTextWidth(str) * g; };
    fill(Rect{0, 0, w, h}, kPanelBg);
    const Rect title = l.title();
    fill(title, kTitleBg);
    text(l.at(kPad), l.textY(title), kTitle, kText);
    text(l.at(kPad), l.textY(l.body()), kBody, kText);
    // Hairlines stay one or two window pixels thick at any scale.
    const int t = std::max(1, g / 2);
    for (int i = 0; i < 2; i++) {
        const Rect b = l.button(i);
        const bool sel = i == s_selected;
        fill(b, sel ? kButtonSelected : kButton);
        if (sel) {
            fill(Rect{b.x0, b.y0, b.x1, b.y0 + t}, kAccent);
            fill(Rect{b.x0, b.y1 - t, b.x1, b.y1}, kAccent);
            fill(Rect{b.x0, b.y0, b.x0 + t, b.y1}, kAccent);
            fill(Rect{b.x1 - t, b.y0, b.x1, b.y1}, kAccent);
        }
        text(b.x0 + (b.x1 - b.x0 - textWidth(kButtons[i])) / 2, l.textY(b), kButtons[i], sel ? kText : kTextDim);
    }
    const int footerY = l.at(footerTop());
    text(l.at(kPad), footerY + l.at(4), kHint, kTextDim);
    fill(Rect{0, footerY, w, footerY + t}, kLine);
    fill(Rect{0, 0, w, t}, kLine);
    fill(Rect{0, h - t, w, h}, kLine);
    fill(Rect{0, 0, t, h}, kLine);
    fill(Rect{w - t, 0, w, h}, kLine);
}

void close() { s_visible = false; }

void pick(int button) {
    if (button == QUIT) s_confirmed = true;
    close();
}

// False for keys that are not the prompt's.
bool menuKey(SDL_Scancode key) {
    switch (key) {
    case SDL_SCANCODE_LEFT:
    case SDL_SCANCODE_RIGHT:
    case SDL_SCANCODE_TAB: s_selected ^= 1; return true;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:
    case SDL_SCANCODE_SPACE: pick(s_selected); return true;
    case SDL_SCANCODE_Y: pick(QUIT); return true;
    case SDL_SCANCODE_N:
    case SDL_SCANCODE_ESCAPE:
    case SDL_SCANCODE_BACKSPACE: pick(CANCEL); return true;
    default: return false;
    }
}

// Every key pressed while the prompt is open is the prompt's, and so are its
// repeats and its release: Escape held after cancelling must not open the
// prompt again, nor Enter held after it press Start.
bool onKey(const SDL_KeyboardEvent& ev) {
    const SDL_Scancode key = ev.keysym.scancode;
    if (unsigned(key) >= SDL_NUM_SCANCODES) return false;
    if (ev.type == SDL_KEYUP) {
        const bool held = s_heldKey[key];
        s_heldKey[key] = false;
        return held;
    }
    if (ev.repeat) return s_heldKey[key] || s_visible;
    if (!s_visible) return false;
    menuKey(key);
    s_heldKey[key] = true;
    return true;
}

bool onButton(const SDL_ControllerButtonEvent& ev) {
    const int b = ev.button;
    if (b < 0 || b >= SDL_CONTROLLER_BUTTON_MAX) return false;
    if (ev.type == SDL_CONTROLLERBUTTONUP) {
        const bool held = s_heldButton[b];
        s_heldButton[b] = false;
        return held;
    }
    if (!s_visible) return false;
    s_heldButton[b] = true;
    switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: s_selected ^= 1; break;
    case SDL_CONTROLLER_BUTTON_A: pick(s_selected); break;
    case SDL_CONTROLLER_BUTTON_B:
    case SDL_CONTROLLER_BUTTON_START: pick(CANCEL); break;
    default: break;
    }
    return true;
}

bool onMouse(const SDL_Event& ev) {
    if (!s_visible) return false;
    const bool motion = ev.type == SDL_MOUSEMOTION;
    if (!motion && ev.type != SDL_MOUSEBUTTONDOWN) return true;
    int winW = 0, winH = 0, drawW = 0, drawH = 0;
    if (SDL_Window* window = SDL_GetWindowFromID(motion ? ev.motion.windowID : ev.button.windowID)) {
        SDL_GetWindowSize(window, &winW, &winH);
        SDL_GL_GetDrawableSize(window, &drawW, &drawH);
    }
    if (winW <= 0 || winH <= 0) return true;
    // Events are in window coordinates, the panel in drawable pixels: they differ at high DPI.
    const Layout l = layoutFor(drawW, drawH);
    const int x = (motion ? ev.motion.x : ev.button.x) * drawW / winW - l.x;
    const int y = (motion ? ev.motion.y : ev.button.y) * drawH / winH - l.y;
    for (int i = 0; i < 2; i++) {
        if (!l.button(i).contains(x, y)) continue;
        s_selected = i;
        if (!motion && ev.button.button == SDL_BUTTON_LEFT) pick(i);
    }
    return true;
}

}  // namespace

namespace gx {

bool quitPromptVisible() { return s_visible; }

void quitPromptOpen() {
    s_visible = true;
    s_selected = CANCEL;
}

bool quitPromptHandleEvent(const SDL_Event& ev) {
    switch (ev.type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        return onKey(ev.key);
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
        return onButton(ev.cbutton);
    case SDL_MOUSEMOTION:
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEWHEEL:
        return onMouse(ev);
    default:
        return false;
    }
}

bool quitPromptConfirmed() { return s_confirmed; }

void quitPromptRender(int winW, int winH) {
    if (!s_visible || winW <= 0 || winH <= 0) {
        s_shownValid = false;
        return;
    }
    const Layout l = layoutFor(winW, winH);
    const Shown state{winW, winH, s_selected};
    const bool changed = !s_shownValid || state != s_shown;
    if (changed) {
        rasterize(l);
        s_shown = state;
        s_shownValid = true;
    }
    drawOverlayPanel(OVERLAY_QUIT_PROMPT, changed ? s_pixels.data() : nullptr, l.width, l.height, l.x, l.y, 1, winW, winH);
}

}  // namespace gx
