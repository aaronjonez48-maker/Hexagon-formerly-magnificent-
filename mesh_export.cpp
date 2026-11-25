// mesh_export.cpp - Source mesh import (.mdl + .vvd + .dx90.vtx), software rasterizer (flat, vertex-colour or textured), PNG / PNG-sequence / MP4 export.
// Std-only, C++11. Included by presto_ui.cpp inside `namespace mx { using namespace std; ... }` (unity build),
// but also compiles on its own when wrapped in a namespace after the std includes below.
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>
using namespace std;

static bool g_baseLight = false;   // "Base light": all lighting effects off -> one cheap vertex-lit pass (fast, flat look)
static float g_focalScale = 1.f;   // field-of-view scale: tan(30deg)/tan(fov/2); 1.0 == 60 degrees
typedef vector<uint8_t> Bytes;

// ============================================================ mesh
struct Mesh3 {
  vector<float> pos, nrm;      // xyz triples, Y-up
  vector<uint32_t> idx;        // triangle list
  vector<float> uv;            // optional: 2 floats per vertex (1e9 = "this vertex has no texture")
  vector<uint8_t> col;         // optional: RGB per vertex (material colour)
  vector<uint8_t> tex;         // optional: RGBA texture (tw x th)
  int tw, th;
  float bmin[3], bmax[3];
  float metal, rough; bool pbr;   // PBR material inputs (from .glb); defaults = plain dielectric
  Mesh3() : tw(0), th(0), metal(0.f), rough(0.6f), pbr(false) { for (int i = 0; i < 3; i++) { bmin[i] = 0; bmax[i] = 0; } }
  size_t vcount() const { return pos.size() / 3; }
  size_t tcount() const { return idx.size() / 3; }
  void take(Mesh3& o) {        // move everything in (used when a freshly imported mesh becomes the scene mesh)
    pos.swap(o.pos); nrm.swap(o.nrm); idx.swap(o.idx); uv.swap(o.uv); col.swap(o.col); tex.swap(o.tex);
    tw = o.tw; th = o.th; metal = o.metal; rough = o.rough; pbr = o.pbr;
    for (int k = 0; k < 3; k++) { bmin[k] = o.bmin[k]; bmax[k] = o.bmax[k]; }
  }
};
static void calcBounds(Mesh3& m) {
  for (int k = 0; k < 3; k++) { m.bmin[k] = 1e30f; m.bmax[k] = -1e30f; }
  for (size_t i = 0; i < m.vcount(); i++)
    for (int k = 0; k < 3; k++) { m.bmin[k] = min(m.bmin[k], m.pos[i * 3 + k]); m.bmax[k] = max(m.bmax[k], m.pos[i * 3 + k]); }
  if (m.vcount() == 0) for (int k = 0; k < 3; k++) { m.bmin[k] = 0; m.bmax[k] = 1; }
}

// p' = (p - c) * s   (centers X/Z, puts the lowest point on y=0, makes the largest side ~2 units)
struct Fit {
  float s, cx, cy, cz;
  Fit() : s(1), cx(0), cy(0), cz(0) {}
};
static Fit makeFit(const float* lo, const float* hi) {
  Fit f;
  float ext = max(hi[0] - lo[0], max(hi[1] - lo[1], hi[2] - lo[2]));
  f.s = ext > 1e-6f ? 2.0f / ext : 1.0f;
  f.cx = (lo[0] + hi[0]) * 0.5f; f.cy = lo[1]; f.cz = (lo[2] + hi[2]) * 0.5f;
  return f;
}
static void fitPoint(const Fit& f, float* p) {
  p[0] = (p[0] - f.cx) * f.s; p[1] = (p[1] - f.cy) * f.s; p[2] = (p[2] - f.cz) * f.s;
}
static void applyFit(Mesh3& m, const Fit& f) {
  for (size_t i = 0; i < m.vcount(); i++) fitPoint(f, &m.pos[i * 3]);
  for (int k = 0; k < 3; k++) { m.bmin[k] = 1e30f; m.bmax[k] = -1e30f; }
  for (size_t i = 0; i < m.vcount(); i++)
    for (int k = 0; k < 3; k++) { m.bmin[k] = min(m.bmin[k], m.pos[i * 3 + k]); m.bmax[k] = max(m.bmax[k], m.pos[i * 3 + k]); }
}

// ============================================================ Source importer
template <class T> static T rd(const Bytes& b, long o) {
  T t; memset(&t, 0, sizeof(T));
  if (o >= 0 && (size_t)o + sizeof(T) <= b.size()) memcpy(&t, &b[(size_t)o], sizeof(T));
  return t;
}
static bool readFileBytes(const string& p, Bytes& b) {
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  b.assign(n > 0 ? (size_t)n : 0, 0);
  bool ok = n <= 0 || fread(&b[0], 1, (size_t)n, f) == (size_t)n;
  fclose(f);
  return ok;
}
static string stripExt(const string& p) {
  size_t d = p.find_last_of('.'), s = p.find_last_of('/');
  if (d == string::npos || (s != string::npos && d < s)) return p;
  return p.substr(0, d);
}
static bool readFirst(const string& base, const char* const* exts, Bytes& b, string* which) {
  for (int i = 0; exts[i]; i++)
    if (readFileBytes(base + exts[i], b)) { if (which) *which = base + exts[i]; return true; }
  return false;
}

