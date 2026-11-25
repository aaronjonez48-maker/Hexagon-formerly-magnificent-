// advanced_timeline_engine.cpp  --  Magnificent engine: continuous-motion timeline module
// Single file, std-only, C++11 (Cxxdroid friendly).
//
//  1. dt / Sigma  : compensated time accumulator + discrete summation engine
//  2. Channels    : keyframeless motion fields  f(t) with analytic derivatives
//  3. Splines     : order-m Hermite splines, C^1 / C^2 / C^3 continuity
//  4. Integrators : Euler, semi-implicit Euler, velocity Verlet, RK4
//  5. Sparse timeline: sparse keys + on-demand evaluation + per-frame cache (Presto-style)
//  6. main()      : numerical self-test with logged output
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <vector>
#include <memory>
#include <functional>
#include <algorithm>
#include <map>
#include <climits>
using namespace std;

static const double PI = 3.14159265358979323846;

// ======================= Vec3 =======================
struct Vec3 {
    double x, y, z;
    Vec3(double X = 0, double Y = 0, double Z = 0) : x(X), y(Y), z(Z) {}
    Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
    Vec3 operator*(double s) const { return Vec3(x * s, y * s, z * s); }
    double dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    double len() const { return sqrt(dot(*this)); }
};

// ======================= 1. dt / Sigma accumulators =======================

// Neumaier compensated summation: error ~O(eps), independent of term count.
struct NeumaierSum {
    double s, c;
    NeumaierSum() : s(0), c(0) {}
    void add(double v) {
        double t = s + v;
        if (fabs(s) >= fabs(v)) c += (s - t) + v; else c += (v - t) + s;
        s = t;
    }
    double value() const { return s + c; }
};

// Fixed-step accumulator. Real frame dt goes in; whole substeps of size h come out.
// Simulated time is steps*h (a single multiply), so it never drifts.
class TimeAccumulator {
public:
    explicit TimeAccumulator(double h, int maxSubsteps = 16) : h_(h), maxSub_(maxSubsteps), pending_(0), steps_(0) {}
    int advance(double frameDt) {          // returns number of substeps to run
        real_.add(frameDt);
        pending_ += frameDt;
        int n = 0;
        while (pending_ >= h_ && n < maxSub_) { pending_ -= h_; steps_++; n++; }
        if (n == maxSub_) pending_ = 0;    // spiral-of-death guard
        return n;
    }
    double h() const { return h_; }
    double simTime() const { return (double)steps_ * h_; }
    double realTime() const { return real_.value(); }
    double alpha() const { return pending_ / h_; }   // sub-frame render blend factor
    int64_t steps() const { return steps_; }
private:
    double h_; int maxSub_; double pending_; int64_t steps_; NeumaierSum real_;
};

// Sigma engine: consumes uniformly spaced samples x_n and accumulates
//   v = (x[n]-x[n-2])/2h        a = (x[n]-2x[n-1]+x[n-2])/h^2     j = third difference/h^3
//   path = sum |v| h     K = sum 1/2 m |v|^2 h     A = sum |a|^2 h     J = sum |j|^2 h
class SigmaEngine {
public:
    explicit SigmaEngine(double h, double mass = 1.0) : h_(h), m_(mass), n_(0), maxV_(0), maxA_(0), maxJ_(0) {}
    void push(const Vec3& x) {
        for (int i = 3; i > 0; i--) b_[i] = b_[i - 1];
        b_[0] = x; n_++;
        if (n_ >= 3) {
            v_ = (b_[0] - b_[2]) * (0.5 / h_);
            a_ = (b_[0] - b_[1] * 2.0 + b_[2]) * (1.0 / (h_ * h_));
            path_.add(v_.len() * h_);
            kin_.add(0.5 * m_ * v_.dot(v_) * h_);
            acc_.add(a_.dot(a_) * h_);
            maxV_ = max(maxV_, v_.len()); maxA_ = max(maxA_, a_.len());
        }
        if (n_ >= 4) {
            j_ = (b_[0] - b_[1] * 3.0 + b_[2] * 3.0 - b_[3]) * (1.0 / (h_ * h_ * h_));
            jerk_.add(j_.dot(j_) * h_);
            maxJ_ = max(maxJ_, j_.len());
        }
    }
    Vec3 velocity() const { return v_; }
    Vec3 acceleration() const { return a_; }
    Vec3 jerk() const { return j_; }
    double pathLength() const { return path_.value(); }
    double kineticAction() const { return kin_.value(); }
    double accelEnergy() const { return acc_.value(); }
    double jerkEnergy() const { return jerk_.value(); }
    double maxSpeed() const { return maxV_; }
    double maxAccel() const { return maxA_; }
    double maxJerk() const { return maxJ_; }
private:
    double h_, m_; int n_; Vec3 b_[4]; Vec3 v_, a_, j_;
    NeumaierSum path_, kin_, acc_, jerk_; double maxV_, maxA_, maxJ_;
};

// Composite Simpson with compensated sum: Sigma f(t_i) w_i dt
double sigmaSimpson(const function<double(double)>& f, double a, double b, int n) {
    if (n & 1) n++;
    double h = (b - a) / n; NeumaierSum s;
    s.add(f(a)); s.add(f(b));
    for (int i = 1; i < n; i++) s.add((i & 1 ? 4.0 : 2.0) * f(a + i * h));
    return s.value() * h / 3.0;
}

