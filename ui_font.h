// ui_font.h - smooth, anti-aliased UI text for Magnificent (no SDL_ttf needed).
// Glyph outlines of DejaVu Sans Condensed (ASCII 32-126), pre-flattened and packed into a small
// base64 table, plus a scanline rasterizer that renders them at any pixel size.
// DejaVu fonts are free to embed (Bitstream Vera / DejaVu license: (c) Bitstream, Inc., Tavmjong Bah).
// Keep this file in the SAME folder as presto_ui.cpp. (Without it the app falls back to the old pixel font.)
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace uif {

struct Pt { float x, y; };
typedef std::vector<std::vector<Pt> > Polys;
struct Glyph { int adv = 0; Polys c; };                 // font units: 1000 per em, y up
struct Bmp { int w = 0, h = 0, ox = 0, oy = 0; float adv = 0; std::vector<uint8_t> a; };

static const float kCap = 0.729f, kXH = 0.547f, kAsc = 0.928f, kDesc = -0.236f;   // per 1 em

static const char* const kData[] = {
"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"
};

static std::vector<Glyph>& glyphs() { static std::vector<Glyph> g; return g; }

static int b64v(int c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  return c == '+' ? 62 : c == '/' ? 63 : -1;
}
static bool init() {
  std::vector<Glyph>& G = glyphs();
  if (!G.empty()) return true;
  std::vector<uint8_t> d; uint32_t acc = 0; int bits = 0;
  for (size_t i = 0; i < sizeof(kData) / sizeof(kData[0]); i++)
    for (const char* p = kData[i]; *p; p++) {
      int v = b64v(*p); if (v < 0) continue;
      acc = (acc << 6) | (uint32_t)v; bits += 6;
      if (bits >= 8) { bits -= 8; d.push_back((uint8_t)((acc >> bits) & 255)); }
    }
  size_t pos = 0;
  auto rv = [&]() { uint32_t n = 0; int sh = 0; while (pos < d.size()) { uint8_t b = d[pos++]; n |= (uint32_t)(b & 127) << sh; if (!(b & 128)) break; sh += 7; } return n; };
  auto unzz = [](uint32_t n) { return (int)(n >> 1) ^ -(int)(n & 1); };
  G.resize(95);
  for (int ch = 0; ch < 95; ch++) {
    G[ch].adv = (int)rv();
    int nc = (int)rv();
    for (int c = 0; c < nc && pos < d.size(); c++) {
      int np = (int)rv(); std::vector<Pt> pts; int x = 0, y = 0;
      for (int i = 0; i < np; i++) { x += unzz(rv()); y += unzz(rv()); Pt p = {(float)x, (float)y}; pts.push_back(p); }
      G[ch].c.push_back(pts);
    }
  }
  return true;
}
static const Glyph* glyph(int ch) { init(); return (ch >= 32 && ch <= 126) ? &glyphs()[ch - 32] : nullptr; }

// Non-zero-winding scanline rasterizer with exact horizontal coverage and SS sub-scanlines per pixel.
// 'polys' are in pixel space (y down); the result bitmap covers [0,w) x [0,h).
static void rasterPolys(const Polys& polys, int w, int h, std::vector<uint8_t>& out, float gamma = 1.0f) {
  const int SS = 5;
  out.assign((size_t)w * h, 0);
  struct E { float x0, y0, x1, y1; int dir; };
  std::vector<E> edges;
  for (size_t c = 0; c < polys.size(); c++) {
    const std::vector<Pt>& p = polys[c]; size_t n = p.size();
    for (size_t i = 0; i < n; i++) {
      Pt a = p[i], b = p[(i + 1) % n];
      if (a.y == b.y) continue;
      E e; if (a.y < b.y) { e.x0 = a.x; e.y0 = a.y; e.x1 = b.x; e.y1 = b.y; e.dir = 1; }
      else { e.x0 = b.x; e.y0 = b.y; e.x1 = a.x; e.y1 = a.y; e.dir = -1; }
      edges.push_back(e);
    }
  }
  std::vector<float> row((size_t)w + 1);
  std::vector<std::pair<float, int> > xs;
  for (int y = 0; y < h; y++) {
    std::fill(row.begin(), row.end(), 0.f);
    for (int s = 0; s < SS; s++) {
      float sy = y + (s + 0.5f) / SS; xs.clear();
      for (size_t i = 0; i < edges.size(); i++) {
        const E& e = edges[i];
        if (sy < e.y0 || sy >= e.y1) continue;
        float t = (sy - e.y0) / (e.y1 - e.y0);
        xs.push_back(std::make_pair(e.x0 + (e.x1 - e.x0) * t, e.dir));
      }
      std::sort(xs.begin(), xs.end());
      int wind = 0;
      for (size_t i = 0; i + 1 < xs.size(); i++) {
        wind += xs[i].second;
        if (wind == 0) continue;
        float xa = std::max(0.f, xs[i].first), xb = std::min((float)w, xs[i + 1].first);
        if (xb <= xa) continue;
        int ia = (int)xa, ib = (int)xb;
        if (ia == ib) row[ia] += (xb - xa) / SS;
        else {
          row[ia] += (ia + 1 - xa) / SS;
          for (int k = ia + 1; k < ib; k++) row[k] += 1.f / SS;
          if (ib < w) row[ib] += (xb - ib) / SS;
        }
      }
    }
    for (int x = 0; x < w; x++) {
      float v = std::min(1.f, row[x]);
      if (gamma != 1.0f) v = std::pow(v, gamma);
      out[(size_t)y * w + x] = (uint8_t)(v * 255.f + 0.5f);
    }
  }
}

// Renders one glyph at 'px' pixels per em. Origin: pen position on the baseline; (ox, oy) = bitmap top-left from the pen.
static bool rasterGlyph(int ch, int px, Bmp& o) {
  const Glyph* g = glyph(ch);
  if (!g) return false;
  float sc = px / 1000.f;
  o.adv = g->adv * sc; o.w = o.h = 0; o.a.clear();
  if (g->c.empty()) return true;
  float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
  for (size_t c = 0; c < g->c.size(); c++)
    for (size_t i = 0; i < g->c[c].size(); i++) {
      x0 = std::min(x0, g->c[c][i].x * sc); x1 = std::max(x1, g->c[c][i].x * sc);
      y0 = std::min(y0, -g->c[c][i].y * sc); y1 = std::max(y1, -g->c[c][i].y * sc);
    }
  int ix0 = (int)std::floor(x0) - 1, iy0 = (int)std::floor(y0) - 1, ix1 = (int)std::ceil(x1) + 1, iy1 = (int)std::ceil(y1) + 1;
  o.w = ix1 - ix0; o.h = iy1 - iy0; o.ox = ix0; o.oy = iy0;
  Polys P;
  for (size_t c = 0; c < g->c.size(); c++) {
    std::vector<Pt> q;
    for (size_t i = 0; i < g->c[c].size(); i++) { Pt p = {g->c[c][i].x * sc - ix0, -g->c[c][i].y * sc - iy0}; q.push_back(p); }
    P.push_back(q);
  }
  rasterPolys(P, o.w, o.h, o.a, 0.82f);   // slight gamma lift: light text on dark looks thin otherwise
  return true;
}

} // namespace uif