// Reads LOD0 geometry of every bodypart's first model. Needs the .mdl (offsets), .vvd (vertices) and .dx90.vtx (triangles).
// Accepts the common typos too (.vvt / .dx90.vxt). Source is Z-up inches: (x,y,z) -> (x, z, -y).
static bool loadSourceMesh(const string& mdlPath, Mesh3& out, string& err, string* info = nullptr) {
  Bytes mdl, vvd, vtx;
  if (!readFileBytes(mdlPath, mdl)) { err = "cannot open " + mdlPath; return false; }
  if (mdl.size() < 240 || rd<int>(mdl, 0) != 0x54534449) { err = "not an IDST .mdl"; return false; }
  int ver = rd<int>(mdl, 4);
  if (ver < 44 || ver > 49) { err = "mesh import needs Source .mdl v44-49 (this is v" + to_string(ver) + ")"; return false; }
  string base = stripExt(mdlPath), vvdName, vtxName;
  static const char* const vvdExt[] = {".vvd", ".VVD", ".vvt", nullptr};
  static const char* const vtxExt[] = {".dx90.vtx", ".DX90.VTX", ".dx90.vxt", ".dx80.vtx", ".sw.vtx", ".vtx", nullptr};
  if (!readFirst(base, vvdExt, vvd, &vvdName)) { err = "no .vvd beside the .mdl"; return false; }
  if (!readFirst(base, vtxExt, vtx, &vtxName)) { err = "no .dx90.vtx beside the .mdl"; return false; }
  if (rd<int>(vvd, 0) != 0x56534449) { err = ".vvd has bad magic (IDSV)"; return false; }
  if (rd<int>(vtx, 0) != 7) { err = ".vtx has unexpected version " + to_string(rd<int>(vtx, 0)); return false; }

  // ---- vertices (LOD0, fixups applied)
  vector<int> src;
  int nfix = rd<int>(vvd, 48), fixStart = rd<int>(vvd, 52), vertStart = rd<int>(vvd, 56);
  if (nfix <= 0) { int n = rd<int>(vvd, 16); for (int i = 0; i < n && i < 2000000; i++) src.push_back(i); }
  else for (int i = 0; i < nfix && i < 100000; i++) {
    int lod = rd<int>(vvd, fixStart + i * 12), s = rd<int>(vvd, fixStart + i * 12 + 4), n = rd<int>(vvd, fixStart + i * 12 + 8);
    if (lod >= 0 && n > 0 && n < 2000000 && s >= 0) for (int j = 0; j < n; j++) src.push_back(s + j);
  }
  if (src.empty()) { err = ".vvd has no vertices"; return false; }
  out = Mesh3();
  out.pos.resize(src.size() * 3); out.nrm.resize(src.size() * 3);
  for (size_t i = 0; i < src.size(); i++) {
    long o = (long)vertStart + (long)src[i] * 48;
    float px = rd<float>(vvd, o + 16), py = rd<float>(vvd, o + 20), pz = rd<float>(vvd, o + 24);
    float nx = rd<float>(vvd, o + 28), ny = rd<float>(vvd, o + 32), nz = rd<float>(vvd, o + 36);
    out.pos[i * 3] = px; out.pos[i * 3 + 1] = pz; out.pos[i * 3 + 2] = -py;
    out.nrm[i * 3] = nx; out.nrm[i * 3 + 1] = nz; out.nrm[i * 3 + 2] = -ny;
  }

  // ---- triangles: vtx bodypart/model/lod/mesh/stripgroup -> indices into the vvd vertex list
  int nbp = rd<int>(vtx, 28), bpOff = rd<int>(vtx, 32);
  int mdlBp = rd<int>(mdl, 232), mdlBpIdx = rd<int>(mdl, 236);
  int sgStride = ver >= 49 ? 33 : 25;     // v49 StripGroupHeader carries 2 extra topology ints
  int nv = (int)src.size();
  for (int b = 0; b < nbp && b < mdlBp && b < 256; b++) {
    int bpH = bpOff + b * 8;
    if (rd<int>(vtx, bpH) < 1) continue;
    int modelH = bpH + rd<int>(vtx, bpH + 4);
    int bpM = mdlBpIdx + b * 16;
    if (rd<int>(mdl, bpM + 4) < 1) continue;
    int modelM = bpM + rd<int>(mdl, bpM + 12);
    int vbase = rd<int>(mdl, modelM + 84) / 48;
    int nMeshMdl = rd<int>(mdl, modelM + 72), meshBase = modelM + rd<int>(mdl, modelM + 76);
    int lodH = modelH + rd<int>(vtx, modelH + 4);
    int nmesh = rd<int>(vtx, lodH), meshOff = rd<int>(vtx, lodH + 4);
    for (int j = 0; j < nmesh && j < nMeshMdl && j < 256; j++) {
      int meshH = lodH + meshOff + j * 9;
      int voff = rd<int>(mdl, meshBase + j * 116 + 12);
      int nsg = rd<int>(vtx, meshH), sgOff = rd<int>(vtx, meshH + 4);
      for (int k = 0; k < nsg && k < 4096; k++) {
        int sgH = meshH + sgOff + k * sgStride;
        int vo = rd<int>(vtx, sgH + 4), ni = rd<int>(vtx, sgH + 8), io = rd<int>(vtx, sgH + 12);
        for (int i = 0; i + 2 < ni; i += 3) {
          int id[3];
          for (int q = 0; q < 3; q++) {
            int li = rd<uint16_t>(vtx, sgH + io + (i + q) * 2);
            int orig = rd<uint16_t>(vtx, sgH + vo + li * 9 + 4);
            id[q] = vbase + voff + orig;
          }
          if (id[0] >= 0 && id[1] >= 0 && id[2] >= 0 && id[0] < nv && id[1] < nv && id[2] < nv) {
            out.idx.push_back((uint32_t)id[0]); out.idx.push_back((uint32_t)id[1]); out.idx.push_back((uint32_t)id[2]);
          }
        }
      }
    }
  }
  if (out.idx.empty()) { err = "no triangles read (unsupported .vtx layout?)"; return false; }
  for (int k = 0; k < 3; k++) { out.bmin[k] = 1e30f; out.bmax[k] = -1e30f; }
  for (size_t i = 0; i < out.vcount(); i++)
    for (int k = 0; k < 3; k++) { out.bmin[k] = min(out.bmin[k], out.pos[i * 3 + k]); out.bmax[k] = max(out.bmax[k], out.pos[i * 3 + k]); }
  if (info) *info = vvdName + " + " + vtxName;
  return true;
}

