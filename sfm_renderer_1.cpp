// sfm_renderer.cpp -- Magnificent engine: SFM-style lighting & viewport pipeline
// Single file, std-only, C++11. Writes sfm_render.bmp and prints an ASCII preview.
//
//  1. Blinn-Phong + exponent maps, half-Lambert diffuse, Schlick Fresnel, rim light
//  2. Spotlight cone + shadow map + volumetric fog raymarch (Henyey-Greenstein) + dust motes
//  3. Accumulation buffer: sub-pixel AA + thin-lens depth of field + shutter motion blur
//  4. SSAO (hemisphere samples against the G-buffer depth)
//
// Tune kW/kH/kSubframes for your device: cost ~ W*H*kSubframes.
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include <functional>
#include <algorithm>
#include <chrono>
using namespace std;

typedef uint32_t u32;
static const float PI = 3.14159265358979f;
static const int   kW = 256, kH = 144;     // output resolution
static const int   kSubframes = 32;        // accumulation passes (AA + DoF + motion blur)
static const int   kShadowRes = 256;
static const int   kVolSteps = 28;
static const int   kAOSamples = 12;
static const int   kMotes = 1500;

// ============================ math ============================
struct Vec2 { float x, y; Vec2(float X = 0, float Y = 0) : x(X), y(Y) {} };
struct Vec3 {
    float x, y, z;
    Vec3(float X = 0, float Y = 0, float Z = 0) : x(X), y(Y), z(Z) {}
    Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
    Vec3 operator*(float s) const { return Vec3(x * s, y * s, z * s); }
    Vec3 operator*(const Vec3& o) const { return Vec3(x * o.x, y * o.y, z * o.z); }
    Vec3 operator/(float s) const { return Vec3(x / s, y / s, z / s); }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
};
static inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline Vec3 cross(const Vec3& a, const Vec3& b) { return Vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline float length(const Vec3& a) { return sqrtf(dot(a, a)); }
static inline Vec3 normalize(const Vec3& a) { float l = length(a); return l > 1e-12f ? a / l : Vec3(0, 1, 0); }
static inline float saturate(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
static inline float mixf(float a, float b, float t) { return a + (b - a) * t; }
static inline Vec3 mix(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }
static inline float smoothstep(float e0, float e1, float x) { float t = saturate((x - e0) / (e1 - e0)); return t * t * (3 - 2 * t); }

static inline u32 hash32(u32 x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }
static inline float hf(u32 x) { return (hash32(x) >> 8) * (1.0f / 16777216.0f); }
static inline u32 hash3(int x, int y, int z) { return hash32((u32)x * 73856093u ^ (u32)y * 19349663u ^ (u32)z * 83492791u); }
static float noise3(const Vec3& p) {   // smooth value noise in [0,1]
    float fx = floorf(p.x), fy = floorf(p.y), fz = floorf(p.z);
    int ix = (int)fx, iy = (int)fy, iz = (int)fz;
    float tx = p.x - fx, ty = p.y - fy, tz = p.z - fz;
    tx = tx * tx * (3 - 2 * tx); ty = ty * ty * (3 - 2 * ty); tz = tz * tz * (3 - 2 * tz);
    float c[2][2][2];
    for (int a = 0; a < 2; a++) for (int b = 0; b < 2; b++) for (int d = 0; d < 2; d++)
        c[a][b][d] = (hash3(ix + a, iy + b, iz + d) >> 8) * (1.0f / 16777216.0f);
    float x00 = mixf(c[0][0][0], c[1][0][0], tx), x10 = mixf(c[0][1][0], c[1][1][0], tx);
    float x01 = mixf(c[0][0][1], c[1][0][1], tx), x11 = mixf(c[0][1][1], c[1][1][1], tx);
    return mixf(mixf(x00, x10, ty), mixf(x01, x11, ty), tz);
}
static float halton(int i, int b) { float f = 1, r = 0; while (i > 0) { f /= b; r += f * (i % b); i /= b; } return r; }

// ============================ camera ============================
// Pinhole with optional NDC shear (shx, shy). Used for sub-pixel jitter and for the
// thin-lens model: moving the eye by (dx,dy) and shearing by dx/(D*tx) keeps the focal plane fixed.
struct Camera {
    Vec3 eye, right, up, fwd;
    float tx, ty, nearZ, shx, shy;
    Camera() : tx(1), ty(1), nearZ(0.05f), shx(0), shy(0) {}
    void lookAt(const Vec3& e, const Vec3& tgt, float fovYdeg, float aspect) {
        eye = e; fwd = normalize(tgt - e);
        right = normalize(cross(fwd, Vec3(0, 1, 0))); up = cross(right, fwd);
        ty = tanf(fovYdeg * 0.5f * PI / 180.0f); tx = ty * aspect; shx = shy = 0;
    }
    float depthOf(const Vec3& p) const { return dot(p - eye, fwd); }
    void toClip(const Vec3& p, float& x, float& y, float& w) const {   // affine in p
        Vec3 r = p - eye; float d = dot(r, fwd);
        w = d; x = dot(r, right) / tx + shx * d; y = dot(r, up) / ty + shy * d;
    }
    bool project(const Vec3& p, int W, int H, float& px, float& py, float& d) const {
        float x, y, w; toClip(p, x, y, w);
        if (w <= nearZ) return false;
        d = w; px = (x / w * 0.5f + 0.5f) * W; py = (1.0f - (y / w * 0.5f + 0.5f)) * H; return true;
    }
    Vec3 rayDir(float ndcx, float ndcy) const {
        return normalize(fwd + right * (tx * (ndcx - shx)) + up * (ty * (ndcy - shy)));
    }
};

// ============================ meshes ============================
struct Vert { Vec3 p, n; Vec2 uv; };
struct Mesh { vector<Vert> v; vector<int> idx; };
static Vert lerpVert(const Vert& a, const Vert& b, float t) {
    Vert r; r.p = mix(a.p, b.p, t); r.n = normalize(mix(a.n, b.n, t));
    r.uv = Vec2(mixf(a.uv.x, b.uv.x, t), mixf(a.uv.y, b.uv.y, t)); return r;
}
static void quad(Mesh& m, int a, int b, int c, int d) {
    m.idx.push_back(a); m.idx.push_back(b); m.idx.push_back(c);
    m.idx.push_back(a); m.idx.push_back(c); m.idx.push_back(d);
}
static Mesh makeSphere(int seg, int rings) {                // unit radius
    Mesh m;
    for (int i = 0; i <= rings; i++) for (int j = 0; j <= seg; j++) {
        float th = PI * i / rings, ph = 2 * PI * j / seg;
        Vec3 n(sinf(th) * cosf(ph), cosf(th), sinf(th) * sinf(ph));
        Vert v; v.p = n; v.n = n; v.uv = Vec2((float)j / seg, (float)i / rings); m.v.push_back(v);
    }
    for (int i = 0; i < rings; i++) for (int j = 0; j < seg; j++) {
        int a = i * (seg + 1) + j, b = a + 1, c = a + seg + 2, d = a + seg + 1; quad(m, a, b, c, d);
    }
    return m;
}
static Mesh makeCylinder(int seg) {                         // r=1, height 1, centered, caps
    Mesh m;
    for (int j = 0; j <= seg; j++) {
        float ph = 2 * PI * j / seg; Vec3 n(cosf(ph), 0, sinf(ph));
        Vert a; a.p = Vec3(n.x, -0.5f, n.z); a.n = n; a.uv = Vec2((float)j / seg, 0);
        Vert b = a; b.p.y = 0.5f; b.uv.y = 1; m.v.push_back(a); m.v.push_back(b);
    }
    for (int j = 0; j < seg; j++) { int a = j * 2; quad(m, a, a + 2, a + 3, a + 1); }
    for (int cap = 0; cap < 2; cap++) {
        float y = cap ? 0.5f : -0.5f; Vec3 n(0, cap ? 1.0f : -1.0f, 0);
        int c0 = (int)m.v.size(); Vert c; c.p = Vec3(0, y, 0); c.n = n; c.uv = Vec2(0.5f, 0.5f); m.v.push_back(c);
        for (int j = 0; j <= seg; j++) {
            float ph = 2 * PI * j / seg; Vert e; e.p = Vec3(cosf(ph), y, sinf(ph)); e.n = n;
            e.uv = Vec2(0.5f + 0.5f * cosf(ph), 0.5f + 0.5f * sinf(ph)); m.v.push_back(e);
        }
        for (int j = 0; j < seg; j++) { m.idx.push_back(c0); m.idx.push_back(c0 + 1 + j); m.idx.push_back(c0 + 2 + j); }
    }
    return m;
}
static Mesh makeBox() {                                     // unit cube, centered
    Mesh m;
    const Vec3 N[6] = {Vec3(1, 0, 0), Vec3(-1, 0, 0), Vec3(0, 1, 0), Vec3(0, -1, 0), Vec3(0, 0, 1), Vec3(0, 0, -1)};
    for (int f = 0; f < 6; f++) {
        Vec3 n = N[f], u = fabsf(n.y) > 0.5f ? Vec3(1, 0, 0) : Vec3(0, 1, 0), t = normalize(cross(n, u)), b = cross(n, t);
        int base = (int)m.v.size();
        for (int k = 0; k < 4; k++) {
            float su = (k == 1 || k == 2) ? 0.5f : -0.5f, sv = (k >= 2) ? 0.5f : -0.5f;
            Vert v; v.p = n * 0.5f + t * su + b * sv; v.n = n; v.uv = Vec2(su + 0.5f, sv + 0.5f); m.v.push_back(v);
        }
        quad(m, base, base + 1, base + 2, base + 3);
    }
    return m;
}
static Mesh makeTorus(float R, float r, int seg, int rseg) {
    Mesh m;
    for (int i = 0; i <= seg; i++) for (int j = 0; j <= rseg; j++) {
        float a = 2 * PI * i / seg, b = 2 * PI * j / rseg;
        Vert v; v.n = Vec3(cosf(b) * cosf(a), sinf(b), cosf(b) * sinf(a));
        v.p = Vec3((R + r * cosf(b)) * cosf(a), r * sinf(b), (R + r * cosf(b)) * sinf(a));
        v.uv = Vec2((float)i / seg, (float)j / rseg); m.v.push_back(v);
    }
    for (int i = 0; i < seg; i++) for (int j = 0; j < rseg; j++) {
        int a = i * (rseg + 1) + j, b = a + rseg + 1; quad(m, a, b, b + 1, a + 1);
    }
    return m;
}
static Mesh makePlane(float tiles) {                        // unit plane on XZ, uv in [0,tiles]
    Mesh m; const float s[4][2] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
    for (int k = 0; k < 4; k++) {
        Vert v; v.p = Vec3(s[k][0], 0, s[k][1]); v.n = Vec3(0, 1, 0);
        v.uv = Vec2((s[k][0] + 0.5f) * tiles, (s[k][1] + 0.5f) * tiles); m.v.push_back(v);
    }
    quad(m, 0, 3, 2, 1); return m;
}

// ============================ textures / materials ============================
struct Tex {
    int w, h; vector<float> d;
    Tex() : w(0), h(0) {}
    float at(int x, int y) const { x = ((x % w) + w) % w; y = ((y % h) + h) % h; return d[y * w + x]; }
    float sample(float u, float v) const {
        float fx = u * w - 0.5f, fy = v * h - 0.5f; int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
        float tx = fx - x0, ty = fy - y0;
        return mixf(mixf(at(x0, y0), at(x0 + 1, y0), tx), mixf(at(x0, y0 + 1), at(x0 + 1, y0 + 1), tx), ty);
    }
};
static Tex makeExpMap(int kind) {   // procedural exponent maps, values in [0,1]
    Tex t; t.w = t.h = 64; t.d.resize(64 * 64);
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
        float v;
        if (kind == 0) v = 0.30f + 0.70f * noise3(Vec3(x * 0.30f, y * 0.30f, 3.0f));            // skin pores
        else if (kind == 1) v = 0.55f + 0.45f * noise3(Vec3(x * 0.04f, y * 0.9f, 1.0f));        // brushed streaks
        else v = smoothstep(0.35f, 0.75f, noise3(Vec3(x * 0.12f, y * 0.12f, 5.0f)));            // floor scuffs
        t.d[y * 64 + x] = v;
    }
    return t;
}

struct Material {
    Vec3 albedo, albedo2; int pattern; float uvScale;      // pattern 1 = checker
    Vec3 specColor; float specStrength, f0;
    float expMin, expMax; const Tex* expMap;
    float rim; Vec3 rimColor;
};
struct Object { const Mesh* mesh; int mat; Vec3 pos, scale; float yaw; };

// ============================ rasterizer ============================
typedef function<void(int, int, const Vec3&, const Vec3&, const Vec2&, float)> FragFn;
static long g_tris = 0;

static void drawTri(const Camera& cam, int W, int H, const Vert& a, const Vert& b, const Vert& c, float* depth, const FragFn* shade) {
    const Vert* V[3] = {&a, &b, &c};
    float sx[3], sy[3], iw[3];
    for (int i = 0; i < 3; i++) {
        float x, y, w; cam.toClip(V[i]->p, x, y, w);
        iw[i] = 1.0f / w; sx[i] = (x / w * 0.5f + 0.5f) * W; sy[i] = (1.0f - (y / w * 0.5f + 0.5f)) * H;
    }
    float area = (sx[2] - sx[0]) * (sy[1] - sy[0]) - (sy[2] - sy[0]) * (sx[1] - sx[0]);
    if (fabsf(area) < 1e-8f) return;
    g_tris++;
    int minx = max(0, (int)floorf(min(sx[0], min(sx[1], sx[2])))), maxx = min(W - 1, (int)ceilf(max(sx[0], max(sx[1], sx[2]))));
    int miny = max(0, (int)floorf(min(sy[0], min(sy[1], sy[2])))), maxy = min(H - 1, (int)ceilf(max(sy[0], max(sy[1], sy[2]))));
    for (int y = miny; y <= maxy; y++) for (int x = minx; x <= maxx; x++) {
        float px = x + 0.5f, py = y + 0.5f;
        float l0 = ((px - sx[1]) * (sy[2] - sy[1]) - (py - sy[1]) * (sx[2] - sx[1])) / area;
        float l1 = ((px - sx[2]) * (sy[0] - sy[2]) - (py - sy[2]) * (sx[0] - sx[2])) / area;
        float l2 = 1.0f - l0 - l1;
        if (l0 < -1e-5f || l1 < -1e-5f || l2 < -1e-5f) continue;
        float q0 = l0 * iw[0], q1 = l1 * iw[1], q2 = l2 * iw[2], s = q0 + q1 + q2;
        float d = 1.0f / s; int id = y * W + x;
        if (d >= depth[id]) continue;
        depth[id] = d;
        if (shade) {
            Vec3 p = (a.p * q0 + b.p * q1 + c.p * q2) * d;
            Vec3 n = normalize((a.n * q0 + b.n * q1 + c.n * q2) * d);
            Vec2 uv((a.uv.x * q0 + b.uv.x * q1 + c.uv.x * q2) * d, (a.uv.y * q0 + b.uv.y * q1 + c.uv.y * q2) * d);
            (*shade)(x, y, p, n, uv, d);
        }
    }
}
static void rasterMesh(const Camera& cam, int W, int H, const vector<Vert>& v, const vector<int>& idx, float* depth, const FragFn* shade) {
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        Vert in[3] = {v[idx[t]], v[idx[t + 1]], v[idx[t + 2]]}, poly[4]; int n = 0;
        float dd[3];
        for (int i = 0; i < 3; i++) dd[i] = cam.depthOf(in[i].p) - cam.nearZ;
        for (int i = 0; i < 3; i++) {                           // clip against near plane
            int j = (i + 1) % 3; bool ia = dd[i] >= 0, ib = dd[j] >= 0;
            if (ia) poly[n++] = in[i];
            if (ia != ib) poly[n++] = lerpVert(in[i], in[j], dd[i] / (dd[i] - dd[j]));
        }
        for (int k = 1; k + 1 < n; k++) drawTri(cam, W, H, poly[0], poly[k], poly[k + 1], depth, shade);
    }
}
static void transformMesh(const Object& o, vector<Vert>& out) {
    float cy = cosf(o.yaw), sy = sinf(o.yaw); out.resize(o.mesh->v.size());
    for (size_t i = 0; i < out.size(); i++) {
        const Vert& s = o.mesh->v[i]; Vec3 p(s.p.x * o.scale.x, s.p.y * o.scale.y, s.p.z * o.scale.z);
        out[i].p = Vec3(p.x * cy + p.z * sy, p.y, -p.x * sy + p.z * cy) + o.pos;
        out[i].n = Vec3(s.n.x * cy + s.n.z * sy, s.n.y, -s.n.x * sy + s.n.z * cy);   // valid: scale never skews these normals
        out[i].uv = s.uv;
    }
}

