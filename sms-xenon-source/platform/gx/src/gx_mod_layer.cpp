// Native mods' drawing (sms_gx/gx_pc.h, GXPC_ModLayerSubmit): the triangles
// the mods built for a frame (ext/modapi), mapped from canvas units onto the
// presented picture and blended over it at the window's resolution, before
// the port's own menus. The game thread submits a frame's list; the GL thread
// draws the newest on each present. Images are uploaded as they are first
// drawn and freed when the mod lets them go.
#include "gx_internal.h"
#include "gl_funcs.h"
#include "gx_glcache.h"
#include "sms_gx/gx_pc.h"

#include <stdio.h>
#include <string.h>

#include <atomic>
#include <map>
#include <mutex>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "third_party/stb_image.h"

#ifdef SMS_GX_HAVE_SDL2
#include <SDL.h>
#endif

namespace {

struct Frame {
    std::vector<GXPCModVertex> vertices;
    std::vector<GXPCModBatch> batches;
    float canvasW = 640, canvasH = 480;
};

struct Image {
    std::vector<uint8_t> pixels;  // until uploaded
    int w = 0, h = 0;
    unsigned tex = 0;
};

std::mutex s_lock;
Frame s_pending;     // the newest submission (game thread → GL thread)
bool s_newFrame = false;
Frame s_drawn;       // what the GL thread draws (its own)
std::map<uint32_t, Image> s_images;
std::vector<unsigned> s_deadTextures;  // freed images' textures, for the GL thread
uint32_t s_nextImage = 1;

// The picture's aspect, from the last present (the canvas width for the next frame).
std::atomic<float> s_pictureAspect(0.0f);

// GL objects (GL thread)
unsigned s_prog = 0, s_vao = 0, s_vbo = 0, s_white = 0;
int s_uWindow = -1, s_uTex = -1;
size_t s_vboBytes = 0;

const char* kVs = R"(#version 330 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;
uniform vec2 u_window;
out vec2 v_uv;
out vec4 v_color;
void main() {
    v_uv = a_uv;
    v_color = a_color;
    gl_Position = vec4(a_pos.x / u_window.x * 2.0 - 1.0, 1.0 - a_pos.y / u_window.y * 2.0, 0.0, 1.0);
}
)";
const char* kFs = R"(#version 330 core
uniform sampler2D u_tex;
in vec2 v_uv;
in vec4 v_color;
out vec4 o_color;
void main() { o_color = v_color * texture(u_tex, v_uv); }
)";

uint32_t addImage(std::vector<uint8_t>&& px, int w, int h) {
    std::lock_guard<std::mutex> hold(s_lock);
    const uint32_t id = s_nextImage++;
    Image& im = s_images[id];
    im.pixels = std::move(px);
    im.w = w;
    im.h = h;
    return id;
}

// The image's texture, uploaded now if it has not been (GL thread, s_lock held).
unsigned textureFor(uint32_t id) {
    auto it = s_images.find(id);
    if (it == s_images.end()) return s_white;
    Image& im = it->second;
    if (!im.tex && !im.pixels.empty()) {
        glGenTextures(1, &im.tex);
        glBindTexture(GL_TEXTURE_2D, im.tex);
        // mipmapped: images (and the mods' font) are often drawn smaller than they are
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, im.w, im.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, im.pixels.data());
        glGenerateMipmap(GL_TEXTURE_2D);
        std::vector<uint8_t>().swap(im.pixels);
    }
    return im.tex ? im.tex : s_white;
}

void initGl() {
    s_prog = gx::compileInternalProgram(kVs, kFs);
    s_uWindow = glGetUniformLocation(s_prog, "u_window");
    s_uTex = glGetUniformLocation(s_prog, "u_tex");
    glGenVertexArrays(1, &s_vao);
    glGenBuffers(1, &s_vbo);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    const GLsizei st = sizeof(GXPCModVertex);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, st, (const void*)offsetof(GXPCModVertex, x));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, st, (const void*)offsetof(GXPCModVertex, u));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, st, (const void*)offsetof(GXPCModVertex, rgba));
    const uint8_t white[4] = {255, 255, 255, 255};
    glGenTextures(1, &s_white);
    glBindTexture(GL_TEXTURE_2D, s_white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
}