// ============================================================ software rasterizer
struct Image {
  int w, h;
  vector<uint8_t> rgba;
  vector<float> z;
  Image() : w(0), h(0) {}
  void init(int W, int H, int r, int g, int b, int a) {
    w = W; h = H;
    rgba.resize((size_t)W * H * 4);
    for (size_t i = 0; i < (size_t)W * H; i++) { rgba[i * 4] = (uint8_t)r; rgba[i * 4 + 1] = (uint8_t)g; rgba[i * 4 + 2] = (uint8_t)b; rgba[i * 4 + 3] = (uint8_t)a; }
    z.assign((size_t)W * H, 1e30f);
  }
  void put(int x, int y, float d, int r, int g, int b) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    size_t k = (size_t)y * w + x;
    if (d < z[k]) { z[k] = d; rgba[k * 4] = (uint8_t)r; rgba[k * 4 + 1] = (uint8_t)g; rgba[k * 4 + 2] = (uint8_t)b; rgba[k * 4 + 3] = 255; }
  }
};

struct ViewCam { float yaw, pitch, dist, tx, ty, tz; ViewCam() : yaw(0.6f), pitch(0.3f), dist(7.f), tx(0), ty(1), tz(0) {} };
struct ModelXf { float tx, ty, tz, rollDeg, yawDeg; ModelXf() : tx(0), ty(0), tz(0), rollDeg(0), yawDeg(0) {} };
struct Line3 { float a[3], b[3]; uint8_t r, g, bl; };
struct Scene3 {
  const Mesh3* mesh;
  const Mesh3* topo;   // optional: triangles / UVs / texture / vertex colours are read from here (pos + nrm still come from mesh) -> no per-frame copies
  ModelXf xf;
  ViewCam cam;
  bool wire, grid;
  vector<Line3> lines;
  Scene3() : mesh(nullptr), topo(nullptr), wire(false), grid(false) {}
};

