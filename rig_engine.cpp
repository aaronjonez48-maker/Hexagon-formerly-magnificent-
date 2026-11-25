// Mini "Marionette-style" rigging engine - std-only, runs in Cxxdroid (C++11+)
// Features: Vec3/Quat math, joint hierarchy (FK), keyframe animation with
// slerp + easing, CCD inverse kinematics, linear-blend skinning,
// ASCII preview + OBJ export.
// Integrated with Magnificent Engine Bridge!

#include <iostream>
#include <vector>
#include <array>
#include <string>
#include <cmath>
#include <fstream>
#include <algorithm>

// Engine Bridge Header
#include "engine_bridge.h"

using namespace std;

// ---------- Math ----------
struct Vec3 {
    float x, y, z;
    Vec3(float X = 0, float Y = 0, float Z = 0) : x(X), y(Y), z(Z) {}
    Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
    Vec3 operator*(float s) const { return Vec3(x * s, y * s, z * s); }
    float dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3& o) const {
        return Vec3(y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x);
    }
    float len() const { return sqrt(dot(*this)); }
    Vec3 norm() const { float l = len(); return l > 1e-8f ? *this * (1 / l) : Vec3(); }
};

struct Quat {
    float w, x, y, z;
    Quat(float W = 1, float X = 0, float Y = 0, float Z = 0) : w(W), x(X), y(Y), z(Z) {}
    static Quat axisAngle(Vec3 a, float ang) {
        a = a.norm();
        float s = sin(ang * 0.5f);
        return Quat(cos(ang * 0.5f), a.x * s, a.y * s, a.z * s);
    }
    // rotation taking direction a onto direction b
    static Quat fromTo(Vec3 a, Vec3 b) {
        a = a.norm(); b = b.norm();
        float d = a.dot(b);
        if (d < -0.9999f) {
            Vec3 ax = Vec3(1, 0, 0).cross(a);
            if (ax.len() < 1e-3f) ax = Vec3(0, 1, 0).cross(a);
            return axisAngle(ax, 3.14159265f);
        }
        Vec3 c = a.cross(b);
        return Quat(1 + d, c.x, c.y, c.z).norm();
    }
    Quat operator*(const Quat& o) const {
        return Quat(w * o.w - x * o.x - y * o.y - z * o.z,
                    w * o.x + x * o.w + y * o.z - z * o.y,
                    w * o.y - x * o.z + y * o.w + z * o.x,
                    w * o.z + x * o.y - y * o.x + z * o.w);
    }
    Quat conj() const { return Quat(w, -x, -y, -z); }
    Quat norm() const {
        float l = sqrt(w * w + x * x + y * y + z * z);
        return l > 1e-8f ? Quat(w / l, x / l, y / l, z / l) : Quat();
    }
    Vec3 rotate(const Vec3& v) const {
        Vec3 u(x, y, z);
        Vec3 t = u.cross(v) * 2.0f;
        return v + t * w + u.cross(t);
    }
    static Quat slerp(Quat a, Quat b, float t) {
        float d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
        if (d < 0) { b = Quat(-b.w, -b.x, -b.y, -b.z); d = -d; }
        if (d > 0.9995f)
            return Quat(a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t,
                        a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t).norm();
        float th = acos(d), s = sin(th);
        float wa = sin((1 - t) * th) / s, wb = sin(t * th) / s;
        return Quat(a.w * wa + b.w * wb, a.x * wa + b.x * wb,
                    a.y * wa + b.y * wb, a.z * wa + b.z * wb).norm();
    }
};

struct Transform {  // rotation + translation (no scale)
    Quat r; Vec3 t;
    Vec3 apply(const Vec3& v) const { return r.rotate(v) + t; }
    Transform operator*(const Transform& o) const {
        Transform out; out.r = (r * o.r).norm(); out.t = t + r.rotate(o.t); return out;
    }
    Transform inverse() const {
        Transform out; out.r = r.conj(); out.t = out.r.rotate(t) * -1.0f; return out;
    }
};

