// hexagon3d_ui.cpp - Hexagon3D: a Prisma-3D-style mobile 3D animation + modeling app (SDL2, software-drawn UI, no extra libs)
//
// Layout (portrait + landscape): full-screen viewport | top bar (home, share, scene list) | timeline with a fixed centre playhead |
// bottom bar (Models / Sky / Camera / Light / Media / Text, or the object tools when something is selected) | bottom sheets.
// Animation: tap a model, tap "+" on its track to add a clip, tap the diamond to add keyframes, tap the curve icon for the graph
//   editor (cubic-bezier easing presets + draggable handles).  Modeling: object toolbar -> "Model" (face / edge / vertex editing,
//   smoothing angle, Catmull-Clark subdivisions).
// Touch: drag viewport = orbit, pinch = zoom, two-finger slide = pan, drag timeline = scrub, long-press a clip/key then drag = move it.
//
// Single-file ("unity") build for Cxxdroid: keep ALL these files in the same folder and compile only hexagon3d_ui.cpp:
//   engine_bridge.h  rig_engine.cpp  mdl_parser.cpp  advanced_timeline_engine.cpp  sfm_renderer_1.cpp  mesh_export.cpp
//   model_import.cpp  ui_font.h
// (presto_ui.cpp is the old layout and is not needed any more.)

#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#include <SDL.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>

#include "ui_font.h"

#define MAGNIFICENT_BUILD 1
#include "engine_bridge.h"
#include "rig_engine.cpp"
namespace mdl { using namespace std;
#include "mdl_parser.cpp"
}
namespace tl  { using namespace std;
#include "advanced_timeline_engine.cpp"
}
namespace sfm { using namespace std;
#include "sfm_renderer_1.cpp"
}
namespace mx  { using namespace std;
#include "mesh_export.cpp"
#include "model_import.cpp"
}

// ================================================================ background jobs + status toast
static std::atomic<bool> g_busy{false};
static std::mutex g_statusMx;
static std::string g_status = "";
static Uint32 g_statusAt = 0;
static void setStatus(const std::string& s) { std::lock_guard<std::mutex> l(g_statusMx); g_status = s; g_statusAt = SDL_GetTicks(); }
static std::string getStatus(Uint32* at = nullptr) { std::lock_guard<std::mutex> l(g_statusMx); if (at) *at = g_statusAt; return g_status; }
static void runJob(const std::string& name, std::function<void()> fn) {
  bool expected = false;
  if (!g_busy.compare_exchange_strong(expected, true)) { setStatus("Busy - wait for the current job"); return; }
  setStatus(name + " ...");
  std::thread([fn]() { fn(); g_busy = false; }).detach();
}

// ================================================================ palette (sampled from the Prisma 3D screenshots)
static const SDL_Color C_VP    = {52, 52, 67, 255};     // viewport
static const SDL_Color C_DARK  = {22, 22, 23, 255};     // timeline track + tab bar
static const SDL_Color C_BAR   = {31, 31, 31, 255};     // object toolbar / side toolbar
static const SDL_Color C_BTN   = {43, 43, 43, 255};     // back button / small buttons
static const SDL_Color C_PANEL = {28, 28, 28, 255};     // floating panels / sheets
static const SDL_Color C_FIELD = {40, 40, 41, 255};     // value fields
static const SDL_Color C_TXT   = {226, 226, 228, 255};
static const SDL_Color C_DIM   = {150, 150, 154, 255};
static const SDL_Color C_OFF   = {84, 84, 88, 255};
static const SDL_Color C_WHITE = {255, 255, 255, 255};
static const SDL_Color C_BLUE  = {76, 151, 196, 255};   // "+" button
static const SDL_Color C_CLIP  = {27, 71, 119, 255};    // animation clip
static const SDL_Color C_TICK  = {47, 85, 128, 255};
static const SDL_Color C_KEYD  = {96, 127, 160, 255};   // dim keyframe
static const SDL_Color C_ACC   = {64, 170, 235, 255};   // diamond / slider accent
static const SDL_Color C_ORG   = {255, 168, 56, 255};   // modeling selection
static const SDL_Color C_AXR   = {206, 46, 46, 255}, C_AXG = {64, 204, 64, 255}, C_AXB = {36, 64, 232, 255};

// ================================================================ draw layer: scale, shapes, smooth text
static SDL_Renderer* R = nullptr;
static float U = 1.f;                                   // layout scale: 1.0 == a 720-wide portrait phone
static int P(float v) { return (int)lroundf(v * U); }
static void setc(SDL_Color c) { SDL_SetRenderDrawColor(R, c.r, c.g, c.b, c.a); }
static void fillR(SDL_Rect r, SDL_Color c) { if (r.w <= 0 || r.h <= 0) return; setc(c); SDL_RenderFillRect(R, &r); }
static void frameR(SDL_Rect r, SDL_Color c) { setc(c); SDL_RenderDrawRect(R, &r); }
static SDL_Color alpha(SDL_Color c, int a) { c.a = (Uint8)std::max(0, std::min(255, a)); return c; }
static bool inside(SDL_Rect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

static SDL_Texture* texFromAlpha(const std::vector<uint8_t>& a, int w, int h) {
  std::vector<uint8_t> px((size_t)w * h * 4);
  for (size_t i = 0; i < (size_t)w * h; i++) { px[i * 4] = 255; px[i * 4 + 1] = 255; px[i * 4 + 2] = 255; px[i * 4 + 3] = a[i]; }
  SDL_Texture* t = SDL_CreateTexture(R, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
  if (t) { SDL_UpdateTexture(t, nullptr, px.data(), w * 4); SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND); }
  return t;
}
struct GTex { SDL_Texture* t = nullptr; int w = 0, h = 0, ox = 0, oy = 0; float adv = 0; };
static std::map<uint32_t, GTex> g_glyphs;
static const GTex& glyphTex(int ch, int px) {
  if (ch < 32 || ch > 126) ch = '?';
  uint32_t key = ((uint32_t)px << 8) | (uint32_t)ch;
  std::map<uint32_t, GTex>::iterator it = g_glyphs.find(key);
  if (it != g_glyphs.end()) return it->second;
  GTex g; uif::Bmp b;
  if (uif::rasterGlyph(ch, px, b)) { g.adv = b.adv; g.w = b.w; g.h = b.h; g.ox = b.ox; g.oy = b.oy; if (b.w > 0 && b.h > 0) g.t = texFromAlpha(b.a, b.w, b.h); }
  return g_glyphs[key] = g;
}
static int textW(const std::string& s, int px) { float w = 0; for (size_t i = 0; i < s.size(); i++) w += glyphTex((unsigned char)s[i], px).adv; return (int)ceilf(w); }
// (x, cy): left edge and the vertical centre of the capital letters
static void txt(int x, int cy, const std::string& s, SDL_Color c, int px) {
  int base = cy + (int)lroundf(uif::kCap * px * 0.5f); float pen = (float)x;
  for (size_t i = 0; i < s.size(); i++) {
    const GTex& g = glyphTex((unsigned char)s[i], px);
    if (g.t) { SDL_Rect d = {(int)lroundf(pen) + g.ox, base + g.oy, g.w, g.h}; SDL_SetTextureColorMod(g.t, c.r, c.g, c.b); SDL_SetTextureAlphaMod(g.t, c.a); SDL_RenderCopy(R, g.t, nullptr, &d); }
    pen += g.adv;
  }
}
static void txtC(SDL_Rect r, const std::string& s, SDL_Color c, int px) { txt(r.x + (r.w - textW(s, px)) / 2, r.y + r.h / 2, s, c, px); }
static void txtR(int xr, int cy, const std::string& s, SDL_Color c, int px) { txt(xr - textW(s, px), cy, s, c, px); }
static std::string fitTxt(const std::string& s, int maxW, int px) {
  if (textW(s, px) <= maxW) return s;
  float w = 0, dots = glyphTex('.', px).adv * 2; size_t n = 0;
  for (; n < s.size(); n++) { float a = glyphTex((unsigned char)s[n], px).adv; if (w + a + dots > maxW) break; w += a; }
  return s.substr(0, std::max((size_t)1, n)) + "..";
}
static int FPX(float designPx) { return std::max(9, P(designPx)); }   // font size from a design-pixel size

// ---- rounded rectangles (quarter-circle textures, cached per radius)
static void orient(uif::Polys& Pl) {
  for (size_t c = 0; c < Pl.size(); c++) {
    double a = 0;
    for (size_t i = 0; i < Pl[c].size(); i++) { const uif::Pt& u = Pl[c][i]; const uif::Pt& v = Pl[c][(i + 1) % Pl[c].size()]; a += (double)u.x * v.y - (double)v.x * u.y; }
    if (a < 0) std::reverse(Pl[c].begin(), Pl[c].end());
  }
}
static std::map<uint32_t, SDL_Texture*> g_shapeTex;
static SDL_Texture* quarterTex(int rad) {
  uint32_t key = 0xFFFF0000u | (uint32_t)rad;
  std::map<uint32_t, SDL_Texture*>::iterator it = g_shapeTex.find(key);
  if (it != g_shapeTex.end()) return it->second;
  uif::Polys Pl(1); uif::Pt c0 = {(float)rad, (float)rad}; Pl[0].push_back(c0);
  for (int i = 0; i <= 16; i++) { float a = 3.14159265f * (1.f + 0.5f * i / 16.f); uif::Pt q = {rad + rad * cosf(a), rad + rad * sinf(a)}; Pl[0].push_back(q); }
  orient(Pl);
  std::vector<uint8_t> a; uif::rasterPolys(Pl, rad, rad, a, 1.0f);
  return g_shapeTex[key] = texFromAlpha(a, rad, rad);
}
static void fillRound(SDL_Rect r, int rad, SDL_Color c) {
  rad = std::min(rad, std::min(r.w, r.h) / 2);
  if (rad <= 1) { fillR(r, c); return; }
  fillR({r.x + rad, r.y, r.w - 2 * rad, r.h}, c);
  fillR({r.x, r.y + rad, rad, r.h - 2 * rad}, c);
  fillR({r.x + r.w - rad, r.y + rad, rad, r.h - 2 * rad}, c);
  SDL_Texture* t = quarterTex(rad); if (!t) return;
  SDL_SetTextureColorMod(t, c.r, c.g, c.b); SDL_SetTextureAlphaMod(t, c.a);
  SDL_Rect a = {r.x, r.y, rad, rad}, b = {r.x + r.w - rad, r.y, rad, rad}, d = {r.x, r.y + r.h - rad, rad, rad}, e = {r.x + r.w - rad, r.y + r.h - rad, rad, rad};
  SDL_RenderCopyEx(R, t, nullptr, &a, 0, nullptr, SDL_FLIP_NONE);
  SDL_RenderCopyEx(R, t, nullptr, &b, 0, nullptr, SDL_FLIP_HORIZONTAL);
  SDL_RenderCopyEx(R, t, nullptr, &d, 0, nullptr, SDL_FLIP_VERTICAL);
  SDL_RenderCopyEx(R, t, nullptr, &e, 0, nullptr, (SDL_RendererFlip)(SDL_FLIP_HORIZONTAL | SDL_FLIP_VERTICAL));
}
static void fillRoundTop(SDL_Rect r, int rad, SDL_Color c) {   // only the top corners rounded (bottom sheets)
  fillRound({r.x, r.y, r.w, std::min(r.h, rad * 3)}, rad, c);
  if (r.h > rad * 3) fillR({r.x, r.y + rad * 2, r.w, r.h - rad * 2}, c);
}
static void fadeRight(SDL_Rect r, SDL_Color bg) {   // soft fade at the right edge of scrolling toolbars
  for (int i = 0; i < r.w; i++) { int a = (int)(255.f * i / std::max(1, r.w - 1)); a = a * a / 255; fillR({r.x + i, r.y, 1, r.h}, alpha(bg, a)); }
}
static void discFill(int cx, int cy, int r, SDL_Color c) {
  static std::map<int, SDL_Texture*> cache; SDL_Texture*& t = cache[r];
  int d = std::max(2, r * 2 + 2);
  if (!t) { uif::Polys Pl(1); for (int i = 0; i < 48; i++) { float a = 6.2831853f * i / 48; uif::Pt q = {d / 2.f + r * cosf(a), d / 2.f + r * sinf(a)}; Pl[0].push_back(q); }
            orient(Pl); std::vector<uint8_t> a; uif::rasterPolys(Pl, d, d, a, 1.0f); t = texFromAlpha(a, d, d); }
  if (!t) return;
  SDL_Rect dr = {cx - d / 2, cy - d / 2, d, d}; SDL_SetTextureColorMod(t, c.r, c.g, c.b); SDL_SetTextureAlphaMod(t, c.a); SDL_RenderCopy(R, t, nullptr, &dr);
}
// anti-aliased thick line (screen space) as a rotated quad via the polygon rasterizer, drawn into a small texture
static void thickLine(int x0, int y0, int x1, int y1, float w, SDL_Color c) {
  float dx = (float)(x1 - x0), dy = (float)(y1 - y0), l = sqrtf(dx * dx + dy * dy);
  if (w <= 1.2f || l < 1) { setc(c); SDL_RenderDrawLine(R, x0, y0, x1, y1); return; }
  if (w <= 4.6f) {   // cheap path: a few parallel hairlines (no texture allocation)
    int k = (int)ceilf(w); float nx = -dy / l, ny = dx / l; setc(c);
    for (int i = 0; i < k; i++) { float o = i - (k - 1) * 0.5f; SDL_RenderDrawLine(R, x0 + (int)lroundf(nx * o), y0 + (int)lroundf(ny * o), x1 + (int)lroundf(nx * o), y1 + (int)lroundf(ny * o)); }
    return;
  }
  int bx = std::min(x0, x1) - (int)w - 1, by = std::min(y0, y1) - (int)w - 1, bw = std::abs(x1 - x0) + (int)(2 * w) + 3, bh = std::abs(y1 - y0) + (int)(2 * w) + 3;
  if (bw > 4000 || bh > 4000) { setc(c); SDL_RenderDrawLine(R, x0, y0, x1, y1); return; }
  float nx = -dy / l * w / 2, ny = dx / l * w / 2;
  uif::Polys Pl(1); float px[4] = {x0 + nx - bx, x1 + nx - bx, x1 - nx - bx, x0 - nx - bx}, py[4] = {y0 + ny - by, y1 + ny - by, y1 - ny - by, y0 - ny - by};
  for (int i = 0; i < 4; i++) { uif::Pt q = {px[i], py[i]}; Pl[0].push_back(q); }
  orient(Pl); std::vector<uint8_t> a; uif::rasterPolys(Pl, bw, bh, a, 1.0f);
  SDL_Texture* t = texFromAlpha(a, bw, bh); if (!t) return;
  SDL_Rect d = {bx, by, bw, bh}; SDL_SetTextureColorMod(t, c.r, c.g, c.b); SDL_SetTextureAlphaMod(t, c.a); SDL_RenderCopy(R, t, nullptr, &d); SDL_DestroyTexture(t);
}

// ================================================================ icon library (outline icons, rasterized from strokes, cached per size)
typedef uif::Polys PP; typedef uif::Pt Pt;
static const float kPI = 3.14159265f;
static void pDisc(PP& Pl, float cx, float cy, float r, int n = 18) { std::vector<Pt> c; for (int i = 0; i < n; i++) { float a = 2 * kPI * i / n; Pt q = {cx + r * cosf(a), cy + r * sinf(a)}; c.push_back(q); } Pl.push_back(c); }
static void pSeg(PP& Pl, float x0, float y0, float x1, float y1, float w) {
  float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
  if (l > 1e-6f) { float nx = -dy / l * w / 2, ny = dx / l * w / 2; std::vector<Pt> r; Pt a = {x0 + nx, y0 + ny}, b = {x1 + nx, y1 + ny}, c = {x1 - nx, y1 - ny}, d = {x0 - nx, y0 - ny}; r.push_back(a); r.push_back(b); r.push_back(c); r.push_back(d); Pl.push_back(r); }
  pDisc(Pl, x0, y0, w / 2, 10); pDisc(Pl, x1, y1, w / 2, 10);
}
static void pPath(PP& Pl, const std::vector<Pt>& p, float w, bool closed = false) {
  for (size_t i = 0; i + 1 < p.size(); i++) pSeg(Pl, p[i].x, p[i].y, p[i + 1].x, p[i + 1].y, w);
  if (closed && p.size() > 2) pSeg(Pl, p.back().x, p.back().y, p[0].x, p[0].y, w);
}
static std::vector<Pt> arcPts(float cx, float cy, float rx, float ry, float a0, float a1, int n = 20) { std::vector<Pt> p; for (int i = 0; i <= n; i++) { float a = a0 + (a1 - a0) * i / n; Pt q = {cx + rx * cosf(a), cy + ry * sinf(a)}; p.push_back(q); } return p; }
static void pRing(PP& Pl, float cx, float cy, float r, float w) { pPath(Pl, arcPts(cx, cy, r, r, 0, 2 * kPI, 28), w); }
static void pFill(PP& Pl, std::initializer_list<float> xy) { std::vector<Pt> c; const float* p = xy.begin(); for (size_t i = 0; i + 1 < xy.size(); i += 2) { Pt q = {p[i], p[i + 1]}; c.push_back(q); } Pl.push_back(c); }
static void pRRect(PP& Pl, float x0, float y0, float x1, float y1, float r, float w) {
  std::vector<Pt> p; auto add = [&](std::vector<Pt> a) { p.insert(p.end(), a.begin(), a.end()); };
  add(arcPts(x1 - r, y0 + r, r, r, -kPI / 2, 0, 6)); add(arcPts(x1 - r, y1 - r, r, r, 0, kPI / 2, 6));
  add(arcPts(x0 + r, y1 - r, r, r, kPI / 2, kPI, 6)); add(arcPts(x0 + r, y0 + r, r, r, kPI, 1.5f * kPI, 6));
  pPath(Pl, p, w, true);
}
static void pLines(PP& Pl, float w, std::initializer_list<float> xy, bool closed = false) { std::vector<Pt> c; const float* p = xy.begin(); for (size_t i = 0; i + 1 < xy.size(); i += 2) { Pt q = {p[i], p[i + 1]}; c.push_back(q); } pPath(Pl, c, w, closed); }
static void pArrowHead(PP& Pl, float tx, float ty, float dx, float dy, float len, float wid) {   // filled triangle with its tip at (tx,ty) pointing along (dx,dy)
  float l = sqrtf(dx * dx + dy * dy); dx /= l; dy /= l; float bx = tx - dx * len, by = ty - dy * len, nx = -dy * wid, ny = dx * wid;
  pFill(Pl, {tx, ty, bx + nx, by + ny, bx - nx, by - ny});
}

enum Ic { IC_HOME, IC_SHARE, IC_LIST, IC_UNDO, IC_REDO, IC_PLAY, IC_PAUSE, IC_CHEV_L, IC_CHEV_R, IC_PLUS, IC_MINUS, IC_CLOSE, IC_DIAMOND, IC_DIAMOND_F,
          IC_KEY_ADD, IC_KEY_DEL, IC_GRAPH, IC_CUBE, IC_SKY, IC_CAMERA, IC_BULB, IC_MEDIA, IC_MOVE, IC_ROTATE, IC_SCALE, IC_PENCIL, IC_COPY, IC_CUT, IC_TRASH,
          IC_EYE, IC_EYE_OFF, IC_AXES, IC_PIVOT, IC_SKEL, IC_ACC_FACE, IC_ACC_EDGE, IC_ACC_VERT, IC_SPHERE_P, IC_CYL_P, IC_CONE_P, IC_PLANE_P, IC_TORUS_P,
          IC_CAR_P, IC_IMPORT, IC_MODEL, IC_CHECK, IC_GRID, IC_WIRE, IC_GEAR, IC_SPARKLE, IC_DOTS, IC_WAVE, IC_COUNT };
static const float SW = 0.115f;   // stroke width in unit coords
static PP iconPolys(int ic) {
  PP Pl; const float w = SW;
  const float cubeV[7][2] = {{0, -0.95f}, {0.82f, -0.48f}, {0.82f, 0.48f}, {0, 0.95f}, {-0.82f, 0.48f}, {-0.82f, -0.48f}, {0, 0}};
  switch (ic) {
    case IC_HOME: pLines(Pl, w, {-0.92f, -0.05f, 0, -0.88f, 0.92f, -0.05f}); pLines(Pl, w, {-0.68f, -0.2f, -0.68f, 0.82f, 0.68f, 0.82f, 0.68f, -0.2f}); pLines(Pl, w, {-0.2f, 0.82f, -0.2f, 0.28f, 0.2f, 0.28f, 0.2f, 0.82f}); break;
    case IC_SHARE: { std::vector<Pt> c; for (int i = 0; i <= 14; i++) { float t = i / 14.f, x = -0.92f + 1.2f * t, y = 0.72f - 1.0f * sinf(t * 1.45f) * 0.95f - 0.0f; Pt q = {x, y}; c.push_back(q); } pPath(Pl, c, 0.3f); }
      pArrowHead(Pl, 0.95f, -0.2f, 1.0f, 0.0f, 0.78f, 0.62f); break;
    case IC_LIST: for (int i = 0; i < 3; i++) { float y = -0.62f + i * 0.62f; pDisc(Pl, -0.72f, y, 0.16f); pSeg(Pl, -0.3f, y, 0.9f, y, 0.2f); } break;
    case IC_UNDO: case IC_REDO: { float sx = ic == IC_UNDO ? 1.f : -1.f; std::vector<Pt> c = arcPts(0.1f * sx, 0.12f, 0.6f, 0.58f, ic == IC_UNDO ? 0.6f : 2.54f, ic == IC_UNDO ? -3.0f : 6.1f, 22);
      pPath(Pl, c, 0.2f); Pt e = c.back(); float a = ic == IC_UNDO ? -3.0f : 6.1f; float dx = sinf(a) * sx, dy = -cosf(a) * (ic == IC_UNDO ? 1.f : -1.f);
      pArrowHead(Pl, e.x + dx * 0.4f, e.y + dy * 0.4f, dx, dy, 0.62f, 0.4f); break; }
    case IC_PLAY: pFill(Pl, {-0.62f, -0.88f, 0.9f, 0, -0.62f, 0.88f}); break;
    case IC_PAUSE: pFill(Pl, {-0.7f, -0.8f, -0.2f, -0.8f, -0.2f, 0.8f, -0.7f, 0.8f}); pFill(Pl, {0.2f, -0.8f, 0.7f, -0.8f, 0.7f, 0.8f, 0.2f, 0.8f}); break;
    case IC_CHEV_L: pLines(Pl, 0.17f, {0.38f, -0.82f, -0.42f, 0, 0.38f, 0.82f}); break;
    case IC_CHEV_R: pLines(Pl, 0.17f, {-0.38f, -0.82f, 0.42f, 0, -0.38f, 0.82f}); break;
    case IC_PLUS: pSeg(Pl, -0.78f, 0, 0.78f, 0, 0.26f); pSeg(Pl, 0, -0.78f, 0, 0.78f, 0.26f); break;
    case IC_MINUS: pSeg(Pl, -0.7f, 0, 0.7f, 0, 0.18f); break;
    case IC_CLOSE: pSeg(Pl, -0.62f, -0.62f, 0.62f, 0.62f, 0.15f); pSeg(Pl, 0.62f, -0.62f, -0.62f, 0.62f, 0.15f); break;
    case IC_DIAMOND: pLines(Pl, 0.13f, {0, -0.92f, 0.92f, 0, 0, 0.92f, -0.92f, 0}, true); break;
    case IC_DIAMOND_F: pFill(Pl, {0, -0.95f, 0.95f, 0, 0, 0.95f, -0.95f, 0}); break;
    case IC_KEY_ADD: case IC_KEY_DEL: pLines(Pl, 0.14f, {-0.1f, -0.9f, 0.7f, -0.1f, -0.1f, 0.7f, -0.9f, -0.1f}, true);
      if (ic == IC_KEY_ADD) { pSeg(Pl, 0.5f, 0.78f, 0.98f, 0.78f, 0.12f); pSeg(Pl, 0.74f, 0.54f, 0.74f, 1.02f, 0.12f); } else pSeg(Pl, 0.5f, 0.78f, 0.98f, 0.78f, 0.12f); break;
    case IC_GRAPH: pLines(Pl, 0.1f, {-0.86f, -0.86f, -0.86f, 0.86f, 0.86f, 0.86f});
      { std::vector<Pt> c; for (int i = 0; i <= 16; i++) { float t = i / 16.f, y = 0.55f - 1.1f * (t * t * (3 - 2 * t)); Pt q = {-0.5f + 1.3f * t, y}; c.push_back(q); } pPath(Pl, c, 0.14f); } break;
    case IC_CUBE: case IC_MODEL: for (int i = 0; i < 6; i++) pSeg(Pl, cubeV[i][0], cubeV[i][1], cubeV[(i + 1) % 6][0], cubeV[(i + 1) % 6][1], 0.1f);
      pSeg(Pl, 0, 0, cubeV[5][0], cubeV[5][1], 0.1f); pSeg(Pl, 0, 0, cubeV[1][0], cubeV[1][1], 0.1f); pSeg(Pl, 0, 0, cubeV[3][0], cubeV[3][1], 0.1f);
      if (ic == IC_MODEL) for (int i = 0; i < 7; i++) pDisc(Pl, cubeV[i][0], cubeV[i][1], 0.17f); break;
    case IC_SKY: pRing(Pl, 0, 0, 0.8f, 0.1f); pPath(Pl, arcPts(0, 0.05f, 0.8f, 0.34f, 0, kPI, 16), 0.1f); pPath(Pl, arcPts(0, 0.05f, 0.8f, 0.34f, kPI, 2 * kPI, 16), 0.1f); break;
    case IC_CAMERA: pRRect(Pl, -0.92f, -0.55f, 0.28f, 0.55f, 0.2f, 0.1f); pLines(Pl, 0.1f, {0.28f, -0.1f, 0.92f, -0.5f, 0.92f, 0.5f, 0.28f, 0.1f}); break;
    case IC_BULB: pPath(Pl, arcPts(0, -0.22f, 0.5f, 0.5f, 2.35f, 0.79f + 2 * kPI * 0 + 2 * kPI - 0.0f, 24), 0.1f); pLines(Pl, 0.1f, {-0.3f, 0.5f, -0.28f, 0.7f, 0.28f, 0.7f, 0.3f, 0.5f}); pSeg(Pl, -0.16f, 0.9f, 0.16f, 0.9f, 0.12f);
      for (int i = 0; i < 5; i++) { float a = -kPI + 0.55f + i * (kPI - 1.1f) / 4; pSeg(Pl, cosf(a) * 0.82f, -0.22f + sinf(a) * 0.82f, cosf(a) * 1.0f, -0.22f + sinf(a) * 1.0f, 0.09f); } break;
    case IC_MEDIA: pRRect(Pl, -0.7f, -0.78f, 0.9f, 0.62f, 0.12f, 0.1f); pLines(Pl, 0.1f, {-0.92f, -0.46f, -0.92f, 0.86f, 0.5f, 0.86f}); pLines(Pl, 0.1f, {-0.5f, 0.4f, -0.12f, -0.05f, 0.18f, 0.28f, 0.42f, 0.0f, 0.78f, 0.42f}); pDisc(Pl, 0.4f, -0.4f, 0.14f); break;
    case IC_MOVE: pSeg(Pl, -0.55f, -0.38f, 0.55f, -0.38f, 0.17f); pArrowHead(Pl, 0.95f, -0.38f, 1, 0, 0.62f, 0.46f); pSeg(Pl, 0.55f, 0.38f, -0.55f, 0.38f, 0.17f); pArrowHead(Pl, -0.95f, 0.38f, -1, 0, 0.62f, 0.46f); break;
    case IC_ROTATE: pPath(Pl, arcPts(0, 0.05f, 0.8f, 0.55f, 0.55f, 5.55f, 26), 0.2f); pArrowHead(Pl, 0.2f, 0.62f, -1, 0.1f, 0.5f, 0.44f); break;
    case IC_SCALE: pSeg(Pl, -0.55f, 0.55f, 0.55f, -0.55f, 0.15f); pArrowHead(Pl, 0.95f, -0.95f, 1, -1, 0.72f, 0.46f); pArrowHead(Pl, -0.95f, 0.95f, -1, 1, 0.72f, 0.46f); break;
    case IC_PENCIL: pFill(Pl, {-0.88f, 0.88f, -0.72f, 0.3f, 0.45f, -0.88f, 0.88f, -0.45f, -0.28f, 0.72f}); break;
    case IC_COPY: pRRect(Pl, -0.9f, -0.5f, 0.3f, 0.9f, 0.14f, 0.1f); pRRect(Pl, -0.3f, -0.9f, 0.9f, 0.5f, 0.14f, 0.1f); break;
    case IC_CUT: pRRect(Pl, -0.92f, -0.62f, -0.12f, 0.62f, 0.12f, 0.1f); pRRect(Pl, 0.12f, -0.62f, 0.92f, 0.62f, 0.12f, 0.1f); break;
    case IC_TRASH: pLines(Pl, 0.1f, {-0.68f, -0.58f, -0.5f, 0.88f, 0.5f, 0.88f, 0.68f, -0.58f}); pSeg(Pl, -0.9f, -0.58f, 0.9f, -0.58f, 0.12f); pLines(Pl, 0.1f, {-0.28f, -0.58f, -0.28f, -0.88f, 0.28f, -0.88f, 0.28f, -0.58f}); break;
    case IC_EYE: case IC_EYE_OFF: pPath(Pl, arcPts(0, 0.55f, 0.95f, 0.95f, -2.2f, -0.94f, 14), 0.12f); pPath(Pl, arcPts(0, -0.55f, 0.95f, 0.95f, 0.94f, 2.2f, 14), 0.12f); pDisc(Pl, 0, 0, 0.3f);
      if (ic == IC_EYE_OFF) pSeg(Pl, -0.78f, -0.8f, 0.78f, 0.8f, 0.14f); break;
    case IC_AXES: pSeg(Pl, 0, 0, 0, -0.92f, 0.12f); pSeg(Pl, 0, 0, -0.8f, 0.55f, 0.12f); pSeg(Pl, 0, 0, 0.8f, 0.55f, 0.12f); pSeg(Pl, -0.5f, -0.28f, -0.5f, -0.28f, 0.12f); break;
    case IC_PIVOT: pRRect(Pl, -0.9f, -0.9f, 0.9f, 0.9f, 0.06f, 0.07f); pRing(Pl, 0, 0, 0.52f, 0.1f); pSeg(Pl, -0.9f, 0, 0.9f, 0, 0.08f); pSeg(Pl, 0, -0.9f, 0, 0.9f, 0.08f); break;
    case IC_SKEL: pDisc(Pl, 0.15f, -0.78f, 0.17f); pLines(Pl, 0.08f, {0.1f, -0.58f, 0.0f, 0.1f, -0.25f, 0.9f}); pLines(Pl, 0.08f, {0.0f, 0.1f, 0.4f, 0.9f}); pLines(Pl, 0.08f, {-0.2f, -0.35f, 0.65f, -0.3f}); pLines(Pl, 0.08f, {-0.2f, -0.35f, -0.55f, 0.15f}); pLines(Pl, 0.08f, {0.65f, -0.3f, 0.92f, 0.12f});
      for (int i = 0; i < 5; i++) { static const float jx[5] = {0.0f, -0.2f, 0.65f, -0.55f, 0.92f}, jy[5] = {0.1f, -0.35f, -0.3f, 0.15f, 0.12f}; pDisc(Pl, jx[i], jy[i], 0.13f); } break;
    case IC_ACC_FACE: pFill(Pl, {cubeV[5][0], cubeV[5][1], 0, 0, cubeV[3][0], cubeV[3][1], cubeV[4][0], cubeV[4][1]}); break;
    case IC_ACC_EDGE: pSeg(Pl, 0, 0, cubeV[3][0], cubeV[3][1], 0.2f); break;
    case IC_ACC_VERT: pDisc(Pl, cubeV[1][0], cubeV[1][1], 0.24f); break;
    case IC_SPHERE_P: pRing(Pl, 0, 0, 0.82f, 0.1f); pPath(Pl, arcPts(0, 0, 0.82f, 0.3f, 0, 2 * kPI, 24), 0.08f); break;
    case IC_CYL_P: pPath(Pl, arcPts(0, -0.62f, 0.6f, 0.24f, 0, 2 * kPI, 20), 0.1f); pLines(Pl, 0.1f, {-0.6f, -0.62f, -0.6f, 0.62f}); pLines(Pl, 0.1f, {0.6f, -0.62f, 0.6f, 0.62f}); pPath(Pl, arcPts(0, 0.62f, 0.6f, 0.24f, 0, kPI, 12), 0.1f); break;
    case IC_CONE_P: pLines(Pl, 0.1f, {-0.62f, 0.62f, 0, -0.9f, 0.62f, 0.62f}); pPath(Pl, arcPts(0, 0.62f, 0.62f, 0.25f, 0, kPI, 12), 0.1f); break;
    case IC_PLANE_P: pLines(Pl, 0.1f, {-0.9f, 0.4f, -0.4f, -0.45f, 0.9f, -0.45f, 0.4f, 0.4f}, true); break;
    case IC_TORUS_P: pPath(Pl, arcPts(0, 0, 0.9f, 0.55f, 0, 2 * kPI, 24), 0.1f); pPath(Pl, arcPts(0, 0.05f, 0.42f, 0.2f, 0, 2 * kPI, 18), 0.1f); break;
    case IC_CAR_P: pLines(Pl, 0.1f, {-0.92f, 0.3f, -0.92f, -0.1f, -0.5f, -0.18f, -0.25f, -0.62f, 0.4f, -0.62f, 0.62f, -0.18f, 0.92f, -0.1f, 0.92f, 0.3f, -0.92f, 0.3f}); pDisc(Pl, -0.5f, 0.38f, 0.22f); pDisc(Pl, 0.5f, 0.38f, 0.22f); break;
    case IC_IMPORT: pLines(Pl, 0.11f, {-0.85f, 0.2f, -0.85f, 0.85f, 0.85f, 0.85f, 0.85f, 0.2f}); pSeg(Pl, 0, -0.85f, 0, 0.4f, 0.14f); pLines(Pl, 0.14f, {-0.42f, 0.0f, 0, 0.45f, 0.42f, 0.0f}); break;
    case IC_CHECK: pLines(Pl, 0.17f, {-0.8f, 0.05f, -0.25f, 0.6f, 0.85f, -0.6f}); break;
    case IC_GRID: for (int i = -1; i <= 1; i++) { pSeg(Pl, i * 0.6f, -0.9f, i * 0.6f, 0.9f, 0.09f); pSeg(Pl, -0.9f, i * 0.6f, 0.9f, i * 0.6f, 0.09f); } break;
    case IC_GEAR: { std::vector<Pt> c; for (int k = 0; k < 8; k++) { float ck = k * kPI / 4; const float rr[4] = {0.70f, 0.93f, 0.93f, 0.70f}, da[4] = {-0.30f, -0.17f, 0.17f, 0.30f};
        for (int j = 0; j < 4; j++) { Pt q = {rr[j] * cosf(ck + da[j]), rr[j] * sinf(ck + da[j])}; c.push_back(q); } } pPath(Pl, c, 0.1f, true); pRing(Pl, 0, 0, 0.3f, 0.1f); } break;
    case IC_SPARKLE: { auto star = [&](float cx, float cy, float s, bool fill) { const float px[8] = {0, 0.24f, 1, 0.24f, 0, -0.24f, -1, -0.24f}, py[8] = {-1, -0.24f, 0, 0.24f, 1, 0.24f, 0, -0.24f}; std::vector<Pt> c;
        for (int i = 0; i < 8; i++) { Pt q = {cx + px[i] * s, cy + py[i] * s}; c.push_back(q); } if (fill) Pl.push_back(c); else pPath(Pl, c, 0.1f, true); };
      star(0.32f, 0.28f, 0.62f, false); star(-0.62f, -0.55f, 0.3f, true); star(-0.66f, 0.42f, 0.24f, true); } break;
    case IC_WAVE: { std::vector<Pt> c; for (int i = 0; i <= 24; i++) { float t = i / 24.f; Pt q = {-0.95f + 1.9f * t, 0.55f * sinf(t * 6.2832f * 1.5f + 0.4f) * (0.35f + 0.65f * sinf(t * 3.1416f))}; c.push_back(q); } pPath(Pl, c, 0.12f);
      pSeg(Pl, -0.28f, -0.92f, -0.28f, 0.92f, 0.09f); pSeg(Pl, 0.5f, -0.92f, 0.5f, 0.92f, 0.09f); } break;
    case IC_DOTS: pDisc(Pl, -0.62f, 0, 0.17f); pDisc(Pl, 0, 0, 0.17f); pDisc(Pl, 0.62f, 0, 0.17f); break;
    case IC_WIRE: for (int i = 0; i < 6; i++) pSeg(Pl, cubeV[i][0], cubeV[i][1], cubeV[(i + 1) % 6][0], cubeV[(i + 1) % 6][1], 0.09f); pSeg(Pl, 0, 0, cubeV[5][0], cubeV[5][1], 0.09f); pSeg(Pl, 0, 0, cubeV[1][0], cubeV[1][1], 0.09f); pSeg(Pl, 0, 0, cubeV[3][0], cubeV[3][1], 0.09f); break;
  }
  orient(Pl);
  return Pl;
}
static SDL_Texture* iconTex(int ic, int sz) {
  if (sz < 4) sz = 4;
  uint32_t key = ((uint32_t)ic << 16) | (uint32_t)sz;
  std::map<uint32_t, SDL_Texture*>::iterator it = g_shapeTex.find(key);
  if (it != g_shapeTex.end()) return it->second;
  PP Pl = iconPolys(ic);
  for (size_t c = 0; c < Pl.size(); c++) for (size_t i = 0; i < Pl[c].size(); i++) { Pl[c][i].x = (Pl[c][i].x + 1.f) * 0.5f * sz; Pl[c][i].y = (Pl[c][i].y + 1.f) * 0.5f * sz; }
  std::vector<uint8_t> a; uif::rasterPolys(Pl, sz, sz, a, 1.0f);
  return g_shapeTex[key] = texFromAlpha(a, sz, sz);
}
static void icon(int ic, int cx, int cy, int sz, SDL_Color c) {
  SDL_Texture* t = iconTex(ic, sz); if (!t) return;
  SDL_Rect d = {cx - sz / 2, cy - sz / 2, sz, sz}; SDL_SetTextureColorMod(t, c.r, c.g, c.b); SDL_SetTextureAlphaMod(t, c.a); SDL_RenderCopy(R, t, nullptr, &d);
}
// selection-mode cube icon: kind 0 = whole object (all orange), 1 = face, 2 = edge, 3 = vertex
static void selIcon(int kind, int cx, int cy, int sz, bool on) {
  SDL_Color base = kind == 0 && on ? C_ORG : C_TXT;
  icon(IC_CUBE, cx, cy, sz, base);
  if (kind == 1) icon(IC_ACC_FACE, cx, cy, sz, C_ORG); else if (kind == 2) icon(IC_ACC_EDGE, cx, cy, sz, C_ORG); else if (kind == 3) icon(IC_ACC_VERT, cx, cy, sz, C_ORG);
}

// ================================================================ small vector math
struct V3 { float x, y, z; };
static V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static V3 sub(V3 a, V3 b) { return a - b; }
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static float len3(V3 a) { return sqrtf(dot(a, a)); }
static V3 norm(V3 a) { float l = len3(a); return l > 1e-12f ? V3{a.x / l, a.y / l, a.z / l} : V3{0, 1, 0}; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static const float kDeg = 0.017453293f;

// ================================================================ editable polygon mesh (the "cage") + Catmull-Clark + render mesh
typedef std::vector<int> Face;
struct EditMesh { std::vector<V3> v; std::vector<Face> f; std::vector<uint8_t> hid; };

static void emAppend(EditMesh& d, const EditMesh& s) {
  int o = (int)d.v.size(); d.v.insert(d.v.end(), s.v.begin(), s.v.end());
  for (size_t i = 0; i < s.f.size(); i++) { Face q = s.f[i]; for (size_t k = 0; k < q.size(); k++) q[k] += o; d.f.push_back(q); }
}
static void emFix(EditMesh& m) { m.hid.resize(m.f.size(), 0); }
// n x n x n lattice cube surface (shared vertices): used for cube and sphere
static EditMesh emLattice(int n, bool sphere, float r) {
  EditMesh m; std::map<std::array<int, 3>, int> id;
  auto vid = [&](int x, int y, int z) {
    std::array<int, 3> k = {x, y, z}; std::map<std::array<int, 3>, int>::iterator it = id.find(k); if (it != id.end()) return it->second;
    V3 p = {(x * 2.f / n - 1.f), (y * 2.f / n - 1.f), (z * 2.f / n - 1.f)}; if (sphere) p = norm(p); p = p * r;
    m.v.push_back(p); return id[k] = (int)m.v.size() - 1;
  };
  for (int ax = 0; ax < 3; ax++) for (int s = 0; s < 2; s++) for (int a = 0; a < n; a++) for (int b = 0; b < n; b++) {
    int c[4][3];
    for (int q = 0; q < 4; q++) { int ua = a + (q == 1 || q == 2), ub = b + (q >= 2); c[q][ax] = s * n; c[q][(ax + 1) % 3] = ua; c[q][(ax + 2) % 3] = ub; }
    Face f; for (int q = 0; q < 4; q++) f.push_back(vid(c[q][0], c[q][1], c[q][2]));
    V3 e1 = m.v[f[1]] - m.v[f[0]], e2 = m.v[f[3]] - m.v[f[0]], nn = cross(e1, e2), cen = (m.v[f[0]] + m.v[f[2]]) * 0.5f;
    if (dot(nn, cen) < 0) std::reverse(f.begin(), f.end());
    m.f.push_back(f);
  }
  emFix(m); return m;
}
static EditMesh emBox(V3 h, int n = 1) { EditMesh m = emLattice(n, false, 1.f); for (size_t i = 0; i < m.v.size(); i++) m.v[i] = {m.v[i].x * h.x, m.v[i].y * h.y, m.v[i].z * h.z}; return m; }
static EditMesh emCylinder(float r, float h, int seg, bool cone = false, int axis = 1) {
  EditMesh m; Face top, bot;
  for (int i = 0; i < seg; i++) { float a = 2 * kPI * i / seg; m.v.push_back({r * cosf(a), -h / 2, r * sinf(a)}); }
  if (cone) m.v.push_back({0, h / 2, 0}); else for (int i = 0; i < seg; i++) { float a = 2 * kPI * i / seg; m.v.push_back({r * cosf(a), h / 2, r * sinf(a)}); }
  for (int i = 0; i < seg; i++) {
    int j = (i + 1) % seg;
    if (cone) m.f.push_back({i, seg, j}); else m.f.push_back({i, seg + i, seg + j, j});
    bot.push_back(seg - 1 - i); if (!cone) top.push_back(seg + i);
  }
  m.f.push_back(bot); if (!cone) m.f.push_back(top);
  if (axis == 2) for (size_t i = 0; i < m.v.size(); i++) m.v[i] = {m.v[i].x, m.v[i].z, m.v[i].y};   // axis along Z (car wheels)
  emFix(m); return m;
}
static EditMesh emGrid(int nx, int nz, float sx, float sz) {
  EditMesh m;
  for (int j = 0; j <= nz; j++) for (int i = 0; i <= nx; i++) m.v.push_back({(i * 1.f / nx - 0.5f) * sx, 0, (j * 1.f / nz - 0.5f) * sz});
  for (int j = 0; j < nz; j++) for (int i = 0; i < nx; i++) { int a = j * (nx + 1) + i; m.f.push_back({a, a + nx + 1, a + nx + 2, a + 1}); }
  emFix(m); return m;
}
static EditMesh emTorus(float R, float r, int seg, int rseg) {
  EditMesh m;
  for (int i = 0; i < seg; i++) for (int j = 0; j < rseg; j++) { float a = 2 * kPI * i / seg, b = 2 * kPI * j / rseg; m.v.push_back({(R + r * cosf(b)) * cosf(a), r * sinf(b), (R + r * cosf(b)) * sinf(a)}); }
  for (int i = 0; i < seg; i++) for (int j = 0; j < rseg; j++) { int i2 = (i + 1) % seg, j2 = (j + 1) % rseg; m.f.push_back({i * rseg + j, i2 * rseg + j, i2 * rseg + j2, i * rseg + j2}); }
  emFix(m); return m;
}
static EditMesh emCar() {   // low-poly car cage like the reference screenshot
  EditMesh m = emBox({2.0f, 0.45f, 0.9f}, 3); for (size_t i = 0; i < m.v.size(); i++) m.v[i].y += 0.1f;
  EditMesh cab = emBox({1.0f, 0.4f, 0.8f}, 2);
  for (size_t i = 0; i < cab.v.size(); i++) { V3& p = cab.v[i]; p.y += 0.95f; if (p.y > 0.95f) { p.x *= 0.78f; p.z *= 0.86f; } p.x -= 0.15f; }
  emAppend(m, cab);
  const float wx[4] = {-1.15f, 1.15f, -1.15f, 1.15f}, wz[4] = {0.95f, 0.95f, -0.95f, -0.95f};
  for (int i = 0; i < 4; i++) { EditMesh w = emCylinder(0.42f, 0.28f, 12, false, 2); for (size_t k = 0; k < w.v.size(); k++) { w.v[k].x += wx[i]; w.v[k].z += wz[i]; w.v[k].y += -0.15f; } emAppend(m, w); }
  emFix(m); return m;
}

// ---- Catmull-Clark subdivision (handles n-gons and open boundaries). OpenSubdiv can replace this one function.
static void ccSubdivide(const std::vector<V3>& v, const std::vector<Face>& f, std::vector<V3>& ov, std::vector<Face>& of) {
  struct Ed { int a, b, e0, e1, idx; };
  std::map<std::pair<int, int>, Ed> E; int nV = (int)v.size(), nF = (int)f.size();
  std::vector<V3> fp(nF); std::vector<std::vector<int> > vF(nV);
  for (int i = 0; i < nF; i++) {
    V3 c = {0, 0, 0}; for (size_t k = 0; k < f[i].size(); k++) { c = c + v[f[i][k]]; vF[f[i][k]].push_back(i); } fp[i] = c * (1.f / f[i].size());
    for (size_t k = 0; k < f[i].size(); k++) {
      int a = f[i][k], b = f[i][(k + 1) % f[i].size()]; std::pair<int, int> key(std::min(a, b), std::max(a, b));
      std::map<std::pair<int, int>, Ed>::iterator it = E.find(key);
      if (it == E.end()) { Ed e; e.a = key.first; e.b = key.second; e.e0 = i; e.e1 = -1; e.idx = 0; E[key] = e; } else if (it->second.e1 < 0) it->second.e1 = i;
    }
  }
  int next = nV + 0; std::vector<std::vector<int> > vE(nV);
  std::vector<V3> ep; for (std::map<std::pair<int, int>, Ed>::iterator it = E.begin(); it != E.end(); ++it) {
    Ed& e = it->second; V3 mid = (v[e.a] + v[e.b]) * 0.5f;
    V3 p = e.e1 >= 0 ? (v[e.a] + v[e.b] + fp[e.e0] + fp[e.e1]) * 0.25f : mid; e.idx = next++; ep.push_back(p);
    vE[e.a].push_back((int)ep.size() - 1); vE[e.b].push_back((int)ep.size() - 1);
  }
  std::vector<V3> mids(ep.size()); { int k = 0; for (std::map<std::pair<int, int>, Ed>::iterator it = E.begin(); it != E.end(); ++it, ++k) mids[k] = (v[it->second.a] + v[it->second.b]) * 0.5f; }
  std::vector<std::vector<int> > vEd(nV);   // edges (in map order index) per vertex, plus boundary info
  { int k = 0; for (std::map<std::pair<int, int>, Ed>::iterator it = E.begin(); it != E.end(); ++it, ++k) { vEd[it->second.a].push_back(k); vEd[it->second.b].push_back(k); } }
  std::vector<int> eFaces(ep.size()); { int k = 0; for (std::map<std::pair<int, int>, Ed>::iterator it = E.begin(); it != E.end(); ++it, ++k) eFaces[k] = it->second.e1 >= 0 ? 2 : 1; }
  std::vector<std::pair<int, int> > eEnds(ep.size()); { int k = 0; for (std::map<std::pair<int, int>, Ed>::iterator it = E.begin(); it != E.end(); ++it, ++k) eEnds[k] = std::make_pair(it->second.a, it->second.b); }
  ov.assign(nV, V3{0, 0, 0});
  for (int i = 0; i < nV; i++) {
    int n = (int)vF[i].size(); if (n == 0) { ov[i] = v[i]; continue; }
    std::vector<int> bnd; for (size_t k = 0; k < vEd[i].size(); k++) if (eFaces[vEd[i][k]] == 1) bnd.push_back(vEd[i][k]);
    if (bnd.size() >= 2) { V3 s = v[i] * 6.f; for (int q = 0; q < 2; q++) { std::pair<int, int> en = eEnds[bnd[q]]; s = s + v[en.first == i ? en.second : en.first]; } ov[i] = s * 0.125f; continue; }
    V3 Q = {0, 0, 0}, Rr = {0, 0, 0}; for (int k = 0; k < n; k++) Q = Q + fp[vF[i][k]]; Q = Q * (1.f / n);
    for (size_t k = 0; k < vEd[i].size(); k++) Rr = Rr + mids[vEd[i][k]]; Rr = Rr * (1.f / vEd[i].size());
    ov[i] = (Q + Rr * 2.f + v[i] * (float)(n - 3)) * (1.f / n);
  }
  ov.insert(ov.end(), ep.begin(), ep.end());
  int fBase = (int)ov.size(); ov.insert(ov.end(), fp.begin(), fp.end());
  of.clear();
  for (int i = 0; i < nF; i++) {
    int k = (int)f[i].size();
    for (int j = 0; j < k; j++) {
      int a = f[i][j], b = f[i][(j + 1) % k], c = f[i][(j + k - 1) % k];
      int eab = E[std::make_pair(std::min(a, b), std::max(a, b))].idx, eca = E[std::make_pair(std::min(c, a), std::max(c, a))].idx;
      of.push_back({a, eab, fBase + i, eca});
    }
  }
}
// cage -> triangle mesh with angle-based smoothing (corner normals averaged over faces within `smoothDeg`)
static void buildRenderMesh(const EditMesh& em, int subdiv, float smoothDeg, mx::Mesh3& out) {
  std::vector<V3> v; std::vector<Face> f;
  { std::vector<int> remap(em.v.size(), -1);
    for (size_t i = 0; i < em.f.size(); i++) { if (i < em.hid.size() && em.hid[i]) continue; Face q = em.f[i]; for (size_t k = 0; k < q.size(); k++) { int& r = remap[q[k]]; if (r < 0) { r = (int)v.size(); v.push_back(em.v[q[k]]); } q[k] = r; } f.push_back(q); } }
  for (int s = 0; s < subdiv && f.size() < 60000; s++) { std::vector<V3> nv; std::vector<Face> nf; ccSubdivide(v, f, nv, nf); v.swap(nv); f.swap(nf); }
  size_t nF = f.size(); std::vector<V3> fn(nF); std::vector<std::vector<int> > vF(v.size());
  for (size_t i = 0; i < nF; i++) {
    V3 n = {0, 0, 0}; for (size_t k = 0; k < f[i].size(); k++) { V3 a = v[f[i][k]], b = v[f[i][(k + 1) % f[i].size()]]; n.x += (a.y - b.y) * (a.z + b.z); n.y += (a.z - b.z) * (a.x + b.x); n.z += (a.x - b.x) * (a.y + b.y); }
    fn[i] = n; for (size_t k = 0; k < f[i].size(); k++) vF[f[i][k]].push_back((int)i);
  }
  float thr = cosf(std::max(0.f, smoothDeg) * kDeg); if (subdiv > 0) thr = std::min(thr, cosf(80.f * kDeg));
  out = mx::Mesh3(); out.pos.reserve(nF * 12); 
  for (size_t i = 0; i < nF; i++) {
    V3 ni = norm(fn[i]); int base = (int)(out.pos.size() / 3);
    for (size_t k = 0; k < f[i].size(); k++) {
      V3 acc = {0, 0, 0}; const std::vector<int>& adj = vF[f[i][k]];
      for (size_t q = 0; q < adj.size(); q++) { V3 nj = norm(fn[adj[q]]); if (dot(ni, nj) >= thr - 1e-5f) acc = acc + fn[adj[q]]; }
      V3 nn = norm(acc); V3 p = v[f[i][k]];
      out.pos.push_back(p.x); out.pos.push_back(p.y); out.pos.push_back(p.z); out.nrm.push_back(nn.x); out.nrm.push_back(nn.y); out.nrm.push_back(nn.z);
    }
    for (size_t k = 1; k + 1 < f[i].size(); k++) { out.idx.push_back(base); out.idx.push_back(base + (int)k); out.idx.push_back(base + (int)k + 1); }
  }
  mx::calcBounds(out);
}

// ================================================================ animation data: clips, keyframes, cubic-bezier easing
struct Bez { float x1, y1, x2, y2; Bez() : x1(0.42f), y1(0.f), x2(0.58f), y2(1.f) {} Bez(float a, float b, float c, float d) : x1(a), y1(b), x2(c), y2(d) {} };
struct HKey { float v[9]; Bez bz; uint8_t mode; HKey() : mode(0) { for (int i = 0; i < 9; i++) v[i] = i >= 6 ? 1.f : 0.f; } };   // mode 0 bezier, 1 linear, 2 hold
struct HClip { int start, len; std::map<int, HKey> keys; HClip() : start(0), len(60) {} };       // key frames are relative to the clip start
static const char* kChanName[9] = {"Position X", "Position Y", "Position Z", "Rotation X", "Rotation Y", "Rotation Z", "Scale X", "Scale Y", "Scale Z"};

static float bezAt(float a, float b, float t) { float s = 1 - t; return 3 * s * s * t * a + 3 * s * t * t * b + t * t * t; }
static float easeBez(const Bez& z, float u) {   // CSS-style cubic-bezier(x1,y1,x2,y2): solve x(t)=u, return y(t)
  if (u <= 0) return 0; if (u >= 1) return 1;
  float t = u;
  for (int i = 0; i < 6; i++) { float x = bezAt(z.x1, z.x2, t) - u, s = 1 - t, dx = 3 * s * s * z.x1 + 6 * s * t * (z.x2 - z.x1) + 3 * t * t * (1 - z.x2); if (fabsf(dx) < 1e-5f) break; t = clampf(t - x / dx, 0, 1); }
  float lo = 0, hi = 1; for (int i = 0; i < 24 && fabsf(bezAt(z.x1, z.x2, t) - u) > 1e-5f; i++) { if (bezAt(z.x1, z.x2, t) < u) lo = t; else hi = t; t = (lo + hi) * 0.5f; }
  return bezAt(z.y1, z.y2, t);
}
struct Preset { const char* name; Bez b; int mode; };
static const Preset kPresets[] = {
  {"Linear", Bez(0, 0, 1, 1), 1}, {"Ease In", Bez(0.42f, 0, 1, 1), 0}, {"Ease Out", Bez(0, 0, 0.58f, 1), 0}, {"Ease In-Out", Bez(0.42f, 0, 0.58f, 1), 0},
  {"Sine", Bez(0.37f, 0, 0.63f, 1), 0}, {"Expo Out", Bez(0.16f, 1, 0.3f, 1), 0}, {"Back Out", Bez(0.34f, 1.56f, 0.64f, 1), 0}, {"Back In", Bez(0.36f, 0, 0.66f, -0.56f), 0},
  {"Hold", Bez(0, 0, 1, 1), 2}};
static const int kNPresets = (int)(sizeof(kPresets) / sizeof(kPresets[0]));

static bool evalClipAt(const HClip& c, float rel, float* out) {
  if (c.keys.empty()) return false;
  std::map<int, HKey>::const_iterator nx = c.keys.upper_bound((int)floorf(rel));
  if (nx == c.keys.begin()) { for (int i = 0; i < 9; i++) out[i] = nx->second.v[i]; return true; }
  std::map<int, HKey>::const_iterator pv = nx; --pv;
  if (nx == c.keys.end()) { for (int i = 0; i < 9; i++) out[i] = pv->second.v[i]; return true; }
  float t = (rel - pv->first) / (float)(nx->first - pv->first), e = pv->second.mode == 2 ? 0.f : pv->second.mode == 1 ? t : easeBez(pv->second.bz, t);
  for (int i = 0; i < 9; i++) out[i] = pv->second.v[i] + (nx->second.v[i] - pv->second.v[i]) * e;
  return true;
}

// ================================================================ scene objects + document (value semantics => undo = copy)
struct SkelBone { float x, y, z; int parent; };
// Motion log: a dense sample stream per transform channel (kSub samples per frame), like SFM's motion-editor logs.
static const int kSub = 2;
struct MotionLog { int n; std::vector<float> ch[9]; float def[9]; MotionLog() : n(0) { for (int i = 0; i < 9; i++) def[i] = i >= 6 ? 1.f : 0.f; } };
// live Motion Editor output stream (log + temporary layer) for the object being edited
static bool g_meOpen = false, g_meOutValid = false; static int g_meObj = -1, g_meOutN = 0; static std::vector<float> g_meOut[9];
static float sampleStream(const std::vector<float>& s, int n, float frame) {
  if (n <= 0 || s.empty()) return 0.f; float x = frame * kSub; if (x <= 0) return s[0]; if (x >= n - 1) return s[n - 1];
  int i = (int)x; float t = x - i; return s[i] + (s[i + 1] - s[i]) * t;
}
struct Obj {
  int id; std::string name;
  std::shared_ptr<mx::Mesh3> imp;            // imported mesh (no cage)
  std::shared_ptr<EditMesh> em;              // editable cage (primitives / modeling)
  std::shared_ptr<mx::Mesh3> rm;             // render mesh in local space (cached)
  int subdiv; float smooth;
  float T[9], base[9];                       // live transform and the un-animated base: pos xyz, rot xyz (deg), scale xyz
  std::vector<HClip> clips; bool hidden, colSet; uint8_t col[3];
  std::shared_ptr<std::vector<SkelBone> > skel;
  std::shared_ptr<MotionLog> log;            // baked dense motion (when present it drives the model)
  float cx, cy, cz, rad;                     // local bounds centre + radius (picking)
  Obj() : id(0), subdiv(0), smooth(30.f), hidden(false), colSet(true), cx(0), cy(0), cz(0), rad(1) {
    for (int i = 0; i < 9; i++) T[i] = base[i] = i >= 6 ? 1.f : 0.f; col[0] = 224; col[1] = 224; col[2] = 228; }
};
struct Doc { std::vector<Obj> objs; int sel, selClip, nextId; Doc() : sel(-1), selClip(-1), nextId(1) {} };
static Doc doc;
// ---- Source Hexagon skin state (see the skin block before drawViewport)
static bool g_maya = true;   // Maya-style interface (landscape); portrait keeps the classic skin
static bool land = false;   // landscape?
static bool g_skin = true, g_autoKey = true, g_autosave = true; static int g_nav = 2;   // g_nav: 0 orbit, 1 select, 2 both
static float g_speed = 1.f, g_fov = 60.f; static SDL_Window* g_win = nullptr;
static bool skinOn(); static void skinLayout();
static std::vector<Doc> undoS, redoS;
static void pushUndo() { undoS.push_back(doc); if (undoS.size() > 60) undoS.erase(undoS.begin()); redoS.clear(); }
static Obj* selObj() { return doc.sel >= 0 && doc.sel < (int)doc.objs.size() ? &doc.objs[doc.sel] : nullptr; }

static void refreshBounds(Obj& o) {
  const mx::Mesh3* m = o.rm ? o.rm.get() : nullptr; if (!m || m->vcount() == 0) { o.cx = o.cy = o.cz = 0; o.rad = 1; return; }
  o.cx = (m->bmin[0] + m->bmax[0]) * 0.5f; o.cy = (m->bmin[1] + m->bmax[1]) * 0.5f; o.cz = (m->bmin[2] + m->bmax[2]) * 0.5f;
  float dx = m->bmax[0] - m->bmin[0], dy = m->bmax[1] - m->bmin[1], dz = m->bmax[2] - m->bmin[2]; o.rad = 0.5f * sqrtf(dx * dx + dy * dy + dz * dz);
}
static void rebuildObj(Obj& o) {
  if (o.em) { std::shared_ptr<mx::Mesh3> m = std::make_shared<mx::Mesh3>(); buildRenderMesh(*o.em, o.subdiv, o.smooth, *m); o.rm = m; }
  else o.rm = o.imp;
  refreshBounds(o);
}
static std::string uniqueName(const std::string& n) {
  int c = 0; for (size_t i = 0; i < doc.objs.size(); i++) if (doc.objs[i].name.compare(0, n.size(), n) == 0) c++;
  return c ? n + " " + std::to_string(c + 1) : n;
}
enum Prim { PR_SPHERE, PR_CUBE, PR_CYL, PR_CONE, PR_PLANE, PR_TORUS, PR_CAR, PR_COUNT };
static const char* kPrimName[PR_COUNT] = {"Sphere", "Cube", "Cylinder", "Cone", "Plane", "Torus", "Car"};
static const int kPrimIcon[PR_COUNT] = {IC_SPHERE_P, IC_CUBE, IC_CYL_P, IC_CONE_P, IC_PLANE_P, IC_TORUS_P, IC_CAR_P};
static int addPrimitive(int kind, bool select = true) {
  Obj o; o.id = doc.nextId++; o.name = uniqueName(kPrimName[kind]); EditMesh m;
  switch (kind) {
    case PR_SPHERE: m = emLattice(8, true, 1.f); break; case PR_CUBE: m = emBox({1, 1, 1}); break;
    case PR_CYL: m = emCylinder(1.f, 2.f, 20); break; case PR_CONE: m = emCylinder(1.f, 2.f, 20, true); break;
    case PR_PLANE: m = emGrid(4, 4, 4.f, 4.f); break; case PR_TORUS: m = emTorus(1.f, 0.38f, 24, 12); break; default: m = emCar(); break;
  }
  o.em = std::make_shared<EditMesh>(m); o.smooth = (kind == PR_SPHERE || kind == PR_TORUS) ? 40.f : 30.f; if (kind == PR_CAR) o.subdiv = 0;
  rebuildObj(o); doc.objs.push_back(o); if (select) { doc.sel = (int)doc.objs.size() - 1; doc.selClip = -1; }
  return (int)doc.objs.size() - 1;
}

// ================================================================ timeline state + animation evaluation
static const int kFps = 30;
static int curFrame = 0; static bool playing = false; static float playAcc = 0.f; static bool loopPlay = true;
static float pxPerSec = 330.f;                 // design pixels per second (zoomed with a pinch)
static int clipEnd(const HClip& c) { return c.start + c.len; }
static int docEnd() {
  int e = 0;
  for (size_t i = 0; i < doc.objs.size(); i++) { const Obj& o = doc.objs[i]; for (size_t k = 0; k < o.clips.size(); k++) e = std::max(e, clipEnd(o.clips[k]));
    if (g_meOpen && o.id == g_meObj && g_meOutValid) e = std::max(e, (g_meOutN - 1) / kSub); else if (o.log && o.log->n > 0) e = std::max(e, (o.log->n - 1) / kSub); }
  return e;
}
static bool evalObjAtF(const Obj& o, float f, float* out) {
  if (g_meOpen && o.id == g_meObj && g_meOutValid && g_meOutN > 0) { for (int c = 0; c < 9; c++) out[c] = sampleStream(g_meOut[c], g_meOutN, f); return true; }
  if (o.log && o.log->n > 0) { for (int c = 0; c < 9; c++) out[c] = sampleStream(o.log->ch[c], o.log->n, f); return true; }
  const HClip* held = nullptr; int heldEnd = -1, firstStart = 1 << 30; const HClip* first = nullptr;
  for (size_t i = 0; i < o.clips.size(); i++) {
    const HClip& c = o.clips[i];
    if (f >= c.start && f < clipEnd(c)) return evalClipAt(c, f - c.start, out);
    if (clipEnd(c) <= f && clipEnd(c) > heldEnd && !c.keys.empty()) { held = &c; heldEnd = clipEnd(c); }
    if (c.start < firstStart && !c.keys.empty()) { first = &c; firstStart = c.start; }
  }
  if (held) return evalClipAt(*held, (float)held->len, out);
  if (first) return evalClipAt(*first, 0.f, out);
  return false;
}
static bool evalObjAt(const Obj& o, int f, float* out) { return evalObjAtF(o, (float)f, out); }
static void applyAnim() {
  for (size_t i = 0; i < doc.objs.size(); i++) { Obj& o = doc.objs[i]; float t[9]; if (evalObjAt(o, curFrame, t)) for (int k = 0; k < 9; k++) o.T[k] = t[k]; else for (int k = 0; k < 9; k++) o.T[k] = o.base[k]; }
}
static void setFrame(int f) { f = std::max(0, f); if (f != curFrame) { curFrame = f; applyAnim(); } }
static HClip* clipAt(Obj& o, int f) { for (size_t i = 0; i < o.clips.size(); i++) if (f >= o.clips[i].start && f <= clipEnd(o.clips[i])) return &o.clips[i]; return nullptr; }
static bool keyHere(Obj& o, HClip** cp = nullptr, int* rel = nullptr) {
  HClip* c = clipAt(o, curFrame); if (!c) return false; int r = curFrame - c->start;
  if (cp) *cp = c; if (rel) *rel = r; return c->keys.count(r) > 0;
}
static void putKey(Obj& o, HClip& c, int rel) {
  std::map<int, HKey>::iterator it = c.keys.find(rel);
  if (it == c.keys.end()) { HKey k; k.bz = kPresets[3].b; it = c.keys.insert(std::make_pair(rel, k)).first; }
  for (int i = 0; i < 9; i++) it->second.v[i] = o.T[i];
}
// Maya-style keys live on frames, not on a clip track: this returns the (implicit) clip that holds frame f, growing / creating it as needed
static HClip* mClip(Obj& o, int f) {
  if (HClip* c = clipAt(o, f)) return c;
  if (o.clips.empty()) { HClip c; c.start = 0; c.len = std::max(f, 2 * kFps); o.clips.push_back(c); return &o.clips[0]; }
  std::sort(o.clips.begin(), o.clips.end(), [](const HClip& a, const HClip& b) { return a.start < b.start; });
  if (f < o.clips[0].start) { HClip c; c.start = f; c.len = std::max(1, o.clips[0].start - f); o.clips.insert(o.clips.begin(), c); return &o.clips[0]; }
  size_t k = 0; for (size_t i = 0; i < o.clips.size(); i++) if (o.clips[i].start <= f) k = i;
  o.clips[k].len = f - o.clips[k].start; return &o.clips[k];
}
// an edit gesture finished: keep it (auto-key inside a clip, otherwise it becomes the base pose)
static void commitEdit(Obj& o) {
  if (o.log) { applyAnim(); setStatus("This model uses a motion log - edit it in the Motion Editor"); return; }
  if (g_maya && land) {   // Maya: auto key only keys objects that already have keys, otherwise the edit becomes the base pose
    bool animated = false; for (size_t i = 0; i < o.clips.size(); i++) if (!o.clips[i].keys.empty()) animated = true;
    if (!animated) { for (int i = 0; i < 9; i++) o.base[i] = o.T[i]; }
    else if (g_autoKey) { HClip* c2 = mClip(o, curFrame); putKey(o, *c2, curFrame - c2->start); }
    else { applyAnim(); setStatus("Auto key is off - use Set Key"); }
    return; }
  HClip* c = clipAt(o, curFrame);
  if (c && curFrame < clipEnd(*c)) { if (g_autoKey || c->keys.count(curFrame - c->start)) putKey(o, *c, curFrame - c->start); else { applyAnim(); setStatus("Auto-key is off - press Add KF"); } }
  else if (o.clips.empty()) for (int i = 0; i < 9; i++) o.base[i] = o.T[i];
  else { applyAnim(); setStatus("Move the playhead into a clip to animate"); }
}

// ================================================================ object transforms
static void mat3Mul(const float* a, const float* b, float* o) { for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) o[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j]; }
static void objR(const float* T, float* R) {   // R = Ry * Rx * Rz (degrees in T[3..5])
  float cx = cosf(T[3] * kDeg), sx = sinf(T[3] * kDeg), cy = cosf(T[4] * kDeg), sy = sinf(T[4] * kDeg), cz = cosf(T[5] * kDeg), sz = sinf(T[5] * kDeg);
  float Rx[9] = {1, 0, 0, 0, cx, -sx, 0, sx, cx}, Ry[9] = {cy, 0, sy, 0, 1, 0, -sy, 0, cy}, Rz[9] = {cz, -sz, 0, sz, cz, 0, 0, 0, 1}, t[9];
  mat3Mul(Ry, Rx, t); mat3Mul(t, Rz, R);
}
static V3 mulR(const float* R, V3 v) { return {R[0] * v.x + R[1] * v.y + R[2] * v.z, R[3] * v.x + R[4] * v.y + R[5] * v.z, R[6] * v.x + R[7] * v.y + R[8] * v.z}; }
static V3 mulRT(const float* R, V3 v) { return {R[0] * v.x + R[3] * v.y + R[6] * v.z, R[1] * v.x + R[4] * v.y + R[7] * v.z, R[2] * v.x + R[5] * v.y + R[8] * v.z}; }
static float safeS(float s) { return fabsf(s) < 1e-4f ? 1e-4f : s; }
static V3 toWorld(const Obj& o, V3 p) { float R[9]; objR(o.T, R); V3 r = mulR(R, {p.x * o.T[6], p.y * o.T[7], p.z * o.T[8]}); return {r.x + o.T[0], r.y + o.T[1], r.z + o.T[2]}; }
static V3 toLocal(const Obj& o, V3 w) { float R[9]; objR(o.T, R); V3 r = mulRT(R, {w.x - o.T[0], w.y - o.T[1], w.z - o.T[2]}); return {r.x / safeS(o.T[6]), r.y / safeS(o.T[7]), r.z / safeS(o.T[8])}; }
static V3 dirToLocal(const Obj& o, V3 d) { float R[9]; objR(o.T, R); V3 r = mulRT(R, d); return {r.x / safeS(o.T[6]), r.y / safeS(o.T[7]), r.z / safeS(o.T[8])}; }
static float maxScale(const Obj& o) { return std::max(fabsf(o.T[6]), std::max(fabsf(o.T[7]), fabsf(o.T[8]))); }
static V3 objCenterW(const Obj& o) { return toWorld(o, {o.cx, o.cy, o.cz}); }

static void worldMesh(const Obj& o, mx::Mesh3& out) {   // local render mesh -> world space, with the object colour
  const mx::Mesh3& m = *o.rm; float R[9]; objR(o.T, R); size_t n = m.vcount();
  out.pos.resize(n * 3); out.nrm.resize(n * 3);
  for (size_t i = 0; i < n; i++) {
    V3 p = mulR(R, {m.pos[i * 3] * o.T[6], m.pos[i * 3 + 1] * o.T[7], m.pos[i * 3 + 2] * o.T[8]});
    out.pos[i * 3] = p.x + o.T[0]; out.pos[i * 3 + 1] = p.y + o.T[1]; out.pos[i * 3 + 2] = p.z + o.T[2];
    V3 q = norm(mulR(R, {m.nrm[i * 3] / safeS(o.T[6]), m.nrm[i * 3 + 1] / safeS(o.T[7]), m.nrm[i * 3 + 2] / safeS(o.T[8])}));
    out.nrm[i * 3] = q.x; out.nrm[i * 3 + 1] = q.y; out.nrm[i * 3 + 2] = q.z;
  }
  out.idx = m.idx; out.uv = m.uv; out.tex = m.tex; out.tw = m.tw; out.th = m.th; out.metal = m.metal; out.rough = m.rough; out.pbr = m.pbr;
  if (o.colSet || (m.col.size() != n * 3 && m.tex.empty())) { out.col.resize(n * 3); for (size_t i = 0; i < n; i++) { out.col[i * 3] = o.col[0]; out.col[i * 3 + 1] = o.col[1]; out.col[i * 3 + 2] = o.col[2]; } }
  else out.col = m.col;
}

// ================================================================ viewport camera (full-screen; projection centre = the free area between the bars)
struct Cam { float yaw, pitch, dist; V3 tgt; Cam() : yaw(0.6f), pitch(0.36f), dist(16.f), tgt{0, 0, 0} {} };
static Cam cam;
static V3 E, Rv, Uv, Fv;
static SDL_Rect VP = {0, 0, 100, 100};            // free area: the projection centre and the mesh texture
static SDL_Color g_bg = C_VP; static bool showGrid = true, wireMode = false;
static void camSetup() {
  float cp = cosf(cam.pitch);
  E = {cam.tgt.x + cam.dist * cp * sinf(cam.yaw), cam.tgt.y + cam.dist * sinf(cam.pitch), cam.tgt.z + cam.dist * cp * cosf(cam.yaw)};
  Fv = norm(sub(cam.tgt, E)); Rv = norm(cross(Fv, {0, 1, 0})); Uv = cross(Rv, Fv);
}
static float focalPx() { return VP.h * 1.1f * mx::g_focalScale; }
static V3 toCam(V3 p) { V3 d = sub(p, E); return {dot(d, Rv), dot(d, Uv), dot(d, Fv)}; }
static void toScreenC(V3 c, int& sx, int& sy) { float f = focalPx(); sx = VP.x + VP.w / 2 + (int)lroundf(c.x / c.z * f); sy = VP.y + VP.h / 2 - (int)lroundf(c.y / c.z * f); }
static bool proj(V3 p, int& sx, int& sy, float* depth = nullptr) { V3 c = toCam(p); if (c.z < 0.2f) return false; toScreenC(c, sx, sy); if (depth) *depth = c.z; return true; }
static void line3(V3 a, V3 b, SDL_Color c, float w = 1.f) {
  V3 A = toCam(a), B = toCam(b); const float n = 0.2f;
  if (A.z < n && B.z < n) return;
  if (A.z < n) { float t = (n - A.z) / (B.z - A.z); A = {A.x + (B.x - A.x) * t, A.y + (B.y - A.y) * t, n}; }
  else if (B.z < n) { float t = (n - B.z) / (A.z - B.z); B = {B.x + (A.x - B.x) * t, B.y + (A.y - B.y) * t, n}; }
  int x1, y1, x2, y2; toScreenC(A, x1, y1); toScreenC(B, x2, y2);
  if (w > 1.5f) thickLine(x1, y1, x2, y2, w, c); else { setc(c); SDL_RenderDrawLine(R, x1, y1, x2, y2); }
}
static V3 rayDirAt(int sx, int sy) { float f = focalPx(); return norm(Fv + Rv * ((sx - VP.x - VP.w / 2) / f) + Uv * (-(sy - VP.y - VP.h / 2) / f)); }
static void fillPoly(const std::vector<std::pair<int, int> >& p, SDL_Color c) {   // even-odd scanline fill for projected faces
  if (p.size() < 3) return; int y0 = 1 << 30, y1 = -(1 << 30);
  for (size_t i = 0; i < p.size(); i++) { y0 = std::min(y0, p[i].second); y1 = std::max(y1, p[i].second); }
  y0 = std::max(y0, 0); setc(c);
  for (int y = y0; y <= y1; y++) {
    std::vector<int> xs;
    for (size_t i = 0; i < p.size(); i++) { std::pair<int, int> a = p[i], b = p[(i + 1) % p.size()]; if ((a.second <= y && b.second > y) || (b.second <= y && a.second > y)) xs.push_back(a.first + (int)((long long)(y - a.second) * (b.first - a.first) / (b.second - a.second))); }
    std::sort(xs.begin(), xs.end()); for (size_t i = 0; i + 1 < xs.size(); i += 2) { SDL_Rect r = {xs[i], y, xs[i + 1] - xs[i] + 1, 1}; SDL_RenderFillRect(R, &r); }
  }
}

// ---- viewport render cache: the software rasterizer (mesh_export.cpp) draws every object into one alpha texture
static SDL_Texture* vpTex = nullptr; static int vpTW = 0, vpTH = 0; static mx::Image vpImg; static uint64_t vpHash = 0, vpCamHash = 0;
static SDL_Rect vpSrc = {0, 0, 0, 0}; static bool vpLow = false, g_vpBusy = false, g_vpScrub = false; static Uint32 vpChangeT = 0;   // g_vpBusy: set each frame by drawViewport (camera drag / gizmo / clip move in progress)
//   // vpSrc: the part of the texture that holds the latest frame (half-size while moving)
static mx::ViewCam toViewCam(const Cam& c) { mx::ViewCam v; v.yaw = c.yaw; v.pitch = c.pitch; v.dist = c.dist; v.tx = c.tgt.x; v.ty = c.tgt.y; v.tz = c.tgt.z; return v; }
static uint64_t hmix(uint64_t h, uint64_t v) { return (h ^ v) * 1099511628211ull; }
static uint64_t hf32(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void worldMeshLite(const Obj& o, mx::Mesh3& out) {   // same transform as worldMesh, but triangles / UVs / texture stay in the source mesh (Scene3::topo): no per-frame copies
  const mx::Mesh3& m = *o.rm; float R[9]; objR(o.T, R); size_t n = m.vcount();
  out.pos.resize(n * 3); out.nrm.resize(n * 3);
  const float sx = o.T[6], sy = o.T[7], sz = o.T[8], isx = 1.f / safeS(sx), isy = 1.f / safeS(sy), isz = 1.f / safeS(sz);
  for (size_t i = 0; i < n; i++) {
    V3 p = mulR(R, {m.pos[i * 3] * sx, m.pos[i * 3 + 1] * sy, m.pos[i * 3 + 2] * sz});
    out.pos[i * 3] = p.x + o.T[0]; out.pos[i * 3 + 1] = p.y + o.T[1]; out.pos[i * 3 + 2] = p.z + o.T[2];
    V3 q = norm(mulR(R, {m.nrm[i * 3] * isx, m.nrm[i * 3 + 1] * isy, m.nrm[i * 3 + 2] * isz}));
    out.nrm[i * 3] = q.x; out.nrm[i * 3 + 1] = q.y; out.nrm[i * 3 + 2] = q.z;
  }
  out.metal = m.metal; out.rough = m.rough; out.pbr = m.pbr;
  if (o.colSet || (m.col.size() != n * 3 && m.tex.empty())) { out.col.resize(n * 3); for (size_t i = 0; i < n; i++) { out.col[i * 3] = o.col[0]; out.col[i * 3 + 1] = o.col[1]; out.col[i * 3 + 2] = o.col[2]; } }
  else out.col.clear();   // renderScene falls back to the source mesh's colours
}
static void renderDoc(mx::Image& im, const Cam& c, const Doc& d, bool wire) {
  static thread_local mx::Mesh3 wm;   // reused buffers (each render thread has its own)
  for (size_t i = 0; i < d.objs.size(); i++) {
    const Obj& o = d.objs[i]; if (o.hidden || !o.rm || o.rm->idx.empty()) continue;
    worldMeshLite(o, wm); mx::Scene3 sc; sc.mesh = &wm; sc.topo = o.rm.get(); sc.cam = toViewCam(c); sc.wire = wire; mx::renderScene(im, sc);
  }
}
// ================================================================ playback cache: a 121-slot ring buffer of rendered viewport frames
// Background threads render the frames just ahead of the playhead (slot = frame % 121) while the UI thread only uploads the finished frame -> the screen
// stays at 60 fps however heavy the model is. Resolution adapts so the workers keep up with real-time; if they cannot, playback waits (buffering) instead of dropping the UI.
struct CacheSlot { int frame, w, h; bool busy; std::vector<uint8_t> rgba; CacheSlot() : frame(-1), w(0), h(0), busy(false) {} };
struct FrameCache {
  enum { N = 121 };
  std::mutex mu; CacheSlot slot[N]; uint64_t gen, hash; std::shared_ptr<Doc> snap; Cam camS; bool wire; int rw, rh, endF;
  std::atomic<int> play; std::atomic<bool> active, run; std::atomic<float> scale; std::vector<std::thread> th; int nWorkers;
  FrameCache() : gen(0), hash(0), wire(false), rw(0), rh(0), endF(0), play(0), active(false), run(false), scale(0.75f), nWorkers(1) {}
  float eff() { return std::min((float)scale, std::min(1.f, sqrtf(280000.f / (float)std::max(1, rw * rh)))); }   // memory cap: <= ~1.1 MB per slot -> ~135 MB for all 121 slots
  void start() {
    if (!th.empty()) return; { mx::Image t; t.init(2, 2, 0, 0, 0, 0); mx::Scene3 e; mx::renderScene(t, e); }   // initialise renderer lookup tables on this thread before the workers start
    run = true; unsigned hc = std::thread::hardware_concurrency(); nWorkers = (int)std::min(3u, std::max(1u, hc > 1 ? hc - 1 : 1u));
    for (int i = 0; i < nWorkers; i++) th.emplace_back([this]() { worker(); });
  }
  void stop() { run = false; for (size_t i = 0; i < th.size(); i++) if (th[i].joinable()) th[i].join(); th.clear(); }
  void sync(uint64_t h, const Doc& d, const Cam& c, bool w, int rw_, int rh_, int end_) {   // new scene content -> drop every cached frame and re-snapshot
    std::lock_guard<std::mutex> l(mu); endF = end_;
    if (snap && h == hash) return;
    hash = h; gen++; snap = std::make_shared<Doc>(d); camS = c; wire = w; rw = rw_; rh = rh_;
    for (int i = 0; i < N; i++) { slot[i].frame = -1; slot[i].busy = false; }
  }
  bool hasFrame(int f) { std::lock_guard<std::mutex> l(mu); return snap && slot[f % N].frame == f; }
  bool fetch(int f, uint64_t h, SDL_Texture* tex, SDL_Rect* src) {   // UI thread: upload the finished frame
    std::lock_guard<std::mutex> l(mu); if (!snap || h != hash) return false; CacheSlot& S = slot[f % N]; if (S.frame != f || S.w <= 0) return false;
    *src = {0, 0, S.w, S.h}; SDL_UpdateTexture(tex, src, S.rgba.data(), S.w * 4); return true;
  }
  int count() { std::lock_guard<std::mutex> l(mu); int n = 0; for (int i = 0; i < N; i++) if (slot[i].frame >= 0) n++; return n; }
  void frames(int* out) { std::lock_guard<std::mutex> l(mu); for (int i = 0; i < N; i++) out[i] = slot[i].frame; }
  void worker() {
    Doc local; uint64_t localGen = ~0ull; mx::Image im; float avg = 0.f; int cnt = 0;
    while (run) {
      int f = -1, s = 0, w = 8, h = 8; uint64_t g = 0; std::shared_ptr<Doc> d; Cam cm; bool wr = false;
      if (active) {
        std::lock_guard<std::mutex> l(mu);
        if (snap && rw > 0) {
          g = gen; d = snap; cm = camS; wr = wire; float sc = eff(); w = std::max(8, (int)(rw * sc)); h = std::max(8, (int)(rh * sc));
          int hi = std::min(play.load() + N - 1, endF);
          for (int fr = play; fr <= hi; fr++) { CacheSlot& S = slot[fr % N]; if (S.frame == fr || S.busy) continue; S.busy = true; f = fr; s = fr % N; break; }
        }
      }
      if (f < 0) { SDL_Delay(2); continue; }
      Uint32 t0 = SDL_GetTicks();
      if (localGen != g) { local = *d; localGen = g; }
      for (size_t i = 0; i < local.objs.size(); i++) { Obj& o = local.objs[i]; float t[9]; if (evalObjAtF(o, (float)f, t)) for (int k = 0; k < 9; k++) o.T[k] = t[k]; else for (int k = 0; k < 9; k++) o.T[k] = o.base[k]; }
      im.init(w, h, 0, 0, 0, 0); renderDoc(im, cm, local, wr); mx::fxaa(im);
      { std::lock_guard<std::mutex> l(mu); CacheSlot& S = slot[s]; if (gen == g) { S.rgba.assign(im.rgba.begin(), im.rgba.end()); S.w = w; S.h = h; S.frame = f; } S.busy = false; }
      float ms = (float)(SDL_GetTicks() - t0); avg = avg == 0.f ? ms : avg * 0.85f + ms * 0.15f;
      if (++cnt % 6 == 0) {   // keep up with real time: trade resolution for speed, win it back when there is headroom
        float need = kFps * std::max(0.1f, g_speed), thr = nWorkers * 1000.f / std::max(1.f, avg), sc = scale;
        if (thr < need * 1.25f) scale = std::max(0.35f, sc * 0.9f); else if (thr > need * 2.5f && sc < 1.f) scale = std::min(1.f, sc * 1.05f);
      }
    }
  }
};
static FrameCache g_fc; static bool g_cacheOn = true, g_cacheStuck = false;
static uint64_t cacheHash(int w, int h) {   // everything that changes the picture EXCEPT the current time
  uint64_t hh = 0x9E3779B97F4A7C15ull;
  for (float v : {cam.yaw, cam.pitch, cam.dist, cam.tgt.x, cam.tgt.y, cam.tgt.z, mx::g_focalScale}) hh = hmix(hh, hf32(v));
  hh = hmix(hh, (uint64_t)w * 7919 + h); hh = hmix(hh, (wireMode ? 1 : 0) | (mx::g_baseLight ? 2 : 0));
  for (size_t i = 0; i < doc.objs.size(); i++) { const Obj& o = doc.objs[i]; hh = hmix(hh, (uint64_t)(uintptr_t)o.rm.get()); hh = hmix(hh, (o.hidden ? 5 : 9) + (o.colSet ? 2 : 0));
    for (int k = 0; k < 9; k++) hh = hmix(hh, hf32(o.base[k])); for (int k = 0; k < 3; k++) hh = hmix(hh, o.col[k]); hh = hmix(hh, (uint64_t)(uintptr_t)o.log.get());
    for (size_t c = 0; c < o.clips.size(); c++) { const HClip& cl = o.clips[c]; hh = hmix(hh, (uint64_t)cl.start * 131 + cl.len);
      for (std::map<int, HKey>::const_iterator k = cl.keys.begin(); k != cl.keys.end(); ++k) { hh = hmix(hh, (uint64_t)k->first * 31 + k->second.mode); for (int q = 0; q < 9; q++) hh = hmix(hh, hf32(k->second.v[q])); hh = hmix(hh, hf32(k->second.bz.x1) ^ hf32(k->second.bz.y1)); hh = hmix(hh, hf32(k->second.bz.x2) ^ hf32(k->second.bz.y2)); } } }
  return hh;
}
static bool anyAnimated() { for (size_t i = 0; i < doc.objs.size(); i++) if (!doc.objs[i].clips.empty() || doc.objs[i].log) return true; return false; }
static bool cacheWaiting(int nf, bool modelingNow) { return anyAnimated() && g_cacheOn && !g_cacheStuck && !modelingNow && !g_meOpen && !doc.objs.empty() && !g_fc.hasFrame(nf); }
static void updateViewportTexture(bool modelingActive) {
  int div = (long)VP.w * VP.h > 700000 ? 2 : 1; int w = std::max(8, VP.w / div), h = std::max(8, VP.h / div);
  uint64_t hc = 1469598103934665603ull;
  for (float v : {cam.yaw, cam.pitch, cam.dist, cam.tgt.x, cam.tgt.y, cam.tgt.z}) hc = hmix(hc, hf32(v));
  uint64_t hh = hc;
  hh = hmix(hh, (uint64_t)w * 7919 + h); hh = hmix(hh, wireMode ? 1 : 0); hh = hmix(hh, modelingActive ? 3 : 0); hh = hmix(hh, mx::g_baseLight ? 11 : 13);
  for (size_t i = 0; i < doc.objs.size(); i++) { const Obj& o = doc.objs[i]; hh = hmix(hh, (uint64_t)(uintptr_t)o.rm.get()); hh = hmix(hh, o.hidden ? 5 : 9);
    for (int k = 0; k < 9; k++) hh = hmix(hh, hf32(o.T[k])); for (int k = 0; k < 3; k++) hh = hmix(hh, o.col[k]); }
  auto ensureTex = [&]() {
    if (!vpTex || vpTW != w || vpTH != h) {
      if (vpTex) SDL_DestroyTexture(vpTex);
      SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
      vpTex = SDL_CreateTexture(R, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, w, h);
      if (vpTex) SDL_SetTextureBlendMode(vpTex, SDL_BLENDMODE_BLEND);
      vpTW = w; vpTH = h;
    }
  };
  const bool cacheOk = g_cacheOn && !g_cacheStuck && !modelingActive && !g_meOpen && !doc.objs.empty() && anyAnimated();
  g_fc.active = playing && cacheOk;
  if (cacheOk && (playing || g_vpScrub)) {   // cached playback / scrubbing: show a finished frame from the ring buffer, never render on the UI thread
    uint64_t ch = cacheHash(w, h);
    if (playing) { g_fc.start(); g_fc.sync(ch, doc, cam, wireMode, w, h, std::max(docEnd(), kFps)); g_fc.play = curFrame; }
    ensureTex(); SDL_Rect src;
    if (vpTex && g_fc.fetch(curFrame, ch, vpTex, &src)) { vpSrc = src; return; }
    if (playing) return;   // buffering: keep the last frame on screen
  }
  Uint32 now = SDL_GetTicks(); bool changed = hh != vpHash || !vpTex;
  if (changed) {   // while the camera moves, an object is dragged or the timeline plays: draw at half size, sharpen once things settle
    bool busyMoving = hc != vpCamHash || playing || g_vpBusy;
    vpLow = vpTex && busyMoving && (long)w * h > 250000; vpHash = hh; vpCamHash = hc; vpChangeT = now;
  } else if (vpLow && now - vpChangeT >= 180) vpLow = false;
  else return;
  int rw = vpLow ? std::max(8, w / 2) : w, rh = vpLow ? std::max(8, h / 2) : h;
  vpImg.init(rw, rh, 0, 0, 0, 0); renderDoc(vpImg, cam, doc, wireMode); if (!vpLow) mx::fxaa(vpImg);
  ensureTex();
  vpSrc = {0, 0, rw, rh};
  if (vpTex) SDL_UpdateTexture(vpTex, &vpSrc, vpImg.rgba.data(), rw * 4);
}

// ---- grid (fades into the distance like Prisma), axes, skeleton overlay
static void drawGrid() {
  const int N = 28; const float fadeD = 30.f;
  for (int i = -N; i <= N; i++) {
    for (int axis = 0; axis < 2; axis++) {
      for (int s = -N; s < N; s += 2) {
        V3 a = axis ? V3{(float)s, 0, (float)i} : V3{(float)i, 0, (float)s}, b = axis ? V3{(float)s + 2, 0, (float)i} : V3{(float)i, 0, (float)s + 2};
        float mx_ = (a.x + b.x) * 0.5f - cam.tgt.x, mz_ = (a.z + b.z) * 0.5f - cam.tgt.z, d = sqrtf(mx_ * mx_ + mz_ * mz_);
        float f = 1.f - d / fadeD; if (f <= 0) continue; int al = (int)(42.f * f * f);
        if (al < 2) continue; line3(a, b, {255, 255, 255, (Uint8)al});
      }
    }
  }
}
static void drawAxes() { line3({0, 0, 0}, {1.5f, 0, 0}, C_AXR, 2.2f * U); line3({0, 0, 0}, {0, 1.5f, 0}, C_AXG, 2.2f * U); line3({0, 0, 0}, {0, 0, 1.5f}, C_AXB, 2.2f * U); }
static void drawSkeletons() {
  for (size_t i = 0; i < doc.objs.size(); i++) { const Obj& o = doc.objs[i]; if (!o.skel || o.hidden) continue;
    for (size_t b = 0; b < o.skel->size(); b++) { const SkelBone& k = (*o.skel)[b]; if (k.parent < 0 || k.parent >= (int)o.skel->size()) continue;
      const SkelBone& p = (*o.skel)[k.parent]; line3(toWorld(o, {p.x, p.y, p.z}), toWorld(o, {k.x, k.y, k.z}), {235, 235, 245, 255}, 1.5f * U); } }
}

// ================================================================ picking
static bool rayTri(V3 ro, V3 rd, V3 a, V3 b, V3 c, float& t) {
  V3 e1 = b - a, e2 = c - a, p = cross(rd, e2); float det = dot(e1, p); if (fabsf(det) < 1e-9f) return false;
  float inv = 1.f / det; V3 s = ro - a; float u = dot(s, p) * inv; if (u < 0 || u > 1) return false;
  V3 q = cross(s, e1); float v = dot(rd, q) * inv; if (v < 0 || u + v > 1) return false; t = dot(e2, q) * inv; return t > 1e-4f;
}
static bool rayObj(const Obj& o, V3 ro, V3 rd, float& tBest) {   // ray against the object's render mesh (ray moved into local space)
  if (!o.rm || o.hidden) return false; V3 lo = toLocal(o, ro), ld = dirToLocal(o, rd); const mx::Mesh3& m = *o.rm; bool hit = false; tBest = 1e30f;
  for (size_t t = 0; t + 2 < m.idx.size(); t += 3) {
    uint32_t i0 = m.idx[t], i1 = m.idx[t + 1], i2 = m.idx[t + 2]; float tt;
    if (rayTri(lo, ld, {m.pos[i0 * 3], m.pos[i0 * 3 + 1], m.pos[i0 * 3 + 2]}, {m.pos[i1 * 3], m.pos[i1 * 3 + 1], m.pos[i1 * 3 + 2]}, {m.pos[i2 * 3], m.pos[i2 * 3 + 1], m.pos[i2 * 3 + 2]}, tt) && tt < tBest) { tBest = tt; hit = true; }
  }
  return hit;
}
static int pickObject(int sx, int sy) {
  V3 rd = rayDirAt(sx, sy); int best = -1; float bt = 1e30f;
  for (size_t i = 0; i < doc.objs.size(); i++) { float t; if (rayObj(doc.objs[i], E, rd, t) && t < bt) { bt = t; best = (int)i; } }
  return best;
}

// ================================================================ gizmo (move / rotate / scale), screen-constant size
static float gzR[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
static int gzAxis = -1, gzPrevX = 0, gzPrevY = 0; static float gzAccum = 0; static V3 gzPivot = {0, 0, 0}; static bool gzActive = false;
static V3 axisDir(int a) { return {gzR[a], gzR[3 + a], gzR[6 + a]}; }
static SDL_Color axisCol(int a) { return a == 0 ? C_AXR : a == 1 ? C_AXG : C_AXB; }
static float gzWorldLen(V3 pivot) { float d = std::max(0.5f, dot(sub(pivot, E), Fv)); return P(150.f) * d / focalPx(); }
static float distPtSeg(float px, float py, float ax, float ay, float bx, float by) {
  float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy, t = l2 > 0 ? clampf(((px - ax) * dx + (py - ay) * dy) / l2, 0, 1) : 0; float qx = ax + dx * t - px, qy = ay + dy * t - py; return sqrtf(qx * qx + qy * qy);
}
static void ringPts(V3 pv, int a, float r, std::vector<V3>& out) {
  out.clear(); V3 u = axisDir((a + 1) % 3), v = axisDir((a + 2) % 3);
  for (int i = 0; i <= 48; i++) { float t = 2 * kPI * i / 48; out.push_back(pv + u * (r * cosf(t)) + v * (r * sinf(t))); }
}
static void drawGizmo(V3 pv, int tool) {
  float L = gzWorldLen(pv); int px, py; if (!proj(pv, px, py)) return;
  for (int a = 0; a < 3; a++) {
    SDL_Color c = axisCol(a); if (gzActive && gzAxis == a) c = {255, 220, 90, 255};
    V3 d = axisDir(a);
    if (tool == 1) {   // rotate: three rings
      std::vector<V3> pts; ringPts(pv, a, L * 0.8f, pts);
      for (size_t i = 0; i + 1 < pts.size(); i++) line3(pts[i], pts[i + 1], c, 2.6f * U);
      continue;
    }
    V3 tip = pv + d * L; int tx, ty; if (!proj(tip, tx, ty)) continue;
    thickLine(px, py, tx, ty, 2.6f * U, c);
    float dx = (float)(tx - px), dy = (float)(ty - py), l = sqrtf(dx * dx + dy * dy); if (l < 1) continue; dx /= l; dy /= l;
    float hl = P(34.f), hw = P(13.f);
    if (tool == 0) { uif::Polys Pl(1); float bx = tx - dx * hl, by = ty - dy * hl; Pt pp[3] = {{(float)tx, (float)ty}, {bx - dy * hw, by + dx * hw}, {bx + dy * hw, by - dx * hw}};
      int x0 = (int)std::min(pp[0].x, std::min(pp[1].x, pp[2].x)) - 2, y0 = (int)std::min(pp[0].y, std::min(pp[1].y, pp[2].y)) - 2, w = (int)(std::max(pp[0].x, std::max(pp[1].x, pp[2].x))) - x0 + 3, h = (int)(std::max(pp[0].y, std::max(pp[1].y, pp[2].y))) - y0 + 3;
      for (int i = 0; i < 3; i++) { Pt q = {pp[i].x - x0, pp[i].y - y0}; Pl[0].push_back(q); } orient(Pl);
      std::vector<uint8_t> al; uif::rasterPolys(Pl, w, h, al, 1.0f); SDL_Texture* t = texFromAlpha(al, w, h);
      if (t) { SDL_Rect dr = {x0, y0, w, h}; SDL_SetTextureColorMod(t, c.r, c.g, c.b); SDL_RenderCopy(R, t, nullptr, &dr); SDL_DestroyTexture(t); } }
    else { int s = P(11.f); fillR({tx - s, ty - s, 2 * s, 2 * s}, c); }
  }
  if (tool == 2) discFill(px, py, P(12.f), C_TXT);
}
// returns 0..2 for an axis / ring, 3 for the uniform-scale centre, -1 for no hit
static int gizmoHit(V3 pv, int tool, int mx_, int my_) {
  float L = gzWorldLen(pv); int px, py; if (!proj(pv, px, py)) return -1; float best = (float)P(26.f); int bi = -1;
  if (tool == 2 && hypotf((float)(mx_ - px), (float)(my_ - py)) < P(22.f)) return 3;
  for (int a = 0; a < 3; a++) {
    if (tool == 1) { std::vector<V3> pts; ringPts(pv, a, L * 0.8f, pts); int x0 = 0, y0 = 0; bool have = false;
      for (size_t i = 0; i < pts.size(); i++) { int x1, y1; if (!proj(pts[i], x1, y1)) { have = false; continue; } if (have) { float d = distPtSeg((float)mx_, (float)my_, (float)x0, (float)y0, (float)x1, (float)y1); if (d < best) { best = d; bi = a; } } x0 = x1; y0 = y1; have = true; } }
    else { int tx, ty; if (!proj(pv + axisDir(a) * L, tx, ty)) continue; float d = distPtSeg((float)mx_, (float)my_, (float)px, (float)py, (float)tx, (float)ty); if (d < best) { best = d; bi = a; } }
  }
  return bi;
}
// one drag step: returns the amount for the axis (world units for move, degrees for rotate, factor for scale)
static float gizmoDragAmount(V3 pv, int tool, int axis, int mx_, int my_) {
  int px, py; if (!proj(pv, px, py)) return 0;
  if (tool == 1) {
    float a0 = atan2f((float)(gzPrevY - py), (float)(gzPrevX - px)), a1 = atan2f((float)(my_ - py), (float)(mx_ - px)), da = a1 - a0;
    while (da > kPI) da -= 2 * kPI; while (da < -kPI) da += 2 * kPI;
    bool toward = dot(axisDir(axis), Fv) < 0; return (toward ? -da : da) * 57.29578f;
  }
  int tx, ty; if (!proj(pv + axisDir(axis), tx, ty)) return 0;
  float sx = (float)(tx - px), sy = (float)(ty - py), l2 = sx * sx + sy * sy; if (l2 < 1e-3f) return 0;
  float amt = ((mx_ - gzPrevX) * sx + (my_ - gzPrevY) * sy) / l2;   // world units along the axis
  if (tool == 2) { float L = gzWorldLen(pv); return amt / std::max(0.05f, L); }
  return amt;
}

// ================================================================ input + immediate-mode touch widgets
struct Input { int mx = 0, my = 0; bool down = false, pressed = false, released = false; float wheel = 0, pinch = 0, panX = 0, panY = 0; bool multi = false; } in;
static int pressX = 0, pressY = 0; static Uint32 pressT = 0; static bool moved = false, tapNow = false, consumed = false, g_lock = false;
static int activeId = 0; static SDL_Rect g_block = {0, 0, 0, 0}; static bool g_blockOn = false;
enum { ID_NONE = 0, ID_TL = 10, ID_HANDLE_L, ID_HANDLE_R, ID_CLIPMOVE, ID_KEYMOVE, ID_VIEW, ID_GIZMO, ID_SLIDER = 100, ID_SCROLL = 200, ID_GRAPH = 300, ID_ME = 400 };
static SDL_Rect g_clip = {0, 0, 0, 0}; static bool g_clipOn = false;   // taps outside this rect are ignored (clipped scroll areas)
static bool blockedAt(int x, int y) { return (g_blockOn && inside(g_block, x, y)) || (g_clipOn && !inside(g_clip, x, y)); }
static bool tapIn(SDL_Rect r) { if (g_lock || consumed || !tapNow) return false; if (blockedAt(pressX, pressY) || !inside(r, pressX, pressY)) return false; consumed = true; return true; }
static bool pressIn(SDL_Rect r) { return !g_lock && in.pressed && !consumed && !blockedAt(in.mx, in.my) && inside(r, in.mx, in.my); }
static bool heldIn(SDL_Rect r) { return !g_lock && in.down && inside(r, pressX, pressY) && inside(r, in.mx, in.my) && !blockedAt(pressX, pressY); }
static bool overUI(int x, int y) { return g_lock || blockedAt(x, y); }
static std::string fmtF(float v, int d = 2) { char b[32]; snprintf(b, sizeof b, "%.*f", d, v); return b; }

// toolbar item: icon on top, label below. Returns true on tap.
static bool toolItem(SDL_Rect r, int ic, const std::string& label, bool active, bool dim, const std::string& num = "", SDL_Color iconCol = {232, 232, 234, 255}) {
  bool tap = tapIn(r), held = heldIn(r);
  if (active || held) fillRound({r.x + P(6), r.y + P(4), r.w - P(12), r.h - P(8)}, P(12), {44, 44, 46, 255});
  SDL_Color tc = dim ? alpha(C_OFF, 255) : (active ? C_WHITE : C_TXT), icc = dim ? C_OFF : (active ? C_WHITE : iconCol);
  int cy = r.y + r.h * 38 / 100;
  if (!num.empty()) txtC({r.x, cy - P(22), r.w, P(44)}, num, dim ? C_OFF : C_TXT, FPX(40)); else if (ic >= 0) icon(ic, r.x + r.w / 2, cy, P(44), icc);
  int lpx = textW(label, FPX(21)) > r.w - P(8) ? FPX(17) : FPX(21);
  txtC({r.x, r.y + r.h * 76 / 100 - P(12), r.w, P(24)}, fitTxt(label, r.w - P(4), lpx), tc, lpx);
  return tap;
}
static bool roundIconBtn(SDL_Rect r, int ic, SDL_Color bg, SDL_Color fg, int isz, int rad = 12) {
  bool tap = tapIn(r), held = heldIn(r); SDL_Color b = bg; if (held) { b.r = (Uint8)std::min(255, b.r + 22); b.g = (Uint8)std::min(255, b.g + 22); b.b = (Uint8)std::min(255, b.b + 22); }
  fillRound(r, P((float)rad), b); icon(ic, r.x + r.w / 2, r.y + r.h / 2, isz, fg); return tap;
}
static bool sliderH(int id, SDL_Rect r, float& v, float lo, float hi, SDL_Color fill, bool isInt = false, bool undo = true) {
  bool changed = false; SDL_Rect hit = {r.x - P(10), r.y - P(14), r.w + P(20), r.h + P(28)};
  if (pressIn(hit) && activeId == 0) { activeId = id; if (undo) pushUndo(); }
  if (activeId == id && (in.down || in.released)) {
    float t = clampf((in.mx - r.x) / (float)std::max(1, r.w), 0, 1), nv = lo + t * (hi - lo); if (isInt) nv = roundf(nv);
    if (nv != v) { v = nv; changed = true; }
  }
  int cy = r.y + r.h / 2, th = P(6); float t = clampf((v - lo) / (hi - lo), 0, 1); int kx = r.x + (int)(t * r.w);
  fillRound({r.x, cy - th / 2, r.w, th}, th / 2, {52, 52, 54, 255});
  if (kx > r.x) fillRound({r.x, cy - th / 2, kx - r.x, th}, th / 2, fill);
  discFill(kx, cy, P(19.f), activeId == id ? SDL_Color{200, 200, 204, 255} : SDL_Color{150, 150, 152, 255});
  return changed;
}
// ================================================================ app state
enum { SH_NONE, SH_MODELS, SH_SKY, SH_CAMERA, SH_MATERIAL, SH_OUTLINER, SH_EXPORT, SH_HOME, SH_SMOOTH, SH_GRAPH, SH_FILE };
static int sheet = SH_NONE; static int tool = 0;        // 0 move, 1 rotate, 2 scale
static bool modeling = false; static int selMode = 0;   // 0 object, 1 face, 2 edge, 3 vertex
static std::set<int> mF, mV; static std::set<std::pair<int, int> > mE;
static bool gzLocal = false, pivotSel = true, showBones = true;
static int W = 720, H = 1600;
enum { SCR_HOME, SCR_EDITOR }; static int screen = SCR_HOME;
static void goHome(); static bool dangerButton(SDL_Rect r, const std::string& label);
static void openMotionEditor(); static void meRequestExit(); static void meGizmoCommit(Obj& o); static void meForceClose();
static SDL_Rect rMePanel, rMeHeader, rMeWeight, rMeLane[3], rMeSliders; static int meLabelW = 0;
static SDL_Rect rBar, rTrack, rRuler, rLeft; static int ctrlY = 0, phx = 0, trackClipH = 84, plusW = 0, rowCy = 0;
static SDL_Rect rHome, rShare, rList, rUndo, rRedo, rPlay, rKey, rGraphBtn, rPlus;
static float barScroll = 0.f;
static std::vector<SDL_Rect> uiRects;                    // areas where a press must not orbit the viewport
static void reg(SDL_Rect r) { uiRects.push_back(r); }

static void clearModelSel() { mF.clear(); mV.clear(); mE.clear(); }
static void leaveModeling() { modeling = false; clearModelSel(); selMode = 0; if (sheet == SH_SMOOTH) sheet = SH_NONE; }
static std::pair<int, int> edgeKey(int a, int b) { return std::make_pair(std::min(a, b), std::max(a, b)); }
static void selectedVerts(const EditMesh& m, std::set<int>& out) {
  out = mV; for (std::set<std::pair<int, int> >::iterator it = mE.begin(); it != mE.end(); ++it) { out.insert(it->first); out.insert(it->second); }
  for (std::set<int>::iterator it = mF.begin(); it != mF.end(); ++it) if (*it < (int)m.f.size()) for (size_t k = 0; k < m.f[*it].size(); k++) out.insert(m.f[*it][k]);
}
static bool hasCompSel() { return !mF.empty() || !mV.empty() || !mE.empty(); }
static V3 pivotWorld(const Obj& o) {
  if (modeling && selMode != 0 && o.em && hasCompSel() && pivotSel) { std::set<int> s; selectedVerts(*o.em, s); V3 c = {0, 0, 0}; for (std::set<int>::iterator it = s.begin(); it != s.end(); ++it) c = c + o.em->v[*it]; if (!s.empty()) c = c * (1.f / s.size()); return toWorld(o, c); }
  return V3{o.T[0], o.T[1], o.T[2]};
}
static void setAxisFrame(const Obj* o) {
  if (gzLocal && o) objR(o->T, gzR); else { float id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; memcpy(gzR, id, sizeof id); }
}
static void cowEM(Obj& o) { if (o.em) o.em = std::make_shared<EditMesh>(*o.em); }

// ---- edits from the gizmo (object level or selected components)
static void applyDelta(Obj& o, int axis, float amt) {
  bool comp = modeling && selMode != 0 && o.em && hasCompSel();
  if (!comp) {
    if (tool == 0) { V3 d = axisDir(axis) * amt; o.T[0] += d.x; o.T[1] += d.y; o.T[2] += d.z; }
    else if (tool == 1) o.T[3 + axis] += amt;
    else { float f = std::max(0.05f, 1.f + amt); if (axis == 3) { for (int i = 6; i < 9; i++) o.T[i] = std::max(0.02f, o.T[i] * f); } else o.T[6 + axis] = std::max(0.02f, o.T[6 + axis] * f); }
    return;
  }
  std::set<int> s; selectedVerts(*o.em, s); V3 pv = pivotWorld(o); V3 ax = axis < 3 ? axisDir(axis) : V3{1, 0, 0};
  for (std::set<int>::iterator it = s.begin(); it != s.end(); ++it) {
    V3& p = o.em->v[*it]; V3 w = toWorld(o, p);
    if (tool == 0) w = w + ax * amt;
    else if (tool == 1) { V3 r = w - pv, k = norm(ax); float a = amt * kDeg, c = cosf(a), sn = sinf(a); r = r * c + cross(k, r) * sn + k * (dot(k, r) * (1 - c)); w = pv + r; }
    else { float f = std::max(0.05f, 1.f + amt); V3 r = w - pv; if (axis == 3) r = r * f; else { V3 k = norm(ax); float al = dot(r, k); r = r + k * (al * (f - 1.f)); } w = pv + r; }
    p = toLocal(o, w);
  }
  rebuildObj(o);
}

// ================================================================ object / clip commands
static void deleteSelected() {
  if (doc.sel < 0 || doc.sel >= (int)doc.objs.size()) return; pushUndo();
  doc.objs.erase(doc.objs.begin() + doc.sel); doc.sel = -1; doc.selClip = -1; leaveModeling(); setStatus("Deleted");
}
static void duplicateSelected() {
  Obj* o = selObj(); if (!o) return; pushUndo(); Obj c = *o; c.id = doc.nextId++; c.name = uniqueName(o->name.substr(0, o->name.find(' ')));
  if (c.em) c.em = std::make_shared<EditMesh>(*c.em);
  c.T[0] += 2.2f; c.base[0] += 2.2f; for (size_t i = 0; i < c.clips.size(); i++) for (std::map<int, HKey>::iterator k = c.clips[i].keys.begin(); k != c.clips[i].keys.end(); ++k) k->second.v[0] += 2.2f;
  doc.objs.push_back(c); doc.sel = (int)doc.objs.size() - 1; doc.selClip = -1; setStatus("Duplicated");
}
static void addClipToSelected() {
  Obj* o = selObj(); if (!o) return; pushUndo();
  HClip c; c.start = curFrame; for (size_t i = 0; i < o->clips.size(); i++) if (curFrame >= o->clips[i].start && curFrame < clipEnd(o->clips[i])) c.start = clipEnd(o->clips[i]);
  c.len = 2 * kFps; for (size_t i = 0; i < o->clips.size(); i++) if (o->clips[i].start >= c.start) c.len = std::min(c.len, std::max(5, o->clips[i].start - c.start));
  o->clips.push_back(c); std::sort(o->clips.begin(), o->clips.end(), [](const HClip& a, const HClip& b) { return a.start < b.start; });
  for (size_t i = 0; i < o->clips.size(); i++) if (o->clips[i].start == c.start) doc.selClip = (int)i;
}
static void copyClip() {
  Obj* o = selObj(); if (!o || doc.selClip < 0 || doc.selClip >= (int)o->clips.size()) return; pushUndo();
  HClip c = o->clips[doc.selClip]; int e = 0; for (size_t i = 0; i < o->clips.size(); i++) e = std::max(e, clipEnd(o->clips[i])); c.start = e;
  o->clips.push_back(c); doc.selClip = (int)o->clips.size() - 1; setStatus("Clip copied");
}
static void cutClip() {
  Obj* o = selObj(); if (!o || doc.selClip < 0 || doc.selClip >= (int)o->clips.size()) return; HClip& c = o->clips[doc.selClip];
  if (curFrame <= c.start || curFrame >= clipEnd(c)) { setStatus("Put the playhead inside the clip to cut"); return; }
  pushUndo(); int split = curFrame - c.start; HClip n; n.start = curFrame; n.len = clipEnd(c) - curFrame;
  float v0[9], v1[9]; bool h0 = evalClipAt(c, split - 1, v0), h1 = evalClipAt(c, split, v1);
  for (std::map<int, HKey>::iterator it = c.keys.begin(); it != c.keys.end(); ++it) if (it->first >= split) n.keys[it->first - split] = it->second;
  for (std::map<int, HKey>::iterator it = c.keys.begin(); it != c.keys.end();) { if (it->first >= split) it = c.keys.erase(it); else ++it; }
  if (h0) { HKey k; for (int i = 0; i < 9; i++) k.v[i] = v0[i]; k.bz = kPresets[3].b; c.keys[split - 1] = k; }
  if (h1 && !n.keys.count(0)) { HKey k; for (int i = 0; i < 9; i++) k.v[i] = v1[i]; k.bz = kPresets[3].b; n.keys[0] = k; }
  c.len = split; o->clips.insert(o->clips.begin() + doc.selClip + 1, n); setStatus("Clip cut");
}
static void deleteClip() { Obj* o = selObj(); if (!o || doc.selClip < 0 || doc.selClip >= (int)o->clips.size()) return; pushUndo(); o->clips.erase(o->clips.begin() + doc.selClip); doc.selClip = -1; applyAnim(); }
static void toggleKey() {
  Obj* o = selObj(); if (!o) return; HClip* c = nullptr; int rel = 0; bool has = keyHere(*o, &c, &rel);
  if (!c) { setStatus("Move the playhead into a clip"); return; } pushUndo();
  if (has) c->keys.erase(rel); else putKey(*o, *c, rel);
  applyAnim();
}
static void hideSelectedFaces() {
  Obj* o = selObj(); if (!o || !o->em) return; pushUndo(); cowEM(*o);
  if (mF.empty()) { for (size_t i = 0; i < o->em->hid.size(); i++) o->em->hid[i] = 0; setStatus("All faces visible"); }
  else { for (std::set<int>::iterator it = mF.begin(); it != mF.end(); ++it) o->em->hid[*it] = 1; mF.clear(); setStatus("Faces hidden"); }
  rebuildObj(*o);
}
static void duplicateFaces() {
  Obj* o = selObj(); if (!o || !o->em || mF.empty()) { duplicateSelected(); return; } pushUndo(); cowEM(*o);
  std::map<int, int> remap; std::set<int> nf;
  for (std::set<int>::iterator it = mF.begin(); it != mF.end(); ++it) { Face q = o->em->f[*it];
    for (size_t k = 0; k < q.size(); k++) { int& r = remap[q[k]]; if (r == 0 && !remap.count(-1 - q[k])) { r = (int)o->em->v.size() + 1; remap[-1 - q[k]] = 1; V3 p = o->em->v[q[k]]; o->em->v.push_back(p); } q[k] = remap[q[k]] - 1; }
    o->em->f.push_back(q); o->em->hid.push_back(0); nf.insert((int)o->em->f.size() - 1); }
  mF = nf; rebuildObj(*o); setStatus("Faces duplicated");
}
static void doUndo() { if (undoS.empty()) return; redoS.push_back(doc); doc = undoS.back(); undoS.pop_back(); clearModelSel(); applyAnim(); vpHash = 0; }
static void doRedo() { if (redoS.empty()) return; undoS.push_back(doc); doc = redoS.back(); redoS.pop_back(); clearModelSel(); applyAnim(); vpHash = 0; }
static void newScene() { pushUndo(); doc.objs.clear(); doc.sel = -1; doc.selClip = -1; setFrame(0); leaveModeling(); }

// ================================================================ model import (.mdl / .obj / .glb / .zip / .prisma)
static std::string extLower(const std::string& p) {
  size_t d = p.rfind('.'), s = p.find_last_of("/\\"); if (d == std::string::npos || (s != std::string::npos && d < s)) return "";
  std::string e = p.substr(d); for (auto& c : e) c = (char)tolower((unsigned char)c); return e;
}
static std::vector<SkelBone> rawSkeleton(const mdl::Model& m) {      // world bone positions, Y-up, unscaled
  size_t n = m.bones.size(); std::vector<mdl::Quat> wq(n); std::vector<mdl::Vec3> wp(n);
  auto mul = [](const mdl::Quat& a, const mdl::Quat& b) { return mdl::Quat{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z}; };
  auto rot = [](const mdl::Quat& q, const mdl::Vec3& v) { mdl::Vec3 c{q.y * v.z - q.z * v.y, q.z * v.x - q.x * v.z, q.x * v.y - q.y * v.x}; mdl::Vec3 d{q.y * c.z - q.z * c.y, q.z * c.x - q.x * c.z, q.x * c.y - q.y * c.x};
    return mdl::Vec3{v.x + 2 * (q.w * c.x + d.x), v.y + 2 * (q.w * c.y + d.y), v.z + 2 * (q.w * c.z + d.z)}; };
  for (size_t i = 0; i < n; i++) { const mdl::Bone& b = m.bones[i];
    if (b.parent < 0) { wq[i] = b.rot; wp[i] = b.pos; } else { mdl::Vec3 r = rot(wq[b.parent], b.pos); wp[i] = {wp[b.parent].x + r.x, wp[b.parent].y + r.y, wp[b.parent].z + r.z}; wq[i] = mul(wq[b.parent], b.rot); } }
  std::vector<SkelBone> sk(n); for (size_t i = 0; i < n; i++) sk[i] = {wp[i].x, wp[i].z, -wp[i].y, m.bones[i].parent}; return sk;
}
static void addImported(mx::Mesh3& mesh, const std::string& name, const std::vector<SkelBone>* sk, const mx::Fit* fit) {
  mx::calcBounds(mesh); mx::Fit f = fit ? *fit : mx::makeFit(mesh.bmin, mesh.bmax); mx::applyFit(mesh, f);
  Obj o; o.id = doc.nextId++; o.name = uniqueName(name.empty() ? "Model" : name); o.imp = std::make_shared<mx::Mesh3>(); o.imp->take(mesh); o.colSet = o.imp->col.empty() && o.imp->tex.empty();
  if (sk && !sk->empty()) { std::vector<SkelBone> s2 = *sk; for (size_t i = 0; i < s2.size(); i++) { float p[3] = {s2[i].x, s2[i].y, s2[i].z}; mx::fitPoint(f, p); s2[i].x = p[0]; s2[i].y = p[1]; s2[i].z = p[2]; } o.skel = std::make_shared<std::vector<SkelBone> >(s2); }
  rebuildObj(o); doc.objs.push_back(o); doc.sel = (int)doc.objs.size() - 1; doc.selClip = -1;
}
void LoadModelFile(const std::string& path);
static void importOther(const std::string& path) {
  mx::ImportResult r; std::string err; setStatus("Importing ...");
  if (!mx::importModelFile(path, r, err)) { setStatus("Import failed: " + err); return; }
  if (!r.mdlPath.empty()) { LoadModelFile(r.mdlPath); return; }
  pushUndo(); addImported(r.mesh, r.name, nullptr, nullptr); setStatus("Loaded " + r.note);
}
void LoadModelFile(const std::string& path) {
  if (g_busy) { setStatus("Busy - wait for the current job"); return; }
  { std::string e = extLower(path); if (e == ".obj" || e == ".glb" || e == ".zip" || e == ".prisma") { importOther(path); return; } }
  mdl::Model m; std::string err; if (!mdl::parseMDL(path, m, err)) { setStatus("Import failed: " + err); return; }
  std::vector<SkelBone> sk = rawSkeleton(m); mx::Mesh3 mesh; std::string merr, minfo; bool got = mx::loadSourceMesh(path, mesh, merr, &minfo);
  std::string nm = m.name; size_t sl = nm.find_last_of("/\\"); if (sl != std::string::npos) nm = nm.substr(sl + 1);
  pushUndo();
  if (got) { mx::calcBounds(mesh); mx::Fit f = mx::makeFit(mesh.bmin, mesh.bmax); addImported(mesh, nm, &sk, &f); setStatus("Loaded " + std::to_string(doc.objs.back().imp->tcount()) + " triangles, " + std::to_string(m.bones.size()) + " bones"); }
  else {   // skeleton only: show the bones on a tiny invisible mesh
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}; for (size_t i = 0; i < sk.size(); i++) { float p[3] = {sk[i].x, sk[i].y, sk[i].z}; for (int k = 0; k < 3; k++) { lo[k] = std::min(lo[k], p[k]); hi[k] = std::max(hi[k], p[k]); } }
    if (sk.empty()) { for (int k = 0; k < 3; k++) { lo[k] = 0; hi[k] = 1; } }
    mx::Fit f = mx::makeFit(lo, hi); mx::Mesh3 empty; empty.bmin[0] = empty.bmin[1] = empty.bmin[2] = 0; empty.bmax[0] = empty.bmax[1] = empty.bmax[2] = 1; addImported(empty, nm, &sk, &f); setStatus("Skeleton only (" + std::to_string(m.bones.size()) + " bones): " + merr);
  }
}
static void LoadDemoMDL() {
  std::vector<uint8_t> d = mdl::buildFakeMDL(); std::istringstream iss(std::string(d.begin(), d.end()), std::ios::binary); mdl::Model m; std::string err;
  if (!mdl::parseMDL(iss, m, err)) { setStatus("Demo failed: " + err); return; }
  std::vector<SkelBone> sk = rawSkeleton(m); float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  for (size_t i = 0; i < sk.size(); i++) { float p[3] = {sk[i].x, sk[i].y, sk[i].z}; for (int k = 0; k < 3; k++) { lo[k] = std::min(lo[k], p[k]); hi[k] = std::max(hi[k], p[k]); } }
  mx::Fit f = mx::makeFit(lo, hi); mx::Mesh3 empty; for (int k = 0; k < 3; k++) { empty.bmin[k] = 0; empty.bmax[k] = 1; } pushUndo(); addImported(empty, "Skeleton", &sk, &f); setStatus("Loaded demo skeleton");
}
int RunTimelineSelfTest() { return tl::timeline_demo(); }
int RunSfmRender() { return sfm::sfm_demo(); }

// ================================================================ layout (portrait + landscape)
static void computeLayoutLegacy() {
  land = W > H; U = std::min(W, H) / 720.f;
  if (!land) {
    rLeft = {0, 0, 0, 0}; rBar = {0, H - P(112), W, P(112)};
    rTrack = {0, rBar.y - P(154), W, P(154)}; trackClipH = P(84); rRuler = {0, rTrack.y - P(28), W, P(28)};
    ctrlY = rRuler.y - P(36); phx = W / 2; plusW = 0;
    VP = {0, 0, W, rRuler.y + P(190)};
    rPlay = {W / 2 - P(40), ctrlY - P(34), P(80), P(68)};
    rUndo = {P(14), ctrlY - P(28), P(52), P(56)}; rRedo = {P(74), ctrlY - P(28), P(52), P(56)};
    rHome = {P(14), P(54), P(58), P(58)}; rShare = {W - P(70), P(56), P(58), P(58)}; rList = {W - P(78), P(145), P(98), P(70)};
  } else {
    int lw = P(163); rLeft = {0, 0, lw, H}; rBar = rLeft;
    rTrack = {lw, H - P(82), W - lw, P(82)}; trackClipH = P(66); rRuler = {lw, rTrack.y - P(25), W - lw, P(25)};
    ctrlY = rRuler.y - P(34); plusW = P(118); phx = lw + (W - lw - plusW) / 2;
    VP = {lw, 0, W - lw, rRuler.y + P(75)};
    rPlay = {P(54), H - P(125), P(98), P(70)};
    rUndo = {lw + P(24), ctrlY - P(28), P(52), P(56)}; rRedo = {lw + P(84), ctrlY - P(28), P(52), P(56)};
    rHome = {P(52), P(20), P(60), P(60)}; rShare = {W - P(70), P(10), P(58), P(58)}; rList = {W - P(80), P(98), P(100), P(72)};
  }
  rKey = {W - P(78), ctrlY - P(30), P(60), P(60)}; rGraphBtn = {W - P(138), ctrlY - P(30), P(60), P(60)};
  if (modeling) { VP = {0, 0, W, land ? H : H - P(40)}; int gy = H - P(106); rUndo = {P(120), gy - P(70), P(52), P(56)}; rRedo = {P(180), gy - P(70), P(52), P(56)}; }
  rPlus = {rTrack.x + rTrack.w - P(30) - P(land ? 68 : 69), rTrack.y + rTrack.h / 2 - P(34), P(land ? 68 : 69), P(land ? 68 : 69)};
  rowCy = rTrack.y + rTrack.h / 2;
  if (g_meOpen) {   // Motion Editor replaces the timeline + bottom bar
    int panelH = land ? P(396) : P(660); rLeft = {0, 0, 0, 0}; rMePanel = {0, H - panelH, W, panelH}; meLabelW = P(land ? 110 : 96);
    int hh = land ? P(56) : P(78), wh = land ? P(50) : P(66), lh = land ? P(52) : P(84), lw = W - meLabelW - P(10);
    rMeHeader = {0, rMePanel.y, W, hh}; rMeWeight = {meLabelW, rMeHeader.y + hh, lw, wh}; rRuler = {meLabelW, rMeWeight.y + wh, lw, land ? P(24) : P(28)};
    for (int g = 0; g < 3; g++) rMeLane[g] = {meLabelW, rRuler.y + rRuler.h + g * lh, lw, lh - P(4)};
    int sy = rMeLane[2].y + lh; rMeSliders = {P(14), sy + P(4), W - P(28), std::max(P(40), H - sy - P(8))};
    phx = rMeLane[0].x + rMeLane[0].w / 2; ctrlY = rMePanel.y - P(36); VP = {0, 0, W, rMePanel.y}; plusW = 0;
    rPlay = {W / 2 - P(40), ctrlY - P(34), P(80), P(68)}; rUndo = {P(14), ctrlY - P(28), P(52), P(56)}; rRedo = {P(74), ctrlY - P(28), P(52), P(56)};
  }
}
static void computeLayout() { computeLayoutLegacy(); if (skinOn()) skinLayout(); }
static float ppf() { return pxPerSec * U / kFps; }
static int frameToX(float f) { return phx + (int)lroundf((f - curFrame) * ppf()); }
static int xToFrame(int x) { return curFrame + (int)lroundf((x - phx) / ppf()); }

static float dragScroll(int id, SDL_Rect r, float& sc, float contentLen, bool horiz) {
  float maxS = std::max(0.f, contentLen - (horiz ? r.w : r.h)); static std::map<int, int> last;
  if (pressIn(r) && activeId == 0) { activeId = id; last[id] = horiz ? in.mx : in.my; }
  if (activeId == id && in.down) { int cur = horiz ? in.mx : in.my; sc -= (float)(cur - last[id]); last[id] = cur; }
  sc = clampf(sc, 0, maxS); return maxS;
}

// ================================================================ time label + ruler
static void drawTimeLabel(int x, int cy) {
  int sec = curFrame / kFps, fr = curFrame % kFps; char b[24]; snprintf(b, sizeof b, "%02d:%02d", sec / 60, sec % 60);
  txt(x, cy, b, C_WHITE, FPX(24)); int w = textW(b, FPX(24)); char f[16]; snprintf(f, sizeof f, "%df", fr); txt(x + w + P(4), cy + P(2), f, C_DIM, FPX(19));
}
static void drawRuler() {
  fillR(rRuler, {255, 255, 255, 6});
  SDL_RenderSetClipRect(R, &rRuler);
  float pf = ppf(); int f0 = std::max(0, (int)floorf(curFrame - (phx - rRuler.x) / pf) - 1), f1 = (int)ceilf(curFrame + (rRuler.x + rRuler.w - phx) / pf) + 1;
  static const int dotSteps[] = {5, 10, 15, 30, 60, 150, 300, 600}; int dotStep = 600, labStep = 30;
  for (int s : dotSteps) if (s * pf >= P(38.f)) { dotStep = s; break; }
  while (labStep * pf < P(110.f)) labStep *= (labStep % 30 == 0 && labStep < 60) ? 2 : 5;
  for (int f = (f0 / dotStep) * dotStep; f <= f1; f += dotStep) {
    if (f < 0) continue; int x = frameToX((float)f), cy = rRuler.y + rRuler.h / 2;
    if (f % labStep == 0) { std::string l = std::to_string(f / kFps) + "s"; txt(x - textW(l, FPX(19)) / 2, cy, l, {190, 190, 194, 255}, FPX(19)); }
    else discFill(x, cy, std::max(1, P(2.4f)), {205, 205, 208, 255});
  }
  SDL_RenderSetClipRect(R, nullptr);
  if (!land) drawTimeLabel(P(12), rRuler.y + rRuler.h / 2);
}

// ================================================================ transport row: undo / redo / play / keyframe buttons
static void drawTransport() {
  bool canU = !undoS.empty(), canR = !redoS.empty();
  if (tapIn(rUndo) && canU) doUndo();
  if (tapIn(rRedo) && canR) doRedo();
  icon(IC_UNDO, rUndo.x + rUndo.w / 2, rUndo.y + rUndo.h / 2, P(38), canU ? C_WHITE : C_OFF);
  icon(IC_REDO, rRedo.x + rRedo.w / 2, rRedo.y + rRedo.h / 2, P(38), canR ? C_WHITE : C_OFF);
  reg(rUndo); reg(rRedo); if (modeling) return;
  if (land && !g_meOpen) fillRound(rPlay, P(14), heldIn(rPlay) ? SDL_Color{52, 52, 54, 255} : SDL_Color{36, 36, 37, 255});
  if (tapIn(rPlay)) { playing = !playing; playAcc = 0.f; if (playing && curFrame >= std::max(docEnd(), kFps)) setFrame(0); }
  icon(playing ? IC_PAUSE : IC_PLAY, rPlay.x + rPlay.w / 2, rPlay.y + rPlay.h / 2, P(land ? 34 : 36), C_WHITE);
  Obj* o = selObj(); HClip* c = nullptr; int rel = 0;
  if (o && !modeling && !g_meOpen) {
    bool has = keyHere(*o, &c, &rel);
    if (c && curFrame <= clipEnd(*c)) {
      if (tapIn(rKey)) toggleKey();
      icon(has ? IC_KEY_DEL : IC_KEY_ADD, rKey.x + rKey.w / 2, rKey.y + rKey.h / 2, P(42), C_ACC);
      if (has) { if (tapIn(rGraphBtn)) sheet = sheet == SH_GRAPH ? SH_NONE : SH_GRAPH; icon(IC_GRAPH, rGraphBtn.x + rGraphBtn.w / 2, rGraphBtn.y + rGraphBtn.h / 2, P(38), sheet == SH_GRAPH ? C_ACC : SDL_Color{107, 107, 109, 255}); }
    }
  }
}

// ================================================================ timeline: ruler, track, clips, keyframes, gestures
static int tlOrigStart = 0, tlOrigLen = 0, tlScrubBase = 0, tlKeyRel = 0, tlTargetClip = -1, tlTargetKey = -1;
static std::map<int, HKey> tlOrigKeys;
static void drawTimeline() {
  fillR(rTrack, C_DARK); drawRuler();
  Obj* o = selObj(); float pf = ppf(); int cy = rowCy, ch = trackClipH;
  SDL_Rect zone = {rTrack.x, rRuler.y, rTrack.w, rRuler.h + rTrack.h};
  // ---- gesture start / drive
  bool onPlus = inside(rPlus, pressX, pressY) && o, onKey = (inside(rKey, pressX, pressY) || inside(rGraphBtn, pressX, pressY));
  if (!g_lock && in.pressed && !consumed && !blockedAt(in.mx, in.my) && inside(zone, in.mx, in.my) && activeId == 0 && !onPlus && !onKey) {
    activeId = ID_TL; tlScrubBase = curFrame; tlTargetClip = -1; tlTargetKey = -1;
    if (o) {
      if (doc.selClip >= 0 && doc.selClip < (int)o->clips.size()) {
        const HClip& c = o->clips[doc.selClip]; int x0 = frameToX((float)c.start), x1 = frameToX((float)clipEnd(c));
        SDL_Rect hl = {x0 - P(34), cy - P(38), P(48), P(76)}, hr = {x1 - P(14), cy - P(38), P(48), P(76)};
        if (inside(hl, in.mx, in.my)) { activeId = ID_HANDLE_L; pushUndo(); tlOrigStart = c.start; tlOrigLen = c.len; tlOrigKeys = c.keys; }
        else if (inside(hr, in.mx, in.my)) { activeId = ID_HANDLE_R; pushUndo(); tlOrigStart = c.start; tlOrigLen = c.len; }
      }
      if (activeId == ID_TL) for (size_t i = 0; i < o->clips.size(); i++) {
        const HClip& c = o->clips[i]; int x0 = frameToX((float)c.start), x1 = frameToX((float)clipEnd(c));
        if (in.mx >= x0 && in.mx <= x1 && abs(in.my - cy) <= ch / 2 + P(8)) { tlTargetClip = (int)i;
          int best = P(22); for (std::map<int, HKey>::const_iterator k = c.keys.begin(); k != c.keys.end(); ++k) { int d = abs(frameToX((float)(c.start + k->first)) - in.mx); if (d < best) { best = d; tlTargetKey = k->first; } } }
      }
    }
  }
  if (activeId == ID_TL && in.down) {
    if (!moved && SDL_GetTicks() - pressT > 380 && o && tlTargetClip >= 0) {   // long press: move the key or the clip
      HClip& c = o->clips[tlTargetClip]; pushUndo(); doc.selClip = tlTargetClip;
      if (tlTargetKey >= 0) { activeId = ID_KEYMOVE; tlKeyRel = tlTargetKey; } else { activeId = ID_CLIPMOVE; tlOrigStart = c.start; }
    } else if (moved) setFrame(tlScrubBase - (int)lroundf((in.mx - pressX) / pf));
  }
  if (o && doc.selClip >= 0 && doc.selClip < (int)o->clips.size()) {
    HClip& c = o->clips[doc.selClip]; int prevEnd = 0, nextStart = 1 << 28;
    for (size_t i = 0; i < o->clips.size(); i++) if ((int)i != doc.selClip) { if (o->clips[i].start < c.start) prevEnd = std::max(prevEnd, clipEnd(o->clips[i])); else nextStart = std::min(nextStart, o->clips[i].start); }
    if (activeId == ID_HANDLE_L && in.down) {
      int ns = (int)clampf((float)xToFrame(in.mx), (float)prevEnd, (float)(tlOrigStart + tlOrigLen - 5)), d = ns - tlOrigStart;
      c.start = ns; c.len = tlOrigLen - d; c.keys.clear(); for (std::map<int, HKey>::iterator k = tlOrigKeys.begin(); k != tlOrigKeys.end(); ++k) if (k->first - d >= 0 && k->first - d <= c.len) c.keys[k->first - d] = k->second;
      applyAnim();
    } else if (activeId == ID_HANDLE_R && in.down) {
      int ne = (int)clampf((float)xToFrame(in.mx), (float)(c.start + 5), (float)nextStart); c.len = ne - c.start;
      for (std::map<int, HKey>::iterator k = c.keys.begin(); k != c.keys.end();) { if (k->first > c.len) k = c.keys.erase(k); else ++k; } applyAnim();
    } else if (activeId == ID_CLIPMOVE && in.down) {
      int ns = tlOrigStart + (int)lroundf((in.mx - pressX) / pf); ns = (int)clampf((float)ns, (float)prevEnd, (float)std::max(prevEnd, nextStart - c.len)); c.start = ns; applyAnim();
    } else if (activeId == ID_KEYMOVE && in.down) {
      int nr = (int)clampf((float)(xToFrame(in.mx) - c.start), 0.f, (float)c.len);
      if (nr != tlKeyRel && !c.keys.count(nr) && c.keys.count(tlKeyRel)) { HKey k = c.keys[tlKeyRel]; c.keys.erase(tlKeyRel); c.keys[nr] = k; tlKeyRel = nr; setFrame(c.start + nr); applyAnim(); }
    }
  }
  if (activeId == ID_TL && in.released && tapNow && !consumed) {   // plain tap
    consumed = true;
    if (o) {
      if (tlTargetClip >= 0) { doc.selClip = tlTargetClip; if (tlTargetKey >= 0) setFrame(o->clips[tlTargetClip].start + tlTargetKey); }
      else doc.selClip = -1;
    }
  }
  if (inside(zone, in.mx, in.my) && in.pinch != 0 && !g_lock) pxPerSec = clampf(pxPerSec * expf(in.pinch * 6.f), 90.f, 1400.f);

  // ---- draw track contents
  if (o) {
    SDL_RenderSetClipRect(R, &rTrack);
    int firstX = phx;
    if (!o->clips.empty()) firstX = frameToX((float)o->clips[0].start);
    std::string nm = fitTxt(o->name, P(260.f), FPX(26)); int lw = textW(nm, FPX(26)) + P(40), lx = std::max(rTrack.x + P(8), firstX - P(34) - lw);
    icon(IC_PENCIL, lx + P(12), cy, P(24), C_DIM); txt(lx + P(36), cy, nm, C_TXT, FPX(26));
    for (size_t i = 0; i < o->clips.size(); i++) {
      const HClip& c = o->clips[i]; int x0 = frameToX((float)c.start), x1 = frameToX((float)clipEnd(c)); bool sel = (int)i == doc.selClip;
      if (x1 < rTrack.x - P(40) || x0 > rTrack.x + rTrack.w + P(40)) continue;
      SDL_Rect cr = {x0, cy - ch / 2, std::max(P(8), x1 - x0), ch};
      if (sel) fillRound({cr.x - P(3), cr.y - P(3), cr.w + P(6), cr.h + P(6)}, P(13), C_WHITE);
      fillRound(cr, P(10), C_CLIP);
      if (c.keys.empty()) { int lx = std::max(cr.x, rTrack.x) + P(22); txt(lx, cy - P(land ? 10 : 12), "Animation", C_WHITE, FPX(land ? 24 : 27)); txt(lx, cy + P(land ? 14 : 18), std::to_string((int)lroundf(c.len / (float)kFps * 10) / 10) + "s", {160, 185, 215, 255}, FPX(19)); }
      if (curFrame >= c.start && curFrame <= clipEnd(c)) fillR({cr.x + P(5), cr.y + cr.h - P(6), std::max(0, std::min(phx, cr.x + cr.w) - cr.x - P(5)), P(4)}, {92, 124, 164, 255});
      int step = pf >= P(6.f) ? 1 : pf >= P(2.f) ? 5 : 30;
      for (int f = c.start + step; f < clipEnd(c); f += step) { int x = frameToX((float)f); bool sec = ((f - c.start) % kFps) == 0; fillR({x, cy - (sec ? P(14) : P(11)), sec ? P(3) : P(2), sec ? P(28) : P(22)}, sec ? SDL_Color{150, 170, 195, 255} : C_TICK); }
      for (std::map<int, HKey>::const_iterator k = c.keys.begin(); k != c.keys.end(); ++k) {
        int x = frameToX((float)(c.start + k->first)); bool here = c.start + k->first == curFrame;
        if (here) { icon(IC_DIAMOND_F, x, cy, P(44), C_WHITE); icon(IC_DIAMOND_F, x, cy, P(27), C_CLIP); } else icon(IC_DIAMOND_F, x, cy, P(25), C_KEYD);
      }
      if (sel) {
        fillRound({x0 - P(26), cy - P(30), P(27), P(60)}, P(8), C_WHITE); icon(IC_CHEV_L, x0 - P(13), cy, P(16), {40, 40, 42, 255});
        fillRound({x1 - P(1), cy - P(30), P(27), P(60)}, P(8), C_WHITE); icon(IC_CHEV_R, x1 + P(13), cy, P(16), {40, 40, 42, 255});
      }
    }
    SDL_RenderSetClipRect(R, nullptr);
    if (tapIn(rPlus)) addClipToSelected();
    fillRound(rPlus, P(12), heldIn(rPlus) ? SDL_Color{96, 168, 212, 255} : C_BLUE); icon(IC_PLUS, rPlus.x + rPlus.w / 2, rPlus.y + rPlus.h / 2, P(30), C_WHITE);
  }
  fillR({phx - P(1.5f), rTrack.y + P(6), std::max(2, P(3)), rTrack.h - P(9)}, C_WHITE);
}

// ================================================================ graph editor: cubic-bezier easing of the segment that starts at the current key
static float presetScroll = 0.f;
static void drawGraphSheet(SDL_Rect s) {
  fillRoundTop(s, P(26), C_PANEL); const int pad = P(24);
  txt(s.x + pad, s.y + P(40), "Graph Editor", C_TXT, FPX(30));
  SDL_Rect cb = {s.x + s.w - P(76), s.y + P(8), P(64), P(64)}; if (tapIn(cb)) sheet = SH_NONE; icon(IC_CLOSE, cb.x + cb.w / 2, cb.y + cb.h / 2, P(26), C_DIM);
  Obj* o = selObj(); HClip* c = nullptr; int rel = 0; if (o) keyHere(*o, &c, &rel);
  if (!o || !c) { txt(s.x + pad, s.y + P(120), "Select a clip and add a keyframe first.", C_DIM, FPX(24)); return; }
  int kr = -1; for (std::map<int, HKey>::iterator it = c->keys.begin(); it != c->keys.end(); ++it) if (it->first <= rel) kr = it->first;
  if (kr < 0) { txt(s.x + pad, s.y + P(120), "Add a keyframe first.", C_DIM, FPX(24)); return; }
  if (c->keys.upper_bound(kr) == c->keys.end()) { std::map<int, HKey>::iterator pv = c->keys.find(kr); if (pv != c->keys.begin()) { --pv; kr = pv->first; } }   // last key: edit the curve that leads into it
  HKey& k = c->keys[kr]; std::map<int, HKey>::iterator nx = c->keys.upper_bound(kr); bool hasNext = nx != c->keys.end();
  char info[96]; if (hasNext) snprintf(info, sizeof info, "Keyframe %df  ->  %df    (%d frames)", c->start + kr, c->start + nx->first, nx->first - kr); else snprintf(info, sizeof info, "Last keyframe %df - nothing to ease into", c->start + kr);
  txt(s.x + pad, s.y + P(88), fitTxt(info, s.w - pad * 2, FPX(21)), C_DIM, FPX(21));
  // presets strip
  SDL_Rect strip = {s.x + pad, s.y + P(118), s.w - pad * 2, P(54)}; float contentW = 0; for (int i = 0; i < kNPresets; i++) contentW += textW(kPresets[i].name, FPX(22)) + P(44) + P(10);
  dragScroll(ID_SCROLL + 1, strip, presetScroll, contentW, true);
  SDL_RenderSetClipRect(R, &strip); int x = strip.x - (int)presetScroll;
  for (int i = 0; i < kNPresets; i++) {
    int w = textW(kPresets[i].name, FPX(22)) + P(44); SDL_Rect r = {x, strip.y, w, strip.h}; x += w + P(10);
    const Preset& p = kPresets[i]; bool on = k.mode == p.mode && (p.mode != 0 || (fabsf(k.bz.x1 - p.b.x1) + fabsf(k.bz.y1 - p.b.y1) + fabsf(k.bz.x2 - p.b.x2) + fabsf(k.bz.y2 - p.b.y2) < 0.02f));
    if (tapIn(r) && inside(strip, pressX, pressY)) { pushUndo(); k.bz = p.b; k.mode = (uint8_t)p.mode; }
    fillRound(r, P(27), on ? C_ACC : (heldIn(r) ? SDL_Color{62, 62, 64, 255} : SDL_Color{44, 44, 46, 255})); txtC(r, p.name, on ? SDL_Color{12, 20, 28, 255} : C_TXT, FPX(22));
  }
  SDL_RenderSetClipRect(R, nullptr);
  // curve area
  int footH = P(76), top = strip.y + strip.h + P(16), side = std::min(s.w - pad * 2, s.y + s.h - footH - top - P(8)); side = std::max(side, P(160));
  SDL_Rect ga = {s.x + (s.w - side) / 2, top, side, side};
  fillRound(ga, P(14), {20, 20, 21, 255});
  const float ylo = -0.4f, yhi = 1.4f;
  auto GX = [&](float xv) { return ga.x + (int)lroundf(xv * ga.w); }; auto GY = [&](float yv) { return ga.y + (int)lroundf((yhi - yv) / (yhi - ylo) * ga.h); };
  for (int i = 0; i <= 4; i++) fillR({GX(i * 0.25f), ga.y, 1, ga.h}, {255, 255, 255, (Uint8)(i == 0 || i == 4 ? 40 : 16)});
  for (float yv : {0.f, 0.5f, 1.f}) fillR({ga.x, GY(yv), ga.w, 1}, {255, 255, 255, (Uint8)(yv == 0.5f ? 16 : 46)});
  thickLine(GX(0), GY(0), GX(1), GY(1), 1.f, {255, 255, 255, 28});
  Bez bz = k.bz; int px0 = GX(0), py0 = GY(0);
  if (k.mode == 2) { thickLine(GX(0), GY(0), GX(1), GY(0), P(3.f), C_ACC); thickLine(GX(1), GY(0), GX(1), GY(1), P(3.f), C_ACC); }
  else if (k.mode == 1) thickLine(GX(0), GY(0), GX(1), GY(1), P(3.f), C_ACC);
  else { int lx = px0, ly = py0; for (int i = 1; i <= 64; i++) { float t = i / 64.f; int xx = GX(bezAt(bz.x1, bz.x2, t)), yy = GY(bezAt(bz.y1, bz.y2, t)); thickLine(lx, ly, xx, yy, P(3.f), C_ACC); lx = xx; ly = yy; } }
  if (k.mode == 0) {
    int h1x = GX(bz.x1), h1y = GY(bz.y1), h2x = GX(bz.x2), h2y = GY(bz.y2);
    thickLine(GX(0), GY(0), h1x, h1y, P(2.f), {160, 160, 166, 255}); thickLine(GX(1), GY(1), h2x, h2y, P(2.f), {160, 160, 166, 255});
    if (pressIn(ga) && activeId == 0) { float d1 = hypotf((float)(in.mx - h1x), (float)(in.my - h1y)), d2 = hypotf((float)(in.mx - h2x), (float)(in.my - h2y)); if (std::min(d1, d2) < P(46.f)) { activeId = d1 <= d2 ? ID_GRAPH + 1 : ID_GRAPH + 2; pushUndo(); } }
    if ((activeId == ID_GRAPH + 1 || activeId == ID_GRAPH + 2) && in.down) {
      float xv = clampf((in.mx - ga.x) / (float)ga.w, 0.f, 1.f), yv = clampf(yhi - (in.my - ga.y) / (float)ga.h * (yhi - ylo), ylo, yhi);
      if (activeId == ID_GRAPH + 1) { k.bz.x1 = xv; k.bz.y1 = yv; } else { k.bz.x2 = xv; k.bz.y2 = yv; } k.mode = 0;
    }
    bool a1 = activeId == ID_GRAPH + 1, a2 = activeId == ID_GRAPH + 2;
    discFill(h1x, h1y, P(a1 ? 20.f : 16.f), C_ORG); discFill(h2x, h2y, P(a2 ? 20.f : 16.f), C_ACC);
  }
  discFill(GX(0), GY(0), P(7.f), C_WHITE); discFill(GX(1), GY(1), P(7.f), C_WHITE);
  if (hasNext && rel >= kr && rel <= nx->first) { float u = (rel - kr) / (float)(nx->first - kr), e = k.mode == 2 ? 0.f : k.mode == 1 ? u : easeBez(k.bz, u); discFill(GX(u), GY(e), P(7.5f), {255, 255, 255, 255}); discFill(GX(u), GY(e), P(4.f), C_CLIP); }
  // footer
  int fy = ga.y + ga.h + P(14); char vb[96]; snprintf(vb, sizeof vb, "x1 %.2f  y1 %.2f   x2 %.2f  y2 %.2f", k.bz.x1, k.bz.y1, k.bz.x2, k.bz.y2);
  if (fy + P(40) < s.y + s.h) txt(s.x + pad, fy + P(20), k.mode == 0 ? vb : (k.mode == 1 ? "Linear" : "Hold (no easing)"), C_DIM, FPX(21));
  int bw = P(150); SDL_Rect ba = {s.x + s.w - pad - bw, fy, bw, P(44)}, br = {ba.x - bw - P(10), fy, bw, P(44)};
  if (fy + P(44) <= s.y + s.h) {
    if (tapIn(ba)) { pushUndo(); for (std::map<int, HKey>::iterator it = c->keys.begin(); it != c->keys.end(); ++it) { it->second.bz = k.bz; it->second.mode = k.mode; } setStatus("Curve applied to every keyframe in this clip"); }
    fillRound(ba, P(22), heldIn(ba) ? SDL_Color{62, 62, 64, 255} : SDL_Color{44, 44, 46, 255}); txtC(ba, "Apply to all", C_TXT, FPX(21));
    if (tapIn(br)) { pushUndo(); k.bz = kPresets[3].b; k.mode = 0; }
    fillRound(br, P(22), heldIn(br) ? SDL_Color{62, 62, 64, 255} : SDL_Color{44, 44, 46, 255}); txtC(br, "Reset", C_TXT, FPX(21));
  }
}

// ================================================================ export (PNG frame / PNG sequence / MP4 / USD)
static int exSize = 1; static const int EX_W[4] = {640, 1280, 1920, 720}, EX_H[4] = {360, 720, 1080, 1280};
static std::string safeName(std::string s) { size_t d = s.rfind('.'); if (d != std::string::npos) s = s.substr(0, d); for (auto& c : s) if (!isalnum((unsigned char)c)) c = '_'; return s.empty() ? "hexagon3d" : s; }
static void renderExportFrame(Doc d, const Cam& c, int f, int w, int h, SDL_Color bg, const std::string& path) {
  for (size_t i = 0; i < d.objs.size(); i++) { float t[9]; if (evalObjAt(d.objs[i], f, t)) for (int k = 0; k < 9; k++) d.objs[i].T[k] = t[k]; }
  mx::Image im; im.init(w, h, bg.r, bg.g, bg.b, 255); renderDoc(im, c, d, false); mx::writePNG(path, im);
}
static bool writeUSD(const std::string& path, const Doc& d, int endFrame) {
  std::ostringstream o; o.precision(6);
  o << "#usda 1.0\n(\n    defaultPrim = \"World\"\n    metersPerUnit = 1\n    upAxis = \"Y\"\n    startTimeCode = 0\n    endTimeCode = " << endFrame << "\n    timeCodesPerSecond = " << kFps << "\n)\n\ndef Xform \"World\"\n{\n";
  for (size_t i = 0; i < d.objs.size(); i++) {
    const Obj& ob = d.objs[i]; if (!ob.rm) continue; std::string nm = safeName(ob.name); bool anim = !ob.clips.empty();
    o << "    def Xform \"" << nm << "_" << ob.id << "\"\n    {\n";
    const char* ops[3] = {"translate", "rotateZXY", "scale"};
    for (int g = 0; g < 3; g++) {
      if (!anim) { o << "        " << (g == 0 ? "double3" : "float3") << " xformOp:" << ops[g] << " = (" << ob.T[g * 3] << ", " << ob.T[g * 3 + 1] << ", " << ob.T[g * 3 + 2] << ")\n"; continue; }
      o << "        " << (g == 0 ? "double3" : "float3") << " xformOp:" << ops[g] << ".timeSamples = {\n";
      for (int f = 0; f <= endFrame; f++) { float t[9]; if (!evalObjAt(ob, f, t)) for (int k = 0; k < 9; k++) t[k] = ob.base[k]; o << "            " << f << ": (" << t[g * 3] << ", " << t[g * 3 + 1] << ", " << t[g * 3 + 2] << "),\n"; }
      o << "        }\n";
    }
    o << "        uniform token[] xformOp:order = [\"xformOp:translate\", \"xformOp:rotateZXY\", \"xformOp:scale\"]\n";
    o << "        def Mesh \"Mesh\"\n        {\n";
    if (ob.em) {   // the cage, so USD/OpenSubdiv can subdivide it
      std::vector<int> remap(ob.em->v.size(), -1), keep; std::vector<Face> fs;
      for (size_t k = 0; k < ob.em->f.size(); k++) { if (k < ob.em->hid.size() && ob.em->hid[k]) continue; Face q = ob.em->f[k]; for (size_t j = 0; j < q.size(); j++) { int& r = remap[q[j]]; if (r < 0) { r = (int)keep.size(); keep.push_back(q[j]); } q[j] = r; } fs.push_back(q); }
      o << "            int[] faceVertexCounts = ["; for (size_t k = 0; k < fs.size(); k++) o << (k ? ", " : "") << fs[k].size(); o << "]\n";
      o << "            int[] faceVertexIndices = ["; bool first = true; for (size_t k = 0; k < fs.size(); k++) for (size_t j = 0; j < fs[k].size(); j++) { o << (first ? "" : ", ") << fs[k][j]; first = false; } o << "]\n";
      o << "            point3f[] points = ["; for (size_t k = 0; k < keep.size(); k++) { const V3& p = ob.em->v[keep[k]]; o << (k ? ", " : "") << "(" << p.x << ", " << p.y << ", " << p.z << ")"; } o << "]\n";
      o << "            uniform token subdivisionScheme = \"" << (ob.subdiv > 0 ? "catmullClark" : "none") << "\"\n";
    } else {
      const mx::Mesh3& m = *ob.rm; o << "            int[] faceVertexCounts = ["; for (size_t k = 0; k < m.tcount(); k++) o << (k ? ", " : "") << 3; o << "]\n";
      o << "            int[] faceVertexIndices = ["; for (size_t k = 0; k < m.idx.size(); k++) o << (k ? ", " : "") << m.idx[k]; o << "]\n";
      o << "            point3f[] points = ["; for (size_t k = 0; k < m.vcount(); k++) o << (k ? ", " : "") << "(" << m.pos[k * 3] << ", " << m.pos[k * 3 + 1] << ", " << m.pos[k * 3 + 2] << ")"; o << "]\n";
      o << "            uniform token subdivisionScheme = \"none\"\n";
    }
    o << "            color3f[] primvars:displayColor = [(" << ob.col[0] / 255.f << ", " << ob.col[1] / 255.f << ", " << ob.col[2] / 255.f << ")]\n        }\n    }\n";
  }
  o << "}\n"; std::ofstream f(path.c_str()); if (!f) return false; f << o.str(); return true;
}
static void startExport(int kind) {   // 0 PNG frame, 1 PNG sequence, 2 MP4, 3 USD
  if (g_busy) { setStatus("Busy - wait for the current job"); return; }
  const std::string dir = mx::exportDir(); const Doc d = doc; const Cam c = cam; const int f0 = curFrame, last = std::max(1, docEnd()), w = EX_W[exSize], h = EX_H[exSize]; const SDL_Color bg = g_bg;
  const std::string prefix = d.objs.empty() ? "hexagon3d" : "hexagon3d_" + safeName(d.objs[0].name);
  runJob(kind == 0 ? "Exporting PNG" : kind == 1 ? "Exporting sequence" : kind == 2 ? "Exporting MP4" : "Exporting USD", [=]() {
    if (kind == 3) { std::string fn = dir + "/" + prefix + ".usda"; setStatus(writeUSD(fn, d, last) ? "Saved " + fn : "Write failed: " + fn); return; }
    if (kind == 0) { std::string fn = dir + "/" + prefix + "_" + mx::pad4(f0) + ".png"; renderExportFrame(d, c, f0, w, h, bg, fn); setStatus("Saved " + fn); return; }
    std::string sdir = dir + "/" + prefix + "_seq"; if (!mx::makeDir(sdir)) { setStatus("Cannot create " + sdir); return; }
    for (int f = 0; f <= last; f++) { renderExportFrame(d, c, f, w, h, bg, sdir + "/" + prefix + "_" + mx::pad4(f) + ".png"); setStatus("Exporting " + std::to_string(f + 1) + "/" + std::to_string(last + 1)); }
    if (kind == 1) { setStatus("Saved " + std::to_string(last + 1) + " PNG in " + sdir); return; }
    std::string cmd; bool ok = mx::encodeMp4(sdir, prefix, dir + "/" + prefix + ".mp4", 0, kFps, cmd);
    setStatus(ok ? "Saved " + dir + "/" + prefix + ".mp4" : "No ffmpeg here: PNG sequence + make_mp4.sh saved in " + sdir);
  });
}

// ================================================================ file browser (full screen)
struct FEntry { std::string name; bool dir; };
static std::string fbDir; static std::vector<FEntry> fbList; static float fbScroll = 0.f;
static std::string lowerS(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
static bool hasModelExt(const std::string& n) { size_t d = n.rfind('.'); if (d == std::string::npos) return false; std::string e = lowerS(n.substr(d)); return e == ".mdl" || e == ".obj" || e == ".glb" || e == ".zip" || e == ".prisma"; }
static bool fbEnter(std::string dir) {
  while (dir.size() > 1 && dir.back() == '/') dir.pop_back(); DIR* d = opendir(dir.c_str()); if (!d) return false; std::vector<FEntry> v;
  while (dirent* e = readdir(d)) { std::string n = e->d_name; if (n.empty() || n[0] == '.') continue; struct stat st; std::string full = dir + (dir == "/" ? "" : "/") + n; if (stat(full.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) v.push_back(FEntry{n, true}); else if (S_ISREG(st.st_mode) && hasModelExt(n)) v.push_back(FEntry{n, false}); }
  closedir(d); std::sort(v.begin(), v.end(), [](const FEntry& a, const FEntry& b) { if (a.dir != b.dir) return a.dir; return lowerS(a.name) < lowerS(b.name); });
  fbList.swap(v); fbDir = dir; fbScroll = 0; return true;
}
static bool fbEnterDownloads() { static const char* dl[] = {"/sdcard/Download", "/storage/emulated/0/Download", "/sdcard", "/storage/emulated/0", "."}; for (const char* s : dl) if (fbEnter(s)) return true; return false; }
static void openFileBrowser() { if ((!fbDir.empty() && fbEnter(fbDir)) || fbEnterDownloads()) sheet = SH_FILE; else setStatus("Cannot read storage - allow file access for Cxxdroid"); }
static void drawFileBrowser() {
  fillR({0, 0, W, H}, C_PANEL); int pad = P(24), y = P(30);
  txt(pad, y + P(20), "Import model", C_TXT, FPX(32)); SDL_Rect cb = {W - P(84), y - P(8), P(64), P(64)}; if (tapIn(cb)) { sheet = SH_NONE; return; } icon(IC_CLOSE, cb.x + cb.w / 2, cb.y + cb.h / 2, P(28), C_DIM);
  y += P(72); txt(pad, y + P(14), fitTxt(fbDir, W - pad * 2, FPX(22)), C_DIM, FPX(22)); y += P(40);
  txt(pad, y + P(12), fitTxt(".obj / .glb / .zip / .prisma load directly - .mdl needs its .vvd and .vtx beside it", W - pad * 2, FPX(19)), {200, 170, 80, 255}, FPX(19)); y += P(40);
  int bw = (W - pad * 2 - P(24)) / 3; const char* bl[3] = {"Up", "Downloads", "Demo"};
  for (int i = 0; i < 3; i++) { SDL_Rect r = {pad + i * (bw + P(12)), y, bw, P(56)}; fillRound(r, P(14), heldIn(r) ? SDL_Color{62, 62, 64, 255} : SDL_Color{44, 44, 46, 255}); txtC(r, bl[i], C_TXT, FPX(24));
    if (tapIn(r)) { if (i == 0) { size_t sl = fbDir.rfind('/'); std::string up = (sl == std::string::npos || sl == 0) ? "/" : fbDir.substr(0, sl); if (!fbEnter(up)) setStatus("Cannot open parent folder"); }
                    else if (i == 1) { if (!fbEnterDownloads()) setStatus("Cannot read storage"); } else { LoadDemoMDL(); sheet = SH_NONE; return; } } }
  y += P(72); SDL_Rect L = {pad, y, W - pad * 2, H - y - pad}; int rh = P(64); float content = (float)fbList.size() * rh;
  dragScroll(ID_SCROLL + 3, L, fbScroll, content, false);
  fillRound(L, P(12), {20, 20, 21, 255}); SDL_RenderSetClipRect(R, &L);
  for (int i = 0; i < (int)fbList.size(); i++) {
    int ry = L.y + i * rh - (int)fbScroll; if (ry + rh < L.y || ry > L.y + L.h) continue; SDL_Rect row = {L.x, ry, L.w, rh};
    if (i % 2) fillR(row, {255, 255, 255, 5}); txt(row.x + P(20), ry + rh / 2, (fbList[i].dir ? "> " : "   ") + fitTxt(fbList[i].name, row.w - P(70), FPX(25)), fbList[i].dir ? C_TXT : SDL_Color{255, 214, 120, 255}, FPX(25));
    if (inside(L, pressX, pressY) && tapIn(row)) { if (fbList[i].dir) { if (!fbEnter(fbDir + (fbDir == "/" ? "" : "/") + fbList[i].name)) setStatus("Cannot open folder"); } else { std::string f = fbDir + (fbDir == "/" ? "" : "/") + fbList[i].name; sheet = SH_NONE; SDL_RenderSetClipRect(R, nullptr); LoadModelFile(f); return; } }
  }
  if (fbList.empty()) txt(L.x + P(20), L.y + P(40), "No folders or model files here", C_DIM, FPX(23));
  SDL_RenderSetClipRect(R, nullptr);
}

// ================================================================ bottom sheets
static float sheetScroll = 0.f, outlineScroll = 0.f; static bool g_toggleDummy = false;
static SDL_Rect sheetRect(int sh) {
  if (land) return {W - P(540), 0, P(540), H};
  int h = sh == SH_MODELS ? P(660) : sh == SH_SKY ? P(430) : sh == SH_CAMERA ? P(440) : sh == SH_MATERIAL ? P(600) : sh == SH_SMOOTH ? P(300) : sh == SH_EXPORT ? P(600) : sh == SH_HOME ? P(560) : sh == SH_GRAPH ? P(780) : P(720);
  h = std::min(h, H - P(100)); return {0, H - h, W, h};
}
static bool sheetHeader(SDL_Rect s, const std::string& title) {
  fillRound(s, P(26), C_PANEL); txt(s.x + P(26), s.y + P(40), title, C_TXT, FPX(30));
  SDL_Rect cb = {s.x + s.w - P(76), s.y + P(8), P(64), P(64)}; bool close = tapIn(cb); icon(IC_CLOSE, cb.x + cb.w / 2, cb.y + cb.h / 2, P(26), C_DIM); return close;
}
static bool switchRow(SDL_Rect r, const std::string& label, bool on) {
  bool tap = tapIn(r); txt(r.x + P(4), r.y + r.h / 2, label, C_TXT, FPX(26));
  SDL_Rect sw = {r.x + r.w - P(86), r.y + r.h / 2 - P(18), P(72), P(36)}; fillRound(sw, P(18), on ? C_ACC : SDL_Color{66, 66, 68, 255}); discFill(on ? sw.x + sw.w - P(18) : sw.x + P(18), sw.y + sw.h / 2, P(14), C_WHITE); return tap;
}
static bool bigButton(SDL_Rect r, const std::string& label, int ic, bool accent = false) {
  bool tap = tapIn(r); fillRound(r, P(16), accent ? C_BLUE : (heldIn(r) ? SDL_Color{62, 62, 64, 255} : SDL_Color{42, 42, 44, 255}));
  if (ic >= 0) { icon(ic, r.x + P(40), r.y + r.h / 2, P(34), C_TXT); txt(r.x + P(78), r.y + r.h / 2, fitTxt(label, r.w - P(92), FPX(25)), C_TXT, FPX(25)); } else txtC(r, label, C_TXT, FPX(25)); return tap;
}
static const SDL_Color kSwatch[12] = {{236, 236, 238, 255}, {160, 164, 170, 255}, {70, 72, 78, 255}, {222, 64, 64, 255}, {240, 140, 50, 255}, {244, 204, 70, 255},
                                      {96, 190, 96, 255}, {60, 190, 190, 255}, {70, 130, 230, 255}, {130, 90, 220, 255}, {226, 100, 170, 255}, {150, 100, 64, 255}};
static const SDL_Color kBgs[6] = {{52, 52, 67, 255}, {30, 30, 32, 255}, {58, 66, 80, 255}, {74, 60, 92, 255}, {96, 138, 178, 255}, {120, 112, 98, 255}};
static const char* kBgNames[6] = {"Dark", "Night", "Slate", "Dusk", "Sky", "Sand"};

static void drawSheet() {
  if (sheet == SH_NONE) return;
  if (sheet == SH_FILE) { drawFileBrowser(); return; }
  SDL_Rect s = sheetRect(sheet); g_block = s; g_blockOn = false; reg(s);   // sheet draws with normal input
  bool modal = sheet == SH_MODELS || sheet == SH_SKY || sheet == SH_CAMERA || sheet == SH_EXPORT || sheet == SH_HOME;
  if (modal && tapNow && !consumed && !inside(s, pressX, pressY)) { consumed = true; sheet = SH_NONE; return; }
  Obj* o = selObj(); int pad = P(26);
  if (sheet == SH_GRAPH) { drawGraphSheet(s); return; }
  const char* titles[] = {"", "Models", "Sky", "Camera", "Material", "Scene", "Export", "Project", "Smoothing", "", ""};
  if (sheetHeader(s, titles[sheet])) { sheet = SH_NONE; return; }
  SDL_Rect body = {s.x + pad, s.y + P(88), s.w - pad * 2, s.h - P(100)};
  if (sheet == SH_MODELS) {
    int tile = P(150), gap = P(14), cols = std::max(2, (body.w + gap) / (tile + gap)); tile = (body.w - gap * (cols - 1)) / cols;
    int n = PR_COUNT + 2, rows = (n + cols - 1) / cols; float content = (float)(rows * (tile + gap)); dragScroll(ID_SCROLL + 4, body, sheetScroll, content, false);
    SDL_RenderSetClipRect(R, &body);
    for (int i = 0; i < n; i++) {
      SDL_Rect r = {body.x + (i % cols) * (tile + gap), body.y + (i / cols) * (tile + gap) - (int)sheetScroll, tile, tile};
      bool tap = inside(body, pressX, pressY) && tapIn(r); fillRound(r, P(18), heldIn(r) ? SDL_Color{56, 56, 58, 255} : SDL_Color{40, 40, 42, 255});
      int ic = i < PR_COUNT ? kPrimIcon[i] : (i == PR_COUNT ? IC_IMPORT : IC_MODEL); std::string lb = i < PR_COUNT ? kPrimName[i] : (i == PR_COUNT ? "Import" : "3D Model");
      icon(ic, r.x + r.w / 2, r.y + r.h * 40 / 100, P(60), i == PR_COUNT + 1 ? C_ORG : C_TXT); txtC({r.x, r.y + r.h * 74 / 100, r.w, P(30)}, lb, C_TXT, FPX(23));
      if (tap) {
        if (i < PR_COUNT) { pushUndo(); addPrimitive(i); sheet = SH_NONE; }
        else if (i == PR_COUNT) openFileBrowser();
        else { pushUndo(); int k = addPrimitive(PR_CUBE); doc.objs[k].name = uniqueName("Model"); modeling = true; selMode = 0; clearModelSel(); tool = 0; sheet = SH_NONE; cam.tgt = objCenterW(doc.objs[k]); cam.dist = 7.5f; setStatus("3D modeling: tap faces, edges or vertices"); }
      }
    }
    SDL_RenderSetClipRect(R, nullptr);
  } else if (sheet == SH_SKY) {
    int sw = (body.w - P(12) * 5) / 6; txt(body.x, body.y + P(14), "Background", C_DIM, FPX(22));
    for (int i = 0; i < 6; i++) { SDL_Rect r = {body.x + i * (sw + P(12)), body.y + P(40), sw, sw}; bool on = g_bg.r == kBgs[i].r && g_bg.g == kBgs[i].g && g_bg.b == kBgs[i].b;
      if (tapIn(r)) g_bg = kBgs[i]; fillRound({r.x - P(3), r.y - P(3), r.w + P(6), r.h + P(6)}, P(14), on ? C_WHITE : SDL_Color{0, 0, 0, 0}); fillRound(r, P(12), kBgs[i]); txtC({r.x, r.y + sw + P(4), r.w, P(26)}, kBgNames[i], C_DIM, FPX(17)); }
    if (switchRow({body.x, body.y + sw + P(86), body.w, P(60)}, "Grid", showGrid)) showGrid = !showGrid;
    if (switchRow({body.x, body.y + sw + P(150), body.w, P(60)}, "Wireframe", wireMode)) { wireMode = !wireMode; vpHash = 0; }
  } else if (sheet == SH_CAMERA) {
    int bw = (body.w - P(14)) / 2, bh = P(66); struct CB { const char* n; int a; } b[6] = {{"Reset view", 0}, {"Front", 1}, {"Side", 2}, {"Top", 3}, {"Frame selection", 4}, {"Zoom out", 5}};
    for (int i = 0; i < 6; i++) { SDL_Rect r = {body.x + (i % 2) * (bw + P(14)), body.y + (i / 2) * (bh + P(14)), bw, bh};
      if (bigButton(r, b[i].n, -1)) switch (b[i].a) { case 0: cam = Cam(); break; case 1: cam.yaw = 0; cam.pitch = 0.05f; break; case 2: cam.yaw = 1.5708f; cam.pitch = 0.05f; break; case 3: cam.yaw = 0; cam.pitch = 1.45f; break;
        case 4: if (o) { V3 c = objCenterW(*o); cam.tgt = c; cam.dist = clampf(o->rad * maxScale(*o) * 6.f, 4.f, 60.f); } break; default: cam.dist = std::min(60.f, cam.dist * 1.4f); } }
  } else if (sheet == SH_MATERIAL) {
    if (!o) { txt(body.x, body.y + P(30), "Select a model first", C_DIM, FPX(24)); return; }
    int sw = (body.w - P(12) * 5) / 6;
    for (int i = 0; i < 12; i++) { SDL_Rect r = {body.x + (i % 6) * (sw + P(12)), body.y + (i / 6) * (sw + P(12)), sw, sw}; bool on = o->col[0] == kSwatch[i].r && o->col[1] == kSwatch[i].g && o->col[2] == kSwatch[i].b;
      if (tapIn(r)) { pushUndo(); o->col[0] = kSwatch[i].r; o->col[1] = kSwatch[i].g; o->col[2] = kSwatch[i].b; o->colSet = true; }
      if (on) fillRound({r.x - P(3), r.y - P(3), r.w + P(6), r.h + P(6)}, P(14), C_WHITE); fillRound(r, P(12), kSwatch[i]); }
    const char* cn[3] = {"R", "G", "B"}; SDL_Color fc[3] = {C_AXR, C_AXG, C_AXB};
    for (int i = 0; i < 3; i++) { SDL_Rect r = {body.x + P(40), body.y + (sw + P(12)) * 2 + P(40) + i * P(70), body.w - P(150), P(40)}; float v = o->col[i];
      txt(body.x, r.y + r.h / 2, cn[i], C_DIM, FPX(24)); if (sliderH(ID_SLIDER + 10 + i, r, v, 0, 255, fc[i], true)) { o->col[i] = (uint8_t)v; o->colSet = true; }
      SDL_Rect vf = {r.x + r.w + P(22), r.y - P(6), P(80), P(52)}; fillRound(vf, P(10), C_FIELD); txtC(vf, std::to_string((int)o->col[i]), C_TXT, FPX(22)); }
  } else if (sheet == SH_SMOOTH) {
    if (!o) { txt(body.x, body.y + P(30), "Select a model first", C_DIM, FPX(24)); return; }
    txt(body.x, body.y + P(14), "Smoothing angle", C_DIM, FPX(23)); SDL_Rect r1 = {body.x, body.y + P(40), body.w - P(130), P(44)}; float sm = o->smooth;
    if (sliderH(ID_SLIDER + 20, r1, sm, 0, 180, C_ACC)) { o->smooth = sm; rebuildObj(*o); }
    SDL_Rect f1 = {r1.x + r1.w + P(24), r1.y - P(4), P(100), P(52)}; fillRound(f1, P(10), C_FIELD); txtC(f1, fmtF(o->smooth, 0), C_TXT, FPX(22));
    txt(body.x, body.y + P(124), "Subdivisions", C_DIM, FPX(23)); SDL_Rect r2 = {body.x, body.y + P(150), body.w - P(130), P(44)}; float sd = (float)o->subdiv;
    if (sliderH(ID_SLIDER + 21, r2, sd, 0, 3, C_ACC, true)) { o->subdiv = (int)sd; rebuildObj(*o); }
    SDL_Rect f2 = {r2.x + r2.w + P(24), r2.y - P(4), P(100), P(52)}; fillRound(f2, P(10), C_FIELD); txtC(f2, std::to_string(o->subdiv), C_TXT, FPX(22));
  } else if (sheet == SH_OUTLINER) {
    int rh = P(70); float content = (float)(doc.objs.size() + 1) * rh; dragScroll(ID_SCROLL + 5, body, outlineScroll, content, false);
    SDL_RenderSetClipRect(R, &body);
    for (int i = 0; i < (int)doc.objs.size(); i++) {
      Obj& ob = doc.objs[i]; SDL_Rect row = {body.x, body.y + i * rh - (int)outlineScroll, body.w, rh - P(6)}; bool sel = i == doc.sel; fillRound(row, P(12), sel ? SDL_Color{40, 70, 104, 255} : SDL_Color{38, 38, 40, 255});
      SDL_Rect eye = {row.x + row.w - P(130), row.y, P(60), row.h}, del = {row.x + row.w - P(66), row.y, P(60), row.h};
      bool inB = inside(body, pressX, pressY);
      if (inB && tapIn(eye)) { pushUndo(); ob.hidden = !ob.hidden; } if (inB && tapIn(del)) { doc.sel = i; deleteSelected(); SDL_RenderSetClipRect(R, nullptr); return; } if (inB && tapIn(row)) { doc.sel = i; doc.selClip = -1; }
      icon(IC_CUBE, row.x + P(34), row.y + row.h / 2, P(32), C_TXT); txt(row.x + P(68), row.y + row.h / 2, fitTxt(ob.name, row.w - P(220), FPX(25)), C_TXT, FPX(25));
      icon(ob.hidden ? IC_EYE_OFF : IC_EYE, eye.x + eye.w / 2, eye.y + eye.h / 2, P(32), ob.hidden ? C_OFF : C_TXT); icon(IC_TRASH, del.x + del.w / 2, del.y + del.h / 2, P(30), C_DIM);
    }
    SDL_Rect add = {body.x, body.y + (int)doc.objs.size() * rh - (int)outlineScroll, body.w, rh - P(6)}; if (inside(body, pressX, pressY) && tapIn(add)) { sheet = SH_MODELS; }
    fillRound(add, P(12), {38, 38, 40, 255}); icon(IC_PLUS, add.x + P(34), add.y + add.h / 2, P(26), C_ACC); txt(add.x + P(68), add.y + add.h / 2, "Add model", C_ACC, FPX(25));
    SDL_RenderSetClipRect(R, nullptr);
  } else if (sheet == SH_EXPORT) {
    int bw = (body.w - P(14)) / 2, bh = P(70); char sz[48]; snprintf(sz, sizeof sz, "Size %dx%d", EX_W[exSize], EX_H[exSize]);
    int last = std::max(1, docEnd()); char info[96]; snprintf(info, sizeof info, "Frame %d  -  animation %d frames (%.1fs) at %d fps", curFrame, last, last / (float)kFps, kFps); txt(body.x, body.y + P(14), fitTxt(info, body.w, FPX(21)), C_DIM, FPX(21));
    SDL_Rect b0 = {body.x, body.y + P(40), bw, bh}, b1 = {body.x + bw + P(14), b0.y, bw, bh}, b2 = {body.x, b0.y + bh + P(14), bw, bh}, b3 = {b1.x, b2.y, bw, bh}, b4 = {body.x, b2.y + bh + P(14), body.w, bh};
    if (bigButton(b0, "PNG frame", IC_MEDIA)) startExport(0); if (bigButton(b1, "PNG sequence", IC_MEDIA)) startExport(1);
    if (bigButton(b2, "MP4 video", IC_CAMERA)) startExport(2); if (bigButton(b3, "USD (.usda)", IC_CUBE)) startExport(3);
    if (bigButton(b4, sz, -1)) exSize = (exSize + 1) % 4;
    txt(body.x, b4.y + bh + P(34), fitTxt(getStatus(), body.w, FPX(21)), g_busy ? C_ORG : C_TXT, FPX(21));
  } else if (sheet == SH_HOME) {
    int bh = P(70), y = body.y; struct HB { const char* n; int ic; } b[6] = {{"New scene", IC_PLUS}, {"Import model...", IC_IMPORT}, {"Load demo skeleton", IC_SKEL}, {"Timeline self-test", IC_GRAPH}, {"Render SFM image", IC_CAMERA}, {"Rig preview (console)", IC_MOVE}};
    for (int i = 0; i < 6; i++) { SDL_Rect r = {body.x, y + i * (bh + P(10)), body.w, bh};
      if (bigButton(r, b[i].n, b[i].ic)) { sheet = SH_NONE; if (i == 0) newScene(); else if (i == 1) openFileBrowser(); else if (i == 2) LoadDemoMDL();
        else if (i == 3) runJob("Timeline test", [] { RunTimelineSelfTest(); setStatus("Timeline test done"); }); else if (i == 4) runJob("SFM render", [] { RunSfmRender(); setStatus("SFM render done (sfm_render.bmp)"); }); else { RigAsciiPreview(); setStatus("Rig preview printed to the console"); } } }
  }
}

// ================================================================ modeling: converting meshes to cages, component picking, overlay
static bool convertToCage(Obj& o) {
  if (o.em) return true; if (!o.imp || o.imp->tcount() == 0 || o.imp->tcount() > 30000) return false;
  std::shared_ptr<EditMesh> em = std::make_shared<EditMesh>(); std::map<std::array<long, 3>, int> ids; const mx::Mesh3& m = *o.imp; std::vector<int> remap(m.vcount());
  for (size_t i = 0; i < m.vcount(); i++) { std::array<long, 3> k = {lroundf(m.pos[i * 3] * 2000.f), lroundf(m.pos[i * 3 + 1] * 2000.f), lroundf(m.pos[i * 3 + 2] * 2000.f)}; std::map<std::array<long, 3>, int>::iterator it = ids.find(k);
    if (it == ids.end()) { ids[k] = (int)em->v.size(); remap[i] = (int)em->v.size(); em->v.push_back({m.pos[i * 3], m.pos[i * 3 + 1], m.pos[i * 3 + 2]}); } else remap[i] = it->second; }
  for (size_t t = 0; t + 2 < m.idx.size(); t += 3) em->f.push_back({remap[m.idx[t]], remap[m.idx[t + 1]], remap[m.idx[t + 2]]});
  emFix(*em); o.em = em; o.imp.reset(); o.colSet = true; rebuildObj(o); return true;
}
static void enterModeling() {
  Obj* o = selObj(); if (!o) return; pushUndo(); if (!convertToCage(*o)) { setStatus("This mesh is too large to edit"); undoS.pop_back(); return; }
  modeling = true; selMode = 0; clearModelSel(); sheet = SH_NONE; cam.tgt = objCenterW(*o); cam.dist = clampf(o->rad * maxScale(*o) * 2.7f, 4.f, 60.f); setStatus("3D modeling: choose face, edge or vertex mode");
}
static void cageEdges(const EditMesh& m, std::set<std::pair<int, int> >& out) {
  for (size_t i = 0; i < m.f.size(); i++) { if (i < m.hid.size() && m.hid[i]) continue; for (size_t k = 0; k < m.f[i].size(); k++) out.insert(edgeKey(m.f[i][k], m.f[i][(k + 1) % m.f[i].size()])); }
}
static void pickComponent(Obj& o, int sx, int sy) {
  if (!o.em) return; const EditMesh& m = *o.em;
  if (selMode == 3) {   // vertex
    int best = -1; float bd = (float)P(38.f); std::set<std::pair<int, int> > es; cageEdges(m, es); std::set<int> vis; for (std::set<std::pair<int, int> >::iterator it = es.begin(); it != es.end(); ++it) { vis.insert(it->first); vis.insert(it->second); }
    for (std::set<int>::iterator it = vis.begin(); it != vis.end(); ++it) { int x, y; if (!proj(toWorld(o, m.v[*it]), x, y)) continue; float d = hypotf((float)(x - sx), (float)(y - sy)); if (d < bd) { bd = d; best = *it; } }
    if (best >= 0) { if (mV.count(best)) mV.erase(best); else mV.insert(best); } else mV.clear();
  } else if (selMode == 2) {   // edge
    std::set<std::pair<int, int> > es; cageEdges(m, es); std::pair<int, int> best(-1, -1); float bd = (float)P(30.f);
    for (std::set<std::pair<int, int> >::iterator it = es.begin(); it != es.end(); ++it) { int x0, y0, x1, y1; if (!proj(toWorld(o, m.v[it->first]), x0, y0) || !proj(toWorld(o, m.v[it->second]), x1, y1)) continue;
      float d = distPtSeg((float)sx, (float)sy, (float)x0, (float)y0, (float)x1, (float)y1); if (d < bd) { bd = d; best = *it; } }
    if (best.first >= 0) { if (mE.count(best)) mE.erase(best); else mE.insert(best); } else mE.clear();
  } else if (selMode == 1) {   // face
    V3 lo = toLocal(o, E), ld = dirToLocal(o, rayDirAt(sx, sy)); int best = -1; float bt = 1e30f;
    for (size_t i = 0; i < m.f.size(); i++) { if (i < m.hid.size() && m.hid[i]) continue; const Face& q = m.f[i];
      for (size_t k = 1; k + 1 < q.size(); k++) { float t; if (rayTri(lo, ld, m.v[q[0]], m.v[q[k]], m.v[q[k + 1]], t) && t < bt) { bt = t; best = (int)i; } } }
    if (best >= 0) { if (mF.count(best)) mF.erase(best); else mF.insert(best); } else mF.clear();
  }
}
static void drawEditOverlay() {
  Obj* o = selObj(); if (!modeling || !o || !o->em) return; const EditMesh& m = *o->em; std::vector<std::pair<int, int> > sp(m.v.size()); std::vector<uint8_t> ok(m.v.size(), 0);
  for (size_t i = 0; i < m.v.size(); i++) { int x, y; if (proj(toWorld(*o, m.v[i]), x, y)) { sp[i] = std::make_pair(x, y); ok[i] = 1; } }
  for (std::set<int>::iterator it = mF.begin(); it != mF.end(); ++it) { if (*it >= (int)m.f.size()) continue; std::vector<std::pair<int, int> > poly; bool good = true; for (size_t k = 0; k < m.f[*it].size(); k++) { if (!ok[m.f[*it][k]]) good = false; poly.push_back(sp[m.f[*it][k]]); } if (good) fillPoly(poly, {255, 168, 56, 110}); }
  std::set<std::pair<int, int> > es; cageEdges(m, es);
  for (std::set<std::pair<int, int> >::iterator it = es.begin(); it != es.end(); ++it) if (ok[it->first] && ok[it->second]) { bool sel = mE.count(*it) > 0; thickLine(sp[it->first].first, sp[it->first].second, sp[it->second].first, sp[it->second].second, sel ? 4.f * U : 1.4f * U, sel ? C_ORG : SDL_Color{12, 12, 12, 215}); }
  for (std::set<int>::iterator it = mF.begin(); it != mF.end(); ++it) { if (*it >= (int)m.f.size()) continue; const Face& q = m.f[*it]; for (size_t k = 0; k < q.size(); k++) { int a = q[k], b = q[(k + 1) % q.size()]; if (ok[a] && ok[b]) thickLine(sp[a].first, sp[a].second, sp[b].first, sp[b].second, 3.f * U, C_ORG); } }
  if (selMode == 3) { std::set<int> vis; for (std::set<std::pair<int, int> >::iterator it = es.begin(); it != es.end(); ++it) { vis.insert(it->first); vis.insert(it->second); }
    for (std::set<int>::iterator it = vis.begin(); it != vis.end(); ++it) if (ok[*it]) discFill(sp[*it].first, sp[*it].second, mV.count(*it) ? P(9.f) : P(5.f), mV.count(*it) ? C_ORG : SDL_Color{235, 235, 238, 255}); }
  else for (std::set<int>::iterator it = mV.begin(); it != mV.end(); ++it) if (ok[*it]) discFill(sp[*it].first, sp[*it].second, P(9.f), C_ORG);
}

// ================================================================ top overlay + toast
static void drawTopOverlay() {
  reg(rShare); reg(rList); if (!land && !modeling) reg(rHome);
  if (!land && !modeling) { if (tapIn(rHome)) goHome(); icon(IC_HOME, rHome.x + rHome.w / 2, rHome.y + rHome.h / 2, P(42), C_WHITE); }
  if (tapIn(rShare)) sheet = sheet == SH_EXPORT ? SH_NONE : SH_EXPORT; icon(IC_SHARE, rShare.x + rShare.w / 2, rShare.y + rShare.h / 2, P(42), C_WHITE);
  fillRound(rList, P(14), heldIn(rList) ? SDL_Color{46, 46, 46, 255} : SDL_Color{32, 32, 32, 255}); if (tapIn(rList)) sheet = sheet == SH_OUTLINER ? SH_NONE : SH_OUTLINER;
  icon(IC_LIST, rList.x + P(38), rList.y + rList.h / 2, P(38), C_WHITE);
}
static void drawToast() {
  Uint32 at = 0; std::string s = getStatus(&at); if (s.empty()) return; Uint32 now = SDL_GetTicks(); if (!g_busy && now - at > 3200) return;
  int px = FPX(22), tw = std::min(textW(s, px) + P(48), W - P(40)); int y = land ? rRuler.y - P(110) : rTrack.y - P(120); if (modeling) y = H - P(190); if (g_meOpen) y = rMePanel.y - P(120);
  int a = g_busy ? 255 : (int)clampf(255.f - (now - at - 2600) * 0.4f, 0.f, 255.f); SDL_Rect r = {(land ? (W + rLeft.w) / 2 : W / 2) - tw / 2, y, tw, P(52)};
  fillRound(r, P(26), {12, 12, 12, (Uint8)(a * 220 / 255)}); txtC(r, fitTxt(s, tw - P(40), px), alpha(C_WHITE, a), px);
}

// ================================================================ bottom bar (tabs / object tools) + landscape side bar
enum { A_MODELS, A_SKY, A_CAMERA, A_LIGHT, A_MEDIA, A_TEXT, A_MOVE, A_ROTATE, A_SCALE, A_MATERIAL, A_SMOOTH, A_COPY, A_CUT, A_DELCLIP, A_DUP, A_DEL, A_MODEL, A_MOTION };
struct Item { int ic; std::string label, num; bool active, dim; int act; };
static void doAction(int a) {
  switch (a) {
    case A_MODELS: sheet = sheet == SH_MODELS ? SH_NONE : SH_MODELS; sheetScroll = 0; break; case A_SKY: sheet = sheet == SH_SKY ? SH_NONE : SH_SKY; break; case A_CAMERA: sheet = sheet == SH_CAMERA ? SH_NONE : SH_CAMERA; break;
    case A_LIGHT: setStatus("Light is not in this build yet"); break; case A_MEDIA: setStatus("Media is not in this build yet"); break; case A_TEXT: setStatus("Text is not in this build yet"); break;
    case A_MOVE: tool = 0; break; case A_ROTATE: tool = 1; break; case A_SCALE: tool = 2; break; case A_MATERIAL: sheet = sheet == SH_MATERIAL ? SH_NONE : SH_MATERIAL; break;
    case A_SMOOTH: sheet = sheet == SH_SMOOTH ? SH_NONE : SH_SMOOTH; break; case A_COPY: copyClip(); break; case A_CUT: cutClip(); break; case A_DELCLIP: deleteClip(); break;
    case A_DUP: duplicateSelected(); break; case A_DEL: deleteSelected(); break; case A_MODEL: enterModeling(); break; case A_MOTION: openMotionEditor(); break;
  }
}
static std::vector<Item> buildItems() {
  std::vector<Item> v; Obj* o = selObj();
  if (!o) { v.push_back({IC_CUBE, "Models", "", false, false, A_MODELS}); v.push_back({IC_SKY, "Sky", "", false, false, A_SKY}); v.push_back({IC_CAMERA, "Camera", "", false, false, A_CAMERA});
            v.push_back({IC_BULB, "Light", "", false, false, A_LIGHT}); v.push_back({IC_WAVE, "Motion Editor", "", false, false, A_MOTION}); v.push_back({IC_MEDIA, "Media", "", false, false, A_MEDIA}); v.push_back({-1, "Text", "T", false, false, A_TEXT}); return v; }
  v.push_back({IC_MOVE, "Move", "", tool == 0, false, A_MOVE}); v.push_back({IC_ROTATE, "Rotate", "", tool == 1, false, A_ROTATE}); v.push_back({IC_SCALE, "Scale", "", tool == 2, false, A_SCALE});
  v.push_back({-77, "Material", "", sheet == SH_MATERIAL, false, A_MATERIAL});
  bool clipSel = doc.selClip >= 0 && doc.selClip < (int)o->clips.size();
  if (clipSel) { const HClip& c = o->clips[doc.selClip]; v.push_back({IC_COPY, "Copy", "", false, false, A_COPY}); v.push_back({IC_CUT, "Cut", "", false, curFrame <= c.start || curFrame >= clipEnd(c), A_CUT}); v.push_back({IC_TRASH, "Delete clip", "", false, false, A_DELCLIP}); }
  else v.push_back({-1, "Smoothing", std::to_string((int)o->smooth), sheet == SH_SMOOTH, false, A_SMOOTH});
  v.push_back({IC_COPY, "Duplicate", "", false, false, A_DUP}); v.push_back({IC_TRASH, "Delete", "", false, false, A_DEL}); v.push_back({IC_WAVE, "Motion", "", false, false, A_MOTION}); v.push_back({IC_MODEL, "Model", "", false, false, A_MODEL}); return v;
}
static void drawItem(SDL_Rect r, const Item& it, SDL_Color matCol) {
  if (it.ic == -77) {   // material: a shaded sphere in the object's colour
    bool tap = tapIn(r), held = heldIn(r); if (it.active || held) fillRound({r.x + P(6), r.y + P(4), r.w - P(12), r.h - P(8)}, P(12), {44, 44, 46, 255});
    int cy = r.y + r.h * 38 / 100; discFill(r.x + r.w / 2, cy, P(14), {(Uint8)(matCol.r * 3 / 4), (Uint8)(matCol.g * 3 / 4), (Uint8)(matCol.b * 3 / 4), 255}); discFill(r.x + r.w / 2 - P(2), cy - P(3), P(10), matCol);
    txtC({r.x, r.y + r.h * 76 / 100 - P(12), r.w, P(24)}, "Material", it.active ? C_WHITE : C_TXT, FPX(21)); if (tap) doAction(it.act); return;
  }
  if (toolItem(r, it.ic, it.label, it.active, it.dim, it.num)) doAction(it.act);
}
static void drawBottomBar() {
  reg(rBar); Obj* o = selObj(); std::vector<Item> items = buildItems(); SDL_Color matCol = o ? SDL_Color{o->col[0], o->col[1], o->col[2], 255} : C_TXT;
  if (!land) {
    fillR(rBar, o ? C_BAR : C_DARK); int ix0 = o ? P(93) : 0, iw = P(124); SDL_Rect strip = {ix0, rBar.y, W - ix0, rBar.h};
    float contentW = (float)(items.size() * iw); dragScroll(ID_SCROLL + 2, strip, barScroll, contentW, true);
    SDL_RenderSetClipRect(R, &strip);
    for (size_t i = 0; i < items.size(); i++) { SDL_Rect r = {ix0 + (int)i * iw - (int)barScroll, rBar.y, iw, rBar.h}; if (r.x + r.w < strip.x || r.x > strip.x + strip.w) continue; g_clip = strip; g_clipOn = true; drawItem(r, items[i], matCol); g_clipOn = false; }
    SDL_RenderSetClipRect(R, nullptr);
    if (contentW > strip.w && barScroll < contentW - strip.w - 2) fadeRight({W - P(90), rBar.y, P(90), rBar.h}, o ? C_BAR : C_DARK);
    if (o) { SDL_Rect bb = {P(16), rBar.y + P(18), P(62), P(78)}; if (roundIconBtn(bb, IC_CHEV_L, C_BTN, C_WHITE, P(30), 10)) { doc.sel = -1; doc.selClip = -1; sheet = SH_NONE; } }
  } else {
    fillR(rBar, C_BAR); int top = P(92), ih = P(105); SDL_Rect strip = {0, top, rBar.w, rPlay.y - P(70) - top};
    float contentW = (float)(items.size() * ih); dragScroll(ID_SCROLL + 2, strip, barScroll, contentW, false);
    SDL_RenderSetClipRect(R, &strip);
    for (size_t i = 0; i < items.size(); i++) { SDL_Rect r = {0, top + (int)i * ih - (int)barScroll, rBar.w, ih}; if (r.y + r.h < strip.y || r.y > strip.y + strip.h) continue; g_clip = strip; g_clipOn = true; drawItem(r, items[i], matCol); g_clipOn = false; }
    SDL_RenderSetClipRect(R, nullptr);
    if (o) { SDL_Rect bb = {P(40), P(18), P(84), P(66)}; if (roundIconBtn(bb, IC_CHEV_L, C_BTN, C_WHITE, P(30), 10)) { doc.sel = -1; doc.selClip = -1; sheet = SH_NONE; } }
    else { if (tapIn(rHome)) goHome(); icon(IC_HOME, rHome.x + rHome.w / 2, rHome.y + rHome.h / 2, P(42), C_WHITE); }
    drawTimeLabel(P(44), rPlay.y - P(26));
  }
}

// ================================================================ modeling screen (left tool column, sliders panel, selection modes)
static void drawModelingUI() {
  Obj* o = selObj(); if (!o) return; const int pad = P(14);
  int top = land ? P(27) : P(130), colW = P(92), colH = land ? P(553) : P(500);
  SDL_Rect col = {pad + (land ? P(3) : 0), top, colW, colH}; fillRound(col, P(16), {28, 28, 28, 255}); reg(col);
  struct MI { int ic; int act; } mi[7] = {{IC_CHEV_L, 0}, {IC_MOVE, 1}, {IC_ROTATE, 2}, {IC_SCALE, 3}, {IC_COPY, 4}, {o->em && !o->em->hid.empty() ? IC_EYE_OFF : IC_EYE_OFF, 5}, {-1, 6}};
  int ch = col.h / 7;
  for (int i = 0; i < 7; i++) {
    SDL_Rect r = {col.x, col.y + i * ch, col.w, ch}; bool active = (i == 1 && tool == 0) || (i == 2 && tool == 1) || (i == 3 && tool == 2);
    if (active) fillRound({r.x + P(8), r.y + P(6), r.w - P(16), r.h - P(12)}, P(12), {46, 46, 48, 255});
    bool tap = tapIn(r); if (mi[i].ic >= 0) icon(mi[i].ic, r.x + r.w / 2, r.y + r.h / 2, i == 0 ? P(34) : P(40), C_WHITE); else discFill(r.x + r.w / 2, r.y + r.h / 2, P(15), {o->col[0], o->col[1], o->col[2], 255});
    if (tap) switch (mi[i].act) { case 0: leaveModeling(); break; case 1: tool = 0; break; case 2: tool = 1; break; case 3: tool = 2; break; case 4: duplicateFaces(); break; case 5: hideSelectedFaces(); break; default: sheet = sheet == SH_MATERIAL ? SH_NONE : SH_MATERIAL; }
  }
  int pw = land ? P(490) : W - (col.x + col.w) - P(28), px0 = col.x + col.w + P(14); SDL_Rect pn = {px0, top, pw, P(265)}; fillRound(pn, P(16), {28, 28, 28, 255}); reg(pn);
  txt(pn.x + P(28), pn.y + P(40), "Smoothing Angle", C_DIM, FPX(24)); SDL_Rect s1 = {pn.x + P(28), pn.y + P(70), pn.w - P(28) - P(150), P(44)}; float sm = o->smooth;
  if (sliderH(ID_SLIDER + 30, s1, sm, 0, 180, C_ACC)) { o->smooth = sm; rebuildObj(*o); }
  SDL_Rect f1 = {pn.x + pn.w - P(124), pn.y + P(52), P(98), P(66)}; fillRound(f1, P(12), C_FIELD); txtC(f1, fmtF(o->smooth, 2), C_DIM, FPX(24));
  txt(pn.x + P(28), pn.y + P(160), "Subdivisions", C_DIM, FPX(24)); SDL_Rect s2 = {pn.x + P(28), pn.y + P(190), pn.w - P(28) - P(150), P(44)}; float sd = (float)o->subdiv;
  if (sliderH(ID_SLIDER + 31, s2, sd, 0, 3, C_ACC, true)) { o->subdiv = (int)sd; rebuildObj(*o); }
  SDL_Rect f2 = {f1.x, pn.y + P(172), P(98), P(66)}; fillRound(f2, P(12), C_FIELD); txtC(f2, std::to_string(o->subdiv), C_DIM, FPX(24));
  // bottom groups
  int gy = H - P(106), gx1 = land ? P(128) : (W - P(668)) / 2, gx2 = gx1 + P(385);
  SDL_Rect g1 = {gx1, gy, P(358), P(80)}, g2 = {gx2, gy, P(283), P(80)}; fillRound(g1, P(16), C_BAR); fillRound(g2, P(16), C_BAR); reg(g1); reg(g2);
  for (int i = 0; i < 4; i++) { SDL_Rect r = {g1.x + i * P(89.5f), g1.y, P(89), g1.h}; if (selMode == i) fillRound({r.x + P(4), r.y + P(4), r.w - P(8), r.h - P(8)}, P(12), {20, 20, 21, 255});
    if (tapIn(r)) { selMode = i; clearModelSel(); } selIcon(i, r.x + r.w / 2, r.y + r.h / 2, P(46), selMode == i); }
  bool* flags[3] = {&gzLocal, &pivotSel, &showBones}; int ics[3] = {IC_AXES, IC_PIVOT, IC_SKEL};
  for (int i = 0; i < 3; i++) { SDL_Rect r = {g2.x + i * P(94), g2.y, P(94), g2.h}; if (*flags[i]) fillRound({r.x + P(4), r.y + P(4), r.w - P(8), r.h - P(8)}, P(12), {20, 20, 21, 255});
    if (tapIn(r)) { *flags[i] = !*flags[i]; setStatus(i == 0 ? (gzLocal ? "Gizmo: local axes" : "Gizmo: world axes") : i == 1 ? (pivotSel ? "Pivot: selection centre" : "Pivot: object origin") : (showBones ? "Skeleton visible" : "Skeleton hidden")); }
    icon(ics[i], r.x + r.w / 2, r.y + r.h / 2, P(44), *flags[i] ? C_WHITE : SDL_Color{170, 170, 174, 255}); }
}

// ================================================================ viewport gestures (orbit / pinch / tap-pick / gizmo drag)
static int pmx = 0, pmy = 0; static float meTBefore[9];
static bool g_vpMenu = false; static int vpMenuX = 0, vpMenuY = 0; static Uint32 vpMenuT = 0, vpHoldPressT = 0;   // hold the viewport -> "Base light" option
static bool gizmoShown(Obj* o) { if (!o || o->hidden) return false; if (!modeling || selMode == 0) return true; return hasCompSel(); }
static void viewportInput() {
  Obj* o = selObj(); camSetup();
  bool pressUI = false; for (size_t i = 0; i < uiRects.size(); i++) if (inside(uiRects[i], in.mx, in.my)) pressUI = true;
  if (in.pressed && !g_lock && !blockedAt(in.mx, in.my) && !pressUI && activeId == 0 && !consumed) {
    setAxisFrame(o);
    if (gizmoShown(o)) { V3 pv = pivotWorld(*o); int hit = gizmoHit(pv, tool, in.mx, in.my);
      if (hit >= 0 && !(tool != 2 && hit == 3)) { activeId = ID_GIZMO; gzAxis = hit; gzActive = true; gzPrevX = in.mx; gzPrevY = in.my; if (!g_meOpen) pushUndo(); for (int k = 0; k < 9; k++) meTBefore[k] = o->T[k]; if (modeling && o->em) cowEM(*o); } }
    if (activeId == 0) activeId = ID_VIEW;
  }
  setAxisFrame(o);
  if (activeId == ID_GIZMO && o) {
    if (in.down) { V3 pv = pivotWorld(*o); float amt = gizmoDragAmount(pv, tool, gzAxis == 3 ? 0 : gzAxis, in.mx, in.my); if (gzAxis == 3) amt = ((in.mx - gzPrevX) - (in.my - gzPrevY)) * 0.004f; if (fabsf(amt) > 1e-6f) applyDelta(*o, gzAxis, amt); gzPrevX = in.mx; gzPrevY = in.my; }
    if (in.released) { gzActive = false; bool comp = modeling && selMode != 0 && o->em && hasCompSel(); if (comp) rebuildObj(*o); else if (g_meOpen) meGizmoCommit(*o); else commitEdit(*o); }
  }
  if (activeId == ID_VIEW) {
    if (in.down && !moved && !in.multi && !g_vpMenu && vpHoldPressT != pressT && SDL_GetTicks() - pressT >= 450) {   // press-and-hold without moving
      vpHoldPressT = pressT; g_vpMenu = true; vpMenuX = pressX; vpMenuY = pressY; vpMenuT = SDL_GetTicks(); moved = true; }   // moved = true: the release is not a tap-pick
    if (in.down && !in.pressed && !in.multi && moved && g_nav != 1) { cam.yaw -= (in.mx - pmx) * 0.01f; cam.pitch = clampf(cam.pitch + (in.my - pmy) * 0.01f, -1.45f, 1.45f); }
    if (in.released && tapNow && !consumed && (g_nav != 0 || modeling)) {
      consumed = true;
      if (modeling && selMode != 0 && o) pickComponent(*o, pressX, pressY);
      else { int idx = pickObject(pressX, pressY); if (idx >= 0) { if (idx != doc.sel) { doc.sel = idx; doc.selClip = -1; } } else if (!modeling && !g_meOpen) { doc.sel = -1; doc.selClip = -1; if (sheet == SH_MATERIAL || sheet == SH_SMOOTH) sheet = SH_NONE; } }
    }
  }
  if (!g_lock && inside(VP, in.mx, in.my) && in.wheel != 0) cam.dist = clampf(cam.dist * powf(0.9f, in.wheel), 2.f, 90.f);
  if (in.pinch != 0 && !g_lock && !inside(rTrack, in.mx, in.my) && !inside(rRuler, in.mx, in.my)) cam.dist = clampf(cam.dist * expf(-in.pinch * 6.f), 2.f, 90.f);
  if ((in.panX != 0 || in.panY != 0) && !g_lock && !inside(rTrack, in.mx, in.my) && !inside(rRuler, in.mx, in.my)) {   // two-finger slide: the scene follows the fingers
    float s = cam.dist / std::max(1.f, focalPx());
    cam.tgt = sub(cam.tgt, Rv * (in.panX * s)); cam.tgt = cam.tgt + Uv * (in.panY * s);
  }
  pmx = in.mx; pmy = in.my;
}

// ================================================================ Motion Editor (SFM-style): dense logs, time selection + falloffs, temporary layer, filters, time warp
struct MeSel { float a0, a1, b0, b1; };      // frames: weight 0 at a0, ramps to 1 at a1, flat to b0, ramps back to 0 at b1
struct MeLayer { float delta[9]; float strength, sDefault, sPlayhead, sZero, sSmooth, sJitter; int warpMode; float warpDelta; };   // warpMode 0 none, 1 stretch, 2 slide
static MeSel meSel = {0, 0, 0, 0}; static MeLayer meLy; static bool meDirty = true, meWarpOn = false, mePrompt = false;
static int meHandle = -1, meScrubBase = 0; static MeSel meSelAtPress = {0, 0, 0, 0}; static float meScroll = 0.f;

static void meResetLayer() {
  for (int i = 0; i < 9; i++) meLy.delta[i] = 0; meLy.strength = 1.f; meLy.sDefault = meLy.sPlayhead = meLy.sZero = meLy.sSmooth = meLy.sJitter = 0.f; meLy.warpMode = 0; meLy.warpDelta = 0.f; meDirty = true;
}
static bool meLayerClean() {
  for (int i = 0; i < 9; i++) if (fabsf(meLy.delta[i]) > 1e-5f) return false;
  return meLy.sDefault < 0.001f && meLy.sPlayhead < 0.001f && meLy.sZero < 0.001f && meLy.sSmooth < 0.001f && meLy.sJitter < 0.001f && meLy.warpMode == 0;
}
static float smooth01(float x) { x = clampf(x, 0, 1); return x * x * (3 - 2 * x); }
static float meWeight(const MeSel& s, float f) {
  if (f <= s.a0 || f >= s.b1) return 0.f; if (f < s.a1) return smooth01((f - s.a0) / std::max(1e-3f, s.a1 - s.a0));
  if (f <= s.b0) return 1.f; return smooth01((s.b1 - f) / std::max(1e-3f, s.b1 - s.b0));
}
static float meHash(uint32_t x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return (x >> 8) * (1.0f / 16777216.0f); }
static float meNoise(float x, int c) {   // smooth deterministic value noise in [-1,1]
  float fx = floorf(x); int i = (int)fx; float t = x - fx; t = t * t * (3 - 2 * t);
  float a = meHash((uint32_t)(i * 7919 + c * 104729 + 13)) * 2.f - 1.f, b = meHash((uint32_t)((i + 1) * 7919 + c * 104729 + 13)) * 2.f - 1.f; return a + (b - a) * t;
}
static Obj* meObjPtr() { for (size_t i = 0; i < doc.objs.size(); i++) if (doc.objs[i].id == g_meObj) return &doc.objs[i]; return nullptr; }

// log (+ time warp) -> + weighted delta -> blend sliders (Default / Playhead / Zero) -> Smooth -> Jitter  ==> g_meOut
static void meCompute(const MotionLog& L, float playF) {
  MeSel s = meSel; int N = L.n; std::vector<float> Wv[9]; float d = meLy.warpDelta; int mode = meLy.warpMode; float winLen = s.b1 - s.a0;
  if (mode == 1 && winLen > 1.f) {          // stretch / compress the window, later data shifts along
    float newLen = std::max(2.f, winLen + d); d = newLen - winLen; float k = newLen / winLen; N = std::max(4, L.n + (int)lroundf(d * kSub));
    for (int c = 0; c < 9; c++) { Wv[c].resize(N); for (int i = 0; i < N; i++) { float tt = i / (float)kSub, src; if (tt <= s.a0) src = tt; else if (tt <= s.a0 + newLen) src = s.a0 + (tt - s.a0) / k; else src = tt - d; Wv[c][i] = sampleStream(L.ch[c], L.n, src); } }
    s.a1 = s.a0 + (s.a1 - s.a0) * k; s.b0 = s.a0 + (s.b0 - s.a0) * k; s.b1 = s.a0 + newLen;
  } else if (mode == 2) {                    // slide the action earlier / later inside the window
    for (int c = 0; c < 9; c++) { Wv[c].resize(N); for (int i = 0; i < N; i++) { float tt = i / (float)kSub, src = tt; if (tt >= s.a0 && tt <= s.b1) src = clampf(tt - d, s.a0, s.b1); Wv[c][i] = sampleStream(L.ch[c], N, src); } }
  } else for (int c = 0; c < 9; c++) Wv[c] = L.ch[c];
  float tp[9]; for (int c = 0; c < 9; c++) tp[c] = sampleStream(Wv[c], N, playF);
  static const float tz[9] = {0, 0, 0, 0, 0, 0, 1, 1, 1}; std::vector<float> w(N); for (int i = 0; i < N; i++) w[i] = meWeight(s, i / (float)kSub) * meLy.strength;
  for (int c = 0; c < 9; c++) for (int i = 0; i < N; i++) { float wi = w[i]; if (wi <= 0.f) continue; float v = Wv[c][i];
    v += meLy.delta[c] * wi; v += (L.def[c] - v) * meLy.sDefault * wi; v += (tp[c] - v) * meLy.sPlayhead * wi; v += (tz[c] - v) * meLy.sZero * wi; Wv[c][i] = v; }
  if (meLy.sSmooth > 0.001f) {
    int r = std::max(1, (int)lroundf(meLy.sSmooth * 8.f * kSub));
    for (int c = 0; c < 9; c++) { std::vector<double> pre(N + 1, 0.0); for (int i = 0; i < N; i++) pre[i + 1] = pre[i] + Wv[c][i]; std::vector<float> B(N);
      for (int i = 0; i < N; i++) { int lo = std::max(0, i - r), hi = std::min(N - 1, i + r); B[i] = (float)((pre[hi + 1] - pre[lo]) / (hi - lo + 1)); }
      for (int i = 0; i < N; i++) Wv[c][i] += (B[i] - Wv[c][i]) * w[i]; }
  }
  if (meLy.sJitter > 0.001f) { static const float amp[9] = {0.06f, 0.06f, 0.06f, 2.5f, 2.5f, 2.5f, 0.015f, 0.015f, 0.015f};
    for (int c = 0; c < 9; c++) for (int i = 0; i < N; i++) if (w[i] > 0.f) Wv[c][i] += meNoise(i / (float)kSub * 0.33f, c) * amp[c] * meLy.sJitter * w[i]; }
  for (int c = 0; c < 9; c++) g_meOut[c].swap(Wv[c]); g_meOutN = N; g_meOutValid = true;
}
static void meRecompute() { Obj* o = meObjPtr(); if (!o || !o->log) { g_meOutValid = false; return; } meCompute(*o->log, (float)curFrame); meDirty = false; }
static void meBake(Obj& o) {   // existing keyframe animation -> dense per-frame (2 per frame) log
  g_meOutValid = false; int frames = std::max(docEnd(), 3 * kFps), n = frames * kSub + 1; std::shared_ptr<MotionLog> L = std::make_shared<MotionLog>(); L->n = n;
  for (int c = 0; c < 9; c++) L->ch[c].resize(n);
  for (int i = 0; i < n; i++) { float t[9]; if (!evalObjAtF(o, i / (float)kSub, t)) for (int c = 0; c < 9; c++) t[c] = o.base[c]; for (int c = 0; c < 9; c++) L->ch[c][i] = t[c]; }
  for (int c = 0; c < 9; c++) L->def[c] = o.base[c]; o.log = L;
}
static void meInitSel(const Obj& o) {
  float len = o.log ? (o.log->n - 1) / (float)kSub : 90.f, c = clampf((float)curFrame, 0.f, len), a0 = std::max(0.f, c - 24.f), b1 = std::min(len, c + 24.f); if (b1 - a0 < 8.f) { a0 = std::max(0.f, len - 48.f); b1 = len; }
  meSel.a0 = a0; meSel.a1 = a0 + (b1 - a0) * 0.3f; meSel.b0 = a0 + (b1 - a0) * 0.7f; meSel.b1 = b1;
}
static void meBind() {
  Obj* o = selObj(); if (!o) { g_meObj = -1; g_meOutValid = false; return; }
  if (g_meObj != o->id) { g_meObj = o->id; meResetLayer(); }
  if (!o->log) { pushUndo(); o = selObj(); meBake(*o); setStatus("Motion log created: " + std::to_string(o->log->n) + " samples per channel"); }
  meInitSel(*o); meDirty = true; g_meOutValid = false;
}
static void openMotionEditor() {
  if (doc.objs.empty()) { setStatus("Add a model first"); return; }
  if (doc.sel < 0 && doc.objs.size() == 1) doc.sel = 0;
  modeling = false; sheet = SH_NONE; g_meOpen = true; mePrompt = false; meWarpOn = false; meResetLayer(); g_meObj = -1; meBind(); if (!selObj()) setStatus("Tap a model to edit its motion");
}
static void meForceClose() { g_meOpen = false; g_meOutValid = false; g_meObj = -1; mePrompt = false; meResetLayer(); }
static void meExit(bool commit);
static void meCommit() {
  Obj* o = meObjPtr(); if (!o || !o->log || meLayerClean()) return; meRecompute(); pushUndo(); o = meObjPtr();
  std::shared_ptr<MotionLog> nl = std::make_shared<MotionLog>(*o->log); nl->n = g_meOutN; for (int c = 0; c < 9; c++) nl->ch[c] = g_meOut[c]; o->log = nl;
  if (meLy.warpMode == 1) { float winLen = meSel.b1 - meSel.a0; if (winLen > 1.f) { float newLen = std::max(2.f, winLen + meLy.warpDelta), k = newLen / winLen; meSel.a1 = meSel.a0 + (meSel.a1 - meSel.a0) * k; meSel.b0 = meSel.a0 + (meSel.b0 - meSel.a0) * k; meSel.b1 = meSel.a0 + newLen; } }
  meResetLayer(); meRecompute(); setStatus("Motion committed to the log");
}
static void meExit(bool commit) { if (commit) meCommit(); meForceClose(); applyAnim(); }
static void meRequestExit() { if (g_meOpen && !meLayerClean()) mePrompt = true; else meExit(false); }
static void meRevert() { Obj* o = meObjPtr(); if (!o || !o->log) return; pushUndo(); o = meObjPtr(); o->log.reset(); meExit(false); setStatus("Back to keyframe animation"); }
static void meGizmoCommit(Obj& o) {   // a viewport edit becomes a relative offset in the temporary layer
  float w = meWeight(meSel, (float)curFrame) * meLy.strength;
  if (w < 0.02f) { for (int k = 0; k < 9; k++) o.T[k] = meTBefore[k]; setStatus("Move the playhead inside the time selection to edit"); return; }
  for (int c = 0; c < 9; c++) meLy.delta[c] += (o.T[c] - meTBefore[c]) / w; meDirty = true;
}

// ---- drawing
static void meSliderCell(int id, SDL_Rect cell, const char* label, float& v, SDL_Color fill) {
  txt(cell.x + P(8), cell.y + P(12), label, C_DIM, FPX(19)); txtR(cell.x + cell.w - P(8), cell.y + P(12), std::to_string((int)lroundf(v * 100)) + "%", C_TXT, FPX(19));
  SDL_Rect sl = {cell.x + P(24), cell.y + P(26), cell.w - P(48), P(34)}; if (sliderH(id, sl, v, 0.f, 1.f, fill, false, false)) meDirty = true;
}
static bool meChip(SDL_Rect r, const std::string& label, bool on, bool dim, SDL_Color onCol) {
  bool tap = !dim && tapIn(r); fillRound(r, P(14), on ? onCol : (dim ? SDL_Color{34, 34, 35, 255} : (heldIn(r) ? SDL_Color{62, 62, 64, 255} : SDL_Color{44, 44, 46, 255})));
  txtC(r, label, on ? SDL_Color{10, 18, 26, 255} : (dim ? C_OFF : C_TXT), FPX(22)); return tap;
}
static void meDrawLane(int g, const Obj& o) {
  SDL_Rect ln = rMeLane[g]; fillRound(ln, P(10), {20, 20, 21, 255}); static const char* nm[3] = {"Position", "Rotation", "Scale"};
  txt(P(10), ln.y + ln.h / 2 - P(land ? 6 : 10), nm[g], C_TXT, FPX(land ? 17 : 20));
  static const SDL_Color cc[3] = {{232, 84, 84, 255}, {96, 214, 96, 255}, {84, 128, 240, 255}}; static const char* xyz[3] = {"X", "Y", "Z"};
  for (int k = 0; k < 3; k++) txt(P(10) + k * P(land ? 24 : 28), ln.y + ln.h / 2 + P(land ? 14 : 18), xyz[k], cc[k], FPX(land ? 15 : 17));
  const MotionLog& L = *o.log; bool layered = !meLayerClean() && g_meOutValid; float pf = ppf();
  float f0 = curFrame - (phx - ln.x) / pf - 1.f, f1 = curFrame + (ln.x + ln.w - phx) / pf + 1.f; int i0 = std::max(0, (int)floorf(f0 * kSub)), nOut = g_meOutValid ? g_meOutN : L.n, i1 = std::min(nOut - 1, (int)ceilf(f1 * kSub));
  int step = std::max(1, (int)ceilf((i1 - i0) / std::max(1.f, ln.w / 3.f)));
  float lo = 1e30f, hi = -1e30f; for (int k = 0; k < 3; k++) { int c = g * 3 + k; for (int i = i0; i <= i1; i += step) { float v = g_meOutValid && i < g_meOutN ? g_meOut[c][i] : L.ch[c][std::min(i, L.n - 1)]; lo = std::min(lo, v); hi = std::max(hi, v);
      if (layered && i < L.n) { lo = std::min(lo, L.ch[c][i]); hi = std::max(hi, L.ch[c][i]); } } }
  if (lo > hi) { lo = -1; hi = 1; } if (hi - lo < 1e-3f) { lo -= 0.5f; hi += 0.5f; } float padv = (hi - lo) * 0.12f; lo -= padv; hi += padv;
  SDL_RenderSetClipRect(R, &ln);
  // selection band behind the curves
  int xa0 = frameToX(meSel.a0), xa1 = frameToX(meSel.a1), xb0 = frameToX(meSel.b0), xb1 = frameToX(meSel.b1);
  fillR({xa0, ln.y, std::max(1, xb1 - xa0), ln.h}, {64, 170, 235, 16}); fillR({xa1, ln.y, std::max(1, xb0 - xa1), ln.h}, {64, 170, 235, 22});
  fillR({xa0, ln.y, 1, ln.h}, {64, 170, 235, 120}); fillR({xb1, ln.y, 1, ln.h}, {64, 170, 235, 120});
  auto Y = [&](float v) { return ln.y + ln.h - P(8) - (int)((v - lo) / (hi - lo) * (ln.h - P(16))); };
  for (int pass = 0; pass < 2; pass++) {
    if (pass == 0 && !layered) continue;
    for (int k = 0; k < 3; k++) { int c = g * 3 + k; int px = 0, py = 0; bool have = false;
      for (int i = i0; i <= i1; i += step) { float v = pass == 0 ? L.ch[c][std::min(i, L.n - 1)] : (g_meOutValid && i < g_meOutN ? g_meOut[c][i] : L.ch[c][std::min(i, L.n - 1)]);
        int x = frameToX(i / (float)kSub), y = Y(v); if (have) thickLine(px, py, x, y, pass == 0 ? 1.f : 1.8f * U, pass == 0 ? SDL_Color{150, 150, 156, 110} : cc[k]); px = x; py = y; have = true; } }
  }
  if (ppf() >= P(9.f)) for (int i = i0; i <= i1; i++) { int x = frameToX(i / (float)kSub); fillR({x, ln.y + ln.h - P(5), 1, P(i % kSub == 0 ? 5 : 3)}, {255, 255, 255, 60}); }   // the dense samples
  SDL_RenderSetClipRect(R, nullptr);
}
static void drawMotionEditor() {
  reg(rMePanel); fillR(rMePanel, C_DARK); Obj* o = meObjPtr(); bool lockAll = mePrompt; if (lockAll) g_lock = true;
  { static const MotionLog* lastLog = nullptr; const MotionLog* cur = o ? o->log.get() : nullptr; if (cur != lastLog) { lastLog = cur; meDirty = true; if (!cur) g_meOutValid = false; } }   // undo / redo swapped the log
  // ---- header: back, title, Warp / Reset / Commit
  SDL_Rect hd = rMeHeader; SDL_Rect bb = {P(12), hd.y + P(8), P(60), hd.h - P(16)}; if (roundIconBtn(bb, IC_CHEV_L, C_BTN, C_WHITE, P(28), 10)) meRequestExit();
  int cw = P(land ? 112 : 120), rw = P(land ? 92 : 92), ww = P(land ? 92 : 86), chh = P(land ? 40 : 52), cy0 = hd.y + (hd.h - chh) / 2, gp = P(8);
  SDL_Rect rc = {W - P(14) - cw, cy0, cw, chh}, rr = {rc.x - gp - rw, cy0, rw, chh}, rwp = {rr.x - gp - ww, cy0, ww, chh};
  bool dirty = !meLayerClean();
  if (meChip(rc, "Commit", dirty, !dirty, C_BLUE)) meCommit();
  if (meChip(rr, "Reset", false, !dirty, C_ACC)) { meResetLayer(); }
  if (meChip(rwp, "Warp", meWarpOn, false, C_ORG)) meWarpOn = !meWarpOn;
  txt(P(86), hd.y + (land ? hd.h / 2 : P(28)), "Motion Editor", C_WHITE, FPX(land ? 25 : 27));
  if (!land) { std::string sub = o ? o->name + (o->log ? "  -  " + std::to_string(o->log->n) + " samples" : "") : "Tap a model in the viewport"; bool canRev = o && o->log && !dirty; int maxw = rwp.x - P(86) - (canRev ? P(104) : P(12));
    txt(P(86), hd.y + P(58), fitTxt(sub, maxw, FPX(19)), C_DIM, FPX(19));
    if (canRev) { SDL_Rect rv = {rwp.x - P(104), hd.y + P(42), P(96), P(34)}; if (tapIn(rv)) meRevert(); txtR(rv.x + rv.w, rv.y + rv.h / 2, "Revert", {110, 170, 220, 255}, FPX(19)); } }
  if (!o || !o->log) { txt(meLabelW, rMeLane[0].y + P(40), "Select a model to see its motion log.", C_DIM, FPX(23)); if (lockAll) g_lock = false; return; }
  const Obj& ob = *o; float pf = ppf();
  // ---- gestures: weight handles / body, lane scrubbing
  SDL_Rect zone = {rMeWeight.x, rMeWeight.y, rMeWeight.w, rMeLane[2].y + rMeLane[2].h - rMeWeight.y}; MeSel dispSel = meSel; if (meLy.warpMode == 1) dispSel.b1 = meSel.b1 + meLy.warpDelta;
  int hx[4] = {frameToX(dispSel.a0), frameToX(dispSel.a1), frameToX(dispSel.b0), frameToX(dispSel.b1)}; int yb = rMeWeight.y + rMeWeight.h - P(10), yt = rMeWeight.y + P(12), hy[4] = {yb, yt, yt, yb};
  if (!g_lock && in.pressed && !consumed && !blockedAt(in.mx, in.my) && inside(zone, in.mx, in.my) && activeId == 0) {
    meHandle = -1; float best = (float)P(44.f);
    if (inside(rMeWeight, in.mx, in.my)) { for (int k = 0; k < 4; k++) { float d = hypotf((float)(in.mx - hx[k]), (float)(in.my - hy[k])); if (d < best) { best = d; meHandle = k; } }
      if (meHandle < 0 && in.mx > hx[0] && in.mx < hx[3]) meHandle = 4; }
    activeId = meHandle >= 0 ? ID_ME + 1 : ID_ME + 2; meSelAtPress = meSel; meScrubBase = curFrame;
  }
  float fNow = curFrame + (in.mx - phx) / pf;
  if (activeId == ID_ME + 1 && in.down) {
    if (meHandle == 0) meSel.a0 = clampf(fNow, 0.f, meSel.a1); else if (meHandle == 1) meSel.a1 = clampf(fNow, meSel.a0, meSel.b0); else if (meHandle == 2) meSel.b0 = clampf(fNow, meSel.a1, meSel.b1);
    else if (meHandle == 3) { if (meWarpOn) { float nb1 = std::max(meSel.b0 + 1.f, fNow); meLy.warpMode = 1; meLy.warpDelta = nb1 - meSelAtPress.b1; } else meSel.b1 = std::max(fNow, meSel.b0); }
    else if (meHandle == 4) { float dx = (in.mx - pressX) / pf; if (meWarpOn) { meLy.warpMode = 2; meLy.warpDelta = dx; } else { float sh = std::max(dx, -meSelAtPress.a0); meSel.a0 = meSelAtPress.a0 + sh; meSel.a1 = meSelAtPress.a1 + sh; meSel.b0 = meSelAtPress.b0 + sh; meSel.b1 = meSelAtPress.b1 + sh; } }
    meDirty = true;
  } else if (activeId == ID_ME + 2 && in.down && moved) setFrame(meScrubBase - (int)lroundf((in.mx - pressX) / pf));
  if (inside(zone, in.mx, in.my) && in.pinch != 0 && !g_lock) pxPerSec = clampf(pxPerSec * expf(in.pinch * 6.f), 90.f, 1400.f);
  if (meDirty) meRecompute();
  // ---- weight strip (the falloff trapezoid)
  fillRound(rMeWeight, P(10), {20, 20, 21, 255}); txt(P(10), rMeWeight.y + rMeWeight.h / 2, land ? "Falloff" : "Weight", C_TXT, FPX(land ? 17 : 20));
  SDL_RenderSetClipRect(R, &rMeWeight);
  { std::vector<std::pair<int, int> > poly; poly.push_back(std::make_pair(hx[0], yb));
    for (int i = 1; i <= 12; i++) { float u = i / 12.f; poly.push_back(std::make_pair(hx[0] + (int)((hx[1] - hx[0]) * u), yb - (int)((yb - yt) * smooth01(u)))); }
    for (int i = 1; i <= 12; i++) { float u = i / 12.f; poly.push_back(std::make_pair(hx[2] + (int)((hx[3] - hx[2]) * u), yt + (int)((yb - yt) * smooth01(u)))); }
    fillPoly(poly, {64, 170, 235, 70}); for (size_t i = 0; i + 1 < poly.size(); i++) thickLine(poly[i].first, poly[i].second, poly[i + 1].first, poly[i + 1].second, 2.2f * U, C_ACC); }
  for (int k = 0; k < 4; k++) { bool act = activeId == ID_ME + 1 && meHandle == k; discFill(hx[k], hy[k], P(act ? 17.f : (k == 0 || k == 3 ? 12.f : 14.f)), k == 3 && meWarpOn ? C_ORG : (k == 0 || k == 3 ? SDL_Color{220, 220, 224, 255} : C_ACC)); }
  SDL_RenderSetClipRect(R, nullptr);
  // ---- ruler + lanes
  drawRuler(); if (land) drawTimeLabel(P(8), rRuler.y + rRuler.h / 2);
  for (int g = 0; g < 3; g++) meDrawLane(g, ob);
  fillR({phx - P(1.5f), rMeWeight.y, std::max(2, P(3)), rMeLane[2].y + rMeLane[2].h - rMeWeight.y}, C_WHITE);
  // ---- sliders (temporary layer controls)
  SDL_Rect sa = rMeSliders; struct SL { const char* n; float* v; SDL_Color c; } sl[6] = {{"Strength", &meLy.strength, C_ACC}, {"Default", &meLy.sDefault, {96, 200, 190, 255}}, {"Playhead", &meLy.sPlayhead, {96, 200, 190, 255}},
    {"Zero", &meLy.sZero, {96, 200, 190, 255}}, {"Smooth", &meLy.sSmooth, {110, 205, 120, 255}}, {"Jitter", &meLy.sJitter, {240, 160, 70, 255}}};
  for (int i = 0; i < 6; i++) { SDL_Rect cell; if (land) { int cwid = sa.w / 6; cell = {sa.x + i * cwid, sa.y, cwid, sa.h}; } else { int cwid = (sa.w - P(16)) / 2, chgt = sa.h / 3; cell = {sa.x + (i % 2) * (cwid + P(16)), sa.y + (i / 2) * chgt, cwid, chgt}; }
    meSliderCell(ID_SLIDER + 60 + i, cell, sl[i].n, *sl[i].v, sl[i].c); }
  if (meWarpOn) txt(rMeWeight.x + P(8), rMeWeight.y - P(2), "", C_ORG, FPX(14));
  if (lockAll) {
    g_lock = false; fillR(rMePanel, {0, 0, 0, 170}); SDL_Rect dlg = {rMePanel.x + W / 2 - P(land ? 300 : 320), rMePanel.y + (rMePanel.h - P(land ? 180 : 210)) / 2, P(land ? 600 : 640), P(land ? 180 : 210)};
    fillRound(dlg, P(20), C_PANEL); txtC({dlg.x, dlg.y + P(14), dlg.w, P(56)}, "Uncommitted motion edits", C_TXT, FPX(27)); int bh = P(land ? 52 : 62), by = dlg.y + P(land ? 80 : 96), bw = (dlg.w - P(48)) / 3;
    if (bigButton({dlg.x + P(16), by, bw, bh}, "Commit", -1, true)) { mePrompt = false; meExit(true); return; }
    if (dangerButton({dlg.x + P(24) + bw, by, bw, bh}, "Discard")) { mePrompt = false; meExit(false); return; }
    if (bigButton({dlg.x + P(32) + 2 * bw, by, bw, bh}, "Keep editing", -1)) mePrompt = false;
  }
}

// ================================================================ project files (.hx3d): binary save / load, thumbnails, auto-save
static const uint32_t kMagic = 0x44335848u;   // "HX3D"
static const uint32_t kVersion = 2;
static uint64_t wallSec() { return (uint64_t)time(nullptr); }
struct BW {
  std::ostream& o; explicit BW(std::ostream& s) : o(s) {}
  template <class T> void p(const T& v) { o.write((const char*)&v, sizeof(T)); }
  void s(const std::string& v) { uint32_t n = (uint32_t)v.size(); p(n); o.write(v.data(), n); }
  template <class T> void vec(const std::vector<T>& v) { uint64_t n = v.size(); p(n); if (n) o.write((const char*)v.data(), (std::streamsize)(n * sizeof(T))); }
};
struct BR {
  std::istream& i; bool ok; uint32_t ver; explicit BR(std::istream& s) : i(s), ok(true), ver(kVersion) {}
  template <class T> void g(T& v) { if (!ok) return; i.read((char*)&v, sizeof(T)); if (!i) ok = false; }
  std::string s() { uint32_t n = 0; g(n); if (!ok || n > (1u << 20)) { ok = false; return ""; } std::string r(n, '\0'); if (n) i.read(&r[0], n); if (!i) ok = false; return r; }
  template <class T> void vec(std::vector<T>& v) { uint64_t n = 0; g(n); if (!ok || n > (1ull << 28)) { ok = false; return; } v.resize((size_t)n); if (n) i.read((char*)v.data(), (std::streamsize)(n * sizeof(T))); if (!i) ok = false; }
};
static void writeObj(BW& w, const Obj& o) {
  w.p(o.id); w.s(o.name); uint8_t fl = (o.hidden ? 1 : 0) | (o.colSet ? 2 : 0); w.p(fl); for (int i = 0; i < 3; i++) w.p(o.col[i]);
  w.p(o.subdiv); w.p(o.smooth); for (int i = 0; i < 9; i++) w.p(o.T[i]); for (int i = 0; i < 9; i++) w.p(o.base[i]);
  uint8_t hasEm = o.em ? 1 : 0, hasImp = o.imp ? 1 : 0; w.p(hasEm); w.p(hasImp);
  if (o.em) { w.vec(o.em->v); uint64_t nf = o.em->f.size(); w.p(nf); for (size_t i = 0; i < o.em->f.size(); i++) w.vec(o.em->f[i]); w.vec(o.em->hid); }
  if (o.imp) { const mx::Mesh3& m = *o.imp; w.vec(m.pos); w.vec(m.nrm); w.vec(m.idx); w.vec(m.uv); w.vec(m.col); w.vec(m.tex); w.p(m.tw); w.p(m.th); }
  uint32_t nc = (uint32_t)o.clips.size(); w.p(nc);
  for (size_t c = 0; c < o.clips.size(); c++) { const HClip& cl = o.clips[c]; w.p(cl.start); w.p(cl.len); uint32_t nk = (uint32_t)cl.keys.size(); w.p(nk);
    for (std::map<int, HKey>::const_iterator k = cl.keys.begin(); k != cl.keys.end(); ++k) { w.p(k->first); for (int i = 0; i < 9; i++) w.p(k->second.v[i]); w.p(k->second.bz.x1); w.p(k->second.bz.y1); w.p(k->second.bz.x2); w.p(k->second.bz.y2); w.p(k->second.mode); } }
  uint32_t ns = o.skel ? (uint32_t)o.skel->size() : 0; w.p(ns); for (uint32_t i = 0; i < ns; i++) { const SkelBone& b = (*o.skel)[i]; w.p(b.x); w.p(b.y); w.p(b.z); w.p(b.parent); }
  uint8_t hasLog = o.log ? 1 : 0; w.p(hasLog); if (o.log) { w.p(o.log->n); for (int c = 0; c < 9; c++) w.vec(o.log->ch[c]); for (int c = 0; c < 9; c++) w.p(o.log->def[c]); }
}
static bool readObj(BR& r, Obj& o) {
  r.g(o.id); o.name = r.s(); uint8_t fl = 0; r.g(fl); o.hidden = (fl & 1) != 0; o.colSet = (fl & 2) != 0; for (int i = 0; i < 3; i++) r.g(o.col[i]);
  r.g(o.subdiv); r.g(o.smooth); for (int i = 0; i < 9; i++) r.g(o.T[i]); for (int i = 0; i < 9; i++) r.g(o.base[i]);
  uint8_t hasEm = 0, hasImp = 0; r.g(hasEm); r.g(hasImp);
  if (hasEm) { o.em = std::make_shared<EditMesh>(); r.vec(o.em->v); uint64_t nf = 0; r.g(nf); if (!r.ok || nf > (1ull << 26)) return false; o.em->f.resize((size_t)nf); for (size_t i = 0; i < nf && r.ok; i++) r.vec(o.em->f[i]); r.vec(o.em->hid);
    if (!r.ok) return false; for (size_t i = 0; i < o.em->f.size(); i++) for (size_t k = 0; k < o.em->f[i].size(); k++) if (o.em->f[i][k] < 0 || o.em->f[i][k] >= (int)o.em->v.size()) return false; emFix(*o.em); }
  if (hasImp) { o.imp = std::make_shared<mx::Mesh3>(); mx::Mesh3& m = *o.imp; r.vec(m.pos); r.vec(m.nrm); r.vec(m.idx); r.vec(m.uv); r.vec(m.col); r.vec(m.tex); r.g(m.tw); r.g(m.th);
    if (!r.ok) return false; for (size_t i = 0; i < m.idx.size(); i++) if (m.idx[i] >= m.vcount()) return false; mx::calcBounds(m); }
  uint32_t nc = 0; r.g(nc); if (!r.ok || nc > 100000) return false; o.clips.resize(nc);
  for (uint32_t c = 0; c < nc && r.ok; c++) { HClip& cl = o.clips[c]; r.g(cl.start); r.g(cl.len); uint32_t nk = 0; r.g(nk); if (nk > 1000000) return false;
    for (uint32_t k = 0; k < nk && r.ok; k++) { int rel = 0; HKey key; r.g(rel); for (int i = 0; i < 9; i++) r.g(key.v[i]); r.g(key.bz.x1); r.g(key.bz.y1); r.g(key.bz.x2); r.g(key.bz.y2); r.g(key.mode); cl.keys[rel] = key; } }
  uint32_t ns = 0; r.g(ns); if (!r.ok || ns > 100000) return false;
  if (ns) { o.skel = std::make_shared<std::vector<SkelBone> >(ns); for (uint32_t i = 0; i < ns; i++) { SkelBone& b = (*o.skel)[i]; r.g(b.x); r.g(b.y); r.g(b.z); r.g(b.parent); } }
  if (r.ver >= 2) { uint8_t hasLog = 0; r.g(hasLog); if (hasLog) { o.log = std::make_shared<MotionLog>(); r.g(o.log->n); for (int c = 0; c < 9; c++) r.vec(o.log->ch[c]); for (int c = 0; c < 9; c++) r.g(o.log->def[c]);
      if (!r.ok || o.log->n < 0 || o.log->n > 5000000) return false; for (int c = 0; c < 9; c++) if ((int)o.log->ch[c].size() != o.log->n) return false; } }
  if (!r.ok) return false; rebuildObj(o); return true;
}
static void writeDoc(BW& w) {
  w.p(doc.nextId); w.p(doc.sel); w.p(doc.selClip); w.p(cam.yaw); w.p(cam.pitch); w.p(cam.dist); w.p(cam.tgt.x); w.p(cam.tgt.y); w.p(cam.tgt.z);
  w.p(curFrame); w.p(g_bg.r); w.p(g_bg.g); w.p(g_bg.b); uint8_t fl = (showGrid ? 1 : 0) | (wireMode ? 2 : 0); w.p(fl); w.p(pxPerSec);
  uint32_t n = (uint32_t)doc.objs.size(); w.p(n); for (uint32_t i = 0; i < n; i++) writeObj(w, doc.objs[i]);
}
static bool readDoc(BR& r) {
  Doc d; r.g(d.nextId); r.g(d.sel); r.g(d.selClip); Cam c; r.g(c.yaw); r.g(c.pitch); r.g(c.dist); r.g(c.tgt.x); r.g(c.tgt.y); r.g(c.tgt.z);
  int frame = 0; r.g(frame); SDL_Color bg = C_VP; r.g(bg.r); r.g(bg.g); r.g(bg.b); uint8_t fl = 0; r.g(fl); float pps = 330.f; r.g(pps);
  uint32_t n = 0; r.g(n); if (!r.ok || n > 100000) return false; d.objs.resize(n);
  for (uint32_t i = 0; i < n; i++) if (!readObj(r, d.objs[i])) return false;
  if (d.sel >= (int)n) d.sel = -1;
  doc = d; cam = c; g_bg = bg; g_bg.a = 255; showGrid = (fl & 1) != 0; wireMode = (fl & 2) != 0; pxPerSec = clampf(pps, 90.f, 1400.f); curFrame = std::max(0, frame); applyAnim(); return true;
}
static std::string projDir() {
  static std::string d; if (!d.empty()) return d;
  char* p = SDL_GetPrefPath("Hexagon3D", "projects"); if (p) { d = p; SDL_free(p); } else d = mx::exportDir() + "/Hexagon3D_projects";
  while (d.size() > 1 && d.back() == '/') d.pop_back(); mx::makeDir(d); return d;
}
struct ProjInfo { std::string path, name; uint32_t version; uint64_t created, modified; int tw, th; std::vector<uint8_t> thumb; SDL_Texture* tex; ProjInfo() : version(kVersion), created(0), modified(0), tw(0), th(0), tex(nullptr) {} };
static std::vector<ProjInfo> g_projects;
static std::string curProjPath, curProjName; static uint64_t curProjCreated = 0, lastSavedHash = 0; static Uint32 lastAutoTick = 0;

static bool saveProjectTo(const std::string& path, const std::string& name, uint64_t created) {
  const int tw = 192, th = 144; mx::Image im; im.init(tw, th, g_bg.r, g_bg.g, g_bg.b, 255);
  Cam tc = cam; { V3 lo = {1e30f, 1e30f, 1e30f}, hi = {-1e30f, -1e30f, -1e30f}; bool any = false;   // thumbnail: frame every visible object
    for (size_t i = 0; i < doc.objs.size(); i++) { const Obj& o = doc.objs[i]; if (o.hidden || !o.rm) continue; V3 c = objCenterW(o); float r = o.rad * maxScale(o); any = true;
      lo = {std::min(lo.x, c.x - r), std::min(lo.y, c.y - r), std::min(lo.z, c.z - r)}; hi = {std::max(hi.x, c.x + r), std::max(hi.y, c.y + r), std::max(hi.z, c.z + r)}; }
    if (any) { tc.tgt = (lo + hi) * 0.5f; tc.dist = clampf(len3(hi - lo) * 1.05f, 3.f, 90.f); } }
  renderDoc(im, tc, doc, false);
  std::string tmp = path + ".tmp"; std::ofstream f(tmp.c_str(), std::ios::binary); if (!f) return false; BW w(f);
  w.p(kMagic); w.p(kVersion); w.s(name); w.p(created); uint64_t mod = wallSec(); w.p(mod); int32_t a = tw, b = th; w.p(a); w.p(b); f.write((const char*)im.rgba.data(), (std::streamsize)tw * th * 4);
  writeDoc(w); f.close(); if (!f) { std::remove(tmp.c_str()); return false; }
  std::remove(path.c_str()); return std::rename(tmp.c_str(), path.c_str()) == 0;
}
static bool readHeader(const std::string& path, ProjInfo& pi, BR** keep = nullptr, std::ifstream* fs = nullptr) {
  std::ifstream local; std::ifstream& f = fs ? *fs : local; f.open(path.c_str(), std::ios::binary); if (!f) return false; BR r(f);
  uint32_t m = 0, v = 0; r.g(m); r.g(v); if (!r.ok || m != kMagic || v < 1 || v > kVersion) return false;
  pi.version = v; pi.path = path; pi.name = r.s(); r.g(pi.created); r.g(pi.modified); int32_t tw = 0, th = 0; r.g(tw); r.g(th); if (!r.ok || tw < 0 || th < 0 || tw > 1024 || th > 1024) return false;
  pi.tw = tw; pi.th = th; pi.thumb.resize((size_t)tw * th * 4); f.read((char*)pi.thumb.data(), (std::streamsize)pi.thumb.size()); if (!f) return false; (void)keep; return true;
}
static bool loadProjectFrom(const std::string& path, ProjInfo& pi) {
  std::ifstream f; if (!readHeader(path, pi, nullptr, &f)) return false; BR r(f); r.ver = pi.version; return readDoc(r);
}
static void freeProjectTextures() { for (size_t i = 0; i < g_projects.size(); i++) if (g_projects[i].tex) { SDL_DestroyTexture(g_projects[i].tex); g_projects[i].tex = nullptr; } }
static void refreshProjects() {
  freeProjectTextures(); g_projects.clear(); std::string dir = projDir(); DIR* d = opendir(dir.c_str());
  if (d) { while (dirent* e = readdir(d)) { std::string n = e->d_name; if (n.size() < 6 || n.substr(n.size() - 5) != ".hx3d") continue; ProjInfo pi; if (readHeader(dir + "/" + n, pi)) g_projects.push_back(pi); } closedir(d); }
  std::sort(g_projects.begin(), g_projects.end(), [](const ProjInfo& a, const ProjInfo& b) { return a.modified > b.modified; });
  for (size_t i = 0; i < g_projects.size(); i++) { ProjInfo& p = g_projects[i]; if (p.tw > 0 && p.th > 0) { p.tex = SDL_CreateTexture(R, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, p.tw, p.th); if (p.tex) SDL_UpdateTexture(p.tex, nullptr, p.thumb.data(), p.tw * 4); } p.thumb.clear(); p.thumb.shrink_to_fit(); }
}
static uint64_t docHash() {   // cheap content hash: changes whenever anything worth saving changes
  uint64_t h = 1469598103934665603ull; auto mixv = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
  mixv((uint64_t)doc.nextId); mixv((uint64_t)(doc.sel + 7)); mixv((uint64_t)curFrame); mixv(showGrid ? 1 : 2); mixv(wireMode ? 3 : 4); mixv(g_bg.r); mixv(g_bg.g); mixv(g_bg.b);
  for (float v : {cam.yaw, cam.pitch, cam.dist, cam.tgt.x, cam.tgt.y, cam.tgt.z}) mixv(hf32(v));
  for (size_t i = 0; i < doc.objs.size(); i++) { const Obj& o = doc.objs[i]; mixv((uint64_t)o.id); for (char ch : o.name) mixv((uint8_t)ch); mixv(o.hidden); mixv(o.subdiv); mixv(hf32(o.smooth));
    for (int k = 0; k < 9; k++) { mixv(hf32(o.T[k])); mixv(hf32(o.base[k])); } for (int k = 0; k < 3; k++) mixv(o.col[k]);
    for (size_t c = 0; c < o.clips.size(); c++) { mixv((uint64_t)o.clips[c].start); mixv((uint64_t)o.clips[c].len); for (std::map<int, HKey>::const_iterator k = o.clips[c].keys.begin(); k != o.clips[c].keys.end(); ++k) { mixv((uint64_t)k->first); for (int q = 0; q < 9; q++) mixv(hf32(k->second.v[q])); mixv(hf32(k->second.bz.x1)); mixv(hf32(k->second.bz.y1)); mixv(hf32(k->second.bz.x2)); mixv(hf32(k->second.bz.y2)); mixv(k->second.mode); } }
    if (o.em) { for (size_t k = 0; k < o.em->v.size(); k++) { mixv(hf32(o.em->v[k].x)); mixv(hf32(o.em->v[k].y)); mixv(hf32(o.em->v[k].z)); } mixv(o.em->f.size()); for (size_t k = 0; k < o.em->hid.size(); k++) mixv(o.em->hid[k]); }
    else if (o.imp) mixv((uint64_t)(uintptr_t)o.imp.get());
    if (o.log) { mixv((uint64_t)o.log->n); for (int c = 0; c < 9; c++) for (size_t k = 0; k < o.log->ch[c].size(); k += 7) mixv(hf32(o.log->ch[c][k])); } }
  return h;
}
static bool saveCurrent() {
  if (curProjPath.empty()) return false; bool ok = saveProjectTo(curProjPath, curProjName, curProjCreated);
  if (ok) lastSavedHash = docHash(); else setStatus("Auto-save failed - check storage space"); return ok;
}
static void autosaveTick(bool force = false) {
  if (!g_autosave) return;
  if (screen != SCR_EDITOR || curProjPath.empty()) return; Uint32 now = SDL_GetTicks();
  if (!force && (now - lastAutoTick < 1200 || in.down || activeId != 0)) return; lastAutoTick = now;
  uint64_t h = docHash(); if (h != lastSavedHash) saveCurrent();
}
static void resetEditorState() {
  meForceClose(); doc = Doc(); undoS.clear(); redoS.clear(); cam = Cam(); curFrame = 0; playing = false; tool = 0; modeling = false; selMode = 0; clearModelSel(); sheet = SH_NONE; g_bg = C_VP; showGrid = true; wireMode = false;
  pxPerSec = 330.f; vpHash = 0; activeId = 0; barScroll = 0; sheetScroll = 0; outlineScroll = 0; presetScroll = 0;
}
static void newProject() {
  int maxN = 0; for (size_t i = 0; i < g_projects.size(); i++) { int n = 0; if (sscanf(g_projects[i].name.c_str(), "Project %d", &n) == 1) maxN = std::max(maxN, n); }
  resetEditorState(); curProjName = "Project " + std::to_string(maxN + 1); curProjCreated = wallSec();
  char buf[64]; snprintf(buf, sizeof buf, "/proj_%llu_%u.hx3d", (unsigned long long)curProjCreated, (unsigned)(SDL_GetTicks() & 0xFFFF)); curProjPath = projDir() + buf;
  lastSavedHash = docHash(); lastAutoTick = SDL_GetTicks(); screen = SCR_EDITOR;
}
static bool openProject(const std::string& path) {
  resetEditorState(); ProjInfo pi; if (!loadProjectFrom(path, pi)) { resetEditorState(); setStatus("Could not open this project (file damaged?)"); return false; }
  curProjPath = path; curProjName = pi.name; curProjCreated = pi.created; lastSavedHash = docHash(); lastAutoTick = SDL_GetTicks(); screen = SCR_EDITOR; return true;
}
static void goHome() {
  if (screen == SCR_EDITOR) autosaveTick(true);
  meForceClose(); playing = false; modeling = false; sheet = SH_NONE; screen = SCR_HOME; curProjPath.clear(); doc = Doc(); undoS.clear(); redoS.clear(); vpHash = 0; refreshProjects();
}
static void duplicateProject(const ProjInfo& src) {
  resetEditorState(); ProjInfo pi; if (!loadProjectFrom(src.path, pi)) { setStatus("Duplicate failed"); return; }
  uint64_t t = wallSec(); char buf[64]; snprintf(buf, sizeof buf, "/proj_%llu_%u.hx3d", (unsigned long long)t, (unsigned)(SDL_GetTicks() & 0xFFFF));
  saveProjectTo(projDir() + buf, src.name + " copy", t); doc = Doc(); undoS.clear(); redoS.clear(); refreshProjects(); setStatus("Project duplicated");
}

// ================================================================ home screen (shown first): New Project + saved projects
static int homeSheet = 0;   // 0 none, 1 settings, 2 project options
static int optIdx = -1; static bool confirmDel = false, confirmAll = false; static float homeScroll = 0.f;
static const SDL_Color C_HBG = {25, 25, 26, 255};
static void txtB(int x, int cy, const std::string& s, SDL_Color c, int px) { txt(x, cy, s, c, px); txt(x + std::max(1, px / 22), cy, s, c, px); }
static void fillRoundGrad(SDL_Rect r, int rad, SDL_Color c1, SDL_Color c2) {
  for (int y = 0; y < r.h; y++) {
    float t = y / (float)std::max(1, r.h - 1); SDL_Color c = {(Uint8)(c1.r + (c2.r - c1.r) * t), (Uint8)(c1.g + (c2.g - c1.g) * t), (Uint8)(c1.b + (c2.b - c1.b) * t), 255};
    int yy = std::min(y, r.h - 1 - y), inset = 0; if (yy < rad) { float dyc = rad - (yy + 0.5f); inset = (int)ceilf(rad - sqrtf(std::max(0.f, (float)rad * rad - dyc * dyc))); }
    fillR({r.x + inset, r.y + y, r.w - 2 * inset, 1}, c);
  }
}
static std::string fmtDate(uint64_t t) { time_t tt = (time_t)t; struct tm* lt = localtime(&tt); char b[48] = ""; if (lt) strftime(b, sizeof b, "%b %d, %Y  %H:%M", lt); return b; }
static bool dangerButton(SDL_Rect r, const std::string& label) {
  bool tap = tapIn(r); fillRound(r, P(16), heldIn(r) ? SDL_Color{150, 56, 56, 255} : SDL_Color{120, 44, 44, 255}); txtC(r, label, {255, 224, 224, 255}, FPX(25)); return tap;
}
static void drawHomeSheets() {
  if (homeSheet == 0) return;
  int sh = homeSheet == 2 ? (confirmDel ? P(470) : P(430)) : P(700); sh = std::min(sh, land ? H - P(48) : H - P(120));
  SDL_Rect s = land ? SDL_Rect{W / 2 - P(380), (H - sh) / 2, P(760), sh} : SDL_Rect{0, H - sh, W, sh};
  if (tapNow && !consumed && !inside(s, pressX, pressY)) { consumed = true; homeSheet = 0; confirmDel = confirmAll = false; return; }
  int pad = P(26); fillRound(s, P(26), C_PANEL);
  const ProjInfo* pi = (homeSheet == 2 && optIdx >= 0 && optIdx < (int)g_projects.size()) ? &g_projects[optIdx] : nullptr; if (homeSheet == 2 && !pi) { homeSheet = 0; return; }
  txt(s.x + pad, s.y + P(40), homeSheet == 1 ? "Settings" : fitTxt(pi->name, s.w - P(160), FPX(30)), C_TXT, FPX(30));
  SDL_Rect cb = {s.x + s.w - P(76), s.y + P(8), P(64), P(64)}; if (tapIn(cb)) { homeSheet = 0; confirmDel = confirmAll = false; return; } icon(IC_CLOSE, cb.x + cb.w / 2, cb.y + cb.h / 2, P(26), C_DIM);
  SDL_Rect body = {s.x + pad, s.y + P(88), s.w - pad * 2, s.h - P(100)}; int bh = P(68), y = body.y;
  if (homeSheet == 2) {
    txt(body.x, y + P(10), "Edited " + fmtDate(pi->modified), C_DIM, FPX(21)); y += P(40);
    if (bigButton({body.x, y, body.w, bh}, "Open", IC_PLAY)) { std::string p = pi->path; homeSheet = 0; openProject(p); return; } y += bh + P(12);
    if (bigButton({body.x, y, body.w, bh}, "Duplicate", IC_COPY)) { ProjInfo c = *pi; homeSheet = 0; duplicateProject(c); return; } y += bh + P(12);
    if (!confirmDel) { if (dangerButton({body.x, y, body.w, bh}, "Delete")) confirmDel = true; }
    else { txt(body.x, y + P(14), "Delete this project forever?", {255, 200, 200, 255}, FPX(23)); y += P(40);
      if (dangerButton({body.x + body.w / 2 + P(6), y, body.w / 2 - P(6), bh}, "Yes, delete")) { std::remove(pi->path.c_str()); homeSheet = 0; confirmDel = false; refreshProjects(); setStatus("Project deleted"); return; }
      if (bigButton({body.x, y, body.w / 2 - P(6), bh}, "Cancel", -1)) confirmDel = false; }
  } else {
    txt(body.x, y + P(14), "Auto-save is always on. Every change is saved.", C_DIM, FPX(21)); y += P(40);
    txt(body.x, y + P(12), "Projects folder", C_DIM, FPX(19)); txt(body.x, y + P(40), fitTxt(projDir(), body.w, FPX(21)), C_TXT, FPX(21)); y += P(72);
    txt(body.x, y + P(10), "Developer tools", C_DIM, FPX(19)); y += P(30);
    if (bigButton({body.x, y, body.w, bh - P(6)}, "Timeline self-test", IC_GRAPH)) runJob("Timeline test", [] { RunTimelineSelfTest(); setStatus("Timeline test done"); }); y += bh;
    if (bigButton({body.x, y, body.w, bh - P(6)}, "Render SFM image", IC_CAMERA)) runJob("SFM render", [] { RunSfmRender(); setStatus("SFM render done (sfm_render.bmp)"); }); y += bh;
    if (bigButton({body.x, y, body.w, bh - P(6)}, "Rig preview (console)", IC_MOVE)) { RigAsciiPreview(); setStatus("Rig preview printed to the console"); } y += bh + P(6);
    if (!confirmAll) { if (dangerButton({body.x, y, body.w, bh - P(6)}, "Delete all projects")) confirmAll = true; }
    else { if (dangerButton({body.x + body.w / 2 + P(6), y, body.w / 2 - P(6), bh - P(6)}, "Yes, delete all")) { for (size_t i = 0; i < g_projects.size(); i++) std::remove(g_projects[i].path.c_str()); confirmAll = false; refreshProjects(); setStatus("All projects deleted"); }
      if (bigButton({body.x, y, body.w / 2 - P(6), bh - P(6)}, "Cancel", -1)) confirmAll = false; }
    txtC({s.x, s.y + s.h - P(52), s.w, P(30)}, "Hexagon3D 1.0", C_OFF, FPX(19));
  }
}
static void drawHome() {
  fillR({0, 0, W, H}, C_HBG); bool sheetOpen = homeSheet != 0; g_lock = sheetOpen;
  int M = P(31), colW = std::min(W - 2 * M, P(1000)), x0 = (W - colW) / 2; bool has = !g_projects.empty();
  int btnH = land ? P(110) : P(161), btnY = (land ? P(96) : P(191)) - (int)homeScroll;
  // gear (fixed)
  SDL_Rect gear = {W - P(31) - P(64), P(54), P(64), P(64)}; if (tapIn(gear)) homeSheet = 1; icon(IC_GEAR, gear.x + gear.w / 2, gear.y + gear.h / 2, P(42), {178, 178, 178, 255});
  // New Project
  SDL_Rect nb = {x0, btnY, colW, btnH}; bool held = heldIn(nb); int lift = held ? 14 : 0;
  fillRoundGrad(nb, P(14), {(Uint8)std::min(255, 109 + lift), (Uint8)std::min(255, 174 + lift), (Uint8)std::min(255, 209 + lift), 255}, {(Uint8)std::min(255, 59 + lift), (Uint8)std::min(255, 142 + lift), (Uint8)std::min(255, 197 + lift), 255});
  icon(IC_PLUS, nb.x + nb.w / 2, nb.y + nb.h * (land ? 30 : 36) / 100, P(34), C_WHITE); { int px = FPX(31); std::string s = "New Project"; txtB(nb.x + (nb.w - textW(s, px)) / 2, nb.y + nb.h * (land ? 72 : 70) / 100, s, C_WHITE, px); }
  if (tapIn(nb) && !g_busy) { homeSheet = 0; newProject(); g_lock = false; return; }
  int gap = has ? P(44) : (land ? P(70) : P(376)); int headY = nb.y + nb.h + gap + P(16);
  { int px = FPX(27); txtB(x0 + P(1), headY, "Projects", C_WHITE, px); }
  int contentBottom;
  if (!has) {
    icon(IC_SPARKLE, W / 2, headY + P(98), P(44), {118, 118, 118, 255});
    txtC({0, headY + P(138), W, P(34)}, "Start your", {118, 118, 118, 255}, FPX(25)); txtC({0, headY + P(177), W, P(34)}, "next masterpiece now.", {118, 118, 118, 255}, FPX(25)); contentBottom = headY + P(230);
  } else {
    int cols = std::max(2, (colW + P(20)) / (P(300) + P(20))), cw = (colW - P(20) * (cols - 1)) / cols, th = (cw - P(16)) * 3 / 4, ch = th + P(16) + P(86), y = headY + P(36);
    for (size_t i = 0; i < g_projects.size(); i++) {
      ProjInfo& p = g_projects[i]; int col = (int)i % cols, row = (int)i / cols; SDL_Rect card = {x0 + col * (cw + P(20)), y + row * (ch + P(20)), cw, ch};
      if (card.y + card.h < 0 || card.y > H) continue;
      SDL_Rect more = {card.x + card.w - P(64), card.y + P(14), P(52), P(52)};
      bool tapMore = tapIn(more), tapCard = !tapMore && tapIn(card);
      fillRound(card, P(18), heldIn(card) ? SDL_Color{44, 44, 46, 255} : SDL_Color{36, 36, 38, 255});
      SDL_Rect tr = {card.x + P(8), card.y + P(8), cw - P(16), th}; if (p.tex) SDL_RenderCopy(R, p.tex, nullptr, &tr); else fillR(tr, {50, 50, 52, 255});
      fillRound(more, P(26), {0, 0, 0, 130}); icon(IC_DOTS, more.x + more.w / 2, more.y + more.h / 2, P(30), C_WHITE);
      txt(card.x + P(18), card.y + P(8) + th + P(30), fitTxt(p.name, cw - P(36), FPX(24)), C_WHITE, FPX(24)); txt(card.x + P(18), card.y + P(8) + th + P(62), fitTxt(fmtDate(p.modified), cw - P(36), FPX(18)), C_DIM, FPX(18));
      if (tapMore) { optIdx = (int)i; homeSheet = 2; confirmDel = false; } else if (tapCard) { std::string path = p.path; g_lock = false; openProject(path); return; }
    }
    int rows = ((int)g_projects.size() + cols - 1) / cols; contentBottom = y + rows * (ch + P(20));
  }
  float contentH = (float)(contentBottom + (int)homeScroll + P(80)); if (!sheetOpen) { SDL_Rect area = {0, P(130), W, H - P(130)}; dragScroll(ID_SCROLL + 9, area, homeScroll, contentH, false); } else homeScroll = homeScroll;
  g_lock = false; drawHomeSheets(); g_lock = false;
}

// ================================================================ viewport drawing
// ================================================================ SOURCE HEXAGON skin (landscape layout from the Source Prisma reference; sidebar + tabs + animation panel)
// Pure presentation layer: every control calls the engine's existing functions (import, undo, clips, keys, export, sheets ...).
static SDL_Color S_TOP = {24, 19, 45, 255}, S_SB = {15, 15, 28, 255}, S_SURF = {20, 20, 42, 255}, S_BORD = {42, 42, 72, 255}, S_PUR = {124, 58, 237, 255},
  S_PURL = {167, 120, 250, 255}, S_PURD = {61, 29, 120, 255}, S_TXT = {205, 205, 226, 255}, S_MUT = {104, 104, 142, 255}, S_SEC = {138, 138, 172, 255},
  S_RED = {248, 113, 113, 255}, S_GRN = {134, 239, 172, 255}, S_CYN = {103, 232, 249, 255}, S_AMB = {251, 191, 36, 255}, S_LANE = {9, 9, 15, 255}, S_RULER = {26, 22, 40, 255};
static bool mayaOn();
static void setPalette(bool maya) {   // Maya mode: neutral greys / Maya blue instead of the classic purple
  if (maya) { S_TOP = {68, 68, 68, 255}; S_SB = {60, 60, 60, 255}; S_SURF = {42, 42, 42, 255}; S_BORD = {30, 30, 30, 255}; S_PUR = {82, 133, 166, 255}; S_PURL = {196, 208, 218, 255}; S_PURD = {50, 66, 80, 255};
    S_TXT = {204, 204, 204, 255}; S_MUT = {128, 128, 128, 255}; S_SEC = {160, 160, 160, 255}; }
  else { S_TOP = {24, 19, 45, 255}; S_SB = {15, 15, 28, 255}; S_SURF = {20, 20, 42, 255}; S_BORD = {42, 42, 72, 255}; S_PUR = {124, 58, 237, 255}; S_PURL = {167, 120, 250, 255}; S_PURD = {61, 29, 120, 255};
    S_TXT = {205, 205, 226, 255}; S_MUT = {104, 104, 142, 255}; S_SEC = {138, 138, 172, 255}; }
}
enum { SK_TABS = 8 };
static const char* kSkTabs[SK_TABS] = {"SCENE", "OBJECT", "MATERIAL", "RENDER", "EDIT", "OBJECTS", "3D EFFECTS", "ANIMATION"};
static int sk_tab = 0, sk_tlTool = 0, sk_durVal = 10, sk_durUnit = 0, sk_dragKind = 0, sk_sbTotal = 0, sk_sbCur = 0;
static float sk_tabScroll = 0, sk_sbScroll = 0, sk_tbScroll = 0, sk_ctlScroll = 0, sk_scroll = 0, sk_scrollPress = 0, sk_fpsAvg = 60.f, sk_tbW = 0, sk_ctlW = 0;
static bool sk_sbOpen = true, sk_proc = false, sk_fs = true, sk_path = false;
static bool sk_delPrompt = false; static int sk_delClip = -1; static const Uint32 kHoldDeleteMs = 5000;   // hold a clip this long -> "Delete the clip?"
static SDL_Rect sk_rSb, sk_rTop, sk_rTabs, sk_rBody, sk_rStat, sk_rAnim, sk_rHamb, sk_rLane, sk_rRulerR;
static bool skinOn() { return g_skin && screen == SCR_EDITOR && !modeling && !g_meOpen; }
static int skTotalFrames() { int unit = sk_durUnit == 0 ? 1 : sk_durUnit == 1 ? 60 : 3600; return std::max(sk_durVal * unit * kFps, docEnd()); }
static float skPF() { return pxPerSec * U / kFps; }
static int skX(float f) { return sk_rLane.x + (int)lroundf((f - sk_scroll) * skPF()); }
static float skF(int x) { return sk_scroll + (x - sk_rLane.x) / skPF(); }
static void mayaLayout();
static void skinLayout() {
  if (g_maya && land) { mayaLayout(); return; }
  int top = P(40), sbw = P(273), stat = P(28), anim = P(179);
  bool side = land && sk_sbOpen, an = sk_tab == SK_TABS - 1; int vx = side ? sbw : 0;
  sk_rTop = {0, 0, W, top}; sk_rSb = {0, top, sk_sbOpen ? sbw : 0, H - top};
  sk_rTabs = {0, top, sk_rSb.w, P(31)}; sk_rBody = {0, top + P(31) + 1, sk_rSb.w, H - top - P(31) - 1};
  VP = {vx, top + 1, W - vx, H - top - 1 - (an ? anim : stat)};
  sk_rStat = {vx, H - stat, W - vx, stat}; sk_rAnim = {vx, H - anim, W - vx, anim};
  sk_rHamb = {W - P(46), top + P(21), P(34), P(34)};
  sk_rRulerR = {vx + P(121), sk_rAnim.y + P(56), W - vx - P(121), P(28)}; sk_rLane = {vx + P(121), sk_rAnim.y + P(85), W - vx - P(121), P(94)};
  rLeft = {0, 0, vx, H}; rRuler = an ? sk_rRulerR : SDL_Rect{vx, H - P(60), W - vx, P(28)}; rTrack = an ? sk_rLane : SDL_Rect{vx, H - P(32), W - vx, P(32)};
  rowCy = sk_rLane.y + sk_rLane.h / 2; trackClipH = P(50);
}

// ---- text with the few non-ASCII glyphs the reference uses (the bundled font is ASCII only)
static std::vector<std::pair<int, std::string> > skRuns(const std::string& s) {
  std::vector<std::pair<int, std::string> > v; std::string cur;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { cur += (char)c; i++; continue; }
    int cp = 0, n = 1; if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; } else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; } else if ((c & 0xF8) == 0xF0) { cp = c & 7; n = 4; }
    for (int k = 1; k < n && i + k < s.size(); k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
    i += n; if (!cur.empty()) { v.push_back(std::make_pair(0, cur)); cur.clear(); } v.push_back(std::make_pair(cp, std::string()));
  }
  if (!cur.empty()) v.push_back(std::make_pair(0, cur)); return v;
}
static int skSpW(int cp, int px) { switch (cp) { case 0x2014: return px; case 0xB7: return px * 5 / 10; case 0x2192: return px * 11 / 10; case 0x2026: return px; case 0xB0: return px * 6 / 10; case 0xD7: return px * 8 / 10; default: return px * 6 / 10; } }
static void skSpDraw(int cp, int x, int cy, int px, SDL_Color c) {
  float w = std::max(1.2f, px * 0.085f); int w2 = skSpW(cp, px), base = cy + (int)(uif::kCap * px * 0.5f);
  if (cp == 0x2014) thickLine(x + px / 16, cy, x + px - px / 16, cy, w, c);
  else if (cp == 0xB7) discFill(x + w2 / 2, cy, std::max(1, px / 12), c);
  else if (cp == 0x2192) { int ax = x + w2 - px / 12; thickLine(x + px / 12, cy, ax, cy, w, c); thickLine(ax, cy, ax - px * 3 / 10, cy - px * 3 / 10, w, c); thickLine(ax, cy, ax - px * 3 / 10, cy + px * 3 / 10, w, c); }
  else if (cp == 0x2026) for (int i = 0; i < 3; i++) discFill(x + px / 6 + i * px * 3 / 10, base - px / 12, std::max(1, px / 12), c);
  else if (cp == 0xB0) { int r = std::max(2, px / 7), ccx = x + w2 / 2, ccy = cy - px * 3 / 10; for (int i = 0; i < 10; i++) { float a0 = 6.2832f * i / 10, a1 = 6.2832f * (i + 1) / 10; thickLine(ccx + (int)(r * cosf(a0)), ccy + (int)(r * sinf(a0)), ccx + (int)(r * cosf(a1)), ccy + (int)(r * sinf(a1)), w, c); } }
  else if (cp == 0xD7) { int s = px / 4, ccx = x + w2 / 2; thickLine(ccx - s, cy - s, ccx + s, cy + s, w, c); thickLine(ccx - s, cy + s, ccx + s, cy - s, w, c); }
}
static int skW(const std::string& s, int px) { int w = 0; std::vector<std::pair<int, std::string> > r = skRuns(s); for (size_t i = 0; i < r.size(); i++) w += r[i].first ? skSpW(r[i].first, px) : textW(r[i].second, px); return w; }
static void skTxt(int x, int cy, const std::string& s, SDL_Color c, int px, bool bold = false) {
  std::vector<std::pair<int, std::string> > r = skRuns(s);
  for (size_t i = 0; i < r.size(); i++) {
    if (r[i].first) { skSpDraw(r[i].first, x, cy, px, c); x += skSpW(r[i].first, px); }
    else { txt(x, cy, r[i].second, c, px); if (bold) txt(x + std::max(1, px / 18), cy, r[i].second, c, px); x += textW(r[i].second, px); }
  }
}
static void skTxtC(SDL_Rect r, const std::string& s, SDL_Color c, int px, bool bold = false) { skTxt(r.x + (r.w - skW(s, px)) / 2, r.y + r.h / 2, s, c, px, bold); }
static void skTxtR(int xr, int cy, const std::string& s, SDL_Color c, int px, bool bold = false) { skTxt(xr - skW(s, px), cy, s, c, px, bold); }
static int skSpacedW(const std::string& s, int px, float sp) { return textW(s, px) + (int)(sp * (s.size() ? s.size() - 1 : 0)); }
static void skSpaced(int x, int cy, const std::string& s, SDL_Color c, int px, float sp, bool bold = false) {
  float pen = (float)x; for (size_t i = 0; i < s.size(); i++) { std::string ch(1, s[i]); txt((int)pen, cy, ch, c, px); if (bold) txt((int)pen + std::max(1, px / 18), cy, ch, c, px); pen += textW(ch, px) + sp; }
}

// ---- boxes + buttons
static void skBox(SDL_Rect r, int rad, SDL_Color fill, SDL_Color border) { int b = std::max(1, P(1.f)); fillRound(r, rad, border); fillRound({r.x + b, r.y + b, r.w - 2 * b, r.h - 2 * b}, std::max(0, rad - b), fill); }
static void skDashed(SDL_Rect r, SDL_Color c) {
  int d = P(5.f), g = P(4.f), t = std::max(1, P(1.f));
  for (int x = r.x; x < r.x + r.w; x += d + g) { int w = std::min(d, r.x + r.w - x); fillR({x, r.y, w, t}, c); fillR({x, r.y + r.h - t, w, t}, c); }
  for (int y = r.y; y < r.y + r.h; y += d + g) { int h = std::min(d, r.y + r.h - y); fillR({r.x, y, t, h}, c); fillR({r.x + r.w - t, y, t, h}, c); }
}
// ---- film-strip clip: dark body, diagonal-striped top/bottom bands, sprocket-hole rails on both ends (timeline lane clips)
static void skClipStrip(SDL_Rect cr, bool sel, const std::string& dur, bool hasKeys) {
  SDL_Rect oldClip; bool hadClip = SDL_RenderIsClipEnabled(R) == SDL_TRUE; SDL_RenderGetClipRect(R, &oldClip);
  auto isect = [&](SDL_Rect r) { if (hadClip) { SDL_Rect o; if (!SDL_IntersectRect(&r, &oldClip, &o)) o = {0, 0, 0, 0}; r = o; } SDL_RenderSetClipRect(R, &r); };
  auto restore = [&]() { SDL_RenderSetClipRect(R, hadClip ? &oldClip : nullptr); };
  const int rad = P(6.f), b = std::max(1, P(1.f)), sw = std::min(P(14.f), std::max(P(4.f), cr.w / 4)), bh = P(7.f), per = P(10.f), sd = P(5.f);
  const SDL_Color brd = sel ? SDL_Color{167, 120, 250, 255} : SDL_Color{124, 58, 237, 255}, rail = {22, 12, 48, 255}, body = {36, 20, 78, 255}, dark = {46, 24, 98, 255}, bright = sel ? SDL_Color{138, 78, 245, 255} : SDL_Color{118, 54, 228, 255};
  // soft glow
  for (int i = 3; i >= 1; i--) { int g = P(2.f) * i; fillRound({cr.x - g, cr.y - g, cr.w + 2 * g, cr.h + 2 * g}, rad + g, {124, 58, 237, (Uint8)((sel ? 26 : 12) / i)}); }
  fillRound(cr, rad, brd);
  SDL_Rect in = {cr.x + b, cr.y + b, cr.w - 2 * b, cr.h - 2 * b}; fillRound(in, std::max(0, rad - b), rail);
  SDL_Rect mid = {in.x + sw, in.y, in.w - 2 * sw, in.h};
  if (mid.w > 0) {
    fillR(mid, body); isect(mid);
    for (int band = 0; band < 2; band++) {
      int yt = band ? mid.y + mid.h - bh : mid.y; SDL_Rect br = {mid.x, yt, mid.w, bh}; fillR(br, dark); isect(br);
      int vis0 = hadClip ? std::max(mid.x, oldClip.x) : mid.x, vis1 = hadClip ? std::min(mid.x + mid.w, oldClip.x + oldClip.w) : mid.x + mid.w;
      int x = mid.x - bh + ((vis0 - bh - (mid.x - bh)) / per) * per;   // "/" stripes anchored to the clip so they don't swim while scrolling
      for (; x < vis1; x += per) { std::vector<std::pair<int, int> > q; q.push_back(std::make_pair(x, yt + bh)); q.push_back(std::make_pair(x + sd, yt + bh)); q.push_back(std::make_pair(x + sd + bh, yt)); q.push_back(std::make_pair(x + bh, yt)); fillPoly(q, bright); }
      isect(mid);
    }
    int px = FPX(11), ty = mid.y + mid.h / 2 - (hasKeys ? P(5.f) : 0), pad = P(8.f);
    isect({mid.x + pad / 2, mid.y, std::max(0, mid.w - pad), mid.h});
    skTxt(mid.x + pad, ty, "Clip", C_WHITE, px, true);
    int dw = skW(dur, FPX(9)); if (mid.w > skW("Clip", px) + dw + pad * 3) skTxt(mid.x + mid.w - pad - dw, ty, dur, S_PURL, FPX(9), true);
  }
  restore();
  if (cr.w >= sw * 2 + P(10.f)) for (int side = 0; side < 2; side++) {   // sprocket holes
    int hx = side ? in.x + in.w - sw / 2 : in.x + sw / 2, hs = P(5.f);
    for (int i = -1; i <= 1; i++) skBox({hx - hs / 2, cr.y + cr.h / 2 + i * P(13.f) - hs / 2, hs, hs}, P(1.5f), {10, 6, 22, 255}, {96, 70, 170, 255});
  }
}
enum { SB_OUT, SB_PRI, SB_DANGER, SB_GREEN, SB_PURP, SB_REDO, SB_CYAN, SB_AMBER, SB_SEG };
static void skStyle(int s, SDL_Color& f, SDL_Color& b, SDL_Color& t) {
  if (mayaOn() && (s == SB_PRI || s == SB_PURP || s == SB_SEG || s == SB_OUT)) {
    if (s == SB_PRI || s == SB_SEG) { f = {82, 133, 166, 255}; b = {110, 160, 192, 255}; t = C_WHITE; } else if (s == SB_PURP) { f = {64, 64, 64, 255}; b = {104, 104, 104, 255}; t = {204, 204, 204, 255}; } else { f = {52, 52, 52, 255}; b = {30, 30, 30, 255}; t = {204, 204, 204, 255}; }
    return; }
  switch (s) {
    case SB_PRI: f = S_PUR; b = {140, 84, 245, 255}; t = C_WHITE; break;
    case SB_DANGER: f = S_SB; b = S_BORD; t = S_RED; break;
    case SB_GREEN: f = {17, 29, 33, 255}; b = {30, 62, 58, 255}; t = S_GRN; break;
    case SB_PURP: f = {29, 22, 47, 255}; b = {88, 50, 160, 255}; t = S_PURL; break;
    case SB_REDO: f = {28, 18, 30, 255}; b = {112, 36, 62, 255}; t = S_RED; break;
    case SB_CYAN: f = {15, 33, 53, 255}; b = {30, 88, 126, 255}; t = S_CYN; break;
    case SB_AMBER: f = S_SB; b = {118, 88, 24, 255}; t = S_AMB; break;
    case SB_SEG: f = {45, 38, 78, 255}; b = {96, 78, 160, 255}; t = {200, 190, 235, 255}; break;
    default: f = S_SURF; b = S_BORD; t = S_TXT; break;
  }
}
enum { SI_NONE, SI_FULL, SI_SAVE, SI_REFRESH, SI_RING, SI_FIT, SI_HAMB, SI_SKIPB, SI_REW, SI_FFW, SI_SKIPF, SI_CROSS, SI_MAGNET, SI_PALETTE, SI_KEY, SI_PATH, SI_FILM, SI_CHEV, SI_GEARI, SI_BOX, SI_DIA, SI_DIAX, SI_DIAP, SI_BOLT, SI_SPARK };
static void skTri(int cx, int cy, int s, int dir, SDL_Color c) {   // dir +1 right, -1 left
  std::vector<std::pair<int, int> > p; p.push_back(std::make_pair(cx + dir * s, cy)); p.push_back(std::make_pair(cx - dir * s, cy - s)); p.push_back(std::make_pair(cx - dir * s, cy + s)); fillPoly(p, c);
}
static void skRingP(int cx, int cy, int r, float w, SDL_Color c) { for (int i = 0; i < 16; i++) { float a0 = 6.2832f * i / 16, a1 = 6.2832f * (i + 1) / 16; thickLine(cx + (int)lroundf(r * cosf(a0)), cy + (int)lroundf(r * sinf(a0)), cx + (int)lroundf(r * cosf(a1)), cy + (int)lroundf(r * sinf(a1)), w, c); } }
static void skDiamond(int cx, int cy, int s, SDL_Color c, bool fill) { std::vector<std::pair<int, int> > p; p.push_back(std::make_pair(cx, cy - s)); p.push_back(std::make_pair(cx + s, cy)); p.push_back(std::make_pair(cx, cy + s)); p.push_back(std::make_pair(cx - s, cy));
  if (fill) fillPoly(p, c); else for (int i = 0; i < 4; i++) thickLine(p[i].first, p[i].second, p[(i + 1) % 4].first, p[(i + 1) % 4].second, std::max(1.2f, s * 0.2f), c); }
static void skIcon(int k, int cx, int cy, int s, SDL_Color c) {   // s = icon box size in px
  float w = std::max(1.3f, s * 0.11f); int h = s / 2;
  switch (k) {
    case SI_FULL: { int a = h, b = h * 5 / 10; for (int sx = -1; sx <= 1; sx += 2) for (int sy = -1; sy <= 1; sy += 2) { thickLine(cx + sx * a, cy + sy * a, cx + sx * (a - b), cy + sy * a, w, c); thickLine(cx + sx * a, cy + sy * a, cx + sx * a, cy + sy * (a - b), w, c); } break; }
    case SI_SAVE: { SDL_Rect r = {cx - h, cy - h, s, s}; frameR(r, c); frameR({r.x + 1, r.y + 1, r.w - 2, r.h - 2}, c); fillR({cx - h * 5 / 10, cy - h, h, h * 7 / 10}, c); fillR({cx - h * 6 / 10, cy + h * 2 / 10, h * 12 / 10, h * 8 / 10}, c); break; }
    case SI_REFRESH: { int r = h * 7 / 10; for (int i = 0; i < 12; i++) { float a0 = 0.6f + 5.0f * i / 12, a1 = 0.6f + 5.0f * (i + 1) / 12; thickLine(cx + (int)(r * cosf(a0)), cy + (int)(r * sinf(a0)), cx + (int)(r * cosf(a1)), cy + (int)(r * sinf(a1)), w, c); } skTri(cx + r - 1, cy - r / 3, std::max(2, s / 6), 1, c); break; }
    case SI_RING: skRingP(cx, cy, h * 6 / 10, w, c); break;
    case SI_FIT: { SDL_Rect r = {cx - h * 8 / 10, cy - h * 8 / 10, h * 16 / 10, h * 16 / 10}; frameR(r, c); fillR({cx - h * 3 / 10, cy - h * 3 / 10, h * 6 / 10, h * 6 / 10}, c); break; }
    case SI_HAMB: for (int i = -1; i <= 1; i++) thickLine(cx - h * 8 / 10, cy + i * h * 55 / 100, cx + h * 8 / 10, cy + i * h * 55 / 100, w * 1.5f, c); break;
    case SI_SKIPB: fillR({cx - h * 8 / 10, cy - h * 7 / 10, std::max(2, s / 8), h * 14 / 10}, c); skTri(cx + h * 2 / 10, cy, h * 6 / 10, -1, c); break;
    case SI_REW: skTri(cx - h * 3 / 10, cy, h * 5 / 10, -1, c); skTri(cx + h * 5 / 10, cy, h * 5 / 10, -1, c); break;
    case SI_FFW: skTri(cx - h * 5 / 10, cy, h * 5 / 10, 1, c); skTri(cx + h * 3 / 10, cy, h * 5 / 10, 1, c); break;
    case SI_SKIPF: fillR({cx + h * 6 / 10, cy - h * 7 / 10, std::max(2, s / 8), h * 14 / 10}, c); skTri(cx - h * 2 / 10, cy, h * 6 / 10, 1, c); break;
    case SI_CROSS: skRingP(cx, cy, h * 5 / 10, w, c); thickLine(cx, cy - h, cx, cy - h * 3 / 10, w, c); thickLine(cx, cy + h * 3 / 10, cx, cy + h, w, c); thickLine(cx - h, cy, cx - h * 3 / 10, cy, w, c); thickLine(cx + h * 3 / 10, cy, cx + h, cy, w, c); break;
    case SI_MAGNET: { int r = h * 6 / 10; for (int i = 0; i < 10; i++) { float a0 = 3.1416f + 3.1416f * i / 10, a1 = 3.1416f + 3.1416f * (i + 1) / 10; thickLine(cx + (int)(r * cosf(a0)), cy - h / 5 + (int)(r * sinf(a0)) + r / 2, cx + (int)(r * cosf(a1)), cy - h / 5 + (int)(r * sinf(a1)) + r / 2, w * 1.7f, {224, 60, 52, 255}); }
                   thickLine(cx - r, cy + r / 2 - h / 5, cx - r, cy + h * 8 / 10, w * 1.7f, {224, 60, 52, 255}); thickLine(cx + r, cy + r / 2 - h / 5, cx + r, cy + h * 8 / 10, w * 1.7f, {224, 60, 52, 255});
                   { int wi = (int)w; fillR({cx - r - wi, cy + h * 5 / 10, wi * 2, h * 4 / 10}, {235, 235, 240, 255}); fillR({cx + r - wi, cy + h * 5 / 10, wi * 2, h * 4 / 10}, {235, 235, 240, 255}); } break; }
    case SI_PALETTE: discFill(cx, cy, h * 9 / 10, {222, 160, 82, 255}); discFill(cx - h * 4 / 10, cy - h * 2 / 10, std::max(1, h / 5), {230, 70, 70, 255}); discFill(cx, cy - h * 5 / 10, std::max(1, h / 5), {80, 190, 90, 255}); discFill(cx + h * 4 / 10, cy - h * 1 / 10, std::max(1, h / 5), {70, 120, 230, 255}); break;
    case SI_KEY: skRingP(cx - h * 4 / 10, cy + h * 2 / 10, h * 4 / 10, w, c); thickLine(cx - h / 10, cy - h / 10, cx + h * 8 / 10, cy - h * 7 / 10, w, c); thickLine(cx + h * 5 / 10, cy - h * 5 / 10, cx + h * 5 / 10, cy - h * 1 / 10, w, c); break;
    case SI_PATH: discFill(cx - h * 7 / 10, cy + h * 5 / 10, std::max(1, h / 5), c); discFill(cx + h * 7 / 10, cy - h * 5 / 10, std::max(1, h / 5), c); thickLine(cx - h * 6 / 10, cy + h * 4 / 10, cx - h * 6 / 10, cy - h * 2 / 10, w, c); thickLine(cx - h * 6 / 10, cy - h * 2 / 10, cx + h * 6 / 10, cy - h * 2 / 10, w, c); thickLine(cx + h * 6 / 10, cy - h * 2 / 10, cx + h * 6 / 10, cy - h * 4 / 10, w, c); break;
    case SI_FILM: frameR({cx - h * 8 / 10, cy - h * 6 / 10, h * 16 / 10, h * 12 / 10}, c); for (int i = -1; i <= 1; i++) { fillR({cx - h * 8 / 10, cy + i * h * 4 / 10 - 1, 3, 2}, c); fillR({cx + h * 8 / 10 - 3, cy + i * h * 4 / 10 - 1, 3, 2}, c); } break;
    case SI_CHEV: skTri(cx, cy, std::max(2, h * 5 / 10), 1, c); break;
    case SI_GEARI: icon(IC_GEAR, cx, cy, s, c); break;
    case SI_BOX: { SDL_Color a = {206, 152, 92, 255}, b = {160, 110, 62, 255}; icon(IC_CUBE, cx, cy, s, a); (void)b; break; }
    case SI_DIA: skDiamond(cx, cy, h * 8 / 10, c, false); break;
    case SI_DIAX: skDiamond(cx, cy, h * 8 / 10, c, false); thickLine(cx - h * 3 / 10, cy - h * 3 / 10, cx + h * 3 / 10, cy + h * 3 / 10, w, c); thickLine(cx - h * 3 / 10, cy + h * 3 / 10, cx + h * 3 / 10, cy - h * 3 / 10, w, c); break;
    case SI_DIAP: skDiamond(cx, cy, h * 8 / 10, c, false); thickLine(cx - h * 3 / 10, cy, cx + h * 3 / 10, cy, w, c); thickLine(cx, cy - h * 3 / 10, cx, cy + h * 3 / 10, w, c); break;
    case SI_BOLT: { std::vector<std::pair<int, int> > p; p.push_back(std::make_pair(cx + h / 4, cy - h)); p.push_back(std::make_pair(cx - h / 2, cy + h / 8)); p.push_back(std::make_pair(cx, cy + h / 8)); p.push_back(std::make_pair(cx - h / 4, cy + h)); p.push_back(std::make_pair(cx + h / 2, cy - h / 8)); p.push_back(std::make_pair(cx, cy - h / 8)); fillPoly(p, c); break; }
    case SI_SPARK: skDiamond(cx, cy, h * 8 / 10, c, true); break;
    default: break;
  }
}
// button: optional icon, centered label. Returns true on tap.
static bool skBtn(SDL_Rect r, const std::string& label, int style, int ic = SI_NONE, int px = 13, bool bold = false, SDL_Color* tcol = nullptr, int rad = 5) {
  bool tap = tapIn(r), held = heldIn(r); SDL_Color f, b, t; skStyle(style, f, b, t); if (tcol) t = *tcol;
  if (held) { f.r = (Uint8)std::min(255, f.r + 18); f.g = (Uint8)std::min(255, f.g + 14); f.b = (Uint8)std::min(255, f.b + 26); }
  skBox(r, P((float)rad), f, b);
  int fp = FPX((float)px), isz = FPX((float)px + 3), lw = skW(label, fp), tot = lw + (ic != SI_NONE ? isz + P(6.f) : 0), x = r.x + (r.w - tot) / 2, cy = r.y + r.h / 2;
  if (ic != SI_NONE) { skIcon(ic, x + isz / 2, cy, isz, t); x += isz + P(6.f); }
  skTxt(x, cy, label, t, fp, bold); return tap;
}
static bool skToggle(SDL_Rect r, bool on) {   // 47x25 switch like the reference
  bool tap = tapIn(r); fillRound(r, r.h / 2, on ? S_PUR : SDL_Color{42, 42, 72, 255}); int k = r.h - P(6.f); discFill(on ? r.x + r.w - P(3.f) - k / 2 : r.x + P(3.f) + k / 2, r.y + r.h / 2, k / 2, C_WHITE); return tap;
}
// ---- sidebar flow layout
static SDL_Rect skNext(int h, int gap) { SDL_Rect r = {sk_rSb.x + P(13.f), sk_rBody.y + P((float)sk_sbCur) - (int)sk_sbScroll, P(235.f), P((float)h)}; sk_sbCur += h + gap; return r; }
static void skHeader(const std::string& t) {
  SDL_Rect r = skNext(14, 8); int px = FPX(11), cy = r.y + r.h / 2, tw = skSpacedW(t, px, 1.1f * U);
  skSpaced(r.x, cy, t, S_SEC, px, 1.1f * U, true); fillR({r.x + tw + P(9.f), cy, r.x + r.w + P(12.f) - (r.x + tw + P(9.f)), std::max(1, P(1.f))}, S_BORD);
}
static void skObjList() {
  if (doc.objs.empty()) { SDL_Rect r = skNext(48, 12); skTxtC(r, "No objects", S_MUT, FPX(13)); return; }
  for (size_t i = 0; i < doc.objs.size(); i++) {
    Obj& o = doc.objs[i]; SDL_Rect r = skNext(30, 5), eye = {r.x + r.w - P(34.f), r.y, P(34.f), r.h}; bool on = (int)i == doc.sel;
    if (tapIn(eye)) { pushUndo(); o.hidden = !o.hidden; setStatus(o.hidden ? "Hidden" : "Shown"); }
    if (tapIn(r)) { doc.sel = (int)i; doc.selClip = -1; }
    skBox(r, P(5.f), on ? SDL_Color{50, 50, 50, 255} : S_SURF, on ? S_PUR : S_BORD);
    skTxt(r.x + P(11.f), r.y + r.h / 2, fitTxt(o.name, r.w - P(56.f), FPX(13)), on ? C_WHITE : S_TXT, FPX(13));
    icon(o.hidden ? IC_EYE_OFF : IC_EYE, eye.x + eye.w / 2, eye.y + eye.h / 2, P(18.f), o.hidden ? S_MUT : S_SEC);
  }
}
static void skToggleRow(const std::string& label, bool& v) {
  SDL_Rect r = skNext(30, 5); skTxt(r.x + P(2.f), r.y + r.h / 2, label, S_TXT, FPX(14));
  if (skToggle({r.x + r.w - P(47.f), r.y + (r.h - P(25.f)) / 2, P(47.f), P(25.f)}, v)) v = !v;
}
static void skField(SDL_Rect r, const std::string& v) { skBox(r, P(4.f), S_SURF, S_BORD); skTxt(r.x + P(10.f), r.y + r.h / 2, v, S_TXT, FPX(12)); }
static void skInfoBox(const std::string& t, int h = 38) { SDL_Rect r = skNext(h, 10); skBox(r, P(5.f), S_SB, S_BORD); skTxtC(r, t, S_MUT, FPX(12)); }
static bool skHalf(SDL_Rect r, int idx, const std::string& label, int style, int ic = SI_NONE) { SDL_Rect q = {r.x + idx * (r.w + P(7.f)) / 2 - (idx ? 0 : 0), r.y, (r.w - P(7.f)) / 2, r.h}; if (idx) q.x = r.x + (r.w - P(7.f)) / 2 + P(7.f); else q.x = r.x; return skBtn(q, label, style, ic, 13); }

// ---- animation presets (clips with keys; all editable afterwards)
static void skAddPreset(int kind) {
  Obj* o = selObj(); if (!o) { setStatus("Select an object first"); return; }
  if (o->log) { setStatus("This model uses a motion log - use the Motion Editor"); return; }
  static const int lens[6] = {75, 30, 90, 60, 45, 48}; int len = lens[kind]; pushUndo();
  HClip c; c.start = curFrame; c.len = len; std::sort(o->clips.begin(), o->clips.end(), [](const HClip& a, const HClip& b) { return a.start < b.start; });
  for (size_t i = 0; i < o->clips.size(); i++) if (c.start < clipEnd(o->clips[i]) && c.start + len > o->clips[i].start) c.start = clipEnd(o->clips[i]);
  const float* B = o->base; auto mk = [&](int rel) { HKey k; for (int i = 0; i < 9; i++) k.v[i] = B[i]; k.bz = kPresets[3].b; c.keys[rel] = k; return &c.keys[rel]; };
  if (kind == 0) { for (int i = 0; i <= 8; i++) { HKey* k = mk(len * i / 8); float a = 6.2832f * i / 8; k->mode = 1; k->v[0] = B[0] + 3.f * (cosf(a) - 1.f); k->v[2] = B[2] + 3.f * sinf(a); } }
  else if (kind == 1) { for (int i = 0; i <= 10; i++) { HKey* k = mk(len * i / 10); k->mode = 1; float s = (i == 0 || i == 10) ? 0.f : 1.f; k->v[0] = B[0] + s * ((i % 2) ? 0.35f : -0.35f); k->v[1] = B[1] + s * ((i % 3) ? 0.12f : -0.1f); } }
  else if (kind == 2) { static const float px[4] = {0, 3, 6, 9}, pz[4] = {0, 3, -2, 1}; for (int i = 0; i < 4; i++) { HKey* k = mk(len * i / 3); k->v[0] = B[0] + px[i]; k->v[2] = B[2] + pz[i]; } }
  else if (kind == 3) { for (int i = 0; i <= 4; i++) { HKey* k = mk(len * i / 4); k->mode = 1; k->v[4] = B[4] + 90.f * i; } }                                   // Spin 360
  else if (kind == 4) { for (int i = 0; i <= 4; i++) { HKey* k = mk(len * i / 4); k->v[1] = B[1] + ((i % 2) ? 1.6f : 0.f); } }                                // Bounce
  else { for (int i = 0; i <= 4; i++) { HKey* k = mk(len * i / 4); float s = (i % 2) ? 1.25f : 1.f; k->v[6] = B[6] * s; k->v[7] = B[7] * s; k->v[8] = B[8] * s; } }  // Pulse
  o->clips.push_back(c); std::sort(o->clips.begin(), o->clips.end(), [](const HClip& a, const HClip& b) { return a.start < b.start; });
  for (size_t i = 0; i < o->clips.size(); i++) if (o->clips[i].start == c.start) doc.selClip = (int)i;
  setFrame(c.start); applyAnim(); setStatus("Clip added");
}
static void skAddKF() { Obj* o = selObj(); if (!o) { setStatus("Select an object first"); return; } if (!keyHere(*o)) toggleKey(); }
static void skDelKF() { Obj* o = selObj(); if (!o) return; if (keyHere(*o)) toggleKey(); else setStatus("No keyframe at the playhead"); }
static void skFit() {
  if (doc.objs.empty()) { cam.tgt = V3{0, 0, 0}; cam.dist = 16.f; return; }
  V3 c = {0, 0, 0}; int n = 0; for (size_t i = 0; i < doc.objs.size(); i++) if (!doc.objs[i].hidden) { V3 w = objCenterW(doc.objs[i]); c = c + w; n++; }
  if (!n) return; c = c * (1.f / n); float r = 1.f; for (size_t i = 0; i < doc.objs.size(); i++) if (!doc.objs[i].hidden) r = std::max(r, len3(objCenterW(doc.objs[i]) - c) + doc.objs[i].rad * maxScale(doc.objs[i]));
  cam.tgt = c; cam.dist = clampf(r * 2.6f, 2.f, 90.f);
}
static void skView(float yaw, float pitch) { cam.yaw = yaw; cam.pitch = pitch; }
static void skApplyFov() { mx::g_focalScale = 0.57735f / tanf(g_fov * 0.5f * kDeg); }

// ---- sidebar tabs
static void skTabScene() {
  skHeader("IMPORT");
  if (skBtn(skNext(30, 7), "Open File\xE2\x80\xA6", SB_PRI, SI_NONE, 14, true)) openFileBrowser();
  if (skBtn(skNext(36, 7), "Fullscreen", SB_OUT, SI_FULL, 14)) { sk_fs = !sk_fs; if (g_win) SDL_SetWindowFullscreen(g_win, sk_fs ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0); }
  if (skBtn(skNext(32, 12), "Clear Scene", SB_DANGER, SI_NONE, 14)) { if (!doc.objs.empty()) { newScene(); setStatus("Scene cleared"); } }
  skHeader("SCENE SPC");
  if (skBtn(skNext(32, 7), "Export Scene (.SPC)", SB_OUT, SI_NONE, 14)) { if (saveCurrent()) setStatus("Scene saved"); else setStatus("Could not save the scene"); }
  if (skBtn(skNext(33, 5), "Import Scene (.SPC)", SB_OUT, SI_NONE, 14)) openFileBrowser();
  if (skBtn(skNext(33, 11), "Saved Projects", SB_GREEN, SI_NONE, 14)) goHome();
  skHeader("OBJECTS IN SCENE"); skObjList();
  skHeader("CAMERA");
  SDL_Rect fr = skNext(18, 4); skTxt(fr.x, fr.y + fr.h / 2, "Field of View", S_TXT, FPX(13)); skTxtR(fr.x + fr.w, fr.y + fr.h / 2, std::to_string((int)lroundf(g_fov)) + "\xC2\xB0", S_PURL, FPX(13), true);
  SDL_Rect sl = skNext(24, 8); if (sliderH(ID_SLIDER + 40, sl, g_fov, 20.f, 110.f, S_PUR, true, false)) skApplyFov();
  if (skBtn(skNext(32, 10), "Reset Camera", SB_OUT, SI_NONE, 14)) { Cam c0; cam = c0; cam.pitch = 0.15f; cam.dist = 18.f; g_fov = 60.f; skApplyFov(); }
}
static void skTabObject() {
  Obj* o = selObj();
  skHeader("TARGET OBJECT"); if (!o) { skInfoBox("No object selected"); return; }
  SDL_Rect nr = skNext(38, 12); skBox(nr, P(5.f), S_SURF, S_BORD); skTxtC(nr, fitTxt(o->name, nr.w - P(20.f), FPX(14)), S_TXT, FPX(14), true);
  skHeader("TOOL"); SDL_Rect tr = skNext(32, 12); const char* tn[3] = {"Move", "Rotate", "Scale"}; int w3 = (tr.w - P(14.f)) / 3;
  for (int i = 0; i < 3; i++) if (skBtn({tr.x + i * (w3 + P(7.f)), tr.y, w3, tr.h}, tn[i], tool == i ? SB_SEG : SB_OUT, SI_NONE, 13)) tool = i;
  skHeader("TRANSFORM"); const char* gn[3] = {"Position", "Rotation", "Scale"};
  for (int g = 0; g < 3; g++) { SDL_Rect l = skNext(16, 3); skTxt(l.x, l.y + l.h / 2, gn[g], S_SEC, FPX(11)); SDL_Rect r = skNext(26, 8); int cw = (r.w - P(10.f)) / 3;
    for (int a = 0; a < 3; a++) { SDL_Rect c = {r.x + a * (cw + P(5.f)), r.y, cw, r.h}; skBox(c, P(4.f), S_SURF, S_BORD); const SDL_Color ac[3] = {C_AXR, C_AXG, C_AXB}; skTxt(c.x + P(7.f), c.y + c.h / 2, a == 0 ? "X" : a == 1 ? "Y" : "Z", ac[a], FPX(11), true); skTxtR(c.x + c.w - P(7.f), c.y + c.h / 2, fmtF(o->T[g * 3 + a], g == 2 ? 2 : 1), S_TXT, FPX(11)); } }
  skHeader("ACTIONS"); SDL_Rect a1 = skNext(32, 7);
  if (skHalf(a1, 0, "Duplicate", SB_OUT, SI_NONE)) duplicateSelected(); if (skHalf(a1, 1, "Delete", SB_DANGER, SI_NONE)) deleteSelected();
  if (selObj()) { o = selObj(); if (skBtn(skNext(32, 7), o->hidden ? "Show" : "Hide", SB_OUT, SI_NONE, 13)) { pushUndo(); o->hidden = !o->hidden; }
    if (skBtn(skNext(32, 7), "Edit Model (cage)", SB_OUT, SI_NONE, 13)) enterModeling(); if (skBtn(skNext(32, 10), "Motion Editor", SB_PURP, SI_NONE, 13)) openMotionEditor(); }
}
static void skTabMaterial() {
  Obj* o = selObj(); skHeader("COLOR"); if (!o) { skInfoBox("No object selected"); return; }
  int cw = P(34.f), gap = (P(235.f) - 6 * cw) / 5; SDL_Rect g = skNext(2 * 34 + 7, 12);
  for (int i = 0; i < 12; i++) { SDL_Rect c = {g.x + (i % 6) * (cw + gap), g.y + (i / 6) * (cw + P(7.f)), cw, cw}; bool on = o->col[0] == kSwatch[i].r && o->col[1] == kSwatch[i].g && o->col[2] == kSwatch[i].b;
    if (tapIn(c)) { pushUndo(); o->col[0] = kSwatch[i].r; o->col[1] = kSwatch[i].g; o->col[2] = kSwatch[i].b; o->colSet = true; }
    fillRound({c.x - 3, c.y - 3, c.w + 6, c.h + 6}, P(9.f), on ? S_PURL : SDL_Color{0, 0, 0, 0}); fillRound(c, P(7.f), kSwatch[i]); }
  skHeader("SMOOTHING"); if (skBtn(skNext(32, 10), "Smoothing " + std::to_string((int)o->smooth), SB_OUT, SI_NONE, 13)) sheet = sheet == SH_SMOOTH ? SH_NONE : SH_SMOOTH;
}
static void skTabRender() {
  skHeader("EXPORT"); if (skBtn(skNext(34, 7), "Export / Record\xE2\x80\xA6", SB_PRI, SI_NONE, 14, true)) sheet = sheet == SH_EXPORT ? SH_NONE : SH_EXPORT;
  skInfoBox("PNG \xC2\xB7 PNG sequence \xC2\xB7 MP4 \xC2\xB7 USD", 34);
  skHeader("DISPLAY"); skToggleRow("Wireframe", wireMode); skToggleRow("Grid", showGrid); skToggleRow("Skeleton", showBones);
  skHeader("BACKGROUND"); SDL_Rect g = skNext(34, 12); int cw = P(34.f), gap = (P(235.f) - 6 * cw) / 5;
  for (int i = 0; i < 6; i++) { SDL_Rect c = {g.x + i * (cw + gap), g.y, cw, cw}; bool on = g_bg.r == kBgs[i].r && g_bg.g == kBgs[i].g && g_bg.b == kBgs[i].b; if (tapIn(c)) g_bg = kBgs[i];
    fillRound({c.x - 3, c.y - 3, c.w + 6, c.h + 6}, P(9.f), on ? S_PURL : SDL_Color{0, 0, 0, 0}); fillRound(c, P(7.f), kBgs[i]); }
}
static void skTabEdit() {
  skHeader("HISTORY"); SDL_Rect r = skNext(32, 12); bool cu = !undoS.empty(), cr = !redoS.empty();
  if (skHalf(r, 0, "Undo", cu ? SB_OUT : SB_DANGER, SI_NONE) && cu) doUndo(); if (skHalf(r, 1, "Redo", cr ? SB_OUT : SB_DANGER, SI_NONE) && cr) doRedo();
  Obj* o = selObj(); skHeader("OBJECT"); if (!o) { skInfoBox("No object selected"); return; }
  SDL_Rect a = skNext(32, 7); if (skHalf(a, 0, "Duplicate", SB_OUT)) duplicateSelected(); if (skHalf(a, 1, "Delete", SB_DANGER)) deleteSelected();
  if (selObj()) { if (skBtn(skNext(32, 7), "Edit Model (cage)", SB_OUT, SI_NONE, 13)) enterModeling(); if (skBtn(skNext(32, 12), "Smoothing", SB_OUT, SI_NONE, 13)) sheet = sheet == SH_SMOOTH ? SH_NONE : SH_SMOOTH; }
  o = selObj(); bool cs = o && doc.selClip >= 0 && doc.selClip < (int)o->clips.size();
  skHeader("CLIP"); if (!cs) { skInfoBox("No clip selected"); return; }
  SDL_Rect c = skNext(32, 7); if (skHalf(c, 0, "Copy", SB_OUT)) copyClip(); if (skHalf(c, 1, "Cut", SB_OUT)) cutClip(); if (skBtn(skNext(32, 10), "Delete Clip", SB_DANGER, SI_NONE, 13)) deleteClip();
}
static void skTabObjects() {
  skHeader("ADD"); if (skBtn(skNext(34, 7), "Add Model\xE2\x80\xA6", SB_PRI, SI_NONE, 14, true)) { sheet = SH_MODELS; sheetScroll = 0; }
  if (skBtn(skNext(32, 7), "Open File\xE2\x80\xA6", SB_OUT, SI_NONE, 13)) openFileBrowser(); if (skBtn(skNext(32, 12), "Load Demo Skeleton", SB_OUT, SI_NONE, 13)) LoadDemoMDL();
  skHeader("OBJECTS IN SCENE"); skObjList();
}
static void skTabFx() {
  skHeader("VIEWPORT"); if (skBtn(skNext(32, 7), "Sky / Background\xE2\x80\xA6", SB_OUT, SI_NONE, 13)) sheet = sheet == SH_SKY ? SH_NONE : SH_SKY;
  skToggleRow("Grid", showGrid); skToggleRow("Skeleton", showBones); skToggleRow("Wireframe", wireMode);
  skHeader("EFFECTS"); skInfoBox("Light and media are not in this build yet", 40);
}
static void skCurveBox(SDL_Rect r) {
  skBox(r, P(5.f), S_SURF, S_BORD); for (int i = 1; i < 4; i++) { fillR({r.x + r.w * i / 4, r.y + 2, 1, r.h - 4}, {52, 52, 52, 255}); fillR({r.x + 2, r.y + r.h * i / 4, r.w - 4, 1}, {52, 52, 52, 255}); }
  Obj* o = selObj(); const Bez* bz = nullptr; if (o) { HClip* c = clipAt(*o, curFrame); if (c) { std::map<int, HKey>::const_iterator it = c->keys.lower_bound(curFrame - c->start); if (it != c->keys.end()) bz = &it->second.bz; } }
  int px0 = r.x + P(8.f), px1 = r.x + r.w - P(8.f), py0 = r.y + r.h - P(8.f), py1 = r.y + P(8.f), lx = px0, ly = py0;
  for (int i = 1; i <= 40; i++) { float u = i / 40.f, v = bz ? easeBez(*bz, u) : u; int x = px0 + (int)((px1 - px0) * u), y = py0 + (int)((py1 - py0) * v); thickLine(lx, ly, x, y, std::max(1.3f, P(1.6f) * 1.f), bz ? S_PURL : SDL_Color{110, 110, 110, 255}); lx = x; ly = y; }
  fillR({r.x + P(6.f), r.y + P(4.f), std::max(2, P(2.f)), r.h - P(8.f)}, {220, 38, 38, 255});
}
static void skTabAnim() {
  Obj* o = selObj();
  skHeader("TARGET OBJECT"); { SDL_Rect r = skNext(38, 12); skBox(r, P(5.f), S_SB, S_BORD); skTxtC(r, o ? fitTxt(o->name, r.w - P(20.f), FPX(13)) : (doc.objs.empty() ? "No objects in the scene" : "Select an object"), o ? S_TXT : S_MUT, FPX(13)); }
  skHeader("CLIPS");
  { SDL_Rect card = skNext(3 * 30 + 26 + 5, 12); skBox(card, P(5.f), S_SURF, S_BORD); skSpaced(card.x + P(9.f), card.y + P(13.f), "DEFAULT CLIPS", S_SEC, FPX(10), 0.9f * U, true); fillR({card.x + 1, card.y + P(26.f), card.w - 2, 1}, S_BORD);
    static const char* nm[3] = {"Circular Path", "Shake", "Custom Path"}; static const char* fr[3] = {"75f", "30f", "90f"}; static const int ik[3] = {SI_RING, SI_BOLT, SI_SPARK};
    for (int i = 0; i < 3; i++) { SDL_Rect r = {card.x + P(5.f), card.y + P(31.f) + i * P(30.f), card.w - P(10.f), P(26.f)}; if (tapIn(r)) skAddPreset(i);
      skBox(r, P(4.f), heldIn(r) ? SDL_Color{48, 48, 48, 255} : S_SB, S_BORD); skIcon(ik[i], r.x + P(14.f), r.y + r.h / 2, P(11.f), i == 1 ? S_AMB : S_PURL); skTxt(r.x + P(27.f), r.y + r.h / 2, nm[i], C_WHITE, FPX(13)); skTxtR(r.x + r.w - P(8.f), r.y + r.h / 2, fr[i], S_MUT, FPX(11)); } }
  { SDL_Rect r = skNext(36, sk_proc ? 6 : 12); bool tap = tapIn(r); skBox(r, P(5.f), S_SURF, S_BORD); skIcon(SI_CHEV, r.x + P(17.f), r.y + r.h / 2, P(12.f), S_CYN); skIcon(SI_GEARI, r.x + P(43.f), r.y + r.h / 2, P(16.f), S_CYN); skTxt(r.x + P(56.f), r.y + r.h / 2, "Procedural", S_CYN, FPX(14), true); if (tap) sk_proc = !sk_proc;
    if (sk_proc) { static const char* pn[3] = {"Spin 360", "Bounce", "Pulse"}; static const char* pf[3] = {"60f", "45f", "48f"}; for (int i = 0; i < 3; i++) { SDL_Rect q = skNext(26, i == 2 ? 12 : 4); if (tapIn(q)) skAddPreset(3 + i); skBox(q, P(4.f), S_SB, S_BORD); skTxt(q.x + P(12.f), q.y + q.h / 2, pn[i], C_WHITE, FPX(13)); skTxtR(q.x + q.w - P(8.f), q.y + q.h / 2, pf[i], S_MUT, FPX(11)); } } }
  skHeader("KEYFRAMES");
  { SDL_Rect r = skNext(52, 8); int w3 = (r.w - P(14.f)) / 3; SDL_Rect a = {r.x, r.y, w3, r.h}, t = {r.x + w3 + P(7.f), r.y, w3, r.h}, d = {r.x + 2 * (w3 + P(7.f)), r.y, w3, r.h};
    if (tapIn(a)) skAddKF(); if (tapIn(t)) sheet = sheet == SH_GRAPH ? SH_NONE : SH_GRAPH; if (tapIn(d)) skDelKF();
    SDL_Color f, b, tc; skStyle(SB_PRI, f, b, tc); skBox(a, P(5.f), f, b); skIcon(SI_DIAP, a.x + a.w / 2 - P(19.f), a.y + P(16.f), P(14.f), C_WHITE); skTxt(a.x + a.w / 2 - P(9.f), a.y + P(16.f), "Add", C_WHITE, FPX(13), true); skTxtC({a.x, a.y + P(26.f), a.w, P(20.f)}, "KF", C_WHITE, FPX(13), true);
    skStyle(SB_PURP, f, b, tc); skBox(t, P(5.f), f, b); { int fp = FPX(13), tw = skW("Type", fp) + P(20.f); skIcon(SI_DIA, t.x + (t.w - tw) / 2 + P(7.f), t.y + t.h / 2, P(15.f), S_PURL); skTxt(t.x + (t.w - tw) / 2 + P(20.f), t.y + t.h / 2, "Type", S_PURL, fp, true); }
    skStyle(SB_REDO, f, b, tc); skBox(d, P(5.f), f, b); skIcon(SI_DIAX, d.x + d.w / 2 - P(19.f), d.y + P(16.f), P(14.f), S_RED); skTxt(d.x + d.w / 2 - P(9.f), d.y + P(16.f), "Del", S_RED, FPX(13), true); skTxtC({d.x, d.y + P(26.f), d.w, P(20.f)}, "KF", S_RED, FPX(13), true); }
  { HClip* c = (o && doc.selClip >= 0 && doc.selClip < (int)o->clips.size()) ? &o->clips[doc.selClip] : nullptr;
    if (!c || c->keys.empty()) skInfoBox("No keyframes", 36);
    else { int n = 0; for (std::map<int, HKey>::const_iterator k = c->keys.begin(); k != c->keys.end() && n < 8; ++k, ++n) { SDL_Rect q = skNext(26, 4); if (tapIn(q)) setFrame(c->start + k->first); bool here = c->start + k->first == curFrame;
        skBox(q, P(4.f), here ? SDL_Color{50, 50, 50, 255} : S_SB, here ? S_PUR : S_BORD); skDiamond(q.x + P(14.f), q.y + q.h / 2, P(5.f), here ? S_PURL : S_MUT, true); skTxt(q.x + P(28.f), q.y + q.h / 2, "Frame " + std::to_string(c->start + k->first), S_TXT, FPX(12));
        skTxtR(q.x + q.w - P(8.f), q.y + q.h / 2, k->second.mode == 1 ? "linear" : k->second.mode == 2 ? "hold" : "bezier", S_MUT, FPX(11)); }
      sk_sbCur += 6; } }
  skHeader("SETTINGS");
  { SDL_Rect l = skNext(18, 8); skTxt(l.x, l.y + l.h / 2, "Duration (s / min / h)", S_TXT, FPX(13)); static const char* un[3] = {"s", "min", "h"}; skTxtR(l.x + l.w, l.y + l.h / 2, std::to_string(sk_durVal) + un[sk_durUnit], S_PURL, FPX(13), true);
    SDL_Rect f = skNext(26, 9); SDL_Rect n = {f.x, f.y, P(72.f), f.h}, u = {f.x + P(77.f), f.y, P(65.f), f.h}; if (tapIn(n)) { static const int pr[6] = {5, 10, 20, 30, 60, 120}; int i = 0; for (int k = 0; k < 6; k++) if (pr[k] == sk_durVal) i = k; sk_durVal = pr[(i + 1) % 6]; }
    if (tapIn(u)) sk_durUnit = (sk_durUnit + 1) % 3; skField(n, std::to_string(sk_durVal)); skField(u, un[sk_durUnit]); }
  for (int i = 0; i < 2; i++) { SDL_Rect r = skNext(26, i ? 12 : 6); skTxt(r.x, r.y + r.h / 2, i ? "Bits" : "FPS", S_TXT, FPX(13)); SDL_Rect f = {r.x + P(43.f), r.y, r.w - P(43.f), r.h}; if (tapIn(f)) setStatus(i ? "Bits is fixed in this build" : "FPS is fixed at 30 in this build"); skField(f, "30"); }
  { SDL_Rect r = skNext(26, 12); skTxt(r.x, r.y + r.h / 2, "Loop", S_TXT, FPX(14)); if (skToggle({r.x + r.w - P(47.f), r.y, P(47.f), r.h}, loopPlay)) loopPlay = !loopPlay; }
  skHeader("ANIMATION CURVE"); skCurveBox(skNext(70, 14));
}
static void skDrawSidebar() {
  if (sk_rSb.w <= 0) return;
  fillR(sk_rSb, S_SB); fillR({sk_rSb.x + sk_rSb.w - 1, sk_rSb.y, 1, sk_rSb.h}, S_BORD);
  // ---- tab strip (scrolls sideways)
  SDL_Rect tb = sk_rTabs; fillR(tb, S_SB); fillR({tb.x, tb.y + tb.h - 1, tb.w, 1}, S_BORD);
  int tpx = FPX(12), tot = 0, tw[SK_TABS]; for (int i = 0; i < SK_TABS; i++) { tw[i] = skSpacedW(kSkTabs[i], tpx, 0.5f * U) + P(26.f); tot += tw[i]; }
  { static int lastTab = -1; if (lastTab != sk_tab) { int x0 = 0; for (int i = 0; i < sk_tab; i++) x0 += tw[i]; if (x0 < sk_tabScroll) sk_tabScroll = (float)x0; else if (x0 + tw[sk_tab] > sk_tabScroll + tb.w) sk_tabScroll = (float)(x0 + tw[sk_tab] - tb.w); lastTab = sk_tab; } }
  dragScroll(ID_SCROLL + 7, tb, sk_tabScroll, (float)tot, true);
  SDL_RenderSetClipRect(R, &tb); g_clip = tb; g_clipOn = true; int x = tb.x - (int)sk_tabScroll;
  for (int i = 0; i < SK_TABS; i++) { SDL_Rect r = {x, tb.y, tw[i], tb.h}; bool on = sk_tab == i;
    if (tapIn(r)) { sk_tab = i; sk_sbScroll = 0; if (r.x < tb.x) sk_tabScroll -= (float)(tb.x - r.x); else if (r.x + r.w > tb.x + tb.w) sk_tabScroll += (float)(r.x + r.w - tb.x - tb.w); }
    if (on) { fillR({r.x, r.y, r.w, r.h - 1}, S_SURF); fillR({r.x, r.y + r.h - std::max(2, P(2.f)), r.w, std::max(2, P(2.f))}, S_PUR); }
    skSpaced(r.x + P(13.f), r.y + (r.h - 2) / 2, kSkTabs[i], on ? S_PURL : S_SEC, tpx, 0.5f * U, on); x += tw[i]; }
  SDL_RenderSetClipRect(R, nullptr); g_clipOn = false;
  // ---- body (scrolls vertically)
  SDL_Rect body = sk_rBody; float maxS = dragScroll(ID_SCROLL + 5, body, sk_sbScroll, (float)P((float)sk_sbTotal), false); (void)maxS;
  SDL_Rect clip = {body.x, body.y, body.w - 1, body.h}; SDL_RenderSetClipRect(R, &clip); g_clip = clip; g_clipOn = true; sk_sbCur = 10;
  switch (sk_tab) { case 0: skTabScene(); break; case 1: skTabObject(); break; case 2: skTabMaterial(); break; case 3: skTabRender(); break; case 4: skTabEdit(); break; case 5: skTabObjects(); break; case 6: skTabFx(); break; default: skTabAnim(); break; }
  sk_sbTotal = sk_sbCur + 8; SDL_RenderSetClipRect(R, nullptr); g_clipOn = false;
  if (P((float)sk_sbTotal) > body.h) { float t = (float)body.h / P((float)sk_sbTotal); int th = std::max(P(20.f), (int)(body.h * t)), ty = body.y + (int)((body.h - th) * (sk_sbScroll / std::max(1.f, (float)(P((float)sk_sbTotal) - body.h)))); fillRound({sk_rSb.x + sk_rSb.w - P(10.f), ty, P(5.f), th}, P(2.f), mayaOn() ? SDL_Color{110, 110, 110, 255} : SDL_Color{58, 54, 96, 255}); }
  reg(sk_rSb);
}

// ---- top bar
static void skDrawTop() {
  fillR(sk_rTop, S_TOP); fillR({0, sk_rTop.h - 1, W, 1}, S_BORD);
  int lpx = FPX(16); float sp = 1.0f * U; const char* nm = "SOURCE HEXAGON";
  skSpaced(P(12.f) + 1, P(20.f) + 1, nm, S_PUR, lpx, sp, true); skSpaced(P(12.f), P(20.f), nm, {232, 224, 255, 255}, lpx, sp, true); skTxt(P(12.f) + skSpacedW(nm, lpx, sp) + P(8.f), P(23.f), "v0.7", S_MUT, FPX(10));
  SDL_Rect tb = {P(273.f) + 1, 0, W - P(273.f) - 1, sk_rTop.h}; dragScroll(ID_SCROLL + 6, tb, sk_tbScroll, sk_tbW, true);
  SDL_RenderSetClipRect(R, &tb); g_clip = tb; g_clipOn = true; int x = tb.x + P(9.f) - (int)sk_tbScroll, cy = P(20.f), px = FPX(13);
  { const char* sn[3] = {"Orbit", "Select", "BOTH"}; int sw[3], tot = P(6.f); for (int i = 0; i < 3; i++) { sw[i] = skW(sn[i], px) + P(24.f); tot += sw[i]; }
    SDL_Rect c = {x, P(5.f), tot, P(30.f)}; skBox(c, P(7.f), S_SURF, S_BORD); int sx = c.x + P(3.f);
    for (int i = 0; i < 3; i++) { SDL_Rect r = {sx, c.y + P(3.f), sw[i], c.h - P(6.f)}; if (tapIn(r)) g_nav = i; bool on = g_nav == i; if (on) fillRound(r, P(5.f), S_PUR); skTxtC(r, sn[i], on ? C_WHITE : SDL_Color{150, 150, 178, 255}, px, on); sx += sw[i]; }
    x += tot + P(10.f); fillR({x, P(9.f), 1, P(22.f)}, S_BORD); x += P(11.f); }
  struct Itm { const char* l; int ic; int kind; };
  auto item = [&](const char* label, int ic, bool boxed, SDL_Color tc, SDL_Color icc) -> bool {
    int w = P(11.f) * 2 + skW(label, px) + (ic != SI_NONE ? P(21.f) : 0); SDL_Rect r = {x, boxed ? P(6.f) : P(4.f), w, boxed ? P(28.f) : P(32.f)}; bool tap = tapIn(r);
    if (boxed) skBox(r, P(5.f), {32, 26, 62, 255}, {76, 62, 130, 255}); else if (heldIn(r)) fillRound(r, P(5.f), {255, 255, 255, 14});
    int tx = r.x + P(11.f); if (ic != SI_NONE) { skIcon(ic, tx + P(7.f), cy, P(14.f), icc); tx += P(21.f); } skTxt(tx, cy, label, tc, px); x += w; return tap; };
  const SDL_Color dim = {150, 150, 178, 255}, ic1 = {118, 118, 152, 255};
  if (item("Camera", SI_REFRESH, false, dim, ic1)) sheet = sheet == SH_CAMERA ? SH_NONE : SH_CAMERA;
  if (item("Viewport", SI_RING, false, dim, ic1)) sheet = sheet == SH_SKY ? SH_NONE : SH_SKY;
  if (item("Autosave", SI_SAVE, g_autosave, C_WHITE, C_WHITE)) { g_autosave = !g_autosave; setStatus(g_autosave ? "Autosave on" : "Autosave off"); }
  x += P(2.f); if (item("Fit", SI_FIT, false, dim, ic1)) skFit();
  if (item("Front", SI_NONE, false, dim, dim)) skView(0.f, 0.f); if (item("Back", SI_NONE, false, dim, dim)) skView(3.1416f, 0.f); if (item("Top", SI_NONE, false, dim, dim)) skView(0.f, 1.45f);
  if (item("Low", SI_NONE, false, dim, dim)) skView(0.6f, -0.3f); if (item("Left", SI_NONE, false, dim, dim)) skView(-1.5708f, 0.f); if (item("Right", SI_NONE, false, dim, dim)) skView(1.5708f, 0.f);
  if (item("ISO", SI_NONE, false, dim, dim)) skView(0.785f, 0.615f);
  { int w = skW("3D", px) + P(22.f); SDL_Rect r = {x, P(4.f), w, P(32.f)}; if (tapIn(r)) wireMode = !wireMode; skTxt(r.x + P(11.f), cy, wireMode ? "WIRE" : "3D", S_PURL, px, true); x += wireMode ? skW("WIRE", px) + P(22.f) : w; }
  x += P(2.f); fillR({x, P(9.f), 1, P(22.f)}, S_BORD); x += P(13.f);
  bool cu = !undoS.empty(), cr = !redoS.empty(); SDL_Rect ru = {x, P(4.f), P(36.f), P(32.f)}, rr = {x + P(41.f), P(4.f), P(36.f), P(32.f)};
  if (tapIn(ru) && cu) doUndo(); if (tapIn(rr) && cr) doRedo(); icon(IC_UNDO, ru.x + ru.w / 2, cy, P(17.f), cu ? S_TXT : SDL_Color{66, 66, 96, 255}); icon(IC_REDO, rr.x + rr.w / 2, cy, P(17.f), cr ? S_TXT : SDL_Color{66, 66, 96, 255});
  x += P(82.f); fillR({x, P(9.f), 1, P(22.f)}, S_BORD); x += P(10.f);
  { SDL_Rect r = {x, P(4.f), P(36.f), P(32.f)}; if (tapIn(r)) { if (selObj()) deleteSelected(); else setStatus("Nothing selected"); } icon(IC_TRASH, r.x + r.w / 2, cy, P(17.f), selObj() ? SDL_Color{196, 170, 200, 255} : SDL_Color{110, 100, 130, 255}); x += P(46.f); }
  sk_tbW = (float)(x + (int)sk_tbScroll - tb.x);
  SDL_RenderSetClipRect(R, nullptr); g_clipOn = false;
  std::string st; if (doc.objs.empty()) st = "No model \xE2\x80\x94 load a file or use Demo"; else if (selObj()) st = selObj()->name; else st = std::to_string(doc.objs.size()) + (doc.objs.size() == 1 ? " object" : " objects");
  int sw = skW(st, FPX(12)); if (x + sw + P(24.f) < W) skTxtR(W - P(8.f), cy, st, {140, 140, 172, 255}, FPX(12));
  reg(sk_rTop);
}
static void skDrawViewportOverlay() {
  // hints (bottom-right) like the reference
  if (sk_tab != SK_TABS - 1) {
    static const char* hk[8] = {"Orbit", "Scroll", "Shift+drag", "Select", "Ctrl+Click", "G/R/S", "F", "H"};
    static const char* hd[8] = {"Rotate", "Zoom", "Pan", "Click object", "Multi-select", "Move/Rotate/Scale", "Fit \xC2\xB7 Del \xE2\x86\x92 Delete", "Show/Hide Hitboxes"};
    int px = FPX(12); for (int i = 0; i < 8; i++) { int cy = VP.y + VP.h - P(30.f) - (7 - i) * P(23.7f); std::string rest = std::string(" \xE2\x86\x92 ") + hd[i]; int w = skW(hk[i], px) + skW(rest, px), x = W - P(18.f) - w;
      skTxt(x, cy, hk[i], {82, 74, 128, 255}, px); skTxt(x + skW(hk[i], px), cy, rest, {74, 74, 100, 255}, px); }
  }
  skBox(sk_rHamb, P(8.f), {16, 16, 23, 255}, {44, 44, 62, 255}); skIcon(SI_HAMB, sk_rHamb.x + sk_rHamb.w / 2, sk_rHamb.y + sk_rHamb.h / 2, P(18.f), C_WHITE);
  if (tapIn(sk_rHamb)) sk_sbOpen = !sk_sbOpen; reg(sk_rHamb);
}
static void skDrawStatus() {
  SDL_Rect s = sk_rStat; fillR(s, S_SB); fillR({s.x, s.y, s.w, std::max(2, P(2.f))}, S_PURD);
  static Uint32 lt = 0; Uint32 now = SDL_GetTicks(); if (lt && now > lt) sk_fpsAvg = sk_fpsAvg * 0.9f + 100.f / (float)(now - lt); lt = now;
  size_t tr = 0, vt = 0, no = 0; for (size_t i = 0; i < doc.objs.size(); i++) { if (doc.objs[i].hidden) continue; no++; if (doc.objs[i].rm) { tr += doc.objs[i].rm->tcount(); vt += doc.objs[i].rm->vcount(); } }
  std::string a = "FPS: " + std::to_string((int)lroundf(std::min(sk_fpsAvg, 99.f))), b = doc.objs.empty() ? "Triangles: \xE2\x80\x94" : "Triangles: " + std::to_string(tr), c = doc.objs.empty() ? "Vertices: \xE2\x80\x94" : "Vertices: " + std::to_string(vt),
    d = doc.objs.empty() ? "Objects: \xE2\x80\x94" : "Objects: " + std::to_string(no), e = selObj() ? "Sel: " + selObj()->name : "Sel: \xE2\x80\x94"; int px = FPX(11), cy = s.y + s.h / 2 + P(1.f), x = s.x + P(11.f);
  const std::string parts[5] = {a, b, c, d, e};
  for (int i = 0; i < 5; i++) { skTxt(x, cy, parts[i], {128, 128, 158, 255}, px); x += skW(parts[i], px) + P(13.f); if (i < 4) { fillR({x, cy - P(6.f), 1, P(12.f)}, S_BORD); x += P(14.f); } }
  reg(s);
}

// ---- animation panel (bottom)
static void skDrawPath() {
  Obj* o = selObj(); if (!o || o->clips.empty() || o->log) return; int f0 = o->clips.front().start, f1 = clipEnd(o->clips.back()); if (f1 - f0 > 3000) return;
  float t0[9]; bool have = false; V3 prev = {0, 0, 0};
  for (int f = f0; f <= f1; f++) { float t[9]; if (!evalObjAtF(*o, (float)f, t)) continue; V3 p = {t[0] + o->cx * 0.f, t[1], t[2]}; if (have) line3(prev, p, {168, 100, 250, 200}, 2.f * U); prev = p; have = true; (void)t0; }
  for (size_t i = 0; i < o->clips.size(); i++) for (std::map<int, HKey>::const_iterator k = o->clips[i].keys.begin(); k != o->clips[i].keys.end(); ++k) { int sx, sy; V3 p = {k->second.v[0], k->second.v[1], k->second.v[2]}; if (proj(p, sx, sy)) discFill(sx, sy, P(4.f), {236, 224, 255, 255}); }
}
static void skDrawAnim() {
  SDL_Rect a = sk_rAnim; int vx = a.x; Obj* o = selObj(); bool hasClips = o && !o->clips.empty(); int total = skTotalFrames(), cy = a.y + P(28.f);
  fillR(a, S_SB); fillR({a.x, a.y, a.w, std::max(2, P(2.f))}, S_PURD);
  // ---- control row (scrolls sideways on narrow screens)
  SDL_Rect row = {a.x, a.y + P(4.f), a.w, P(48.f)}; dragScroll(ID_SCROLL + 8, row, sk_ctlScroll, sk_ctlW, true); SDL_RenderSetClipRect(R, &row); g_clip = row; g_clipOn = true;
  int ox = vx - (int)sk_ctlScroll, px = FPX(11);
  { SDL_Rect r = {ox + P(14.f), cy - P(13.f), P(62.f), P(26.f)}; skBox(r, P(5.f), S_SURF, S_BORD); skTxtC(r, std::to_string(curFrame) + " / " + std::to_string(total), {170, 170, 200, 255}, px); }
  { SDL_Rect r = {ox + P(86.f), cy - P(13.f), P(72.f), P(26.f)}; bool on = sk_tlTool == 0; if (tapIn(r)) sk_tlTool = 0; skBox(r, P(5.f), on ? SDL_Color{45, 38, 78, 255} : SDL_Color{26, 22, 48, 255}, on ? SDL_Color{96, 78, 160, 255} : S_BORD); skTxt(r.x + P(10.f), cy, "Select", {206, 196, 240, 255}, px, true); skIcon(SI_FILM, r.x + r.w - P(14.f), cy, P(12.f), {170, 160, 210, 255}); }
  { SDL_Rect r = {ox + P(164.f), cy - P(13.f), P(64.f), P(26.f)}; bool on = sk_tlTool == 1; if (tapIn(r)) sk_tlTool = 1; skBox(r, P(5.f), on ? SDL_Color{18, 52, 40, 255} : SDL_Color{15, 30, 28, 255}, on ? SDL_Color{44, 160, 100, 255} : SDL_Color{30, 90, 66, 255}); skIcon(SI_MAGNET, r.x + P(14.f), cy, P(15.f), C_WHITE); skTxt(r.x + P(27.f), cy, "Grab", C_WHITE, px, true); }
  { static const int cx[6] = {302, 341, 378, 420, 461, 499}; static const int ik[6] = {SI_SKIPB, SI_REW, SI_CROSS, -1, SI_FFW, SI_SKIPF};
    for (int i = 0; i < 6; i++) { SDL_Rect r = {ox + P((float)cx[i] - 17.f), cy - P(17.f), P(34.f), P(34.f)}; bool tap = tapIn(r), held = heldIn(r); int icx = r.x + r.w / 2;
      if (i == 3) { r = {ox + P(400.f), cy - P(17.f), P(40.f), P(34.f)}; tap = tapIn(r); skBox(r, P(6.f), {28, 20, 54, 255}, {76, 52, 140, 255}); icon(playing ? IC_PAUSE : IC_PLAY, r.x + r.w / 2, cy, P(18.f), C_WHITE); }
      else { if (held) fillRound(r, P(6.f), {255, 255, 255, 14}); skIcon(ik[i], icx, cy, P(15.f), i == 2 ? SDL_Color{80, 150, 255, 255} : SDL_Color{190, 190, 215, 255}); }
      if (!tap) continue;
      if (i == 0) setFrame(0);
      else if (i == 1 || i == 4) { int best = -1; bool fwd = i == 4; if (o) for (size_t c = 0; c < o->clips.size(); c++) for (std::map<int, HKey>::const_iterator k = o->clips[c].keys.begin(); k != o->clips[c].keys.end(); ++k) { int f = o->clips[c].start + k->first; if (fwd ? (f > curFrame && (best < 0 || f < best)) : (f < curFrame && f > best)) best = f; }
        setFrame(best >= 0 ? best : curFrame + (fwd ? kFps : -kFps)); }
      else if (i == 2) { sk_scroll = 0; pxPerSec = clampf((float)(sk_rLane.w - P(30.f)) / (float)std::max(1, total) * kFps / U, 40.f, 800.f); }
      else if (i == 3) { playing = !playing; playAcc = 0.f; if (playing && curFrame >= std::max(docEnd(), kFps)) setFrame(0); }
      else setFrame(std::max(docEnd(), 0)); } }
  { SDL_Rect r = {ox + P(531.f), cy - P(14.f), P(86.f), P(28.f)}; if (skBtn(r, "Add KF", SB_PURP, SI_DIAP, 11, true)) skAddKF();
    r = {ox + P(621.f), cy - P(14.f), P(81.f), P(28.f)}; if (skBtn(r, "Del KF", SB_REDO, SI_DIAX, 11, true)) skDelKF();
    r = {ox + P(706.f), cy - P(14.f), P(74.f), P(28.f)}; if (skBtn(r, "Type", SB_PURP, SI_DIA, 11, true)) sheet = sheet == SH_GRAPH ? SH_NONE : SH_GRAPH; }
  int rx = std::max(vx + P(790.f), W - P(425.f)) - (int)sk_ctlScroll;
  { SDL_Rect r = {rx, cy - P(14.f), P(128.f), P(28.f)}; if (skBtn(r, "Material \xE2\x86\x92 KFs", SB_CYAN, SI_PALETTE, 11, true)) setStatus("Material keyframes are not in this build yet");
    r = {rx + P(135.f), cy - P(14.f), P(99.f), P(28.f)}; if (skBtn(r, "AUTO-KEY", g_autoKey ? SB_AMBER : SB_OUT, SI_KEY, 10, true)) { g_autoKey = !g_autoKey; setStatus(g_autoKey ? "Auto-key on" : "Auto-key off"); }
    r = {rx + P(244.f), cy - P(14.f), P(77.f), P(28.f)}; if (skBtn(r, "PATH", sk_path ? SB_PURP : SB_OUT, SI_PATH, 12, true)) sk_path = !sk_path;
    static const float sp[5] = {0.25f, 0.5f, 1.f, 2.f, 4.f}; static int si = 2; r = {rx + P(327.f), cy - P(14.f), P(56.f), P(28.f)}; char sb[16]; snprintf(sb, sizeof sb, "%g\xC3\x97", sp[si]);
    if (skBtn(r, sb, SB_OUT, SI_NONE, 11)) { si = (si + 1) % 5; g_speed = sp[si]; }
    for (int i = 0; i < 3; i++) discFill(rx + P(398.f) + i * P(7.f), cy, std::max(1, P(2.f)), {70, 70, 104, 255}); sk_ctlW = (float)(rx + (int)sk_ctlScroll + P(410.f) - vx); }
  SDL_RenderSetClipRect(R, nullptr); g_clipOn = false;
  // ---- ruler
  SDL_Rect rl = sk_rRulerR; fillR({vx, rl.y, a.w, rl.h}, S_RULER); fillR({vx, rl.y + rl.h - 1, a.w, 1}, S_BORD);
  { SDL_Rect cr = {vx, rl.y, P(120.f), rl.h}; fillR(cr, S_PUR); skSpaced(cr.x + (cr.w - skSpacedW("CLIPS", FPX(11), 1.2f * U)) / 2, cr.y + cr.h / 2, "CLIPS", C_WHITE, FPX(11), 1.2f * U, true); }
  float pf = skPF(); { int f0 = std::max(0, (int)floorf(sk_scroll) - 1), f1 = (int)ceilf(skF(rl.x + rl.w)) + 1; SDL_Rect cl = {rl.x, rl.y, rl.w, rl.h}; SDL_RenderSetClipRect(R, &cl);
    for (int f = (f0 / (kFps / 2)) * (kFps / 2); f <= f1; f += kFps / 2) { if (f < 0) continue; int x = skX((float)f); bool sec = f % kFps == 0; fillR({x, rl.y + rl.h - (sec ? P(10.f) : P(6.f)), std::max(1, P(1.f)), sec ? P(10.f) : P(6.f)}, sec ? SDL_Color{90, 84, 130, 255} : SDL_Color{52, 48, 82, 255});
      if (sec && f > 0) { std::string l = std::to_string(f / kFps) + "s"; skTxt(x - skW(l, FPX(10)) / 2, rl.y + P(9.f), l, {110, 106, 150, 255}, FPX(10), true); } }
    SDL_RenderSetClipRect(R, nullptr); }
  // ---- lane
  SDL_Rect ln = sk_rLane; fillR({vx, ln.y, a.w, ln.h}, S_LANE); fillR({vx, ln.y, P(120.f), ln.h}, S_SB); fillR({vx + P(120.f), ln.y, 1, ln.h}, S_BORD);
  { SDL_Rect b1 = {vx + P(27.f), ln.y + P(4.f), P(83.f), P(26.f)}; if (skBtn(b1, "+ Add Clip", SB_PURP, SI_NONE, 10, true)) { if (o) addClipToSelected(); else setStatus("Select an object first"); }
    SDL_Rect b2 = {vx + P(27.f), ln.y + P(36.f), P(83.f), P(28.f)}; bool t2 = tapIn(b2); skBox(b2, P(4.f), {21, 17, 40, 255}, {21, 17, 40, 255}); skDashed(b2, {82, 52, 150, 255}); skTxtC(b2, "Import .clip", S_PURL, FPX(10), true); if (t2) setStatus("Import .clip is not in this build yet");
    SDL_Rect b3 = {vx + P(6.f), ln.y + P(34.f), P(18.f), P(24.f)}; bool cs = o && doc.selClip >= 0 && doc.selClip < (int)o->clips.size(); if (tapIn(b3) && cs) deleteClip(); skTxtC(b3, "\xE2\x80\x94", cs ? S_RED : S_MUT, FPX(11)); }
  SDL_Rect lz = {ln.x, ln.y, ln.w, ln.h}; SDL_RenderSetClipRect(R, &lz); int ccy = ln.y + ln.h / 2, ch = trackClipH;
  if (!hasClips) skTxtC({ln.x, ln.y, ln.w, ln.h}, "+ click to create clip", {110, 110, 110, 255}, FPX(10));
  else for (size_t i = 0; i < o->clips.size(); i++) { const HClip& c = o->clips[i]; int x0 = skX((float)c.start), x1 = skX((float)clipEnd(c)); bool sel = (int)i == doc.selClip; if (x1 < ln.x - P(30.f) || x0 > ln.x + ln.w + P(30.f)) continue;
    SDL_Rect cr = {x0, ccy - ch / 2, std::max(P(8.f), x1 - x0), ch}; char dtb[16]; snprintf(dtb, sizeof dtb, "%.1fs", c.len / (float)kFps); skClipStrip(cr, sel, dtb, !c.keys.empty());
    if (activeId == ID_TL && in.down && !moved && sk_dragKind >= 1 && sk_dragKind <= 5 && tlTargetClip == (int)i && SDL_GetTicks() - pressT > 800) {   // hold-to-delete progress
      float hp = clampf((SDL_GetTicks() - pressT) / (float)kHoldDeleteMs, 0.f, 1.f); fillR({cr.x, cr.y + cr.h + P(3.f), (int)(cr.w * hp), std::max(2, P(3.f))}, S_RED); }
    for (std::map<int, HKey>::const_iterator k = c.keys.begin(); k != c.keys.end(); ++k) { int x = skX((float)(c.start + k->first)); bool here = c.start + k->first == curFrame; skDiamond(x, ccy + P(10.f), P(here ? 7.f : 5.f), here ? C_WHITE : S_PURL, true); }
    if (sel && sk_tlTool == 1) { fillRound({x0 - P(4.f), ccy - P(14.f), P(8.f), P(28.f)}, P(3.f), C_WHITE); fillRound({x1 - P(4.f), ccy - P(14.f), P(8.f), P(28.f)}, P(3.f), C_WHITE); } }
  SDL_RenderSetClipRect(R, nullptr);
  // ---- gestures: ruler = scrub, lane = pan / select / create, Grab tool = move clips, keys and edges
  float scrollNow = sk_scroll; SDL_Rect rz = {rl.x, rl.y, rl.w, rl.h};
  if (!g_lock && in.pressed && !consumed && !blockedAt(in.mx, in.my) && activeId == 0) {
    if (inside(rz, in.mx, in.my)) { activeId = ID_TL; sk_dragKind = 6; setFrame((int)clampf(skF(in.mx), 0.f, (float)total)); }
    else if (inside(lz, in.mx, in.my)) { activeId = ID_TL; sk_dragKind = 1; sk_scrollPress = scrollNow; tlTargetClip = -1; tlTargetKey = -1;
      if (o) for (size_t i = 0; i < o->clips.size(); i++) { const HClip& c = o->clips[i]; int x0 = skX((float)c.start), x1 = skX((float)clipEnd(c));
        if (in.mx >= x0 - P(12.f) && in.mx <= x1 + P(12.f) && abs(in.my - ccy) <= ch / 2 + P(6.f)) { tlTargetClip = (int)i; int best = P(16.f); for (std::map<int, HKey>::const_iterator k = c.keys.begin(); k != c.keys.end(); ++k) { int d = abs(skX((float)(c.start + k->first)) - in.mx); if (d < best) { best = d; tlTargetKey = k->first; } }
          if (sk_tlTool == 1) { pushUndo(); doc.selClip = (int)i; tlOrigStart = c.start; tlOrigLen = c.len; tlOrigKeys = c.keys; tlKeyRel = tlTargetKey;
            if ((int)i == doc.selClip && abs(in.mx - x0) <= P(14.f)) sk_dragKind = 3; else if (abs(in.mx - x1) <= P(14.f)) sk_dragKind = 4; else if (tlTargetKey >= 0) sk_dragKind = 5; else sk_dragKind = 2; } } } } }
  if (activeId == ID_TL && in.down && !moved && !sk_delPrompt && !g_lock && sk_dragKind >= 1 && sk_dragKind <= 5 && tlTargetClip >= 0 && o && tlTargetClip < (int)o->clips.size() && SDL_GetTicks() - pressT >= kHoldDeleteMs) {
    sk_delPrompt = true; sk_delClip = tlTargetClip; doc.selClip = tlTargetClip; sk_dragKind = 0; }
  if (activeId == ID_TL && in.down) {
    if (sk_dragKind == 6) setFrame((int)clampf(skF(in.mx), 0.f, (float)total));
    else if (sk_dragKind == 1 && moved) sk_scroll = clampf(sk_scrollPress - (in.mx - pressX) / pf, 0.f, (float)std::max(0, total + kFps - (int)(ln.w / pf)));
    else if (o && doc.selClip >= 0 && doc.selClip < (int)o->clips.size() && sk_dragKind >= 2 && sk_dragKind <= 5) {
      HClip& c = o->clips[doc.selClip]; int prevEnd = 0, nextStart = 1 << 28;
      for (size_t i = 0; i < o->clips.size(); i++) if ((int)i != doc.selClip) { if (o->clips[i].start < c.start) prevEnd = std::max(prevEnd, clipEnd(o->clips[i])); else nextStart = std::min(nextStart, o->clips[i].start); }
      if (sk_dragKind == 3) { int ns = (int)clampf((float)lroundf(skF(in.mx)), (float)prevEnd, (float)(tlOrigStart + tlOrigLen - 5)), d = ns - tlOrigStart; c.start = ns; c.len = tlOrigLen - d; c.keys.clear();
        for (std::map<int, HKey>::iterator k = tlOrigKeys.begin(); k != tlOrigKeys.end(); ++k) if (k->first - d >= 0 && k->first - d <= c.len) c.keys[k->first - d] = k->second; applyAnim(); }
      else if (sk_dragKind == 4) { int ne = (int)clampf((float)lroundf(skF(in.mx)), (float)(c.start + 5), (float)nextStart); c.len = ne - c.start; for (std::map<int, HKey>::iterator k = c.keys.begin(); k != c.keys.end();) { if (k->first > c.len) k = c.keys.erase(k); else ++k; } applyAnim(); }
      else if (sk_dragKind == 2) { int ns = tlOrigStart + (int)lroundf((in.mx - pressX) / pf); c.start = (int)clampf((float)ns, (float)prevEnd, (float)std::max(prevEnd, nextStart - c.len)); applyAnim(); }
      else if (sk_dragKind == 5) { int nr = (int)clampf((float)lroundf(skF(in.mx)) - c.start, 0.f, (float)c.len); if (nr != tlKeyRel && !c.keys.count(nr) && c.keys.count(tlKeyRel)) { HKey k = c.keys[tlKeyRel]; c.keys.erase(tlKeyRel); c.keys[nr] = k; tlKeyRel = nr; setFrame(c.start + nr); applyAnim(); } } } }
  if (activeId == ID_TL && in.released && tapNow && !consumed && sk_dragKind >= 1 && sk_dragKind <= 5) { consumed = true;
    if (o && tlTargetClip >= 0) { doc.selClip = tlTargetClip; if (tlTargetKey >= 0) setFrame(o->clips[tlTargetClip].start + tlTargetKey); }
    else if (o) { doc.selClip = -1; setFrame((int)clampf((float)lroundf(skF(pressX)), 0.f, (float)total)); addClipToSelected(); }
    else setStatus("Select an object first"); }
  if (!g_lock && in.pinch != 0 && (inside(lz, in.mx, in.my) || inside(rz, in.mx, in.my))) pxPerSec = clampf(pxPerSec * expf(in.pinch * 6.f), 40.f, 800.f);
  if (playing) { int x = skX((float)curFrame); if (x > ln.x + ln.w - P(20.f) || x < ln.x) sk_scroll = std::max(0.f, (float)curFrame - kFps * 0.5f); }
  // ---- playhead
  { int x = skX((float)curFrame); if (x >= ln.x - P(4.f) && x <= ln.x + ln.w) { SDL_Rect cl = {ln.x, rl.y, ln.w, rl.h + ln.h}; SDL_RenderSetClipRect(R, &cl); fillR({x, rl.y + P(2.f), std::max(2, P(2.f)), rl.h + ln.h - P(2.f)}, {220, 38, 38, 255});
      std::vector<std::pair<int, int> > fl; fl.push_back(std::make_pair(x - P(6.f), rl.y + P(2.f))); fl.push_back(std::make_pair(x + P(7.f), rl.y + P(2.f))); fl.push_back(std::make_pair(x + P(1.f), rl.y + P(13.f))); fillPoly(fl, {236, 232, 250, 255}); SDL_RenderSetClipRect(R, nullptr); } }
  // ---- KF/Clips pill
  { SDL_Rect pl = {W - P(139.f), a.y - P(33.f), P(127.f), P(28.f)}; bool on = hasClips; if (tapIn(pl)) { if (o && !on) addClipToSelected(); else if (!o) setStatus("Select an object first"); }
    fillRound(pl, P(14.f), on ? SDL_Color{22, 163, 74, 255} : SDL_Color{220, 38, 38, 255}); skTxtC(pl, on ? "KF/Clips ON" : "KF/Clips OFF", C_WHITE, FPX(13), true); reg(pl); }
  reg(a);
}
// ---- "Delete the clip?" confirmation (opened by holding a clip)
static void skDrawDeletePrompt() {
  if (!sk_delPrompt) return;
  g_lock = false; g_blockOn = false;
  fillR({0, 0, W, H}, {0, 0, 0, 150});
  int dw = std::min(W - P(40.f), P(340.f)), dh = P(142.f); SDL_Rect dlg = {(W - dw) / 2, (H - dh) / 2, dw, dh};
  skBox(dlg, P(10.f), {24, 19, 45, 255}, S_PUR);
  skTxtC({dlg.x, dlg.y + P(14.f), dlg.w, P(34.f)}, "Delete the clip?", C_WHITE, FPX(17), true);
  skTxtC({dlg.x, dlg.y + P(48.f), dlg.w, P(24.f)}, "This removes the clip and its keyframes.", S_MUT, FPX(11));
  int bw = (dlg.w - P(48.f)) / 2, bh = P(38.f), by = dlg.y + dlg.h - bh - P(16.f);
  if (skBtn({dlg.x + P(16.f), by, bw, bh}, "Cancel", SB_OUT, SI_NONE, 13, true)) sk_delPrompt = false;
  if (skBtn({dlg.x + P(32.f) + bw, by, bw, bh}, "Delete", SB_DANGER, SI_NONE, 13, true)) {
    Obj* o = selObj(); if (o && sk_delClip >= 0 && sk_delClip < (int)o->clips.size()) { doc.selClip = sk_delClip; deleteClip(); setStatus("Clip deleted"); }
    sk_delPrompt = false; sk_delClip = -1; }
  g_lock = true;
}

// ---- "Base light" option (opened by holding the viewport)
static void skDrawViewportMenu() {
  if (!g_vpMenu) return;
  g_lock = false; g_blockOn = false;
  int bw = P(200.f), bh = P(48.f); SDL_Rect r = {std::max(P(8.f), std::min(vpMenuX - bw / 2, W - bw - P(8.f))), std::max(P(8.f), std::min(vpMenuY - bh - P(36.f), H - bh - P(8.f))), bw, bh};
  bool on = mx::g_baseLight;
  skBox({r.x - P(2.f), r.y - P(2.f), r.w + P(4.f), r.h + P(4.f)}, P(10.f), {0, 0, 0, 90}, {0, 0, 0, 0});
  skBox(r, P(8.f), heldIn(r) ? SDL_Color{44, 30, 84, 255} : SDL_Color{24, 19, 45, 255}, on ? S_GRN : S_PUR);
  skTxt(r.x + P(16.f), r.y + r.h / 2, "Base light", C_WHITE, FPX(14), true);
  skTxtR(r.x + r.w - P(16.f), r.y + r.h / 2, on ? "ON" : "OFF", on ? S_GRN : S_MUT, FPX(12), true);
  if (tapIn(r)) { mx::g_baseLight = !mx::g_baseLight; vpHash = 0; g_vpMenu = false; setStatus(mx::g_baseLight ? "Base light on - lighting effects off" : "Base light off - lighting effects on"); }
  else if (in.released && pressT > vpMenuT && !inside(r, in.mx, in.my)) g_vpMenu = false;   // tap anywhere else closes it
  g_lock = true;
}

// ======================================================================== Maya-style interface (landscape) ========================================================================
// Layout: menu bar / status line / shelf / toolbox | viewport panel | channel box + layer editor / time slider / range slider / command line / help line.
static const SDL_Color M_BG = {68, 68, 68, 255}, M_DK = {42, 42, 42, 255}, M_DKR = {30, 30, 30, 255}, M_LT = {104, 104, 104, 255}, M_HI = {132, 132, 132, 255}, M_TX = {204, 204, 204, 255}, M_TXD = {140, 140, 140, 255},
  M_TABOFF = {96, 96, 96, 255}, M_TABON = {150, 150, 150, 255}, M_KEY = {152, 84, 84, 255}, M_RULER = {115, 115, 115, 255}, M_LANE = {56, 56, 56, 255}, M_SEL = {82, 133, 166, 255};
static SDL_Rect m_menu, m_stat, m_tabs, m_shelf, m_tool, m_panel, m_dock, m_vtab, m_cbox, m_layer, m_ts, m_pbk, m_rs, m_cmd, m_help;
static int m_open = -1, m_openX = 0, m_openY = 0, m_shelfTab = 1, m_dockTab = 1, m_mtool = 1, m_scrubIdx = -1; static float m_scrubAcc = 0.f; static Uint32 m_openT = 0;
static bool mayaOn() { return g_maya && land && skinOn(); }
static void mayaLayout() {
  int mb = P(22.f), sl = P(30.f), tb = P(20.f), sh = P(40.f), top = mb + sl + tb + sh, tsH = P(36.f), rsH = P(22.f), cmH = P(22.f), hlH = P(16.f), bot = tsH + rsH + cmH + hlH;
  int tw = P(38.f), dw = P(246.f), vt = P(16.f);
  m_menu = {0, 0, W, mb}; m_stat = {0, mb, W, sl}; m_tabs = {0, mb + sl, W, tb}; m_shelf = {0, mb + sl + tb, W, sh};
  m_tool = {0, top, tw, H - top - bot}; m_dock = {W - dw - vt, top, dw, H - top - bot}; m_vtab = {W - vt, top, vt, H - top - bot};
  m_panel = {tw, top, W - tw - dw - vt, H - top - bot};
  m_cbox = {m_dock.x, m_dock.y, dw, (int)(m_dock.h * 0.64f)}; m_layer = {m_dock.x, m_cbox.y + m_cbox.h, dw, m_dock.h - m_cbox.h};
  m_ts = {0, H - bot, W, tsH}; m_rs = {0, m_ts.y + tsH, W, rsH}; m_cmd = {0, m_rs.y + rsH, W, cmH}; m_help = {0, m_cmd.y + cmH, W, hlH};
  int pbw = P(318.f); m_pbk = {W - pbw, m_ts.y, pbw, tsH};
  VP = {m_panel.x, m_panel.y + P(44.f), m_panel.w, m_panel.h - P(44.f)};
  sk_rSb = m_dock; sk_rTabs = {m_dock.x, m_dock.y + P(18.f), m_dock.w, P(31.f)}; sk_rBody = {m_dock.x, sk_rTabs.y + sk_rTabs.h + 1, m_dock.w, m_dock.h - P(18.f) - sk_rTabs.h - 1};
  sk_rStat = {0, 0, 0, 0}; sk_rAnim = m_ts; sk_rRulerR = {0, m_ts.y, W - pbw, tsH}; sk_rLane = sk_rRulerR; sk_rHamb = {0, 0, 0, 0};
  rLeft = {0, 0, 0, 0}; rRuler = sk_rRulerR; rTrack = sk_rRulerR; rowCy = sk_rRulerR.y + sk_rRulerR.h / 2; trackClipH = P(32.f);
}
static void mayaViewportBg() {   // Maya-style vertical gradient behind the 3D view
  for (int i = 0; i < VP.h; i += 2) { float t = (float)i / std::max(1, VP.h - 1); float k = t < 0.55f ? t / 0.55f : 1.f; float k2 = t < 0.55f ? 0.f : (t - 0.55f) / 0.45f;
    SDL_Color c = {(Uint8)(lroundf(112.f + (81.f - 112.f) * k + (29.f - 81.f) * k2)), (Uint8)(lroundf(132.f + (93.f - 132.f) * k + (33.f - 93.f) * k2)), (Uint8)(lroundf(150.f + (107.f - 150.f) * k + (36.f - 107.f) * k2)), 255};
    fillR({VP.x, VP.y + i, VP.w, std::min(2, VP.h - i)}, c); }
}
static void mBevel(SDL_Rect r, bool down, bool on = false) {
  fillR(r, on ? SDL_Color{96, 96, 100, 255} : (down ? SDL_Color{54, 54, 54, 255} : SDL_Color{84, 84, 84, 255}));
  fillR({r.x, r.y, r.w, 1}, down ? M_DKR : M_HI); fillR({r.x, r.y, 1, r.h}, down ? M_DKR : M_HI); fillR({r.x, r.y + r.h - 1, r.w, 1}, down ? M_HI : M_DKR); fillR({r.x + r.w - 1, r.y, 1, r.h}, down ? M_HI : M_DKR);
}
static bool mBtn(SDL_Rect r, bool on = false) { bool t = tapIn(r); mBevel(r, heldIn(r), on); reg(r); return t; }
static void mField(SDL_Rect r, const std::string& s, SDL_Color bg, SDL_Color tc, int px, bool right = false) {
  fillR(r, M_DKR); fillR({r.x + 1, r.y + 1, r.w - 2, r.h - 2}, bg); if (right) skTxtR(r.x + r.w - P(5.f), r.y + r.h / 2, s, tc, px); else skTxt(r.x + P(5.f), r.y + r.h / 2, s, tc, px);
}
static void mSep(int x, int y, int h) { fillR({x, y, 1, h}, M_DKR); fillR({x + 1, y, 1, h}, M_HI); }
static void mPrim(int k, int cx, int cy, int s, SDL_Color c) {
  SDL_Color d = {(Uint8)(c.r * 0.6f), (Uint8)(c.g * 0.6f), (Uint8)(c.b * 0.6f), 255};
  if (k == PR_SPHERE) { discFill(cx, cy, s, d); discFill(cx - s / 4, cy - s / 4, s * 2 / 3, c); }
  else if (k == PR_CUBE) { fillR({cx - s, cy - s + 2, s * 2 - 2, s * 2 - 2}, d); fillR({cx - s + 2, cy - s, s * 2 - 2, s * 2 - 2}, c); }
  else if (k == PR_CYL) { fillRound({cx - s * 3 / 4, cy - s, s * 3 / 2, s * 2}, s / 3, d); fillRound({cx - s * 3 / 4, cy - s, s * 3 / 2, s / 2 + 1}, s / 4, c); }
  else if (k == PR_CONE) { std::vector<std::pair<int, int> > p; p.push_back(std::make_pair(cx, cy - s)); p.push_back(std::make_pair(cx + s, cy + s)); p.push_back(std::make_pair(cx - s, cy + s)); fillPoly(p, d); std::vector<std::pair<int, int> > q; q.push_back(std::make_pair(cx, cy - s)); q.push_back(std::make_pair(cx - s / 3, cy + s)); q.push_back(std::make_pair(cx - s, cy + s)); fillPoly(q, c); }
  else if (k == PR_PLANE) { std::vector<std::pair<int, int> > p; p.push_back(std::make_pair(cx - s / 2, cy - s / 2)); p.push_back(std::make_pair(cx + s, cy - s / 2)); p.push_back(std::make_pair(cx + s / 2, cy + s / 2)); p.push_back(std::make_pair(cx - s, cy + s / 2)); fillPoly(p, c); }
  else { skRingP(cx, cy, s * 3 / 4, std::max(2.f, s * 0.55f), c); }
}
// ---- popup menus (top menu bar and viewport panel menus)
struct MItem { const char* l; int act; };
static const char* kMTop[8] = {"File", "Edit", "Modify", "Create", "Display", "Window", "Animate", "Help"};
static const char* kMPan[6] = {"View", "Shading", "Lighting", "Show", "Renderer", "Panels"};
static const std::vector<MItem>& mItems(int m) {
  static std::vector<MItem> t[14];
  if (t[0].empty()) {
    t[0] = {{"New Scene", 1}, {"Import Model...", 2}, {"Export / Render...", 3}, {"Autosave", 4}};
    t[1] = {{"Undo", 5}, {"Redo", 6}, {"Delete", 7}, {"Duplicate", 8}};
    t[2] = {{"Move Tool", 9}, {"Rotate Tool", 10}, {"Scale Tool", 11}, {"Frame Selection", 12}};
    t[3] = {{"Polygon Sphere", 20}, {"Polygon Cube", 21}, {"Polygon Cylinder", 22}, {"Polygon Cone", 23}, {"Polygon Plane", 24}, {"Polygon Torus", 25}};
    t[4] = {{"Grid", 30}, {"Skeleton / Bones", 31}, {"Wireframe", 32}, {"Base Light", 33}};
    t[5] = {{"Camera Settings", 40}, {"Viewport Settings", 41}, {"Graph Editor", 42}, {"Classic Interface", 43}};
    t[6] = {{"Set Key", 50}, {"Delete Key", 51}, {"Play / Stop", 53}, {"Auto Key", 54}, {"Graph Editor", 42}};
    t[7] = {{"About this app", 60}};
    t[8] = {{"Frame All", 12}, {"Perspective", 70}, {"Front View", 71}, {"Back View", 72}, {"Top View", 73}, {"Left View", 74}, {"Right View", 75}};
    t[9] = {{"Wireframe", 32}, {"Smooth Shade All", 34}};
    t[10] = {{"Use Default Lighting (Base Light)", 33}, {"Use All Lights (SFM Lighting)", 35}};
    t[11] = {{"Grid", 30}, {"Skeleton / Bones", 31}};
    t[12] = {{"Default Quality Renderer", 61}};
    t[13] = {{"Perspective View", 70}};
  }
  return t[m];
}
static void mSetKey() { Obj* o = selObj(); if (!o) { setStatus("Select an object first"); return; } if (o->log) { setStatus("This model uses a motion log - use the Motion Editor"); return; } pushUndo(); HClip* c = mClip(*o, curFrame); putKey(*o, *c, curFrame - c->start); setStatus("Key set at frame " + std::to_string(curFrame)); }
static void mDelKey() { Obj* o = selObj(); if (!o) return; HClip* c = nullptr; int rel = 0; if (keyHere(*o, &c, &rel)) { pushUndo(); c->keys.erase(rel); applyAnim(); setStatus("Key deleted"); } else setStatus("No key at this frame"); }
static void mAct(int a) {
  switch (a) {
    case 1: newScene(); break; case 2: openFileBrowser(); break; case 3: sheet = SH_EXPORT; break; case 4: g_autosave = !g_autosave; setStatus(g_autosave ? "Autosave on" : "Autosave off"); break;
    case 5: if (!undoS.empty()) doUndo(); break; case 6: if (!redoS.empty()) doRedo(); break; case 7: if (selObj()) deleteSelected(); else setStatus("Nothing selected"); break; case 8: if (selObj()) duplicateSelected(); else setStatus("Nothing selected"); break;
    case 9: tool = 0; m_mtool = 1; break; case 10: tool = 1; m_mtool = 2; break; case 11: tool = 2; m_mtool = 3; break; case 12: skFit(); break;
    case 20: case 21: case 22: case 23: case 24: case 25: { static const int pk[6] = {PR_SPHERE, PR_CUBE, PR_CYL, PR_CONE, PR_PLANE, PR_TORUS}; pushUndo(); addPrimitive(pk[a - 20]); setStatus("Created polygon primitive"); } break;
    case 30: showGrid = !showGrid; break; case 31: showBones = !showBones; break; case 32: wireMode = true; break; case 34: wireMode = false; break;
    case 33: mx::g_baseLight = true; vpHash = 0; setStatus("Default lighting (base light)"); break; case 35: mx::g_baseLight = false; vpHash = 0; setStatus("All lights (SFM lighting)"); break;
    case 40: sheet = SH_CAMERA; break; case 41: sheet = SH_SKY; break; case 42: sheet = SH_GRAPH; break; case 43: g_maya = false; setStatus("Classic interface"); break;
    case 50: mSetKey(); break; case 51: mDelKey(); break; case 52: if (selObj()) addClipToSelected(); else setStatus("Select an object first"); break;
    case 53: playing = !playing; playAcc = 0.f; break; case 54: g_autoKey = !g_autoKey; setStatus(g_autoKey ? "Auto key on" : "Auto key off"); break;
    case 60: setStatus("Source Hexagon - Maya-style interface"); break; case 61: setStatus("Renderer: default quality"); break;
    case 70: skView(0.6f, 0.3f); break; case 71: skView(0.f, 0.f); break; case 72: skView(3.1416f, 0.f); break; case 73: skView(0.f, 1.45f); break; case 74: skView(-1.5708f, 0.f); break; case 75: skView(1.5708f, 0.f); break;
  }
}
static bool mMenuHead(SDL_Rect r, int idx, const char* label, int px, bool bar) {   // header button that opens a popup under itself
  bool t = tapIn(r); bool on = m_open == idx; if (on) { fillR(r, bar ? SDL_Color{150, 150, 150, 255} : SDL_Color{110, 110, 110, 255}); } else if (heldIn(r)) fillR(r, {92, 92, 92, 255});
  skTxt(r.x + P(7.f), r.y + r.h / 2, label, on && bar ? SDL_Color{20, 20, 20, 255} : M_TX, px); reg(r);
  if (t) { m_open = on ? -1 : idx; m_openX = r.x; m_openY = r.y + r.h; m_openT = SDL_GetTicks(); } return t;
}
static void mayaPopup() {
  if (m_open < 0) return; g_lock = false; g_blockOn = false;
  const std::vector<MItem>& it = mItems(m_open); int px = FPX(12), w = P(120.f); for (size_t i = 0; i < it.size(); i++) w = std::max(w, skW(it[i].l, px) + P(40.f));
  int rh = P(22.f), h = (int)it.size() * rh + P(6.f); SDL_Rect r = {std::min(m_openX, W - w - P(4.f)), std::min(m_openY, H - h - P(4.f)), w, h};
  fillR({r.x + 3, r.y + 3, r.w, r.h}, {0, 0, 0, 90}); fillR(r, {58, 58, 58, 255}); fillR({r.x, r.y, r.w, 1}, M_HI); fillR({r.x, r.y, 1, r.h}, M_HI); fillR({r.x, r.y + r.h - 1, r.w, 1}, M_DKR); fillR({r.x + r.w - 1, r.y, 1, r.h}, M_DKR);
  bool did = false;
  for (size_t i = 0; i < it.size(); i++) { SDL_Rect ir = {r.x + 2, r.y + P(3.f) + (int)i * rh, r.w - 4, rh}; bool hv = heldIn(ir); if (hv) fillR(ir, M_SEL);
    bool chk = (it[i].act == 30 && showGrid) || (it[i].act == 31 && showBones) || (it[i].act == 32 && wireMode) || (it[i].act == 34 && !wireMode) || (it[i].act == 33 && mx::g_baseLight) || (it[i].act == 35 && !mx::g_baseLight) || (it[i].act == 4 && g_autosave) || (it[i].act == 54 && g_autoKey);
    if (chk) fillR({ir.x + P(7.f), ir.y + rh / 2 - P(2.f), P(5.f), P(5.f)}, hv ? C_WHITE : M_TX);
    skTxt(ir.x + P(20.f), ir.y + rh / 2, it[i].l, hv ? C_WHITE : M_TX, px);
    if (tapIn(ir)) { mAct(it[i].act); m_open = -1; did = true; } }
  if (!did && in.released && pressT > m_openT && !inside(r, in.mx, in.my)) m_open = -1;
  g_lock = true;
}
static void mayaTopBars() {
  fillR(m_menu, M_BG); fillR({0, m_menu.h - 1, W, 1}, M_DKR); int x = P(4.f), px = FPX(12);
  for (int i = 0; i < 8; i++) { int w = skW(kMTop[i], px) + P(16.f); mMenuHead({x, m_menu.y, w, m_menu.h}, i, kMTop[i], px, true); x += w; }
  // ---- status line
  fillR(m_stat, M_BG); fillR({0, m_stat.y + m_stat.h - 1, W, 1}, M_DKR); int cy = m_stat.y + m_stat.h / 2, b = P(22.f); x = P(6.f);
  { SDL_Rect r = {x, cy - P(11.f), P(96.f), P(22.f)}; mField(r, "Animation", {58, 58, 58, 255}, M_TX, FPX(11)); fillPoly(std::vector<std::pair<int, int> >{{r.x + r.w - P(14.f), cy - P(2.f)}, {r.x + r.w - P(6.f), cy - P(2.f)}, {r.x + r.w - P(10.f), cy + P(3.f)}}, M_TX); x += r.w + P(6.f); }
  mSep(x, P(4.f) + m_stat.y, m_stat.h - P(8.f)); x += P(6.f);
  struct SB { int ic; int act; } ib[] = {{IC_PLUS, 1}, {IC_IMPORT, 2}, {SI_SAVE, 4}};
  for (int i = 0; i < 3; i++) { SDL_Rect r = {x, cy - b / 2, b, b}; if (mBtn(r)) mAct(ib[i].act); if (i == 2) skIcon(SI_SAVE, r.x + b / 2, cy, P(13.f), M_TX); else icon(ib[i].ic, r.x + b / 2, cy, P(13.f), M_TX); x += b + P(2.f); }
  mSep(x + P(2.f), P(4.f) + m_stat.y, m_stat.h - P(8.f)); x += P(8.f);
  { SDL_Rect r = {x, cy - P(11.f), P(136.f), P(22.f)}; mField(r, "Select by object type", {58, 58, 58, 255}, M_TX, FPX(11)); x += r.w + P(6.f); }
  for (int i = 0; i < 3; i++) { SDL_Rect r = {x, cy - b / 2, b, b}; if (mBtn(r)) setStatus("Selection masks are not in this build"); fillR({r.x + b / 2 - P(5.f), cy - P(5.f), P(10.f), P(10.f)}, i == 0 ? SDL_Color{90, 170, 220, 255} : SDL_Color{150, 150, 150, 255}); x += b + P(2.f); }
  mSep(x + P(2.f), P(4.f) + m_stat.y, m_stat.h - P(8.f)); x += P(8.f);
  for (int i = 0; i < 4; i++) { SDL_Rect r = {x, cy - b / 2, b, b}; if (mBtn(r)) setStatus("Snapping is not in this build"); skIcon(SI_MAGNET, r.x + b / 2, cy, P(13.f), M_TX); x += b + P(2.f); }
  mSep(x + P(2.f), P(4.f) + m_stat.y, m_stat.h - P(8.f)); x += P(8.f);
  { SDL_Rect r = {W - P(120.f), cy - b / 2, b, b}; if (mBtn(r)) mAct(5); icon(IC_UNDO, r.x + b / 2, cy, P(13.f), undoS.empty() ? M_TXD : M_TX); r = {W - P(94.f), cy - b / 2, b, b}; if (mBtn(r)) mAct(6); icon(IC_REDO, r.x + b / 2, cy, P(13.f), redoS.empty() ? M_TXD : M_TX);
    r = {W - P(66.f), cy - b / 2, b, b}; if (mBtn(r)) sheet = SH_EXPORT; icon(IC_MEDIA, r.x + b / 2, cy, P(13.f), M_TX); r = {W - P(40.f), cy - b / 2, b, b}; if (mBtn(r)) sheet = SH_CAMERA; icon(IC_CAMERA, r.x + b / 2, cy, P(13.f), M_TX); }
}
static void mayaShelf() {
  static const char* tabs[12] = {"Polygons", "Animation", "Rendering", "Dynamics", "PaintEffects", "Toon", "Muscle", "Fluids", "Fur", "Hair", "nCloth", "Custom"};
  fillR(m_tabs, {58, 58, 58, 255}); int x = P(4.f), px = FPX(11);
  for (int i = 0; i < 12; i++) { int w = skW(tabs[i], px) + P(20.f); SDL_Rect r = {x, m_tabs.y + P(2.f), w, m_tabs.h - P(2.f)}; bool on = m_shelfTab == i; if (tapIn(r)) m_shelfTab = i; reg(r);
    fillR(r, on ? M_TABON : M_TABOFF); fillR({r.x, r.y, r.w, 1}, M_HI); fillR({r.x, r.y, 1, r.h}, M_HI); fillR({r.x + r.w - 1, r.y, 1, r.h}, M_DKR); skTxt(r.x + P(10.f), r.y + r.h / 2, tabs[i], on ? SDL_Color{20, 20, 20, 255} : M_TX, px); x += w + P(1.f); }
  fillR(m_shelf, M_BG); fillR({0, m_shelf.y + m_shelf.h - 1, W, 1}, M_DKR); fillR({0, m_shelf.y, W, 1}, M_TABON);
  int cy = m_shelf.y + m_shelf.h / 2, s = P(34.f); x = P(8.f); fillR({x - P(4.f), cy - s / 2, P(2.f), s}, M_LT);
  auto cell = [&]() -> SDL_Rect { SDL_Rect r = {x, cy - s / 2, s, s}; x += s + P(3.f); return r; };
  if (m_shelfTab == 0) { static const int pk[6] = {PR_SPHERE, PR_CUBE, PR_CYL, PR_CONE, PR_PLANE, PR_TORUS}; static const SDL_Color pc[6] = {{120, 190, 235, 255}, {235, 200, 110, 255}, {150, 220, 150, 255}, {240, 150, 120, 255}, {190, 170, 235, 255}, {235, 130, 190, 255}};
    for (int i = 0; i < 6; i++) { SDL_Rect r = cell(); if (mBtn(r)) mAct(20 + i); mPrim(pk[i], r.x + s / 2, cy, P(9.f), pc[i]); } }
  else if (m_shelfTab == 1) {
    SDL_Rect r = cell(); if (mBtn(r)) mAct(50); icon(IC_KEY_ADD, r.x + s / 2, cy, P(20.f), {240, 120, 120, 255});
    r = cell(); if (mBtn(r)) mAct(51); icon(IC_KEY_DEL, r.x + s / 2, cy, P(20.f), {240, 170, 120, 255});
    r = cell(); if (mBtn(r)) mAct(53); icon(playing ? IC_PAUSE : IC_PLAY, r.x + s / 2, cy, P(18.f), {130, 190, 240, 255});
    r = cell(); if (mBtn(r, g_autoKey)) mAct(54); skIcon(SI_KEY, r.x + s / 2, cy, P(18.f), g_autoKey ? SDL_Color{255, 90, 90, 255} : M_TX);
    r = cell(); if (mBtn(r)) mAct(12); skIcon(SI_FIT, r.x + s / 2, cy, P(18.f), M_TX);
    r = cell(); if (mBtn(r)) mAct(42); icon(IC_GRAPH, r.x + s / 2, cy, P(18.f), {200, 160, 240, 255});
    r = cell(); if (mBtn(r)) mAct(7); icon(IC_TRASH, r.x + s / 2, cy, P(18.f), M_TX); }
  else if (m_shelfTab == 2) {
    SDL_Rect r = cell(); if (mBtn(r)) mAct(40); icon(IC_CAMERA, r.x + s / 2, cy, P(18.f), M_TX); r = cell(); if (mBtn(r)) mAct(41); icon(IC_SKY, r.x + s / 2, cy, P(18.f), {140, 190, 240, 255});
    r = cell(); if (mBtn(r)) mAct(3); icon(IC_MEDIA, r.x + s / 2, cy, P(18.f), M_TX); r = cell(); if (mBtn(r)) mAct(33); icon(IC_BULB, r.x + s / 2, cy, P(18.f), {250, 220, 120, 255}); }
  else skTxt(x + P(6.f), cy, "(no tools on this shelf in this build)", M_TXD, FPX(11));
  reg(m_tabs); reg(m_shelf);
}
static void mayaToolbox() {
  fillR(m_tool, M_BG); fillR({m_tool.x + m_tool.w - 1, m_tool.y, 1, m_tool.h}, M_DKR); int s = P(32.f), x = P(3.f), y = m_tool.y + P(4.f);
  struct TB { int id; } ids[] = {{0}, {1}, {2}, {3}, {4}, {5}, {6}, {7}, {8}};
  for (int i = 0; i < 9; i++) { SDL_Rect r = {x, y, s, s}; int mt = i; bool on = (i == 0 && g_nav == 1) || (i >= 4 && i <= 6 && m_mtool == i - 3 && g_nav != 1);
    if (mBtn(r, on)) { if (i == 0) { g_nav = 1; m_mtool = 0; } else if (i == 4) { g_nav = 2; mAct(9); } else if (i == 5) { g_nav = 2; mAct(10); } else if (i == 6) { g_nav = 2; mAct(11); } else setStatus("This tool is not in this build"); }
    int cx = r.x + s / 2, cy = r.y + s / 2; SDL_Color c = on ? C_WHITE : M_TX; (void)mt; (void)ids;
    if (i == 0) { std::vector<std::pair<int, int> > p; p.push_back(std::make_pair(cx - P(6.f), cy - P(9.f))); p.push_back(std::make_pair(cx + P(7.f), cy + P(2.f))); p.push_back(std::make_pair(cx + P(1.f), cy + P(3.f))); p.push_back(std::make_pair(cx + P(4.f), cy + P(10.f))); p.push_back(std::make_pair(cx + P(1.f), cy + P(11.f))); p.push_back(std::make_pair(cx - P(2.f), cy + P(4.f))); p.push_back(std::make_pair(cx - P(6.f), cy + P(8.f))); fillPoly(p, c); }
    else if (i == 4) icon(IC_MOVE, cx, cy, P(18.f), c); else if (i == 5) icon(IC_ROTATE, cx, cy, P(18.f), c); else if (i == 6) icon(IC_SCALE, cx, cy, P(18.f), c);
    else if (i == 1) skRingP(cx, cy, P(8.f), 1.6f * U, c); else if (i == 2) { fillRound({cx - P(8.f), cy - P(5.f), P(16.f), P(10.f)}, P(3.f), {200, 120, 80, 255}); } else if (i == 3) { fillR({cx - P(1.f), cy - P(9.f), P(2.f), P(18.f)}, c); fillR({cx - P(9.f), cy - P(1.f), P(18.f), P(2.f)}, c); }
    else if (i == 7) { fillR({cx - P(8.f), cy - P(8.f), P(16.f), P(16.f)}, {90, 130, 170, 255}); fillR({cx - P(3.f), cy - P(3.f), P(6.f), P(6.f)}, c); } else skIcon(SI_BOX, cx, cy, P(16.f), c);
    y += s + P(2.f); }
  y += P(10.f); fillR({x, y, s, 1}, M_DKR); y += P(8.f);
  for (int i = 0; i < 3; i++) { SDL_Rect r = {x, y, s, s}; if (mBtn(r, i == 0)) { if (i == 0) skView(0.6f, 0.3f); else setStatus("Multi-view layouts are not in this build"); }
    int cx = r.x + s / 2, cy = r.y + s / 2; if (i == 0) fillR({cx - P(9.f), cy - P(7.f), P(18.f), P(14.f)}, M_TX); else for (int q = 0; q < 4; q++) fillR({cx - P(9.f) + (q % 2) * P(10.f), cy - P(7.f) + (q / 2) * P(8.f), P(8.f), P(6.f)}, M_TX);
    y += s + P(2.f); }
  reg(m_tool);
}
static void mayaPanelBar() {
  fillR({m_panel.x, m_panel.y, m_panel.w, P(44.f)}, M_BG); int x = m_panel.x + P(4.f), px = FPX(12);
  for (int i = 0; i < 6; i++) { int w = skW(kMPan[i], px) + P(16.f); mMenuHead({x, m_panel.y + P(1.f), w, P(20.f)}, 8 + i, kMPan[i], px, false); x += w; }
  fillR({m_panel.x, m_panel.y + P(21.f), m_panel.w, 1}, M_DKR);
  int cy = m_panel.y + P(33.f), s = P(20.f); x = m_panel.x + P(6.f);
  struct PI { int act; bool on; } pi[] = {{70, false}, {30, showGrid}, {32, wireMode}, {34, !wireMode}, {33, mx::g_baseLight}, {35, !mx::g_baseLight}, {12, false}, {31, showBones}};
  for (int i = 0; i < 8; i++) { SDL_Rect r = {x, cy - s / 2, s, s}; if (mBtn(r, pi[i].on)) mAct(pi[i].act); int cx = r.x + s / 2;
    if (i == 0) icon(IC_CAMERA, cx, cy, P(13.f), M_TX); else if (i == 1) { for (int q = 0; q < 3; q++) { fillR({cx - P(7.f), cy - P(5.f) + q * P(5.f), P(14.f), 1}, M_TX); fillR({cx - P(5.f) + q * P(5.f), cy - P(7.f), 1, P(14.f)}, M_TX); } }
    else if (i == 2) skRingP(cx, cy, P(6.f), 1.2f * U, M_TX); else if (i == 3) discFill(cx, cy, P(6.f), {170, 170, 175, 255}); else if (i == 4) discFill(cx, cy, P(6.f), {230, 210, 120, 255}); else if (i == 5) { discFill(cx, cy, P(6.f), {120, 170, 220, 255}); discFill(cx - P(2.f), cy - P(2.f), P(3.f), C_WHITE); }
    else if (i == 6) skIcon(SI_FIT, cx, cy, P(13.f), M_TX); else skIcon(SI_BOX, cx, cy, P(13.f), M_TX);
    x += s + P(2.f); if (i == 0 || i == 3 || i == 5) { mSep(x, cy - s / 2, s); x += P(5.f); } }
  reg({m_panel.x, m_panel.y, m_panel.w, P(44.f)});
}
static void mayaViewportOverlay() {
  skTxtC({VP.x, VP.y + VP.h - P(22.f), VP.w, P(16.f)}, "persp", {118, 120, 122, 255}, FPX(11));   // panel name, bottom centre
  int ox = VP.x + P(24.f), oy = VP.y + VP.h - P(24.f), L = P(16.f); V3 ax[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}; SDL_Color ac[3] = {{230, 60, 60, 255}, {90, 210, 90, 255}, {70, 110, 240, 255}};
  for (int i = 0; i < 3; i++) { float sx = (ax[i].x * Rv.x + ax[i].y * Rv.y + ax[i].z * Rv.z) * L, sy = -(ax[i].x * Uv.x + ax[i].y * Uv.y + ax[i].z * Uv.z) * L; int n = 10; for (int k = 0; k <= n; k++) fillR({ox + (int)(sx * k / n), oy + (int)(sy * k / n), std::max(1, P(1.f)), std::max(1, P(1.f))}, ac[i]); skTxt(ox + (int)(sx * 1.25f) - P(3.f), oy + (int)(sy * 1.25f), i == 0 ? "x" : i == 1 ? "y" : "z", ac[i], FPX(9)); }
}
static std::string mNum(float v) { char b[24]; if (fabsf(v - roundf(v)) < 0.0005f) snprintf(b, sizeof b, "%d", (int)lroundf(v)); else snprintf(b, sizeof b, "%.3f", v); return b; }
static void mayaDock() {
  fillR(m_vtab, M_BG); fillR({m_vtab.x, m_vtab.y, 1, m_vtab.h}, M_DKR);   // vertical tab strip: Attribute Editor / Channel Box
  { const char* nm[2] = {"ATTRIBUTES", "CHANNELS"}; int ty = m_vtab.y + P(2.f); for (int t = 0; t < 2; t++) { int n = (int)strlen(nm[t]), h = n * P(9.f) + P(10.f); SDL_Rect r = {m_vtab.x + 1, ty, m_vtab.w - 1, h}; bool on = m_dockTab == t; if (tapIn(r)) m_dockTab = t; reg(r);
      fillR(r, on ? SDL_Color{110, 110, 110, 255} : M_TABOFF); fillR({r.x, r.y, r.w, 1}, M_HI);
      for (int c = 0; c < n; c++) { char s[2] = {nm[t][c], 0}; skTxt(r.x + r.w / 2 - P(3.f), r.y + P(9.f) + c * P(9.f), s, on ? C_WHITE : M_TX, FPX(8)); } ty += h + P(2.f); } }
  if (m_dockTab == 0) { skDrawSidebar(); return; }   // Attribute Editor = the object / material / scene panels
  SDL_Rect d = m_cbox; fillR(d, M_BG); int px = FPX(11), x = d.x + P(8.f);
  skTxt(d.x + P(30.f), d.y + P(9.f), "Channel Box / Layer Editor", M_TX, px); fillR({d.x, d.y + P(18.f), d.w, 1}, M_DKR);
  const char* cm[4] = {"Channels", "Edit", "Object", "Show"}; for (int i = 0; i < 4; i++) { skTxt(x, d.y + P(27.f), cm[i], M_TX, px); x += skW(cm[i], px) + P(14.f); }
  Obj* o = selObj(); int ny = d.y + P(38.f);
  fillR({d.x, ny, d.w, P(20.f)}, {68, 50, 40, 255}); skTxt(d.x + P(8.f), ny + P(10.f), o ? o->name : std::string("(nothing selected)"), C_WHITE, px, true);
  static const char* an[10] = {"Translate X", "Translate Y", "Translate Z", "Rotate X", "Rotate Y", "Rotate Z", "Scale X", "Scale Y", "Scale Z", "Visibility"}; static const float st[9] = {0.02f, 0.02f, 0.02f, 0.5f, 0.5f, 0.5f, 0.01f, 0.01f, 0.01f};
  int ry = ny + P(22.f), rh = P(17.f), fx = d.x + P(108.f), fw = d.w - P(108.f) - P(24.f); HClip* kc = nullptr; bool keyed = o && keyHere(*o, &kc);
  fillR({d.x, ry, d.w, rh * 10 + P(4.f)}, M_BG); fillR({d.x, ry + rh * 10 + P(4.f), d.w, d.h - (ry - d.y) - rh * 10 - P(4.f)}, M_DK);
  for (int i = 0; i < 10; i++) { int y = ry + i * rh; skTxtR(fx - P(6.f), y + rh / 2, an[i], M_TX, px); SDL_Rect fr = {fx, y + 1, fw, rh - 2}; reg(fr);
    if (i == 9) { mField(fr, o && o->hidden ? "off" : "on", M_DK, M_TX, px); if (o && tapIn(fr)) { pushUndo(); o->hidden = !o->hidden; } continue; }
    float v = o ? o->T[i] : 0.f; bool kk = keyed && o; mField(fr, o ? mNum(v) : "0", kk ? M_KEY : M_DK, M_TX, px);
    if (!o) continue;
    if (!g_lock && in.pressed && !consumed && !blockedAt(in.mx, in.my) && activeId == 0 && inside(fr, in.mx, in.my)) { activeId = ID_SCROLL + 12; m_scrubIdx = i; m_scrubAcc = 0.f; pushUndo(); }
    if (activeId == ID_SCROLL + 12 && m_scrubIdx == i) {
      if (in.down) { static int lx = 0; if (in.pressed) lx = in.mx; float dx = (float)(in.mx - lx); lx = in.mx; if (dx != 0.f) { o->T[i] += dx * st[i] * (i < 3 ? 1.f : 1.f); commitEdit(*o); } }
      else if (in.released) { activeId = 0; m_scrubIdx = -1; } } }
  fillR({d.x, d.y + d.h - 1, d.w, 1}, M_DKR); reg(d);
  // ---- layer editor (objects listed like layers: V toggles visibility, tap selects)
  SDL_Rect l = m_layer; fillR(l, M_BG); int tx = l.x + P(4.f);
  { const char* lt[3] = {"Display", "Render", "Anim"}; for (int i = 0; i < 3; i++) { int w = skW(lt[i], px) + P(16.f); SDL_Rect r = {tx, l.y + P(2.f), w, P(18.f)}; fillR(r, i == 0 ? M_TABON : M_TABOFF); skTxt(r.x + P(8.f), r.y + r.h / 2, lt[i], i == 0 ? SDL_Color{20, 20, 20, 255} : M_TX, px); tx += w + P(1.f); } }
  fillR({l.x, l.y + P(21.f), l.w, 1}, M_DKR); { const char* lm[3] = {"Layers", "Options", "Help"}; int x2 = l.x + P(8.f); for (int i = 0; i < 3; i++) { skTxt(x2, l.y + P(31.f), lm[i], M_TX, px); x2 += skW(lm[i], px) + P(14.f); } }
  SDL_Rect list = {l.x + P(4.f), l.y + P(44.f), l.w - P(8.f), l.h - P(48.f)}; fillR(list, M_DK); SDL_RenderSetClipRect(R, &list); g_clip = list; g_clipOn = true;
  for (size_t i = 0; i < doc.objs.size(); i++) { int y = list.y + (int)i * P(18.f); if (y > list.y + list.h) break; SDL_Rect row = {list.x, y, list.w, P(18.f)}; Obj& ob = doc.objs[i]; bool sel = doc.sel == (int)i; if (sel) fillR(row, M_SEL);
    SDL_Rect vb = {row.x + P(4.f), row.y + P(2.f), P(14.f), P(14.f)}; if (tapIn(vb)) { pushUndo(); ob.hidden = !ob.hidden; } fillR(vb, M_DKR); fillR({vb.x + 1, vb.y + 1, vb.w - 2, vb.h - 2}, M_BG); skTxtC(vb, ob.hidden ? "" : "V", M_TX, FPX(9));
    SDL_Rect nm = {row.x + P(22.f), row.y, row.w - P(22.f), row.h}; if (tapIn(nm)) { doc.sel = (int)i; doc.selClip = -1; } skTxt(nm.x + P(4.f), row.y + row.h / 2, ob.name, sel ? C_WHITE : M_TX, px); reg(row); }
  SDL_RenderSetClipRect(R, nullptr); g_clipOn = false; reg(l);
}
static bool mFindKey(Obj& o, int f, int* ci) { for (size_t i = 0; i < o.clips.size(); i++) if (o.clips[i].keys.count(f - o.clips[i].start)) { *ci = (int)i; return true; } return false; }
static void mayaTimeline() {   // Maya Time Slider: frame ruler, keys of the selected object as red ticks, red current-time marker
  SDL_Rect a = sk_rAnim, rl = sk_rRulerR; Obj* o = selObj(); float pf = skPF(); int total = skTotalFrames(), px = FPX(10);
  fillR(a, M_BG); fillR(rl, M_RULER); fillR({rl.x, rl.y, rl.w, 1}, M_DKR); fillR({rl.x, rl.y + rl.h - 1, rl.w, 1}, M_DKR);
  int step = 1; { static const int steps[] = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800}; for (int i = 0; i < 11; i++) { step = steps[i]; if (step * pf >= P(40.f)) break; } }
  SDL_Rect cl = {rl.x, rl.y, rl.w, rl.h}; SDL_RenderSetClipRect(R, &cl); int f0 = std::max(0, (int)floorf(skF(rl.x)) - step), f1 = (int)ceilf(skF(rl.x + rl.w)) + step;
  if (pf >= 5.f) for (int f = f0; f <= f1; f++) { int x = skX((float)f); fillR({x, rl.y + rl.h - P(6.f), 1, P(5.f)}, {90, 90, 90, 255}); }
  for (int f = (f0 / step) * step; f <= f1; f += step) { int x = skX((float)f); fillR({x, rl.y + rl.h - P(13.f), std::max(1, P(1.f)), P(12.f)}, {40, 40, 40, 255}); skTxt(x + P(4.f), rl.y + P(9.f), std::to_string(f), {30, 30, 30, 255}, px); }
  { int fr[FrameCache::N]; g_fc.frames(fr); int bw = std::max(1, (int)ceilf(pf)); for (int i = 0; i < FrameCache::N; i++) if (fr[i] >= 0) fillR({skX((float)fr[i]), rl.y + rl.h - P(4.f), bw, P(3.f)}, {70, 200, 90, 255}); }   // cached frames (green, like Maya cached playback)
  if (o) for (size_t c = 0; c < o->clips.size(); c++) for (std::map<int, HKey>::const_iterator k = o->clips[c].keys.begin(); k != o->clips[c].keys.end(); ++k) { int x = skX((float)(o->clips[c].start + k->first)); fillR({x - 1, rl.y + 1, std::max(2, P(3.f)), rl.h - 2}, {204, 38, 38, 255}); }   // keys
  { int x = skX((float)curFrame); std::string n = std::to_string(curFrame); int w = skW(n, px) + P(10.f); fillR({x - w / 2, rl.y + 1, w, rl.h - 2}, {200, 36, 36, 255}); skTxtC({x - w / 2, rl.y + 1, w, rl.h - 2}, n, C_WHITE, px, true); }   // current time
  SDL_RenderSetClipRect(R, nullptr);
  // ---- gestures: drag = scrub, drag a red key = move it, two fingers = pan / zoom the slider
  static int keyF = -1;
  if (!g_lock && in.pressed && !consumed && !blockedAt(in.mx, in.my) && activeId == 0 && inside(rl, in.mx, in.my)) { activeId = ID_TL; keyF = -1; sk_dragKind = 6;
    if (o) { int best = P(9.f); for (size_t c = 0; c < o->clips.size(); c++) for (std::map<int, HKey>::const_iterator k = o->clips[c].keys.begin(); k != o->clips[c].keys.end(); ++k) { int d = abs(skX((float)(o->clips[c].start + k->first)) - in.mx); if (d < best && o->clips[c].start + k->first != curFrame) { best = d; keyF = o->clips[c].start + k->first; } } }
    if (keyF >= 0) { pushUndo(); setFrame(keyF); sk_dragKind = 5; } else setFrame((int)clampf((float)lroundf(skF(in.mx)), 0.f, (float)total)); }
  if (activeId == ID_TL && in.down && o) {
    int nf = (int)clampf((float)lroundf(skF(in.mx)), 0.f, (float)total);
    if (sk_dragKind == 6) setFrame(nf);
    else if (sk_dragKind == 5 && keyF >= 0 && nf != keyF) { int ci = -1; int occ = -1; if (!mFindKey(*o, nf, &occ) && mFindKey(*o, keyF, &ci)) { HKey kk = o->clips[ci].keys[keyF - o->clips[ci].start]; o->clips[ci].keys.erase(keyF - o->clips[ci].start); HClip* t = mClip(*o, nf); t->keys[nf - t->start] = kk; keyF = nf; setFrame(nf); } } }
  if (activeId == ID_TL && in.released) { activeId = 0; sk_dragKind = 0; keyF = -1; }
  if (!g_lock && inside(rl, in.mx, in.my)) {
    if (in.pinch != 0) pxPerSec = clampf(pxPerSec * expf(in.pinch * 6.f), 20.f, 800.f);
    if (in.panX != 0) sk_scroll = clampf(sk_scroll - in.panX / pf, 0.f, (float)std::max(0, total + kFps)); }
  if (playing) { int x = skX((float)curFrame); if (x > rl.x + rl.w - P(20.f) || x < rl.x) sk_scroll = std::max(0.f, (float)curFrame - kFps * 0.5f); }
  reg(a);
}
// ---- playback cluster (current time field + transport) and range slider, command line, help line
static void mayaBottom() {
  SDL_Rect pb = m_pbk; fillR(pb, M_BG); fillR({pb.x, pb.y, 1, pb.h}, M_DKR); int cy = pb.y + pb.h / 2, px = FPX(11); Obj* o = selObj();
  { char b[24]; snprintf(b, sizeof b, "%d.00", curFrame); mField({pb.x + P(6.f), cy - P(10.f), P(60.f), P(20.f)}, b, {58, 58, 58, 255}, M_TX, px, true); }
  int x = pb.x + P(72.f), bw = P(28.f), bh = P(22.f);
  for (int i = 0; i < 8; i++) { SDL_Rect r = {x, cy - bh / 2, bw, bh}; bool tap = mBtn(r, i == 3 && playing); int cx = r.x + bw / 2; SDL_Color c = M_TX;
    if (i == 0 || i == 7) fillR({cx + (i == 0 ? -P(6.f) : P(5.f)), cy - P(5.f), P(2.f), P(10.f)}, c);
    if (i == 0) skTri(cx + P(1.f), cy, P(5.f), -1, c); else if (i == 1 || i == 2) skTri(cx, cy, P(5.f), -1, i == 1 ? SDL_Color{230, 150, 150, 255} : c); else if (i == 3) { if (playing) { fillR({cx - P(5.f), cy - P(5.f), P(3.f), P(10.f)}, c); fillR({cx + P(2.f), cy - P(5.f), P(3.f), P(10.f)}, c); } else skTri(cx, cy, P(6.f), 1, {120, 220, 120, 255}); }
    else if (i == 4 || i == 5) skTri(cx, cy, P(5.f), 1, i == 5 ? SDL_Color{230, 150, 150, 255} : c); else if (i == 7) skTri(cx - P(1.f), cy, P(5.f), 1, c);
    else if (i == 6) skTri(cx, cy, P(5.f), 1, c);
    if (!tap) { x += bw + P(1.f); continue; }
    auto jumpKey = [&](bool fwd) { int best = -1; if (o) for (size_t c2 = 0; c2 < o->clips.size(); c2++) for (std::map<int, HKey>::const_iterator k = o->clips[c2].keys.begin(); k != o->clips[c2].keys.end(); ++k) { int f = o->clips[c2].start + k->first; if (fwd ? (f > curFrame && (best < 0 || f < best)) : (f < curFrame && f > best)) best = f; } setFrame(best >= 0 ? best : curFrame + (fwd ? kFps : -kFps)); };
    if (i == 0) setFrame(0); else if (i == 1) jumpKey(false); else if (i == 2) setFrame(curFrame - 1); else if (i == 3) { playing = !playing; playAcc = 0.f; if (playing && curFrame >= std::max(docEnd(), kFps)) setFrame(0); }
    else if (i == 4) { playing = !playing; playAcc = 0.f; } else if (i == 5) jumpKey(true); else if (i == 6) setFrame(curFrame + 1); else setFrame(std::max(docEnd(), 0));
    x += bw + P(1.f); }
  // range slider row
  SDL_Rect rs = m_rs; fillR(rs, {100, 100, 100, 255}); fillR({0, rs.y, W, 1}, M_DKR); int ry = rs.y + rs.h / 2; int total = skTotalFrames(); char b0[24];
  mField({P(4.f), ry - P(9.f), P(44.f), P(18.f)}, "0.00", {58, 58, 58, 255}, M_TX, px, true); mField({P(52.f), ry - P(9.f), P(44.f), P(18.f)}, "0.00", {58, 58, 58, 255}, M_TX, px, true);
  int rx = pb.x - P(430.f); SDL_Rect bar = {P(102.f), ry - P(8.f), std::max(P(60.f), rx - P(102.f)), P(16.f)}; fillR(bar, M_DKR); fillR({bar.x + 1, bar.y + 1, bar.w - 2, bar.h - 2}, {150, 150, 150, 255});
  { int hx = bar.x + 1 + (int)((bar.w - 2) * clampf((float)curFrame / (float)std::max(1, total), 0.f, 1.f)); fillR({bar.x + 1, bar.y + 1, hx - bar.x, bar.h - 2}, {120, 120, 124, 255}); fillR({hx - P(1.f), bar.y, P(3.f), bar.h}, {40, 40, 40, 255}); snprintf(b0, sizeof b0, "%d", curFrame); skTxt(bar.x + P(6.f), ry, "0", {30, 30, 30, 255}, px); skTxtR(bar.x + bar.w - P(6.f), ry, std::to_string(total), {30, 30, 30, 255}, px); }
  snprintf(b0, sizeof b0, "%d.00", total); mField({bar.x + bar.w + P(4.f), ry - P(9.f), P(44.f), P(18.f)}, b0, {58, 58, 58, 255}, M_TX, px, true); mField({bar.x + bar.w + P(52.f), ry - P(9.f), P(44.f), P(18.f)}, b0, {58, 58, 58, 255}, M_TX, px, true);
  { int x3 = bar.x + bar.w + P(104.f); SDL_Rect r = {x3, ry - P(9.f), P(104.f), P(18.f)}; mField(r, "No Anim Layer", {58, 58, 58, 255}, M_TX, px); x3 += r.w + P(6.f); r = {x3, ry - P(9.f), P(110.f), P(18.f)}; mField(r, "No Character Set", {58, 58, 58, 255}, M_TX, px); }
  { SDL_Rect r = {W - P(54.f), ry - P(10.f), P(24.f), P(20.f)}; if (mBtn(r, g_autoKey)) mAct(54); skIcon(SI_KEY, r.x + r.w / 2, ry, P(14.f), g_autoKey ? SDL_Color{255, 80, 80, 255} : M_TX); r = {W - P(28.f), ry - P(10.f), P(24.f), P(20.f)}; if (mBtn(r)) sheet = SH_CAMERA; skIcon(SI_GEARI, r.x + r.w / 2, ry, P(14.f), M_TX); }
  // command line (MEL label, input, result = last status message)
  SDL_Rect c = m_cmd; fillR(c, M_BG); fillR({0, c.y, W, 1}, M_DKR); int cy2 = c.y + c.h / 2; fillR({P(2.f), c.y + P(2.f), P(44.f), c.h - P(4.f)}, {79, 82, 89, 255}); skTxtC({P(2.f), c.y + P(2.f), P(44.f), c.h - P(4.f)}, "MEL", C_WHITE, px, true);
  fillR({P(50.f), c.y + P(2.f), W / 2 - P(52.f), c.h - P(4.f)}, M_DK); { std::string st; { std::lock_guard<std::mutex> l(g_statusMx); st = g_status; } if (SDL_GetTicks() - g_statusAt > 6000) st.clear(); fillR({W / 2 + P(2.f), c.y + P(2.f), W / 2 - P(6.f), c.h - P(4.f)}, M_DK); skTxt(W / 2 + P(8.f), cy2, st, M_TX, px); }
  // help line
  SDL_Rect h = m_help; fillR(h, M_BG); const char* ht = m_mtool == 0 ? "Select Tool: tap an object to select it" : m_mtool == 1 ? "Move Tool: drag the manipulator to move the selection" : m_mtool == 2 ? "Rotate Tool: drag the rings to rotate the selection" : "Scale Tool: drag the handles to scale the selection";
  skTxt(P(8.f), h.y + h.h / 2, ht, M_TX, FPX(10));
  { int cn = g_fc.count(); char cb[64]; snprintf(cb, sizeof cb, "Cache %d/%d  %d%%", cn, (int)FrameCache::N, (int)lroundf(g_fc.eff() * 100.f)); skTxtR(W - P(8.f), h.y + h.h / 2, cb, cn >= FrameCache::N ? SDL_Color{110, 220, 120, 255} : M_TXD, FPX(10)); }
  reg(m_pbk); reg(m_rs); reg(m_cmd); reg(m_help);
}
static void mayaDraw() {
  mayaPanelBar(); mayaViewportOverlay(); mayaToolbox(); mayaDock(); mayaTimeline(); mayaBottom(); mayaShelf(); mayaTopBars(); mayaPopup();
  { SDL_Rect fr = {m_panel.x, m_panel.y, m_panel.w, m_panel.h}; fillR({fr.x, fr.y + P(44.f), fr.w, 1}, M_DKR); }
}

static void drawSkin() {
  setPalette(mayaOn());
  if (mayaOn()) { mayaDraw(); drawToast(); return; }
  if (sk_path && sk_tab == SK_TABS - 1) skDrawPath();
  skDrawSidebar(); skDrawTop(); skDrawViewportOverlay();
  if (sk_tab == SK_TABS - 1) skDrawAnim(); else skDrawStatus();
  drawToast();
}

static void drawViewport() {
  fillR({0, 0, W, H}, g_bg); camSetup(); if (mayaOn()) mayaViewportBg();
  if (showGrid) drawGrid(); if (!skinOn()) drawAxes();
  g_vpBusy = activeId == ID_GIZMO || activeId == ID_KEYMOVE || activeId == ID_CLIPMOVE; g_vpScrub = activeId == ID_TL; updateViewportTexture(modeling);
  if (vpTex && !doc.objs.empty()) SDL_RenderCopy(R, vpTex, vpSrc.w > 0 ? &vpSrc : nullptr, &VP);
  if (showBones) drawSkeletons();
  drawEditOverlay();
  Obj* o = selObj(); if (gizmoShown(o)) { setAxisFrame(o); drawGizmo(pivotWorld(*o), tool); }
}

int main(int, char**) {
  InitEngineModules();
  if (SDL_Init(SDL_INIT_VIDEO) != 0) { printf("SDL_Init: %s\n", SDL_GetError()); return 1; }
  SDL_Window* win = SDL_CreateWindow("Source Hexagon", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 720, 1600, SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP);
  if (!win) { printf("window: %s\n", SDL_GetError()); return 1; }
  R = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!R) R = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
  if (!R) { printf("renderer: %s\n", SDL_GetError()); return 1; }
  SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
  g_win = win; uif::init(); refreshProjects();
  pxPerSec = 120.7f; cam.pitch = 0.15f; cam.dist = 18.f;   // reference view: low camera, 10 s across the timeline

  bool mgValid = false; float mgX = 0, mgY = 0;   // two-finger centroid for panning
  Uint32 lastEvt = SDL_GetTicks();                 // idle throttle: nothing touched for a moment -> redraw at a lower rate (battery / heat)
  bool running = true; Uint32 last = SDL_GetTicks(); float tAcc = 0.f;
  while (running) {
    SDL_GetRendererOutputSize(R, &W, &H); computeLayout();
    in.pressed = in.released = false; in.wheel = in.pinch = in.panX = in.panY = 0; in.multi = false; tapNow = false; consumed = false;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      lastEvt = SDL_GetTicks();
      switch (e.type) {
        case SDL_QUIT: autosaveTick(true); running = false; break;
        case SDL_APP_WILLENTERBACKGROUND: case SDL_APP_TERMINATING: autosaveTick(true); break;
        case SDL_KEYDOWN:
          if (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_AC_BACK) {
            if (m_open >= 0) m_open = -1;
            else if (g_vpMenu) g_vpMenu = false;
            else if (sk_delPrompt) sk_delPrompt = false;
            else if (screen == SCR_HOME) { if (homeSheet) { homeSheet = 0; confirmDel = confirmAll = false; } else running = false; }
            else if (g_meOpen) meRequestExit(); else if (sheet != SH_NONE) sheet = SH_NONE; else if (modeling) leaveModeling(); else if (doc.sel >= 0) { doc.sel = -1; doc.selClip = -1; } else goHome();
          }
          break;
        case SDL_MOUSEMOTION: in.mx = e.motion.x; in.my = e.motion.y; break;
        case SDL_MOUSEBUTTONDOWN: in.mx = e.button.x; in.my = e.button.y; in.down = true; in.pressed = true; pressX = in.mx; pressY = in.my; pressT = SDL_GetTicks(); moved = false; break;
        case SDL_MOUSEBUTTONUP:
          in.mx = e.button.x; in.my = e.button.y; in.down = false; in.released = true;
          if (hypotf((float)(in.mx - pressX), (float)(in.my - pressY)) > P(12.f)) moved = true;
          tapNow = !moved && !in.multi && SDL_GetTicks() - pressT < 600; break;
        case SDL_MOUSEWHEEL: in.wheel += (float)e.wheel.y; break;
        case SDL_FINGERDOWN: case SDL_FINGERUP: mgValid = false; break;
        case SDL_MULTIGESTURE: if (e.mgesture.numFingers == 2) { in.pinch += e.mgesture.dDist; in.multi = true;
            if (mgValid) { in.panX += (e.mgesture.x - mgX) * W; in.panY += (e.mgesture.y - mgY) * H; } mgX = e.mgesture.x; mgY = e.mgesture.y; mgValid = true; } break;
      }
    }
    if (in.down && hypotf((float)(in.mx - pressX), (float)(in.my - pressY)) > P(12.f)) moved = true;

    Uint32 now = SDL_GetTicks(); float dt = (now - last) / 1000.f; last = now;
    if (playing) {
      tAcc += dt * g_speed; int end = std::max(docEnd(), kFps);
      static Uint32 waitSince = 0;
      while (tAcc >= 1.f / kFps) {
        int nf = curFrame + 1; if (nf > end) { if (loopPlay) nf = 0; else nf = end; }
        if (cacheWaiting(nf, modeling) && nf != curFrame) {   // buffering: the background renderer is still working on this frame
          tAcc = std::min(tAcc, 1.f / kFps); Uint32 tn = SDL_GetTicks(); if (!waitSince) waitSince = tn; else if (tn - waitSince > 3000) { g_cacheStuck = true; setStatus("Playback cache unavailable - drawing live"); } break; }
        waitSince = 0; tAcc -= 1.f / kFps; if (curFrame + 1 > end && !loopPlay) { playing = false; setFrame(end); break; } setFrame(nf);
      }
    } else g_cacheStuck = false;
    if (doc.sel >= (int)doc.objs.size()) doc.sel = -1;
    if (modeling && !selObj()) leaveModeling();

    uiRects.clear(); g_lock = false; g_blockOn = false;
    if (screen == SCR_HOME) { drawHome(); drawToast(); if (in.released) { activeId = 0; } SDL_RenderPresent(R); SDL_Delay(1); continue; }
    autosaveTick();
    bool modal = sheet == SH_MODELS || sheet == SH_SKY || sheet == SH_CAMERA || sheet == SH_EXPORT || sheet == SH_HOME || sheet == SH_FILE;
    drawViewport();
    if (sheet != SH_NONE && sheet != SH_FILE) { g_block = sheetRect(sheet); g_blockOn = true; }
    if (modal) g_lock = true;
    if (sk_delPrompt || g_vpMenu || m_open >= 0) g_lock = true;   // a popup is open: everything behind it is inert
    if (skinOn()) { drawSkin(); skDrawDeletePrompt(); } else {
    drawTopOverlay();
    if (g_meOpen) { Obj* mo = selObj(); if ((mo ? mo->id : -1) != g_meObj) { if (!meLayerClean() && g_meObj >= 0) setStatus("Uncommitted edits discarded"); meBind(); } if (activeId != ID_GIZMO) applyAnim(); }
    if (modeling) drawModelingUI(); else if (g_meOpen) { reg({0, ctrlY - P(36), W, P(72)}); drawMotionEditor(); } else drawBottomBar();
    if (!modeling && !g_meOpen) { reg({0, ctrlY - P(36), W, P(72)}); reg(rTrack); reg(rRuler); drawTimeline(); }
    drawTransport();
    drawToast();
    }
    skDrawViewportMenu();
    g_lock = false; g_blockOn = false;
    drawSheet();
    g_lock = modal || sk_delPrompt || g_vpMenu || m_open >= 0; viewportInput(); g_lock = false;
    if (in.released) { activeId = 0; gzActive = false; }
    SDL_RenderPresent(R); SDL_Delay(!playing && !in.down && !g_busy && SDL_GetTicks() - lastEvt > 1200 ? 24 : 1);
  }
  g_fc.stop();
  if (vpTex) SDL_DestroyTexture(vpTex);
  for (std::map<uint32_t, GTex>::iterator it = g_glyphs.begin(); it != g_glyphs.end(); ++it) if (it->second.t) SDL_DestroyTexture(it->second.t);
  for (std::map<uint32_t, SDL_Texture*>::iterator it = g_shapeTex.begin(); it != g_shapeTex.end(); ++it) if (it->second) SDL_DestroyTexture(it->second);
  SDL_DestroyRenderer(R); SDL_DestroyWindow(win); SDL_Quit(); return 0;
}