// ============================ scene ============================
struct Spot {
    Vec3 pos, dir, color; float intensity, cosInner, cosOuter; Camera cam; vector<float> sm;
};
enum { M_FLOOR, M_WALL, M_SKIN, M_EYE, M_METAL, M_CLOTH, M_COLLAR, M_SLAT };
struct Scene {
    Mesh sphere, cyl, box, torus, plane, floorPlane;
    Tex skinExp, metalExp, floorExp;
    vector<Material> mats;
    struct Mote { Vec3 base, vel; float phase, bright, size; };
    vector<Mote> motes;
    Spot spot;
    Vec3 rimDir, rimColor;
    void init() {
        sphere = makeSphere(32, 20); cyl = makeCylinder(32); box = makeBox();
        torus = makeTorus(1.0f, 0.3f, 40, 14); plane = makePlane(1); floorPlane = makePlane(12);
        skinExp = makeExpMap(0); metalExp = makeExpMap(1); floorExp = makeExpMap(2);
        mats.resize(8);
        mats[M_FLOOR]  = Material{Vec3(0.20f, 0.22f, 0.27f), Vec3(0.09f, 0.10f, 0.13f), 1, 1.0f, Vec3(1, 1, 1), 0.35f, 0.04f, 12, 120, &floorExp, 0.0f, Vec3()};
        mats[M_WALL]   = Material{Vec3(0.16f, 0.14f, 0.18f), Vec3(), 0, 1, Vec3(1, 1, 1), 0.05f, 0.04f, 8, 8, 0, 0.0f, Vec3()};
        mats[M_SKIN]   = Material{Vec3(0.78f, 0.52f, 0.42f), Vec3(), 0, 1, Vec3(1, 0.9f, 0.85f), 0.45f, 0.028f, 10, 60, &skinExp, 0.9f, Vec3(0.55f, 0.75f, 1.0f)};
        mats[M_EYE]    = Material{Vec3(0.02f, 0.02f, 0.03f), Vec3(), 0, 1, Vec3(1, 1, 1), 1.0f, 0.05f, 400, 400, 0, 0.0f, Vec3()};
        mats[M_METAL]  = Material{Vec3(0.03f, 0.03f, 0.035f), Vec3(), 0, 1, Vec3(0.95f, 0.85f, 0.7f), 1.0f, 0.90f, 60, 600, &metalExp, 0.8f, Vec3(0.6f, 0.8f, 1.0f)};
        mats[M_CLOTH]  = Material{Vec3(0.12f, 0.20f, 0.45f), Vec3(), 0, 1, Vec3(1, 1, 1), 0.06f, 0.04f, 6, 6, 0, 0.6f, Vec3(0.55f, 0.75f, 1.0f)};
        mats[M_COLLAR] = Material{Vec3(0.85f, 0.85f, 0.82f), Vec3(), 0, 1, Vec3(1, 1, 1), 0.2f, 0.04f, 20, 20, 0, 0.3f, Vec3(0.55f, 0.75f, 1.0f)};
        mats[M_SLAT]   = Material{Vec3(0.05f, 0.05f, 0.06f), Vec3(), 0, 1, Vec3(1, 1, 1), 0.1f, 0.04f, 16, 16, 0, 0.0f, Vec3()};

        spot.pos = Vec3(3.6f, 5.0f, 2.0f); spot.dir = normalize(Vec3(0, 1.3f, 0) - spot.pos);
        spot.color = Vec3(1.0f, 0.86f, 0.68f); spot.intensity = 70.0f;
        spot.cosInner = cosf(17.0f * PI / 180); spot.cosOuter = cosf(30.0f * PI / 180);
        spot.cam.lookAt(spot.pos, spot.pos + spot.dir, 2 * 33.0f, 1.0f);
        spot.cam.nearZ = 0.3f; spot.sm.assign(kShadowRes * kShadowRes, 1e9f);
        rimDir = normalize(Vec3(-0.5f, 0.55f, -1.0f)); rimColor = Vec3(0.35f, 0.55f, 1.0f);

        for (int i = 0; i < kMotes; i++) {
            Mote m; m.base = Vec3(-5 + 10 * hf(i * 7 + 1), 5.5f * hf(i * 7 + 2), -4.5f + 8.5f * hf(i * 7 + 3));
            m.vel = Vec3(0.05f * (hf(i * 7 + 4) - 0.5f), 0.04f * (hf(i * 7 + 5) - 0.3f), 0.05f * (hf(i * 7 + 6) - 0.5f));
            m.phase = 6.28f * hf(i * 11 + 9); m.bright = 0.4f + 1.6f * powf(hf(i * 13 + 5), 2.0f); m.size = 0.012f + 0.02f * hf(i * 3 + 17);
            motes.push_back(m);
        }
    }
    Vec3 motePos(const Mote& m, float t) const {
        Vec3 p = m.base + m.vel * t; p.x += 0.05f * sinf(1.3f * t + m.phase); p.y += 0.04f * sinf(1.7f * t + 2 * m.phase);
        auto wrap = [](float v, float lo, float hi) { float s = hi - lo; v = fmodf(v - lo, s); if (v < 0) v += s; return v + lo; };
        return Vec3(wrap(p.x, -5, 5), wrap(p.y, 0, 5.5f), wrap(p.z, -4.5f, 4));
    }
    void build(float t, vector<Object>& o) const {   // animated scene at time t
        o.clear();
        float bob = 0.015f * sinf(2 * PI * 0.8f * t), yaw = 0.55f * sinf(2 * PI * 0.35f * t);
        o.push_back(Object{&floorPlane, M_FLOOR, Vec3(0, 0, 0), Vec3(24, 1, 24), 0});
        o.push_back(Object{&box, M_WALL, Vec3(0, 3, -5.5f), Vec3(16, 6, 0.3f), 0});
        o.push_back(Object{&cyl, M_CLOTH, Vec3(0, 0.95f + bob, 0), Vec3(0.36f, 1.0f, 0.36f), 0});     // torso
        o.push_back(Object{&cyl, M_SKIN, Vec3(0, 1.58f + bob, 0), Vec3(0.10f, 0.20f, 0.10f), 0});     // neck
        o.push_back(Object{&torus, M_COLLAR, Vec3(0, 1.50f + bob, 0), Vec3(0.17f, 0.17f, 0.17f), 0}); // collar
        Vec3 head(0, 1.80f + bob, 0);
        o.push_back(Object{&sphere, M_SKIN, head, Vec3(0.24f, 0.24f, 0.24f), yaw});
        float cy = cosf(yaw), sy = sinf(yaw);
        auto rot = [&](Vec3 p) { return Vec3(p.x * cy + p.z * sy, p.y, -p.x * sy + p.z * cy); };
        o.push_back(Object{&sphere, M_EYE, head + rot(Vec3(-0.085f, 0.04f, 0.205f)), Vec3(0.045f, 0.045f, 0.045f), 0});
        o.push_back(Object{&sphere, M_EYE, head + rot(Vec3(0.085f, 0.04f, 0.205f)), Vec3(0.045f, 0.045f, 0.045f), 0});
        o.push_back(Object{&sphere, M_SKIN, head + rot(Vec3(0, -0.03f, 0.235f)), Vec3(0.04f, 0.04f, 0.04f), 0});   // nose
        // pendulum (fast, for motion blur)
        float th = 0.9f * sinf(2 * PI * 0.9f * t); Vec3 piv(0.5f, 4.4f, 1.3f);
        o.push_back(Object{&sphere, M_METAL, piv + Vec3(3.0f * sinf(th), -3.0f * cosf(th), 0), Vec3(0.22f, 0.22f, 0.22f), 0});
        // vertical blinds in front of the spotlight -> light shafts. Slats are thin across the
        // beam (local X) and long along it, so they cut stripes instead of blocking the cone.
        Vec3 c = spot.pos + spot.dir * 1.3f; float ys = atan2f(spot.dir.x, spot.dir.z);
        Vec3 xa(cosf(ys), 0, -sinf(ys));
        for (int i = 0; i < 7; i++)
            o.push_back(Object{&box, M_SLAT, c + xa * (-0.63f + 0.21f * i), Vec3(0.07f, 1.7f, 0.45f), ys});
    }
};