// ======================= 2. Procedural channels f(t) =======================

class Channel {
public:
    virtual ~Channel() {}
    virtual double eval(double t) const = 0;
    virtual double d1(double t) const {          // 5-point stencil fallback
        const double h = 1e-3;
        return (-eval(t + 2 * h) + 8 * eval(t + h) - 8 * eval(t - h) + eval(t - 2 * h)) / (12 * h);
    }
    virtual double d2(double t) const {
        const double h = 2e-3;
        return (-eval(t + 2 * h) + 16 * eval(t + h) - 30 * eval(t) + 16 * eval(t - h) - eval(t - 2 * h)) / (12 * h * h);
    }
};
typedef shared_ptr<Channel> Ch;

// A sin(2 pi f t + phi) + c
class SineCh : public Channel {
public:
    SineCh(double A, double f, double phi, double c) : A_(A), w_(2 * PI * f), p_(phi), c_(c) {}
    double eval(double t) const { return A_ * sin(w_ * t + p_) + c_; }
    double d1(double t) const { return A_ * w_ * cos(w_ * t + p_); }
    double d2(double t) const { return -A_ * w_ * w_ * sin(w_ * t + p_); }
private: double A_, w_, p_, c_;
};

// A e^{-lambda t} + c
class ExpDecayCh : public Channel {
public:
    ExpDecayCh(double A, double lam, double c) : A_(A), l_(lam), c_(c) {}
    double eval(double t) const { return A_ * exp(-l_ * t) + c_; }
    double d1(double t) const { return -l_ * A_ * exp(-l_ * t); }
    double d2(double t) const { return l_ * l_ * A_ * exp(-l_ * t); }
private: double A_, l_, c_;
};

// Underdamped harmonic oscillator: A e^{-zeta w0 t} cos(wd t + phi), wd = w0 sqrt(1-zeta^2)
class DampedOscCh : public Channel {
public:
    DampedOscCh(double A, double zeta, double w0, double phi)
        : A_(A), s_(zeta * w0), wd_(w0 * sqrt(1 - zeta * zeta)), p_(phi) {}
    double eval(double t) const { return A_ * exp(-s_ * t) * cos(wd_ * t + p_); }
    double d1(double t) const {
        double th = wd_ * t + p_;
        return A_ * exp(-s_ * t) * (-s_ * cos(th) - wd_ * sin(th));
    }
    double d2(double t) const {
        double th = wd_ * t + p_;
        return A_ * exp(-s_ * t) * ((s_ * s_ - wd_ * wd_) * cos(th) + 2 * s_ * wd_ * sin(th));
    }
private: double A_, s_, wd_, p_;
};

// Smooth value-noise fBm (quintic fade => C^2 noise), deterministic from seed.
class NoiseCh : public Channel {
public:
    NoiseCh(double amp, double freq, int octaves, uint32_t seed) : a_(amp), f_(freq), o_(octaves), s_(seed) {}
    double eval(double t) const {
        double sum = 0, norm = 0, g = 1, fr = f_;
        for (int o = 0; o < o_; o++) { sum += g * vnoise(t * fr, s_ + o); norm += g; g *= 0.5; fr *= 2.0; }
        return a_ * sum / norm;
    }
private:
    double a_, f_; int o_; uint32_t s_;
    static double hash1(int64_t i, uint32_t seed) {
        uint32_t x = (uint32_t)i * 374761393u + seed * 668265263u;
        x = (x ^ (x >> 13)) * 1274126177u; x ^= x >> 16;
        return (x & 0xFFFFFF) / 8388607.5 - 1.0;
    }
    static double vnoise(double t, uint32_t seed) {
        double fl = floor(t); int64_t i = (int64_t)fl; double f = t - fl;
        double u = f * f * f * (f * (f * 6 - 15) + 10);
        return hash1(i, seed) * (1 - u) + hash1(i + 1, seed) * u;
    }
};

class SumCh : public Channel {
public:
    SumCh(Ch a, Ch b) : a_(a), b_(b) {}
    double eval(double t) const { return a_->eval(t) + b_->eval(t); }
    double d1(double t) const { return a_->d1(t) + b_->d1(t); }
    double d2(double t) const { return a_->d2(t) + b_->d2(t); }
private: Ch a_, b_;
};

class ProductCh : public Channel {   // product rule, analytic
public:
    ProductCh(Ch a, Ch b) : a_(a), b_(b) {}
    double eval(double t) const { return a_->eval(t) * b_->eval(t); }
    double d1(double t) const { return a_->d1(t) * b_->eval(t) + a_->eval(t) * b_->d1(t); }
    double d2(double t) const {
        return a_->d2(t) * b_->eval(t) + 2 * a_->d1(t) * b_->d1(t) + a_->eval(t) * b_->d2(t);
    }
private: Ch a_, b_;
};

Ch sine(double A, double f, double phi = 0, double c = 0) { return Ch(new SineCh(A, f, phi, c)); }
Ch expDecay(double A, double lam, double c = 0) { return Ch(new ExpDecayCh(A, lam, c)); }
Ch dampedOsc(double A, double zeta, double w0, double phi = 0) { return Ch(new DampedOscCh(A, zeta, w0, phi)); }
Ch noise(double amp, double freq, int oct = 3, uint32_t seed = 1) { return Ch(new NoiseCh(amp, freq, oct, seed)); }
Ch add(Ch a, Ch b) { return Ch(new SumCh(a, b)); }
Ch mul(Ch a, Ch b) { return Ch(new ProductCh(a, b)); }