namespace {
struct Cm {                       // camera basis, same convention as the UI viewport
  float E[3], R[3], U[3], F[3], f; int W, H;
  void setup(const ViewCam& c, int w, int h) {
    W = w; H = h; f = h * 1.1f * g_focalScale;
    float cp = cosf(c.pitch);
    E[0] = c.tx + c.dist * cp * sinf(c.yaw); E[1] = c.ty + c.dist * sinf(c.pitch); E[2] = c.tz + c.dist * cp * cosf(c.yaw);
    F[0] = c.tx - E[0]; F[1] = c.ty - E[1]; F[2] = c.tz - E[2]; nrm(F);
    R[0] = F[1] * 0 - F[2] * 1; R[1] = F[2] * 0 - F[0] * 0; R[2] = F[0] * 1 - F[1] * 0; nrm(R);   // cross(F, up)
    U[0] = R[1] * F[2] - R[2] * F[1]; U[1] = R[2] * F[0] - R[0] * F[2]; U[2] = R[0] * F[1] - R[1] * F[0];
  }
  static void nrm(float* v) { float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 1e-9f) { v[0] /= l; v[1] /= l; v[2] /= l; } }
  void toCam(const float* p, float* o) const {
    float d0 = p[0] - E[0], d1 = p[1] - E[1], d2 = p[2] - E[2];
    o[0] = d0 * R[0] + d1 * R[1] + d2 * R[2]; o[1] = d0 * U[0] + d1 * U[1] + d2 * U[2]; o[2] = d0 * F[0] + d1 * F[1] + d2 * F[2];
  }
  void toScr(const float* c, float& sx, float& sy) const { sx = W * 0.5f + c[0] / c[2] * f; sy = H * 0.5f - c[1] / c[2] * f; }
};
void seg(Image& im, const Cm& cm, const float* a, const float* b, int r, int g, int bl, float bias) {
  float A[3], B[3]; cm.toCam(a, A); cm.toCam(b, B);
  const float n = 0.1f;
  if (A[2] < n && B[2] < n) return;
  if (A[2] < n) { float t = (n - A[2]) / (B[2] - A[2]); A[0] += (B[0] - A[0]) * t; A[1] += (B[1] - A[1]) * t; A[2] = n; }
  else if (B[2] < n) { float t = (n - B[2]) / (A[2] - B[2]); B[0] += (A[0] - B[0]) * t; B[1] += (A[1] - B[1]) * t; B[2] = n; }
  float x0, y0, x1, y1; cm.toScr(A, x0, y0); cm.toScr(B, x1, y1);
  int steps = (int)max(fabsf(x1 - x0), fabsf(y1 - y0)) + 1;
  if (steps > 6000) return;
  for (int i = 0; i <= steps; i++) {
    float t = (float)i / steps;
    im.put((int)(x0 + (x1 - x0) * t), (int)(y0 + (y1 - y0) * t), A[2] + (B[2] - A[2]) * t - bias, r, g, bl);
  }
}
}  // namespace

// ---- cheap anti-aliasing: alpha-aware 5-tap blend applied only on silhouette / high-contrast edges (FXAA-style)
static void fxaa(Image& im) {
  const int w = im.w, h = im.h; if (w < 3 || h < 3) return;
  static thread_local vector<uint8_t> src; src = im.rgba;
  const int st = w * 4;
  for (int y = 1; y < h - 1; y++) {
    for (int x = 1; x < w - 1; x++) {
      const uint8_t* c = &src[((size_t)y * w + x) * 4]; const uint8_t* n = c - st; const uint8_t* s2 = c + st; const uint8_t* wv = c - 4; const uint8_t* e = c + 4;
      int amax = max(max(n[3], s2[3]), max(wv[3], e[3])), amin = min(min(n[3], s2[3]), min(wv[3], e[3]));
      if (amax == 0 && c[3] == 0) continue;
      bool edge = max(amax, (int)c[3]) - min(amin, (int)c[3]) > 40;
      if (!edge) {
        int lc = (c[0] * 77 + c[1] * 151 + c[2] * 28) >> 8, l0 = (n[0] * 77 + n[1] * 151 + n[2] * 28) >> 8, l1 = (s2[0] * 77 + s2[1] * 151 + s2[2] * 28) >> 8, l2 = (wv[0] * 77 + wv[1] * 151 + wv[2] * 28) >> 8, l3 = (e[0] * 77 + e[1] * 151 + e[2] * 28) >> 8;
        int lmax = max(max(max(l0, l1), max(l2, l3)), lc), lmin = min(min(min(l0, l1), min(l2, l3)), lc);
        if (lmax - lmin < 26) continue;
      }
      int wc = c[3] * 2, w0 = n[3], w1 = s2[3], w2 = wv[3], w3 = e[3], sa = wc + w0 + w1 + w2 + w3; if (sa == 0) continue;
      uint8_t* o = &im.rgba[((size_t)y * w + x) * 4];
      for (int k = 0; k < 3; k++) o[k] = (uint8_t)((c[k] * wc + n[k] * w0 + s2[k] * w1 + wv[k] * w2 + e[k] * w3) / sa);
      o[3] = (uint8_t)(sa / 6);
    }
  }
}