// ============================ shading helpers ============================
static float hgPhase(float cosT, float g) {            // Henyey-Greenstein (integrates to 1 over the sphere)
    float d = 1 + g * g - 2 * g * cosT; return (1 - g * g) / (4 * PI * d * sqrtf(d));
}
static float blinnPhong(const Vec3& n, const Vec3& l, const Vec3& v, float e) {
    Vec3 h = normalize(l + v); return powf(max(0.0f, dot(n, h)), e);
}
static float spotCone(const Spot& s, const Vec3& P, Vec3& toL, float& dist) {
    Vec3 v = s.pos - P; dist = length(v); toL = v / dist;
    return smoothstep(s.cosOuter, s.cosInner, -dot(toL, s.dir));
}
static float shadowAt(const Spot& s, const Vec3& P, bool pcf) {
    float x, y, w; s.cam.toClip(P, x, y, w);
    if (w <= s.cam.nearZ) return 1.0f;
    float u = (x / w * 0.5f + 0.5f) * kShadowRes, v = (1.0f - (y / w * 0.5f + 0.5f)) * kShadowRes;
    float bias = (2.0f * w * s.cam.tx / kShadowRes) * 1.8f + 0.01f;
    int cx = (int)floorf(u), cy = (int)floorf(v);
    if (!pcf) {
        if (cx < 0 || cy < 0 || cx >= kShadowRes || cy >= kShadowRes) return 1.0f;
        return (w - bias > s.sm[cy * kShadowRes + cx]) ? 0.0f : 1.0f;
    }
    float lit = 0; int cnt = 0;
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
        int X = cx + dx, Y = cy + dy; cnt++;
        if (X < 0 || Y < 0 || X >= kShadowRes || Y >= kShadowRes) { lit += 1; continue; }
        lit += (w - bias > s.sm[Y * kShadowRes + X]) ? 0.0f : 1.0f;
    }
    return lit / cnt;
}
static Vec3 tonemapACES(const Vec3& c) {
    auto f = [](float x) { return saturate((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f)); };
    return Vec3(f(c.x), f(c.y), f(c.z));
}