struct Field3 {   // a bone-space motion field: one channel per axis
    Ch c[3];
    Vec3 eval(double t) const { return Vec3(c[0]->eval(t), c[1]->eval(t), c[2]->eval(t)); }
    Vec3 d1(double t) const { return Vec3(c[0]->d1(t), c[1]->d1(t), c[2]->d1(t)); }
};

// ======================= 3. Higher-order Hermite splines =======================

static double fall(int j, int k) {   // falling factorial j(j-1)...(j-k+1); 0 if k>j
    double r = 1; for (int i = 0; i < k; i++) r *= (j - i); return r;
}

struct Poly {
    vector<double> c;
    double eval(double u, int k = 0) const {   // k-th derivative by Horner
        double r = 0;
        for (int j = (int)c.size() - 1; j >= k; j--) r = r * u + c[j] * fall(j, k);
        return r;
    }
};

// Degree 2m+1 polynomial on u in [0,1] matching derivatives 0..m at both ends.
static Poly buildHermite(const double* D0, const double* D1, int m) {
    int n = 2 * m + 1, N = m + 1;
    Poly p; p.c.assign(n + 1, 0.0);
    double fact = 1;
    for (int k = 0; k <= m; k++) { if (k) fact *= k; p.c[k] = D0[k] / fact; }
    double A[4][5];
    for (int k = 0; k <= m; k++) {
        double rhs = D1[k];
        for (int j = 0; j <= m; j++) rhs -= p.c[j] * fall(j, k);
        for (int jj = 0; jj < N; jj++) A[k][jj] = fall(m + 1 + jj, k);
        A[k][N] = rhs;
    }
    for (int i = 0; i < N; i++) {              // Gaussian elimination, partial pivoting
        int piv = i;
        for (int r = i + 1; r < N; r++) if (fabs(A[r][i]) > fabs(A[piv][i])) piv = r;
        for (int c = 0; c <= N; c++) swap(A[i][c], A[piv][c]);
        for (int r = i + 1; r < N; r++) {
            double f = A[r][i] / A[i][i];
            for (int c = i; c <= N; c++) A[r][c] -= f * A[i][c];
        }
    }
    double sol[4];
    for (int i = N - 1; i >= 0; i--) {
        double s = A[i][N];
        for (int c = i + 1; c < N; c++) s -= A[i][c] * sol[c];
        sol[i] = s / A[i][i];
    }
    for (int jj = 0; jj < N; jj++) p.c[m + 1 + jj] = sol[jj];
    return p;
}

// order 1: cubic Hermite (C1, Catmull-Rom velocities)
// order 2: quintic Hermite (C2, knot vel+acc from natural cubic spline)
// order 3: septic Hermite  (C3, + knot jerk from differenced accelerations)
class HermiteSpline {
public:
    void build(const vector<double>& t, const vector<double>& y, int order) {
        t_ = t; order_ = order;
        int n = (int)t.size();
        vector<double> h(n - 1), vel(n), acc(n, 0.0), jerk(n, 0.0);
        for (int i = 0; i + 1 < n; i++) h[i] = t[i + 1] - t[i];

        vector<double> M(n, 0.0);                 // natural cubic spline second derivatives
        if (n >= 3) {
            vector<double> cp(n, 0.0), dp(n, 0.0);
            for (int i = 1; i <= n - 2; i++) {
                double a = h[i - 1], b = 2 * (h[i - 1] + h[i]), c = h[i];
                double d = 6 * ((y[i + 1] - y[i]) / h[i] - (y[i] - y[i - 1]) / h[i - 1]);
                double pc = i > 1 ? cp[i - 1] : 0, pd = i > 1 ? dp[i - 1] : 0;
                double den = b - a * pc;
                cp[i] = c / den; dp[i] = (d - a * pd) / den;
            }
            M[n - 2] = dp[n - 2];
            for (int i = n - 3; i >= 1; i--) M[i] = dp[i] - cp[i] * M[i + 1];
        }

        if (order == 1) {
            for (int i = 0; i < n; i++) {
                if (i == 0) vel[i] = (y[1] - y[0]) / h[0];
                else if (i == n - 1) vel[i] = (y[n - 1] - y[n - 2]) / h[n - 2];
                else vel[i] = (y[i + 1] - y[i - 1]) / (t[i + 1] - t[i - 1]);
            }
        } else {
            for (int i = 0; i < n - 1; i++) vel[i] = (y[i + 1] - y[i]) / h[i] - h[i] * (2 * M[i] + M[i + 1]) / 6.0;
            vel[n - 1] = (y[n - 1] - y[n - 2]) / h[n - 2] + h[n - 2] * (M[n - 2] + 2 * M[n - 1]) / 6.0;
            for (int i = 0; i < n; i++) acc[i] = M[i];
            for (int i = 0; i < n; i++) {
                if (i == 0) jerk[i] = (M[1] - M[0]) / h[0];
                else if (i == n - 1) jerk[i] = (M[n - 1] - M[n - 2]) / h[n - 2];
                else jerk[i] = (M[i + 1] - M[i - 1]) / (t[i + 1] - t[i - 1]);
            }
        }

        seg_.clear(); T_.clear();
        for (int i = 0; i + 1 < n; i++) {
            double T = h[i], D0[4], D1[4];
            double d0[4] = {y[i], vel[i], acc[i], jerk[i]}, d1[4] = {y[i + 1], vel[i + 1], acc[i + 1], jerk[i + 1]};
            double s = 1;
            for (int k = 0; k <= order; k++) { D0[k] = d0[k] * s; D1[k] = d1[k] * s; s *= T; }
            seg_.push_back(buildHermite(D0, D1, order)); T_.push_back(T);
        }
    }
    double eval(double t, int deriv = 0) const {
        int i = locate(t);
        double u = (t - t_[i]) / T_[i];
        return seg_[i].eval(u, deriv) / pow(T_[i], deriv);
    }
    double jump(int knot, int deriv) const {   // right-limit minus left-limit at interior knot
        double L = seg_[knot - 1].eval(1.0, deriv) / pow(T_[knot - 1], deriv);
        double R = seg_[knot].eval(0.0, deriv) / pow(T_[knot], deriv);
        return R - L;
    }
    int knots() const { return (int)t_.size(); }
    double tStart() const { return t_.front(); }
    double tEnd() const { return t_.back(); }
private:
    int locate(double t) const {
        int i = (int)(upper_bound(t_.begin(), t_.end(), t) - t_.begin()) - 1;
        return max(0, min(i, (int)seg_.size() - 1));
    }
    vector<double> t_, T_; vector<Poly> seg_; int order_;
};