// The GL state the layer changes, put back after it (as drawOverlayPanel does).
struct SavedState {
    GLint activeUnit = 0, boundTex = 0, rowLength = 0, unpackAlign = 4, program = 0, vao = 0, arrayBuf = 0, drawFbo = 0;
    GLint viewport[4] = {}, scissorBox[4] = {}, colorMask[4] = {};
    GLint blend = 0, depth = 0, stencil = 0, cull = 0, scissor = 0, logic = 0, clip0 = 0, clip1 = 0;
    GLint srcRGB = 0, dstRGB = 0, srcAlpha = 0, dstAlpha = 0, eqRGB = 0, eqAlpha = 0;
    void save() {
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeUnit);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &boundTex);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackAlign);
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuf);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_SCISSOR_BOX, scissorBox);
        glGetIntegerv(GL_COLOR_WRITEMASK, colorMask);
        glGetIntegerv(GL_BLEND, &blend);
        glGetIntegerv(GL_DEPTH_TEST, &depth);
        glGetIntegerv(GL_STENCIL_TEST, &stencil);
        glGetIntegerv(GL_CULL_FACE, &cull);
        glGetIntegerv(GL_SCISSOR_TEST, &scissor);
        glGetIntegerv(GL_COLOR_LOGIC_OP, &logic);
        glGetIntegerv(GL_CLIP_DISTANCE0, &clip0);
        glGetIntegerv(GL_CLIP_DISTANCE1, &clip1);
        glGetIntegerv(GL_BLEND_SRC_RGB, &srcRGB);
        glGetIntegerv(GL_BLEND_DST_RGB, &dstRGB);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcAlpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &dstAlpha);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &eqRGB);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &eqAlpha);
    }
    void restore() {
        glBindTexture(GL_TEXTURE_2D, GLuint(boundTex));
        glActiveTexture(GLenum(activeUnit));
        glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
        glPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlign);
        glBindVertexArray(GLuint(vao));
        glBindBuffer(GL_ARRAY_BUFFER, GLuint(arrayBuf));
        glUseProgram(GLuint(program));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(drawFbo));
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glScissor(scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]);
        glBlendFuncSeparate(GLenum(srcRGB), GLenum(dstRGB), GLenum(srcAlpha), GLenum(dstAlpha));
        glBlendEquationSeparate(GLenum(eqRGB), GLenum(eqAlpha));
        glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
        if (!blend) glDisable(GL_BLEND);
        if (depth) glEnable(GL_DEPTH_TEST);
        if (stencil) glEnable(GL_STENCIL_TEST);
        if (cull) glEnable(GL_CULL_FACE);
        if (scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
        if (logic) glEnable(GL_COLOR_LOGIC_OP);
        if (clip0) glEnable(GL_CLIP_DISTANCE0);
        if (clip1) glEnable(GL_CLIP_DISTANCE1);
    }
};

}  // namespace

extern "C" void GXPC_ModLayerSubmit(const GXPCModVertex* v, int nv, const GXPCModBatch* b, int nb, float canvasW,
                                    float canvasH) {
    std::lock_guard<std::mutex> hold(s_lock);
    s_pending.vertices.assign(v, v + (v && nv > 0 ? nv : 0));
    s_pending.batches.assign(b, b + (b && nb > 0 ? nb : 0));
    s_pending.canvasW = canvasW > 0 ? canvasW : 640;
    s_pending.canvasH = canvasH > 0 ? canvasH : 480;
    s_newFrame = true;
}

extern "C" void GXPC_ModLayerCanvas(float* w, float* h) {
    float aspect = s_pictureAspect.load();
    if (aspect <= 0) aspect = 4.0f / 3.0f * GXPC_GetWidescreen();
    if (w) *w = 480.0f * aspect;
    if (h) *h = 480.0f;
}

extern "C" uint32_t GXPC_ModImageCreate(const uint8_t* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0 || w > 8192 || h > 8192) return 0;
    return addImage(std::vector<uint8_t>(rgba, rgba + size_t(w) * h * 4), w, h);
}

extern "C" uint32_t GXPC_ModImageLoad(const char* path, int* w, int* h) {
    int iw = 0, ih = 0, n = 0;
    stbi_uc* px = path ? stbi_load(path, &iw, &ih, &n, 4) : nullptr;
    if (!px) return 0;
    std::vector<uint8_t> copy(px, px + size_t(iw) * ih * 4);
    stbi_image_free(px);
    if (w) *w = iw;
    if (h) *h = ih;
    return addImage(std::move(copy), iw, ih);
}