// ============================ renderer ============================
struct Stats { double tShadow, tRaster, tAO, tLight, tVol, tMotes; long lit, px; double aoSum; Stats() { memset(this, 0, sizeof(*this)); } };
static double nowSec() { return chrono::duration<double>(chrono::steady_clock::now().time_since_epoch()).count(); }

struct RenderCfg {
    float t0, shutter;            // frame start time, shutter open duration (s)
    float aperture, focusDist;    // thin lens radius (m), focus distance (m)
    Vec3 eye, target; float fov;
};

static void renderAccum(Scene& sc, const RenderCfg& cfg, vector<Vec3>& accum, Stats& st) {
    const int W = kW, H = kH; accum.assign(W * H, Vec3());
    Camera base; base.lookAt(cfg.eye, cfg.target, cfg.fov, (float)W / H);
    vector<float> depth(W * H); vector<Vec3> P(W * H), N(W * H), col(W * H); vector<Vec2> UV(W * H); vector<int> oid(W * H);
    vector<Object> objs; vector<vector<Vert> > wv;

    for (int f = 0; f < kSubframes; f++) {
        // --- accumulation jitter: pixel (AA), lens (DoF), time (motion blur) ---
        float jx = (halton(f + 1, 2) - 0.5f) * 2.0f / W, jy = (halton(f + 1, 3) - 0.5f) * 2.0f / H;
        float lr = sqrtf(halton(f + 1, 5)), la = 2 * PI * halton(f + 1, 7);
        float ldx = cfg.aperture * lr * cosf(la), ldy = cfg.aperture * lr * sinf(la);
        float t = cfg.t0 + cfg.shutter * ((f + halton(f + 1, 11)) / kSubframes);
        Camera cam = base; cam.eye = base.eye + base.right * ldx + base.up * ldy;
        cam.shx = ldx / (cfg.focusDist * base.tx) + jx; cam.shy = ldy / (cfg.focusDist * base.ty) + jy;

        sc.build(t, objs); wv.resize(objs.size());
        for (size_t i = 0; i < objs.size(); i++) transformMesh(objs[i], wv[i]);

        // --- shadow map from the spotlight ---
        double t0 = nowSec();
        fill(sc.spot.sm.begin(), sc.spot.sm.end(), 1e9f);
        for (size_t i = 0; i < objs.size(); i++) rasterMesh(sc.spot.cam, kShadowRes, kShadowRes, wv[i], objs[i].mesh->idx, &sc.spot.sm[0], 0);
        double t1 = nowSec(); st.tShadow += t1 - t0;

        // --- G-buffer ---
        fill(depth.begin(), depth.end(), 1e9f); fill(oid.begin(), oid.end(), -1);
        for (size_t i = 0; i < objs.size(); i++) {
            int id = (int)i;
            FragFn fn = [&, id](int x, int y, const Vec3& p, const Vec3& n, const Vec2& uv, float) {
                int k = y * W + x; P[k] = p; N[k] = n; UV[k] = uv; oid[k] = id;
            };
            rasterMesh(cam, W, H, wv[i], objs[i].mesh->idx, &depth[0], &fn);
        }
        double t2 = nowSec(); st.tRaster += t2 - t1;

        // --- SSAO ---
        vector<float> ao(W * H, 1.0f);
        const float R = 0.55f;
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            int k = y * W + x; if (oid[k] < 0) continue;
            Vec3 n = N[k], up = fabsf(n.y) < 0.99f ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
            Vec3 tg = normalize(cross(up, n)), bt = cross(n, tg);
            u32 seed = hash32((u32)k * 2654435761u + (u32)f * 97u); float rot = 2 * PI * hf(seed);
            float occ = 0;
            for (int s = 0; s < kAOSamples; s++) {
                float u1 = (s + hf(seed + s * 3 + 1)) / kAOSamples, u2 = hf(seed + s * 5 + 2);
                float rr = sqrtf(u1), ph = 2 * PI * u2 + rot;
                Vec3 dir = tg * (rr * cosf(ph)) + bt * (rr * sinf(ph)) + n * sqrtf(1 - u1);
                float sc2 = 0.2f + 0.8f * ((s + 1.0f) / kAOSamples) * ((s + 1.0f) / kAOSamples);
                Vec3 S = P[k] + dir * (R * sc2); float px, py, ds;
                if (!cam.project(S, W, H, px, py, ds)) continue;
                int ix = (int)px, iy = (int)py; if (ix < 0 || iy < 0 || ix >= W || iy >= H) continue;
                float db = depth[iy * W + ix];
                if (db < ds - 0.03f) occ += smoothstep(0.0f, 1.0f, R / max(1e-4f, fabsf(depth[k] - db)));
            }
            ao[k] = powf(saturate(1.0f - occ / kAOSamples), 1.5f); st.aoSum += ao[k];
        }
        double t3 = nowSec(); st.tAO += t3 - t2;

        // --- lighting + volumetrics ---
        double tl = 0, tv = 0;
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            int k = y * W + x; st.px++;
            float ndcx = (x + 0.5f) / W * 2 - 1, ndcy = 1 - (y + 0.5f) / H * 2;
            Vec3 rd = cam.rayDir(ndcx, ndcy), c;
            float tHit = 24.0f;
            double a0 = nowSec();
            if (oid[k] < 0) {
                c = mix(Vec3(0.004f, 0.005f, 0.008f), Vec3(0.015f, 0.02f, 0.035f), saturate(0.5f + 0.5f * rd.y));
            } else {
                const Material& m = sc.mats[objs[oid[k]].mat];
                Vec3 n = N[k], p = P[k], v = normalize(cam.eye - p); tHit = min(24.0f, length(p - cam.eye));
                Vec3 alb = m.albedo;
                if (m.pattern == 1) { int cx = (int)floorf(UV[k].x * 12), cy = (int)floorf(UV[k].y * 12); if ((cx + cy) & 1) alb = m.albedo2; }
                float e = m.expMap ? mixf(m.expMin, m.expMax, m.expMap->sample(UV[k].x, UV[k].y)) : m.expMin;
                // ambient: hemisphere, occluded by SSAO
                Vec3 amb = mix(Vec3(0.010f, 0.010f, 0.012f), Vec3(0.055f, 0.070f, 0.110f), 0.5f + 0.5f * n.y);
                c = alb * amb * ao[k] * 1.6f;
                // key spotlight: half-Lambert (Valve) diffuse + Blinn-Phong spec, shadowed
                Vec3 l; float dist; float cone = spotCone(sc.spot, p, l, dist);
                if (cone > 0) {
                    float sh = shadowAt(sc.spot, p, true); st.lit += (sh > 0.5f);
                    float att = sc.spot.intensity / (1.0f + 0.35f * dist * dist) * cone * sh;
                    float hl = 0.5f * dot(n, l) + 0.5f; hl *= hl;
                    float ndl = saturate(dot(n, l));
                    Vec3 h = normalize(l + v); float vh = saturate(dot(v, h));
                    float fres = m.f0 + (1 - m.f0) * powf(1 - vh, 5.0f);
                    float spec = blinnPhong(n, l, v, e) * (e + 8.0f) / 8.0f * m.specStrength * fres * ndl;
                    c += (alb * sc.spot.color * hl + m.specColor * sc.spot.color * spec) * att;
                }
                // cool back/rim light (no shadow): half-Lambert fill + spec + rim
                float fl = 0.5f * dot(n, sc.rimDir) + 0.5f;
                c += alb * sc.rimColor * (fl * fl * 0.35f) * ao[k];
                c += m.specColor * sc.rimColor * (blinnPhong(n, sc.rimDir, v, e) * m.specStrength * 0.8f * saturate(dot(n, sc.rimDir)));
                float rimTerm = powf(1.0f - saturate(dot(n, v)), 3.0f) * m.rim;
                c += m.rimColor * rimTerm * (0.25f + 0.75f * saturate(dot(n, sc.rimDir) * 0.5f + 0.5f)) * 0.9f * ao[k];
            }
            tl += nowSec() - a0; a0 = nowSec();

            // volumetric spotlight: raymarch camera -> hit, accumulate in-scatter, attenuate scene by transmittance
            const float sigT = 0.06f; Vec3 scat; float T = 1.0f;
            float ds = tHit / kVolSteps, off = hf(hash32((u32)k + (u32)f * 7919u));
            for (int s = 0; s < kVolSteps; s++) {
                float tt = (s + off) * ds; Vec3 X = cam.eye + rd * tt;
                float dens = 0.35f + 1.3f * noise3(X * 0.55f + Vec3(0.2f * t, 0, 0.1f * t)); dens *= 0.5f;
                Vec3 l; float dist; float cone = spotCone(sc.spot, X, l, dist);
                if (cone > 0) {
                    float sh = shadowAt(sc.spot, X, false);
                    if (sh > 0) {
                        float att = sc.spot.intensity / (1.0f + 0.35f * dist * dist);
                        scat += sc.spot.color * (att * cone * sh * dens * sigT * ds * hgPhase(dot(rd, l), 0.55f) * 4.0f) * T;
                    }
                }
                scat += Vec3(0.010f, 0.014f, 0.025f) * (dens * sigT * ds) * T;   // faint ambient haze
                T *= expf(-sigT * dens * ds);
            }
            col[k] = c * T + scat;
            tv += nowSec() - a0;
        }
        st.tLight += tl; st.tVol += tv;

        // --- dust motes: additive splats, lit by the cone, shadowed, depth-tested ---
        double t4 = nowSec();
        for (size_t i = 0; i < sc.motes.size(); i++) {
            const Scene::Mote& m = sc.motes[i]; Vec3 p = sc.motePos(m, t);
            Vec3 l; float dist; float cone = spotCone(sc.spot, p, l, dist); if (cone <= 0) continue;
            float sh = shadowAt(sc.spot, p, false); if (sh <= 0) continue;
            float px, py, d; if (!cam.project(p, W, H, px, py, d)) continue;
            int ix = (int)px, iy = (int)py; if (ix < 0 || iy < 0 || ix >= W || iy >= H) continue;
            if (d > depth[iy * W + ix] + 0.05f) continue;
            float att = sc.spot.intensity / (1.0f + 0.35f * dist * dist) * cone * sh;
            Vec3 vdir = normalize(p - cam.eye);
            float amt = att * m.bright * (0.35f + 6.0f * hgPhase(dot(vdir, l), 0.55f)) * 0.5f;
            float rad = max(0.7f, m.size / (d * cam.tx) * W * 0.5f);
            for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                int X = ix + dx, Y = iy + dy; if (X < 0 || Y < 0 || X >= W || Y >= H) continue;
                float ex = (X + 0.5f) - px, ey = (Y + 0.5f) - py;
                col[Y * W + X] += sc.spot.color * (amt * expf(-(ex * ex + ey * ey) / (2 * rad * rad * 0.45f)));
            }
        }
        st.tMotes += nowSec() - t4;
        for (int k = 0; k < W * H; k++) accum[k] += col[k] * (1.0f / kSubframes);
    }
}