// ======================= 4. State integration =======================

struct BoneState { Vec3 x, v; };
typedef function<Vec3(double, const Vec3&, const Vec3&)> AccelFn;   // a = f(t, x, v)

BoneState stepEuler(const BoneState& s, double t, double h, const AccelFn& f) {
    BoneState r; Vec3 a = f(t, s.x, s.v);
    r.x = s.x + s.v * h; r.v = s.v + a * h; return r;
}
BoneState stepSemiImplicit(const BoneState& s, double t, double h, const AccelFn& f) {
    BoneState r; Vec3 a = f(t, s.x, s.v);
    r.v = s.v + a * h; r.x = s.x + r.v * h; return r;
}
// Velocity Verlet (symplectic for conservative forces); aCache carries a(t_n) between calls.
BoneState stepVerlet(const BoneState& s, Vec3& aCache, double t, double h, const AccelFn& f) {
    BoneState r;
    r.x = s.x + s.v * h + aCache * (0.5 * h * h);
    Vec3 vPred = s.v + aCache * h;                 // predictor for velocity-dependent forces
    Vec3 a1 = f(t + h, r.x, vPred);
    r.v = s.v + (aCache + a1) * (0.5 * h);
    aCache = a1; return r;
}
BoneState stepRK4(const BoneState& s, double t, double h, const AccelFn& f) {
    Vec3 k1x = s.v,                       k1v = f(t, s.x, s.v);
    Vec3 k2x = s.v + k1v * (h / 2),       k2v = f(t + h / 2, s.x + k1x * (h / 2), s.v + k1v * (h / 2));
    Vec3 k3x = s.v + k2v * (h / 2),       k3v = f(t + h / 2, s.x + k2x * (h / 2), s.v + k2v * (h / 2));
    Vec3 k4x = s.v + k3v * h,             k4v = f(t + h, s.x + k3x * h, s.v + k3v * h);
    BoneState r;
    r.x = s.x + (k1x + k2x * 2.0 + k3x * 2.0 + k4x) * (h / 6.0);
    r.v = s.v + (k1v + k2v * 2.0 + k3v * 2.0 + k4v) * (h / 6.0);
    return r;
}

// ======================= 5. Sparse timeline: sparse keys + evaluation cache =======================
// Like Presto's timeline, this is an evaluation engine, not just a scrubber:
//   * keys are SPARSE - a channel stores only the frames the animator touched
//   * a frame is evaluated ON DEMAND (lazily) and the whole pose is CACHED per frame
//   * editing a key invalidates ONLY the frames that key can influence (local spline support)
//   * idle time refills the cache outward from the playhead, so the green "cached" bar sweeps back in
// Interpolation between keys is monotone cubic Hermite (Fritsch-Carlson), so there is no overshoot;
// first/last keys ease in/out and hold outside the keyed range.  Each key can also be LINEAR or STEP.
enum KeyMode { KEY_SPLINE = 0, KEY_LINEAR = 1, KEY_STEP = 2 };

typedef map<int, float> KeyMap;
typedef map<int, uint8_t> ModeMap;           // only non-spline keys are stored here