// ---------- Rig ----------
struct Joint {
    string name; int parent;
    Transform rest;   // local rest transform
    Quat pose;        // animated rotation on top of rest
    Transform world, bindInv;
};

struct Rig {
    vector<Joint> j;
    int add(const string& n, int parent, Vec3 localPos) {
        Joint J; J.name = n; J.parent = parent; J.rest.t = localPos;
        j.push_back(J); return (int)j.size() - 1;   // parents must be added first
    }
    void resetPose() { for (auto& x : j) x.pose = Quat(); }
    void update() {   // forward kinematics
        for (auto& x : j) {
            Transform local; local.t = x.rest.t; local.r = (x.rest.r * x.pose).norm();
            x.world = x.parent >= 0 ? j[x.parent].world * local : local;
        }
    }
    void bind() { update(); for (auto& x : j) x.bindInv = x.world.inverse(); }

    // Cyclic Coordinate Descent IK. chain = root ... effector (joint indices)
    bool solveCCD(const vector<int>& chain, Vec3 target, int iters = 20) {
        int eff = chain.back();
        for (int it = 0; it < iters; it++) {
            for (int i = (int)chain.size() - 2; i >= 0; i--) {
                update();
                Joint& J = j[chain[i]];
                Vec3 jp = J.world.t, ep = j[eff].world.t;
                Vec3 e = ep - jp, t = target - jp;
                if (e.len() < 1e-5f || t.len() < 1e-5f) continue;
                Quat q = Quat::fromTo(e, t);
                Quat pr = J.parent >= 0 ? j[J.parent].world.r : Quat();
                Quat base = (pr * J.rest.r).norm();
                J.pose = (base.conj() * q * base * J.pose).norm();
            }
            update();
            if ((j[eff].world.t - target).len() < 0.01f) return true;
        }
        return false;
    }
};

// Global rig instance for bridge communication
static Rig g_activeRig;

// ---------- Engine Bridge Implementation ----------
void InitEngineModules() {
    g_activeRig.j.clear();
    int sh = g_activeRig.add("shoulder", -1, Vec3(0, 0, 0));
    int el = g_activeRig.add("elbow", sh, Vec3(1.5f, 0, 0));
    int wr = g_activeRig.add("wrist", el, Vec3(1.5f, 0, 0));
    g_activeRig.add("hand", wr, Vec3(1.0f, 0, 0));
    g_activeRig.bind();
    cout << "[Magnificent Engine] Rig modules initialized." << endl;
}

// UI track index -> rig joint. Value units follow the UI track:
//   0 ROOT BOUNCE (0..1) -> shoulder lift, 1 ROOT SLIDE (-2..2) -> elbow bend,
//   2 ARM ROT (deg)      -> wrist,        3 HEAD TURN (deg)     -> hand
void UpdateRigPose(int frameNumber, int activeTrackIndex, float value) {
    (void)frameNumber;
    if (activeTrackIndex < 0 || activeTrackIndex >= (int)g_activeRig.j.size()) return;
    static const float kScale[4] = {45.f, 30.f, 1.f, 1.f};   // degrees per UI unit
    float deg = value * (activeTrackIndex < 4 ? kScale[activeTrackIndex] : 1.f);
    g_activeRig.j[activeTrackIndex].pose = Quat::axisAngle(Vec3(0, 0, 1), deg * 0.0174533f);
    g_activeRig.update();
}

int EngineJointCount() { return (int)g_activeRig.j.size(); }
int EngineJointParent(int i) { return (i >= 0 && i < EngineJointCount()) ? g_activeRig.j[i].parent : -1; }
bool EngineJointPos(int i, float* xyz) {
    if (i < 0 || i >= EngineJointCount()) return false;
    const Vec3& p = g_activeRig.j[i].world.t;
    xyz[0] = p.x; xyz[1] = p.y; xyz[2] = p.z;
    return true;
}