// ============================ output ============================
static bool writeBMP(const char* path, const vector<unsigned char>& rgb, int W, int H) {
    FILE* f = fopen(path, "wb"); if (!f) return false;
    int rowSize = (W * 3 + 3) & ~3, imgSize = rowSize * H; unsigned char hdr[54]; memset(hdr, 0, 54);
    hdr[0] = 'B'; hdr[1] = 'M'; u32 fs = 54 + imgSize; memcpy(hdr + 2, &fs, 4);
    u32 off = 54, bi = 40, w = W, h = H, comp = 0, sz = imgSize; unsigned short pl = 1, bpp = 24;
    memcpy(hdr + 10, &off, 4); memcpy(hdr + 14, &bi, 4); memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4);
    memcpy(hdr + 26, &pl, 2); memcpy(hdr + 28, &bpp, 2); memcpy(hdr + 30, &comp, 4); memcpy(hdr + 34, &sz, 4);
    fwrite(hdr, 1, 54, f);
    vector<unsigned char> row(rowSize, 0);
    for (int y = H - 1; y >= 0; y--) {
        for (int x = 0; x < W; x++) { const unsigned char* s = &rgb[(y * W + x) * 3]; row[x * 3] = s[2]; row[x * 3 + 1] = s[1]; row[x * 3 + 2] = s[0]; }
        fwrite(&row[0], 1, rowSize, f);
    }
    fclose(f); return true;
}
static void asciiPreview(const vector<unsigned char>& rgb, int W, int H) {
    const char* ramp = " .:-=+*#%@"; const int cols = 96, rows = cols * H / W / 2;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int x0 = c * W / cols, x1 = max(x0 + 1, (c + 1) * W / cols), y0 = r * H / rows, y1 = max(y0 + 1, (r + 1) * H / rows);
            float s = 0; int n = 0;
            for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) { const unsigned char* p = &rgb[(y * W + x) * 3]; s += 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]; n++; }
            putchar(ramp[min(9, (int)(s / n / 255.0f * 9.99f))]);
        }
        putchar('\n');
    }
}