static int keyModeOf(const ModeMap* m, int f) {
    if (!m) return KEY_SPLINE;
    ModeMap::const_iterator it = m->find(f);
    return it == m->end() ? (int)KEY_SPLINE : (int)it->second;
}
// Pure function (thread safe, used by the viewport AND by exports): value of one channel at a frame.
static double sparseEval(const KeyMap& K, const ModeMap* modes, int f, double rest) {
    if (K.empty()) return rest;
    KeyMap::const_iterator hi = K.lower_bound(f);              // first key >= f
    if (hi != K.end() && hi->first == f) return hi->second;
    if (hi == K.begin()) return hi->second;                    // before the first key: hold
    if (hi == K.end()) return K.rbegin()->second;              // after the last key: hold
    KeyMap::const_iterator lo = hi; --lo;
    double f1 = lo->first, f2 = hi->first, v1 = lo->second, v2 = hi->second, h = f2 - f1;
    int mode = keyModeOf(modes, lo->first);                    // a segment takes the mode of its left key
    if (mode == KEY_STEP) return v1;
    double t = (f - f1) / h;
    if (mode == KEY_LINEAR) return v1 + (v2 - v1) * t;
    // monotone tangents (0 at the ends and at local extrema)
    auto slope = [&](KeyMap::const_iterator a, KeyMap::const_iterator b, KeyMap::const_iterator c, bool hasA, bool hasC) -> double {
        if (!hasA || !hasC) return 0.0;
        double h0 = b->first - a->first, h1 = c->first - b->first;
        double d0 = (b->second - a->second) / h0, d1 = (c->second - b->second) / h1;
        if (d0 * d1 <= 0.0) return 0.0;
        double w1 = 2 * h1 + h0, w2 = h1 + 2 * h0;
        return (w1 + w2) / (w1 / d0 + w2 / d1);
    };
    bool hasPrev = lo != K.begin(); KeyMap::const_iterator pv = lo; if (hasPrev) --pv;
    KeyMap::const_iterator nx = hi; ++nx; bool hasNext = nx != K.end();
    double m1 = slope(pv, lo, hi, hasPrev, true), m2 = slope(lo, hi, nx, true, hasNext);
    double t2 = t * t, t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * v1 + (t3 - 2 * t2 + t) * h * m1 + (-2 * t3 + 3 * t2) * v2 + (t3 - t2) * h * m2;
}

// Frames whose value can change when the key at frame f is added / removed / moved / re-valued:
// the spline segments from two keys before f to two keys after f.  Fewer than two neighbours => open ended.
static void keyInfluence(const KeyMap& K, int f, int& lo, int& hi) {
    const int NEG = INT_MIN / 2, POS = INT_MAX / 2;
    lo = NEG; hi = POS;
    KeyMap::const_iterator p = K.lower_bound(f); int n = 0;
    while (p != K.begin() && n < 2) { --p; ++n; }
    if (n == 2) lo = p->first;
    KeyMap::const_iterator s = K.upper_bound(f); n = 0;
    while (s != K.end() && n < 2) { ++n; if (n == 2) hi = s->first; ++s; }
}

class SparseTimeline {
public:
    struct Stats { long hits, misses, filled, invalidated; Stats() : hits(0), misses(0), filled(0), invalidated(0) {} } stats;

    // Channels reference the owner's key maps (no copies, so the UI and the engine never disagree).
    int bind(const KeyMap* keys, const ModeMap* modes, double rest) {
        keys_.push_back(keys); modes_.push_back(modes); rest_.push_back(rest); invalidateAll(); return (int)keys_.size() - 1;
    }
    int channels() const { return (int)keys_.size(); }
    void setRange(int a, int b) {                      // play range: cache keeps the frames that stay inside it
        if (b < a) b = a;
        if (a == a_ && b == b_ && !ok_.empty()) return;
        int n = b - a + 1; vector<uint8_t> ok(n, 0); vector<vector<float> > data(n);
        for (int f = max(a, a_); f <= min(b, b_) && !ok_.empty(); f++)
            if (ok_[f - a_]) { ok[f - a] = 1; data[f - a].swap(data_[f - a_]); }
        a_ = a; b_ = b; ok_.swap(ok); data_.swap(data);
    }
    int rangeStart() const { return a_; }
    int rangeEnd() const { return b_; }
    int rangeCount() const { return (int)ok_.size(); }
    bool cached(int f) const { return f >= a_ && f <= b_ && ok_[f - a_]; }
    int cachedCount() const { int n = 0; for (size_t i = 0; i < ok_.size(); i++) n += ok_[i]; return n; }

    // --- edits: call willEdit BEFORE changing the key at f, didEdit right AFTER (also for mode changes)
    void willEdit(int c, int f) { int lo, hi; keyInfluence(*keys_[c], f, lo, hi); pendLo_ = lo; pendHi_ = hi; }
    void didEdit(int c, int f) {
        int lo, hi; keyInfluence(*keys_[c], f, lo, hi);
        invalidate(min(lo, pendLo_), max(hi, pendHi_));
    }
    void keysCleared() { invalidateAll(); }
    void setRest(int c, double v) {                    // value of a channel that has no keys
        if (rest_[c] == v) return;
        rest_[c] = v;
        if (keys_[c]->empty()) invalidateAll();
    }
    void invalidate(int lo, int hi) {
        lo = max(lo, a_); hi = min(hi, b_);
        for (int f = lo; f <= hi; f++) if (ok_[f - a_]) { ok_[f - a_] = 0; data_[f - a_].clear(); stats.invalidated++; }
    }
    void invalidateAll() { invalidate(a_, b_); }