namespace {
// ---- SFM / Source-style shading: 6-colour ambient cube, half-Lambert diffuse, Blinn-Phong lobe driven by PBR roughness,
//      Schlick Fresnel with a metal/dielectric F0, key + cool back light + camera fill, rim light, filmic tone-map.
float g_lin8[256]; bool g_lin8Ok = false;
inline float sat01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
inline void ambCube(const float* n, float* o) {
  static const float C[6][3] = {{0.27f, 0.29f, 0.35f}, {0.22f, 0.24f, 0.31f}, {0.40f, 0.44f, 0.54f}, {0.10f, 0.09f, 0.09f}, {0.30f, 0.30f, 0.33f}, {0.20f, 0.22f, 0.29f}};   // +x -x +y -y +z -z
  float wx = n[0] * n[0], wy = n[1] * n[1], wz = n[2] * n[2];
  const float* cx = C[n[0] >= 0 ? 0 : 1]; const float* cy = C[n[1] >= 0 ? 2 : 3]; const float* cz = C[n[2] >= 0 ? 4 : 5];
  for (int k = 0; k < 3; k++) o[k] = cx[k] * wx + cy[k] * wy + cz[k] * wz;
}
inline float pow5(float x) { float x2 = x * x; return x2 * x2 * x; }
inline float filmic(float x) { return sat01((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f)); }
void sfmShade(const float* n0, const float* pos, const float* E, const float* camF, const float* alb, float metal, float rough, int* out) {
  float v[3] = {E[0] - pos[0], E[1] - pos[1], E[2] - pos[2]}; float vl = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (vl > 1e-9f) { v[0] /= vl; v[1] /= vl; v[2] /= vl; }
  float n[3] = {n0[0], n0[1], n0[2]}, nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
  if (nl < 1e-6f) { n[0] = v[0]; n[1] = v[1]; n[2] = v[2]; } else { n[0] /= nl; n[1] /= nl; n[2] /= nl; }
  float ndv0 = n[0] * v[0] + n[1] * v[1] + n[2] * v[2]; if (ndv0 < 0.f) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; ndv0 = -ndv0; }   // two-sided
  float ndv = max(ndv0, 1e-4f);
  metal = sat01(metal); float r = min(1.f, max(0.045f, rough)), r2 = r * r, e = min(900.f, max(2.f, 2.f / (r2 * r2 + 1e-4f) - 2.f));   // Blinn-Phong exponent from roughness
  float f0[3], dif[3]; for (int k = 0; k < 3; k++) { f0[k] = 0.04f * (1.f - metal) + alb[k] * metal; dif[k] = alb[k] * (1.f - metal); }
  struct L { float d[3], c[3]; }; static const L Ls[3] = {
    {{0.4482f, 0.7968f, 0.3984f}, {1.05f, 0.99f, 0.90f}},     // key (warm)
    {{-0.6850f, 0.2446f, -0.6850f}, {0.28f, 0.34f, 0.55f}},   // cool back / rim light
    {{0.f, 0.f, 0.f}, {0.30f, 0.30f, 0.30f}}};                // camera fill (direction = -forward)
  float amb[3]; ambCube(n, amb); float lin[3]; for (int k = 0; k < 3; k++) lin[k] = dif[k] * amb[k];
  for (int li = 0; li < 3; li++) {
    float Ld[3] = {Ls[li].d[0], Ls[li].d[1], Ls[li].d[2]}; if (li == 2) { Ld[0] = -camF[0]; Ld[1] = -camF[1]; Ld[2] = -camF[2]; }
    float ndl = n[0] * Ld[0] + n[1] * Ld[1] + n[2] * Ld[2], hl = 0.5f * ndl + 0.5f; hl *= hl;   // half-Lambert (Valve)
    for (int k = 0; k < 3; k++) lin[k] += dif[k] * Ls[li].c[k] * hl;
    if (ndl > 0.f) {
      float h[3] = {Ld[0] + v[0], Ld[1] + v[1], Ld[2] + v[2]}, hlen = sqrtf(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]); if (hlen < 1e-9f) continue; h[0] /= hlen; h[1] /= hlen; h[2] /= hlen;
      float ndh = max(0.f, n[0] * h[0] + n[1] * h[1] + n[2] * h[2]), vh = sat01(v[0] * h[0] + v[1] * h[1] + v[2] * h[2]), fr = pow5(1.f - vh);
      if (e * (1.f - ndh) < 12.f) {   // ndh^e ~ exp(-e(1-ndh)): below this the highlight is invisible, skip the pow
        float sp = powf(ndh, e) * (e + 8.f) / 25.1327f * 1.5f * ndl * (li == 1 ? 0.8f : 1.f);
        for (int k = 0; k < 3; k++) lin[k] += (f0[k] + (1.f - f0[k]) * fr) * Ls[li].c[k] * sp;
      }
    }
  }
  float Rv[3] = {2.f * ndv * n[0] - v[0], 2.f * ndv * n[1] - v[1], 2.f * ndv * n[2] - v[2]}, env[3]; ambCube(Rv, env);   // stands in for a reflection probe
  float fe = pow5(1.f - ndv), gl = 1.f - r; for (int k = 0; k < 3; k++) { float Fr = f0[k] + (max(gl, f0[k]) - f0[k]) * fe; lin[k] += env[k] * Fr * (1.f - 0.5f * r); }
  float rim = powf(1.f - ndv, 3.f) * 0.30f * (0.25f + 0.75f * sat01((n[0] * Ls[1].d[0] + n[1] * Ls[1].d[1] + n[2] * Ls[1].d[2]) * 0.5f + 0.5f)) * (1.f - 0.5f * metal);
  static const float rc[3] = {0.60f, 0.70f, 1.00f}; for (int k = 0; k < 3; k++) lin[k] += rc[k] * rim;
  for (int k = 0; k < 3; k++) out[k] = (int)(sqrtf(filmic(lin[k])) * 255.f + 0.5f);   // tone-map, then display gamma
}
}  // namespace