// ============================ self-checks ============================
static void selfChecks() {
    printf("=== math checks ===\n");
    printf("Blinn-Phong pow(N.H, e) vs half-vector angle:\n  exp ");
    const float ang[] = {0, 2, 5, 10, 20}; for (int i = 0; i < 5; i++) printf("%7.0fdeg", ang[i]); printf("\n");
    const float ex[] = {8, 32, 128, 512};
    for (int e = 0; e < 4; e++) { printf("  %4.0f", ex[e]); for (int i = 0; i < 5; i++) printf("%10.4f", powf(cosf(ang[i] * PI / 180), ex[e])); printf("\n"); }
    printf("Henyey-Greenstein sphere integral (should be 1):");
    const float gs[] = {0.0f, 0.55f, 0.8f};
    for (int gi = 0; gi < 3; gi++) {
        double s = 0; const int n = 20000;
        for (int i = 0; i < n; i++) { double c = -1 + 2.0 * (i + 0.5) / n; s += hgPhase((float)c, gs[gi]) * 2 * PI * (2.0 / n); }
        printf("  g=%.2f -> %.5f", gs[gi], s);
    }
    printf("\nThin-lens DoF (D=6, aperture 0.3): NDC shift in pixels, numeric vs analytic dx/tx*(1/D-1/d)*W/2:\n");
    Camera c0; c0.lookAt(Vec3(0, 1.7f, 6), Vec3(0, 1.6f, 0), 40, (float)kW / kH);
    float D = 6.0f, A = 0.3f;
    const float depths[] = {3.0f, 6.0f, 12.0f};
    for (int di = 0; di < 3; di++) {
        float d = depths[di]; Vec3 F = c0.eye + c0.fwd * d + c0.right * (0.3f * d / 6) + c0.up * (0.1f * d / 6);
        float x0, y0, w0; c0.toClip(F, x0, y0, w0); float n0 = x0 / w0, maxNum = 0, maxAna = 0;
        for (int k = 0; k < 8; k++) {
            float a = 2 * PI * k / 8, dx = A * cosf(a), dy = A * sinf(a);
            Camera cj = c0; cj.eye = c0.eye + c0.right * dx + c0.up * dy; cj.shx = dx / (D * c0.tx); cj.shy = dy / (D * c0.ty);
            float xj, yj, wj; cj.toClip(F, xj, yj, wj);
            maxNum = max(maxNum, fabsf(xj / wj - n0) * kW * 0.5f);
            maxAna = max(maxAna, fabsf(dx / c0.tx * (1 / D - 1 / d)) * kW * 0.5f);
        }
        printf("  depth %5.1f: numeric %.4f px, analytic %.4f px%s\n", d, maxNum, maxAna, d == D ? "  (in focus)" : "");
    }
}