    double evalChannel(int c, int f) const { return sparseEval(*keys_[c], modes_[c], f, rest_[c]); }
    // Whole pose at a frame: cache hit, or evaluate now (lazy) and store.  Frames outside the range are not cached.
    const vector<float>& frame(int f) {
        if (cached(f)) { stats.hits++; return data_[f - a_]; }
        stats.misses++;
        vector<float>& dst = (f >= a_ && f <= b_) ? data_[f - a_] : scratch_;
        evaluate(f, dst);
        if (f >= a_ && f <= b_) ok_[f - a_] = 1;
        return dst;
    }
    // Background refill: evaluates up to maxFrames missing frames, nearest to 'center' first, ahead of it before behind.
    int fill(int maxFrames, int center) {
        int done = 0, n = (int)ok_.size();
        for (int k = 0; k < n && done < maxFrames; k++) {
            int f1 = center + k, f2 = center - k;
            if (f1 >= a_ && f1 <= b_ && !ok_[f1 - a_]) { evaluate(f1, data_[f1 - a_]); ok_[f1 - a_] = 1; done++; stats.filled++; }
            if (k > 0 && done < maxFrames && f2 >= a_ && f2 <= b_ && !ok_[f2 - a_]) { evaluate(f2, data_[f2 - a_]); ok_[f2 - a_] = 1; done++; stats.filled++; }
        }
        return done;
    }
    const vector<float>* peek(int f) const { return cached(f) ? &data_[f - a_] : nullptr; }

private:
    void evaluate(int f, vector<float>& out) const {
        out.resize(keys_.size());
        for (size_t c = 0; c < keys_.size(); c++) out[c] = (float)sparseEval(*keys_[c], modes_[c], f, rest_[c]);
    }
    vector<const KeyMap*> keys_; vector<const ModeMap*> modes_; vector<double> rest_;
    int a_ = 0, b_ = -1;
    vector<uint8_t> ok_; vector<vector<float> > data_; vector<float> scratch_;
    int pendLo_ = 0, pendHi_ = 0;
};

// ======================= 6. Tests =======================

static void testAccumulators() {
    printf("\n=== [1] dt accumulator & Sigma summation ===\n");
    float naiveF = 0; double naiveD = 0; NeumaierSum comp;
    for (int i = 0; i < 1000000; i++) { naiveF += 0.001f; naiveD += 0.001; comp.add(0.001); }
    printf("sum of 1e6 * 0.001 (exact = 1000):\n");
    printf("  float naive   : %.6f  (err %.3e)\n", naiveF, naiveF - 1000.0);
    printf("  double naive  : %.10f  (err %.3e)\n", naiveD, naiveD - 1000.0);
    printf("  Neumaier      : %.10f  (err %.3e)\n", comp.value(), comp.value() - 1000.0);

    TimeAccumulator acc(1.0 / 240.0);
    uint32_t rng = 12345; int frames = 0; int totalSub = 0;
    while (acc.realTime() < 1.0) {
        rng = rng * 1664525u + 1013904223u;
        double dt = (1.0 / 60.0) * (0.7 + 0.6 * ((rng >> 8) & 0xFFFF) / 65535.0);   // jittery frames
        totalSub += acc.advance(dt); frames++;
    }
    printf("jittery frames: %d, substeps: %d, realTime=%.6f simTime=%.6f lag=%.6f alpha=%.3f\n",
           frames, totalSub, acc.realTime(), acc.simTime(), acc.realTime() - acc.simTime(), acc.alpha());

    printf("Simpson Sigma of v^2 for x=0.5 sin(2pi*1.5t) over 2 periods:\n");
    Ch s = sine(0.5, 1.5);
    double num = sigmaSimpson([&](double t) { double v = s->d1(t); return v * v; }, 0, 4.0 / 3.0, 200);
    double w = 2 * PI * 1.5, ex = 0.25 * w * w * (2.0 / 3.0);   // A^2 w^2 * T_total/2
    printf("  numeric=%.10f exact=%.10f err=%.3e\n", num, ex, num - ex);
}

static void testChannels() {
    printf("\n=== [2] Procedural channels (keyframeless f(t)) ===\n");
    Ch env = expDecay(1.0, 0.8);
    Ch ch = add(add(mul(env, sine(0.6, 1.5, 0.3)), dampedOsc(0.4, 0.15, 2 * PI * 2.0)), noise(0.05, 3.0, 3, 7));
    printf("%6s %9s %9s %9s %9s %10s\n", "t", "f(t)", "f'(t)", "f'num", "f''(t)", "|f'-f'num|");
    double maxErr = 0;
    for (double t = 0; t <= 2.0001; t += 0.25) {
        double an = ch->d1(t), nu;
        { const double h = 1e-5; nu = (ch->eval(t + h) - ch->eval(t - h)) / (2 * h); }
        double e = fabs(an - nu); maxErr = max(maxErr, e);
        printf("%6.2f %9.5f %9.5f %9.5f %9.4f %10.2e\n", t, ch->eval(t), an, nu, ch->d2(t), e);
    }
    printf("max |analytic d1 - central difference| = %.3e (noise term uses stencil)\n", maxErr);
}