static void renderScene(Image& im, const Scene3& sc) {
  if (!g_lin8Ok) { for (int i = 0; i < 256; i++) g_lin8[i] = (i / 255.f) * (i / 255.f); g_lin8Ok = true; }
  Cm cm; cm.setup(sc.cam, im.w, im.h);
  if (sc.mesh && !(sc.topo ? sc.topo->idx : sc.mesh->idx).empty()) {
    const Mesh3& m = *sc.mesh; const Mesh3& T = sc.topo ? *sc.topo : m; const vector<uint8_t>& COLV = m.col.empty() ? T.col : m.col;
    size_t n = m.vcount();
    float cr = cosf(sc.xf.rollDeg * 0.0174533f), sr = sinf(sc.xf.rollDeg * 0.0174533f);
    float cy = cosf(sc.xf.yawDeg * 0.0174533f), sy = sinf(sc.xf.yawDeg * 0.0174533f);
    const bool baseLit = g_baseLight;
    static thread_local vector<float> sx, sY, sz, wpos, wnrm, bshade; static thread_local vector<uint8_t> ok;   // reused every call: no per-frame allocations
    sx.resize(n); sY.resize(n); sz.resize(n); ok.resize(n);
    if (baseLit) bshade.resize(n); else { wpos.resize(n * 3); wnrm.resize(n * 3); }
    static const float L2[3] = {0.45f, 0.80f, 0.40f}; static const float L2n = sqrtf(0.45f * 0.45f + 0.80f * 0.80f + 0.40f * 0.40f);
    for (size_t i = 0; i < n; i++) {
      float p[3] = {m.pos[i * 3], m.pos[i * 3 + 1], m.pos[i * 3 + 2]}, q[3] = {m.nrm[i * 3], m.nrm[i * 3 + 1], m.nrm[i * 3 + 2]};
      float* v[2] = {p, q};
      for (int k = 0; k < 2; k++) {
        float x = v[k][0] * cr - v[k][1] * sr, y = v[k][0] * sr + v[k][1] * cr, z = v[k][2];
        float x2 = x * cy + z * sy, z2 = -x * sy + z * cy;
        v[k][0] = x2; v[k][1] = y; v[k][2] = z2;
      }
      p[0] += sc.xf.tx; p[1] += sc.xf.ty; p[2] += sc.xf.tz;
      float c[3]; cm.toCam(p, c);
      ok[i] = c[2] > 0.1f;
      if (ok[i]) {
        cm.toScr(c, sx[i], sY[i]); sz[i] = c[2];
        if (baseLit) {   // base light: one soft headlight + one fixed light, per vertex
          float l1 = max(0.f, -(q[0] * cm.F[0] + q[1] * cm.F[1] + q[2] * cm.F[2])), l2 = max(0.f, (q[0] * L2[0] + q[1] * L2[1] + q[2] * L2[2]) / L2n);
          bshade[i] = min(1.f, 0.20f + 0.55f * l1 + 0.35f * l2);
        } else for (int k = 0; k < 3; k++) { wpos[i * 3 + k] = p[k]; wnrm[i * 3 + k] = q[k]; }
      }
    }
    const float base[3] = {205, 210, 228};
    bool hasTex = !T.tex.empty() && T.uv.size() == n * 2 && T.tw > 0 && T.th > 0, hasCol = COLV.size() == n * 3;
    for (size_t t = 0; t + 2 < T.idx.size(); t += 3) {
      uint32_t a = T.idx[t], b = T.idx[t + 1], c = T.idx[t + 2];
      if (!ok[a] || !ok[b] || !ok[c]) continue;
      if (sc.wire) {
        uint32_t ids[4] = {a, b, c, a};
        for (int e = 0; e < 3; e++) {
          float pa[3] = {m.pos[ids[e] * 3], m.pos[ids[e] * 3 + 1], m.pos[ids[e] * 3 + 2]}, pb[3] = {m.pos[ids[e + 1] * 3], m.pos[ids[e + 1] * 3 + 1], m.pos[ids[e + 1] * 3 + 2]};
          for (int k = 0; k < 2; k++) { float* v = k ? pb : pa;
            float x = v[0] * cr - v[1] * sr, y = v[0] * sr + v[1] * cr, z = v[2];
            v[0] = x * cy + z * sy + sc.xf.tx; v[1] = y + sc.xf.ty; v[2] = -x * sy + z * cy + sc.xf.tz; }
          seg(im, cm, pa, pb, 220, 224, 235, 0.f);
        }
        continue;
      }
      float x0 = sx[a], y0 = sY[a], x1 = sx[b], y1 = sY[b], x2 = sx[c], y2 = sY[c];
      float area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
      if (fabsf(area) < 1e-6f) continue;
      int bx0 = max(0, (int)floorf(min(x0, min(x1, x2)))), bx1 = min(im.w - 1, (int)ceilf(max(x0, max(x1, x2))));
      int by0 = max(0, (int)floorf(min(y0, min(y1, y2)))), by1 = min(im.h - 1, (int)ceilf(max(y0, max(y1, y2))));
      float iz0 = 1.f / sz[a], iz1 = 1.f / sz[b], iz2 = 1.f / sz[c], inv = 1.f / area;
      bool triTex = hasTex && T.uv[a * 2] < 1e8f && T.uv[b * 2] < 1e8f && T.uv[c * 2] < 1e8f;
      if (bx0 > bx1 || by0 > by1) continue;
      const float dw0 = -(y2 - y1) * inv, dw1 = -(y0 - y2) * inv;   // barycentrics advance incrementally along a row
      for (int y = by0; y <= by1; y++) {
        const float py = y + 0.5f, pxs = bx0 + 0.5f;
        float w0 = ((x2 - x1) * (py - y1) - (y2 - y1) * (pxs - x1)) * inv, w1 = ((x0 - x2) * (py - y2) - (y0 - y2) * (pxs - x2)) * inv;
        const float* zr = &im.z[(size_t)y * im.w];
        for (int x = bx0; x <= bx1; x++, w0 += dw0, w1 += dw1) {
          float w2 = 1.f - w0 - w1;
          if (w0 < 0 || w1 < 0 || w2 < 0) continue;
          float iw = w0 * iz0 + w1 * iz1 + w2 * iz2, d = 1.f / iw;
          if (!(d < zr[x])) continue;   // early depth reject: hidden pixels are never shaded
          float pw0 = w0 * iz0 * d, pw1 = w1 * iz1 * d, pw2 = w2 * iz2 * d;   // perspective-correct weights
          if (baseLit) {
            float s = w0 * bshade[a] + w1 * bshade[b] + w2 * bshade[c], s2 = min(1.f, s * 1.1f + 0.12f); int r, g, bl;
            if (triTex) {
              float u = pw0 * T.uv[a * 2] + pw1 * T.uv[b * 2] + pw2 * T.uv[c * 2], vv = pw0 * T.uv[a * 2 + 1] + pw1 * T.uv[b * 2 + 1] + pw2 * T.uv[c * 2 + 1];
              u -= floorf(u); vv -= floorf(vv);
              int tx = min(T.tw - 1, (int)(u * T.tw)), ty = min(T.th - 1, (int)(vv * T.th)); const uint8_t* tp = &T.tex[((size_t)ty * T.tw + tx) * 4];
              if (tp[3] < 128) continue;
              r = (int)(tp[0] * s2); g = (int)(tp[1] * s2); bl = (int)(tp[2] * s2);
            } else if (hasCol) {
              r = (int)((w0 * COLV[a * 3] + w1 * COLV[b * 3] + w2 * COLV[c * 3]) * s2); g = (int)((w0 * COLV[a * 3 + 1] + w1 * COLV[b * 3 + 1] + w2 * COLV[c * 3 + 1]) * s2); bl = (int)((w0 * COLV[a * 3 + 2] + w1 * COLV[b * 3 + 2] + w2 * COLV[c * 3 + 2]) * s2);
            } else { r = (int)(base[0] * s); g = (int)(base[1] * s); bl = (int)(base[2] * s); }
            im.put(x, y, d, r, g, bl); continue;
          }
          float alb[3];
          if (triTex) {
            float u = pw0 * T.uv[a * 2] + pw1 * T.uv[b * 2] + pw2 * T.uv[c * 2], vv = pw0 * T.uv[a * 2 + 1] + pw1 * T.uv[b * 2 + 1] + pw2 * T.uv[c * 2 + 1];
            u -= floorf(u); vv -= floorf(vv);
            int tx = min(T.tw - 1, (int)(u * T.tw)), ty = min(T.th - 1, (int)(vv * T.th));
            const uint8_t* tp = &T.tex[((size_t)ty * T.tw + tx) * 4];
            if (tp[3] < 128) continue;
            alb[0] = g_lin8[tp[0]]; alb[1] = g_lin8[tp[1]]; alb[2] = g_lin8[tp[2]];
          } else if (hasCol) {
            for (int k = 0; k < 3; k++) { float cv = (pw0 * COLV[a * 3 + k] + pw1 * COLV[b * 3 + k] + pw2 * COLV[c * 3 + k]) * (1.f / 255.f); alb[k] = cv * cv; }
          } else { for (int k = 0; k < 3; k++) { float cv = base[k] * (1.f / 255.f); alb[k] = cv * cv; } }
          float nn[3], pp[3];
          for (int k = 0; k < 3; k++) { nn[k] = pw0 * wnrm[a * 3 + k] + pw1 * wnrm[b * 3 + k] + pw2 * wnrm[c * 3 + k]; pp[k] = pw0 * wpos[a * 3 + k] + pw1 * wpos[b * 3 + k] + pw2 * wpos[c * 3 + k]; }
          int rgb[3]; sfmShade(nn, pp, cm.E, cm.F, alb, m.metal, m.rough, rgb);
          im.put(x, y, d, rgb[0], rgb[1], rgb[2]);
        }
      }
    }
  }
  if (sc.grid)
    for (int i = -6; i <= 6; i++) {
      float a[3] = {(float)i, 0, -6}, b[3] = {(float)i, 0, 6}, c[3] = {-6, 0, (float)i}, d[3] = {6, 0, (float)i};
      int g = i == 0 ? 120 : 72;
      seg(im, cm, a, b, g, g, g + 8, 0.f); seg(im, cm, c, d, g, g, g + 8, 0.f);
    }
  for (size_t i = 0; i < sc.lines.size(); i++) seg(im, cm, sc.lines[i].a, sc.lines[i].b, sc.lines[i].r, sc.lines[i].g, sc.lines[i].bl, 0.02f);
}