extern "C" void GXPC_ModImageFree(uint32_t image) {
    std::lock_guard<std::mutex> hold(s_lock);
    auto it = s_images.find(image);
    if (it == s_images.end()) return;
    if (it->second.tex) s_deadTextures.push_back(it->second.tex);
    s_images.erase(it);
}

extern "C" int GXPC_KeyFromName(const char* name) {
#ifdef SMS_GX_HAVE_SDL2
    if (!name || !*name) return -1;
    const SDL_Scancode sc = SDL_GetScancodeFromName(name);
    return sc == SDL_SCANCODE_UNKNOWN ? -1 : int(sc);
#else
    (void)name;
    return -1;
#endif
}

extern "C" const char* GXPC_KeyName(int scancode) {
#ifdef SMS_GX_HAVE_SDL2
    if (scancode <= 0 || scancode >= SDL_NUM_SCANCODES) return "";
    return SDL_GetScancodeName(SDL_Scancode(scancode));
#else
    (void)scancode;
    return "";
#endif
}

void gx::modLayerRender(int winW, int winH) {
    const PresentRect r = lastPresentRect();
    if (r.w > 0 && r.h > 0) s_pictureAspect.store(float(r.w) / float(r.h));
    std::lock_guard<std::mutex> hold(s_lock);
    if (!s_deadTextures.empty()) {
        glDeleteTextures(GLsizei(s_deadTextures.size()), s_deadTextures.data());
        s_deadTextures.clear();
    }
    if (s_newFrame) {
        std::swap(s_drawn, s_pending);
        s_newFrame = false;
    }
    if (s_drawn.batches.empty() || s_drawn.vertices.empty() || winW <= 0 || winH <= 0 || r.w <= 0 || r.h <= 0) return;
    flushBatch();
    glcInvalidate();
    SavedState saved;
    saved.save();
    if (!s_prog) initGl();

    // canvas units → window pixels (from the top left)
    const float sx = float(r.w) / s_drawn.canvasW, sy = float(r.h) / s_drawn.canvasH;
    std::vector<GXPCModVertex> px(s_drawn.vertices);
    for (GXPCModVertex& v : px) {
        v.x = float(r.x) + v.x * sx;
        v.y = float(r.y) + v.y * sy;
    }
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    const size_t bytes = px.size() * sizeof(GXPCModVertex);
    if (bytes > s_vboBytes) {
        s_vboBytes = bytes + bytes / 2;
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(s_vboBytes), nullptr, GL_STREAM_DRAW);
    }
    glBufferSubData(GL_ARRAY_BUFFER, 0, GLsizeiptr(bytes), px.data());

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_COLOR_LOGIC_OP);
    glDisable(GL_CLIP_DISTANCE0);
    glDisable(GL_CLIP_DISTANCE1);
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_presentFbo);
    glViewport(0, 0, winW, winH);
    glUseProgram(s_prog);
    glUniform2f(s_uWindow, float(winW), float(winH));
    glUniform1i(s_uTex, 0);
    glActiveTexture(GL_TEXTURE0);
    for (const GXPCModBatch& b : s_drawn.batches) {
        if (!b.count || b.first + b.count > px.size()) continue;
        if (b.clip[2] >= 0) {
            // window pixels, GL's rows from the bottom
            const int x0 = int(float(r.x) + b.clip[0] * sx), y0 = int(float(r.y) + b.clip[1] * sy);
            const int x1 = int(float(r.x) + (b.clip[0] + b.clip[2]) * sx + 0.999f);
            const int y1 = int(float(r.y) + (b.clip[1] + b.clip[3]) * sy + 0.999f);
            if (x1 <= x0 || y1 <= y0) continue;
            glEnable(GL_SCISSOR_TEST);
            glScissor(x0, winH - y1, x1 - x0, y1 - y0);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
        glBindTexture(GL_TEXTURE_2D, b.image ? textureFor(b.image) : s_white);
        glDrawArrays(GL_TRIANGLES, GLint(b.first), GLsizei(b.count));
    }
    saved.restore();
    glcInvalidate();
}