static void testSplines() {
    printf("\n=== [3] Derivative continuity at knots (jump = right - left limit) ===\n");
    double tk[] = {0, 0.4, 1.1, 1.5, 2.4, 3.0};
    double yk[] = {0, 1.0, 0.2, 0.9, -0.3, 0.4};
    vector<double> t(tk, tk + 6), y(yk, yk + 6);
    HermiteSpline sp[4];
    for (int o = 1; o <= 3; o++) sp[o].build(t, y, o);

    printf("max |jump| over interior knots:\n%-22s %9s %9s %9s %9s %9s\n", "spline", "pos", "vel", "accel", "jerk", "snap");
    const char* names[] = {"", "cubic Hermite (C1)", "quintic Hermite (C2)", "septic Hermite (C3)"};
    for (int o = 1; o <= 3; o++) {
        double mj[5] = {0, 0, 0, 0, 0};
        for (int i = 1; i < sp[o].knots() - 1; i++)
            for (int d = 0; d <= 4; d++) mj[d] = max(mj[d], fabs(sp[o].jump(i, d)));
        printf("%-22s %9.2e %9.2e %9.2e %9.2e %9.2e\n", names[o], mj[0], mj[1], mj[2], mj[3], mj[4]);
    }

    printf("\nseptic (C3) trajectory:\n%6s %9s %9s %9s %9s\n", "t", "x", "v", "a", "j");
    for (double tt = 0; tt <= 3.0001; tt += 0.25)
        printf("%6.2f %9.4f %9.4f %9.4f %9.3f\n", tt, sp[3].eval(tt), sp[3].eval(tt, 1), sp[3].eval(tt, 2), sp[3].eval(tt, 3));

    printf("\nSigma smoothness metrics sampled at 480 Hz (lower = smoother):\n");
    printf("%-22s %9s %11s %12s %12s\n", "spline", "path", "sum|a|^2 dt", "sum|j|^2 dt", "max|j|");
    for (int o = 1; o <= 3; o++) {
        SigmaEngine se(1.0 / 480.0);
        for (int i = 0; i <= 3 * 480; i++) se.push(Vec3(sp[o].eval(i / 480.0), 0, 0));
        printf("%-22s %9.4f %11.2f %12.1f %12.1f\n", names[o], se.pathLength(), se.accelEnergy(), se.jerkEnergy(), se.maxJerk());
    }
}

static void testIntegrators() {
    printf("\n=== [4] Integrators on x'' = -w^2 x  (w=2pi, 10 periods, h=1/100) ===\n");
    const double w = 2 * PI, h = 0.01; const int N = 1000;
    AccelFn f = [=](double, const Vec3& x, const Vec3&) { return x * (-w * w); };
    double E0 = 0.5 * w * w;
    const char* nm[] = {"explicit Euler", "semi-implicit", "velocity Verlet", "RK4"};
    BoneState s[4]; Vec3 aC = f(0, Vec3(1, 0, 0), Vec3());
    for (int i = 0; i < 4; i++) { s[i].x = Vec3(1, 0, 0); s[i].v = Vec3(); }
    for (int n = 0; n < N; n++) {
        double t = n * h;
        s[0] = stepEuler(s[0], t, h, f);
        s[1] = stepSemiImplicit(s[1], t, h, f);
        s[2] = stepVerlet(s[2], aC, t, h, f);
        s[3] = stepRK4(s[3], t, h, f);
    }
    double xe = cos(w * N * h);
    printf("%-16s %12s %12s %10s\n", "method", "x(T)", "|x-exact|", "E/E0");
    for (int i = 0; i < 4; i++) {
        double E = 0.5 * s[i].v.dot(s[i].v) + 0.5 * w * w * s[i].x.dot(s[i].x);
        printf("%-16s %12.6f %12.3e %10.5f\n", nm[i], s[i].x.x, fabs(s[i].x.x - xe), E / E0);
    }
}

static void testBoneSim() {
    printf("\n=== [5] Bone driven by procedural field, RK4 + sub-frame accumulator ===\n");
    Field3 target;
    target.c[0] = sine(0.5, 1.5);
    target.c[1] = add(dampedOsc(0.8, 0.15, 2 * PI * 2.0), noise(0.05, 3.0, 3, 7));
    target.c[2] = sine(0.3, 0.75, PI / 2);

    const double k = 400.0, zeta = 0.7, c = 2 * zeta * sqrt(k);
    AccelFn spring = [&](double t, const Vec3& x, const Vec3& v) { return (target.eval(t) - x) * k - v * c; };

    const double H = 1.0 / 240.0;
    TimeAccumulator acc(H);
    SigmaEngine sigma(H, 1.0);
    BoneState cur, prev; cur.x = target.eval(0); prev = cur;
    uint32_t rng = 777; int frame = 0;

    printf("%5s %7s %6s | %8s %8s %8s | %7s %7s\n", "frame", "t_sim", "alpha", "x", "y", "z", "|v|", "|vtgt|");
    while (acc.realTime() < 1.0) {
        rng = rng * 1664525u + 1013904223u;
        double dt = (1.0 / 60.0) * (0.7 + 0.6 * ((rng >> 8) & 0xFFFF) / 65535.0);
        int n = acc.advance(dt);
        for (int i = 0; i < n; i++) {
            double t = (acc.steps() - n + i) * H;
            prev = cur; cur = stepRK4(cur, t, H, spring);
            sigma.push(cur.x);
        }
        double a = acc.alpha();
        Vec3 render = prev.x * (1 - a) + cur.x * a;     // sub-frame interpolated pose
        if (frame % 6 == 0)
            printf("%5d %7.4f %6.3f | %8.4f %8.4f %8.4f | %7.3f %7.3f\n", frame, acc.simTime(), a,
                   render.x, render.y, render.z, cur.v.len(), target.d1(acc.simTime()).len());
        frame++;
    }
    printf("Sigma totals: path=%.4f  K-action=%.4f  sum|a|^2dt=%.1f  sum|j|^2dt=%.0f\n",
           sigma.pathLength(), sigma.kineticAction(), sigma.accelEnergy(), sigma.jerkEnergy());
    printf("peaks: |v|=%.3f |a|=%.2f |j|=%.1f\n", sigma.maxSpeed(), sigma.maxAccel(), sigma.maxJerk());
}