// ============================================================ PNG writer (stored deflate, no zlib)
static uint32_t g_crcT[256];
static bool g_crcReady = false;
static void be32(Bytes& b, uint32_t v) { b.push_back((uint8_t)(v >> 24)); b.push_back((uint8_t)(v >> 16)); b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)v); }
static void pngChunk(FILE* f, const char* type, const Bytes& d) {
  Bytes h; be32(h, (uint32_t)d.size());
  fwrite(h.data(), 1, 4, f);
  Bytes td(type, type + 4); td.insert(td.end(), d.begin(), d.end());
  fwrite(td.data(), 1, td.size(), f);
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < td.size(); i++) c = g_crcT[(c ^ td[i]) & 255] ^ (c >> 8);
  Bytes e; be32(e, ~c);
  fwrite(e.data(), 1, 4, f);
}
static bool writePNG(const string& path, const Image& im) {
  if (!g_crcReady) {
    for (uint32_t n = 0; n < 256; n++) { uint32_t c = n; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; g_crcT[n] = c; }
    g_crcReady = true;
  }
  Bytes raw; raw.reserve((size_t)(im.w * 3 + 1) * im.h);
  for (int y = 0; y < im.h; y++) {
    raw.push_back(0);
    for (int x = 0; x < im.w; x++) { const uint8_t* p = &im.rgba[((size_t)y * im.w + x) * 4]; raw.push_back(p[0]); raw.push_back(p[1]); raw.push_back(p[2]); }
  }
  Bytes z; z.push_back(0x78); z.push_back(0x01);
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < raw.size(); i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
  size_t pos = 0;
  while (pos < raw.size()) {
    size_t n = min<size_t>(65535, raw.size() - pos);
    z.push_back(pos + n >= raw.size() ? 1 : 0);
    z.push_back((uint8_t)(n & 255)); z.push_back((uint8_t)((n >> 8) & 255));
    z.push_back((uint8_t)(~n & 255)); z.push_back((uint8_t)((~n >> 8) & 255));
    z.insert(z.end(), raw.begin() + (long)pos, raw.begin() + (long)(pos + n));
    pos += n;
  }
  be32(z, (b << 16) | a);
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return false;
  static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  fwrite(sig, 1, 8, f);
  Bytes ih; be32(ih, (uint32_t)im.w); be32(ih, (uint32_t)im.h);
  ih.push_back(8); ih.push_back(2); ih.push_back(0); ih.push_back(0); ih.push_back(0);
  pngChunk(f, "IHDR", ih); pngChunk(f, "IDAT", z); pngChunk(f, "IEND", Bytes());
  return fclose(f) == 0;
}