// ---------- Animation ----------
struct Key { float t; Quat q; };
struct Track { int joint; vector<Key> keys; };
struct Clip {
    float duration; vector<Track> tracks;
    static float ease(float u) { return u * u * (3 - 2 * u); }   // smoothstep
    void apply(Rig& rig, float time) const {
        time = fmod(time, duration);
        for (auto& tr : tracks) {
            const auto& k = tr.keys;
            for (size_t i = 0; i + 1 < k.size(); i++) {
                if (time >= k[i].t && time <= k[i + 1].t) {
                    float u = (time - k[i].t) / (k[i + 1].t - k[i].t);
                    rig.j[tr.joint].pose = Quat::slerp(k[i].q, k[i + 1].q, ease(u));
                    break;
                }
            }
        }
    }
};

// ---------- Mesh + Skinning ----------
struct Skin { int j[2]; float w[2]; };
struct Mesh {
    vector<Vec3> rest;
    vector<Skin> skin;
    vector<array<int, 4>> quads;
};

Mesh makeTube(const Rig& rig, float length, int rings, int sides, float radius) {
    Mesh m;
    for (int s = 0; s <= rings; s++)
        for (int k = 0; k < sides; k++) {
            float a = k * 6.2831853f / sides;
            m.rest.push_back(Vec3(length * s / rings, cos(a) * radius, sin(a) * radius));
        }
    for (int s = 0; s < rings; s++)
        for (int k = 0; k < sides; k++)
            m.quads.push_back({s * sides + k, s * sides + (k + 1) % sides,
                               (s + 1) * sides + (k + 1) % sides, (s + 1) * sides + k});
    // auto-weights: two nearest joints, inverse-square distance
    for (auto& v : m.rest) {
        int a = -1, b = -1; float da = 1e9f, db = 1e9f;
        for (int i = 0; i < (int)rig.j.size(); i++) {
            float d = (v - rig.j[i].world.t).len();
            if (d < da) { db = da; b = a; da = d; a = i; }
            else if (d < db) { db = d; b = i; }
        }
        float wa = 1 / (da * da + 0.01f), wb = 1 / (db * db + 0.01f), s = wa + wb;
        Skin sk; sk.j[0] = a; sk.j[1] = b; sk.w[0] = wa / s; sk.w[1] = wb / s;
        m.skin.push_back(sk);
    }
    return m;
}

vector<Vec3> skinMesh(const Mesh& m, const Rig& rig) {   // linear blend skinning
    vector<Vec3> out(m.rest.size());
    for (size_t i = 0; i < m.rest.size(); i++) {
        Vec3 p;
        for (int k = 0; k < 2; k++) {
            const Joint& J = rig.j[m.skin[i].j[k]];
            p = p + (J.world * J.bindInv).apply(m.rest[i]) * m.skin[i].w[k];
        }
        out[i] = p;
    }
    return out;
}

// ---------- Output ----------
void renderASCII(const vector<Vec3>& pts, const Rig& rig) {   // side view (X,Y)
    const int W = 56, H = 18;
    vector<string> g(H, string(W, ' '));
    auto plot = [&](Vec3 p, char c) {
        int cx = (int)((p.x + 1.0f) / 6.0f * (W - 1) + 0.5f);
        int cy = (int)((3.0f - p.y) / 6.0f * (H - 1) + 0.5f);
        if (cx >= 0 && cx < W && cy >= 0 && cy < H) g[cy][cx] = c;
    };
    for (auto& p : pts) plot(p, '.');
    for (auto& J : rig.j) plot(J.world.t, 'O');
    for (auto& row : g) cout << "|" << row << "|\n";
}

void exportOBJ(const string& path, const vector<Vec3>& pts, const Mesh& m) {
    ofstream f(path);
    for (auto& p : pts) f << "v " << p.x << " " << p.y << " " << p.z << "\n";
    for (auto& q : m.quads) f << "f " << q[0] + 1 << " " << q[1] + 1 << " " << q[2] + 1 << " " << q[3] + 1 << "\n";
}

// Console preview of a skinned tube following the current pose (RIG menu).
void RigAsciiPreview() {
    Mesh m = makeTube(g_activeRig, 4.0f, 8, 8, 0.25f);
    vector<Vec3> pts = skinMesh(m, g_activeRig);
    renderASCII(pts, g_activeRig);
}