static void testSparseTimeline() {
    printf("\n--- sparse timeline: sparse keys + evaluation cache ---\n");
    KeyMap k0, k1; ModeMap m0, m1;
    SparseTimeline tl;
    tl.bind(&k0, &m0, 0.0); tl.bind(&k1, &m1, 5.0);
    tl.setRange(1300, 1360);
    // 1. interpolation sanity: hits the keys, holds outside, no overshoot on a monotone run
    k0[1310] = 0; k0[1320] = 1; k0[1330] = 1.2f; k0[1350] = 4;
    bool mono = true; double prev = -1e9;
    for (int f = 1310; f <= 1350; f++) { double v = tl.evalChannel(0, f); if (v < prev - 1e-9) mono = false; prev = v; }
    double lo1 = tl.evalChannel(0, 1300), hi1 = tl.evalChannel(0, 1360);
    printf("  keys exact: %s | hold before/after: %.2f / %.2f | monotone, no overshoot: %s\n",
           (tl.evalChannel(0, 1320) == 1.0 && tl.evalChannel(0, 1350) == 4.0) ? "ok" : "FAIL", lo1, hi1, mono ? "ok" : "FAIL");
    // 2. lazy evaluation + caching
    tl.invalidateAll(); tl.stats = SparseTimeline::Stats();
    tl.frame(1325); tl.frame(1325); tl.frame(1326);
    printf("  lazy eval: misses=%ld hits=%ld cached=%d/%d\n", tl.stats.misses, tl.stats.hits, tl.cachedCount(), tl.rangeCount());
    // 3. background fill order + completion
    int first = tl.fill(4, 1340); bool ahead = tl.cached(1340) && tl.cached(1341) && tl.cached(1339);
    while (tl.fill(8, 1340)) {}
    printf("  fill: first batch %d frames (near playhead first: %s), complete: %d/%d\n", first, ahead ? "yes" : "NO", tl.cachedCount(), tl.rangeCount());
    // 4. invalidation is local, and NEVER leaves a stale frame (brute-force check against fresh evaluation)
    tl.willEdit(0, 1330); k0[1330] = 9.0f; tl.didEdit(0, 1330);
    printf("  edit key@1330: cache now %d/%d (only the keys' spline neighbourhood was dropped)\n", tl.cachedCount(), tl.rangeCount());
    unsigned seed = 12345; auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) & 0xFFFF; };
    long stale = 0, checks = 0;
    for (int it = 0; it < 4000; it++) {
        int c = rnd() % 2, f = 1296 + (int)(rnd() % 70), op = rnd() % 5; KeyMap& K = c ? k1 : k0; ModeMap& M = c ? m1 : m0;
        if (op <= 1) { tl.willEdit(c, f); K[f] = (float)((int)(rnd() % 200) - 100) / 10.f; tl.didEdit(c, f); }
        else if (op == 2) { tl.willEdit(c, f); K.erase(f); M.erase(f); tl.didEdit(c, f); }
        else if (op == 3 && K.count(f)) { tl.willEdit(c, f); M[f] = (uint8_t)(rnd() % 3); tl.didEdit(c, f); }
        else if (op == 4 && K.count(f)) {                      // move a key
            int g = 1296 + (int)(rnd() % 70); if (K.count(g)) continue;
            float v = K[f]; tl.willEdit(c, f); K.erase(f); tl.didEdit(c, f);
            tl.willEdit(c, g); K[g] = v; tl.didEdit(c, g);
        }
        if (it % 3 == 0) tl.fill(1 + rnd() % 9, 1300 + rnd() % 61);
        for (int ff = 1300; ff <= 1360; ff++) {
            const vector<float>* p = tl.peek(ff); if (!p) continue;
            checks++;
            for (int cc = 0; cc < 2; cc++) if ((*p)[cc] != (float)tl.evalChannel(cc, ff)) stale++;
        }
    }
    printf("  random edits (add/erase/mode/move) x4000: %ld cached-frame checks, stale frames = %ld  %s\n", checks, stale, stale == 0 ? "OK" : "FAIL");
    // 5. range change keeps cached frames that stay inside
    while (tl.fill(16, 1330)) {}
    tl.setRange(1320, 1380);
    printf("  range 1300-1360 -> 1320-1380: kept %d cached frames (expected 41)\n", tl.cachedCount());
}

#ifdef MAGNIFICENT_BUILD
int timeline_demo() {
#else
int main() {
#endif
    printf("advanced_timeline_engine -- continuous motion field self-test\n");
    testAccumulators();
    testChannels();
    testSplines();
    testIntegrators();
    testBoneSim();
    testSparseTimeline();
    printf("\nAll tests finished.\n");
    return 0;
}