// ============================================================ export helpers
static string pad4(int n) { char b[16]; snprintf(b, sizeof b, "%04d", n); return b; }

// Folder for exports: <Downloads>/Magnificent, falling back to the current folder.
static string exportDir() {
  static const char* roots[] = {"/sdcard/Download", "/storage/emulated/0/Download", "/sdcard", nullptr};
  for (int i = 0; roots[i]; i++) {
    string d = string(roots[i]) + "/Magnificent";
    if (mkdir(d.c_str(), 0777) != 0 && errno != EEXIST) continue;
    string t = d + "/.w";
    FILE* f = fopen(t.c_str(), "wb");
    if (f) { fclose(f); remove(t.c_str()); return d; }
  }
  return ".";
}

static bool exportPNG(const string& path, int W, int H, const Scene3& sc, int bgR, int bgG, int bgB) {
  Image im; im.init(W, H, bgR, bgG, bgB, 255);
  renderScene(im, sc);
  return writePNG(path, im);
}

// Writes make_mp4.sh next to the frames and runs ffmpeg if it exists. Returns true only if an .mp4 was produced.
static bool encodeMp4(const string& framesDir, const string& prefix, const string& outMp4, int startFrame, int fps, string& cmdOut) {
  string in = framesDir + "/" + prefix + "_%04d.png";
  cmdOut = "ffmpeg -y -framerate " + to_string(fps) + " -start_number " + to_string(startFrame) + " -i \"" + in +
           "\" -c:v libx264 -pix_fmt yuv420p \"" + outMp4 + "\"";
  FILE* f = fopen((framesDir + "/make_mp4.sh").c_str(), "w");
  if (f) { fprintf(f, "#!/bin/sh\n%s\n", cmdOut.c_str()); fclose(f); }
  if (system("command -v ffmpeg >/dev/null 2>&1") != 0) return false;
  return system((cmdOut + " >/dev/null 2>&1").c_str()) == 0;
}
static bool makeDir(const string& d) { return mkdir(d.c_str(), 0777) == 0 || errno == EEXIST; }