#ifdef MAGNIFICENT_BUILD
int sfm_demo() {
#else
int main() {
#endif
    selfChecks();
    Scene sc; sc.init();
    RenderCfg cfg; cfg.t0 = 0.50f; cfg.shutter = 1.0f / 24.0f; cfg.aperture = 0.35f;
    cfg.eye = Vec3(0, 1.7f, 4.9f); cfg.target = Vec3(0.2f, 1.35f, 0); cfg.fov = 40.0f;
    cfg.focusDist = length(Vec3(0, 1.80f, 0) - cfg.eye);   // focus on the head

    printf("\n=== render %dx%d, %d subframes (jittered pixel + lens + shutter) ===\n", kW, kH, kSubframes);
    vector<Vec3> hdr; Stats st; double t0 = nowSec();
    renderAccum(sc, cfg, hdr, st);
    double total = nowSec() - t0;

    vector<unsigned char> rgb(kW * kH * 3); double lum = 0; float mx = 0;
    for (int i = 0; i < kW * kH; i++) {
        Vec3 c = tonemapACES(hdr[i] * 1.0f);
        lum += 0.2126f * hdr[i].x + 0.7152f * hdr[i].y + 0.0722f * hdr[i].z; mx = max(mx, max(hdr[i].x, max(hdr[i].y, hdr[i].z)));
        rgb[i * 3] = (unsigned char)(powf(c.x, 1 / 2.2f) * 255 + 0.5f);
        rgb[i * 3 + 1] = (unsigned char)(powf(c.y, 1 / 2.2f) * 255 + 0.5f);
        rgb[i * 3 + 2] = (unsigned char)(powf(c.z, 1 / 2.2f) * 255 + 0.5f);
    }
    printf("triangles drawn: %ld | mean HDR luminance %.4f | peak %.2f | mean AO %.3f\n", g_tris, lum / (kW * kH), mx, st.aoSum / max(1.0, (double)st.px));
    printf("time (s): total %.2f | shadow %.2f | raster %.2f | SSAO %.2f | lighting %.2f | volumetric %.2f | motes %.2f\n",
           total, st.tShadow, st.tRaster, st.tAO, st.tLight, st.tVol, st.tMotes);
    bool ok = writeBMP("sfm_render.bmp", rgb, kW, kH);
    printf("%s sfm_render.bmp\n\n", ok ? "wrote" : "FAILED to write");
    asciiPreview(rgb, kW, kH);
    return 0;
}
