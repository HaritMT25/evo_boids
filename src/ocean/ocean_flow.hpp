#pragma once
// ----------------------------------------------------------------------------
// ocean_flow — the E1 flow's evaluation cache (OceanWorld::flowAt while
// flowE1()). Ocean-only: CurrentField (src/core/current.hpp) and Classic are
// untouched; the cache is derived scratch (tools/ocean_fingerprint.hpp does
// not hash it: everything in it is rebuilt from hashed state — the configs,
// the world size, the CurrentField's vortices, the frame — before it is read).
//
// The eddy current: OceanWorld copies the vortices once at init through
// CurrentField::vortexState() (Ocean never calls restoreVortexState), with the
// strength / core it built the field with, and recomputes the centres for each
// advanced frame with CurrentField::advance's own expressions. Before the first
// advance after init CurrentField's centres are all 0 (init only sizes them),
// and so are these. The velocity is CurrentField::velocityAt's sum, in the same
// vortex order with the same prefactor (sign · strength / core · e^{1/2}) and
// raw p − centre differences (no minimum image: the current is not periodic),
// but e^{−u}, u = (dx² + dy²) / (2 core²), comes from a table (radial below):
//   g(u) = lerp(E, u) − E[2048] for finite 0 <= u < 16, else 0 (the vortex
//   adds nothing), E[j] = e^{−j/128} (2048 intervals, the portable expm1f).
// Subtracting the last node makes g continuous at the cutoff; the field stays
// (−dy, dx)·g(r² / 2c²), a radial multiplier: divergence-free for any g.
// Error per vortex <= ~8.7e-6 · strength units/s (interpolation 7.63e-6·S at
// u = 1/2, the cutoff / endpoint shift 1.05e-6·S at u = 16; tests/ocean_tests.cpp
// flow_tables).
//
// The waves: per train the vertical amplitude V(d) = −A e^{−kd} B(d) (u_d =
// V sin θ) is a C¹ cubic Hermite spline in depth on four pieces whose ends are
// the taper's joins, [0, b], [b, D/2], [D/2, D − b], [D − b, D] (B is only C¹
// at b and D − b: a knot there keeps each piece smooth), >= 64 uniform cells
// each (128 in the two band pieces), more while k_max · cell > 0.04; over
// kDepthCellsMax cells in all the
// exact formulas stay. Node values are the exact float formulas'; node slopes
// are V' = k·H, H = A e^{−kd} (B − B'/k) the horizontal amplitude — so the
// spline's own derivative V' gives the horizontal flow. V and V' are exactly
// 0 at d = 0 and d = D (and outside [0, D], as the formulas are).
//
// Horizontally, θ = K·p − phase + φ0 splits as θx + θy with θx = 2π m x / W
// and θy = 2π n y / H + (φ0 − phase). Static tables hold cos / sin of
// 2π m s / 512 (s = x · 512 / W, the cell coordinate: one period of the
// world) and of 2π n s / 256 for the trains with n != 0 — they depend on the
// integer modes only, built once with integer modular node phases. The y
// ones are rotated by (φ0 − phase) whenever a phase moves, always from the
// static base (never from the previous step's: no drift); train 0 (n = 0)
// needs two scalars. With P = cos θ = cx·cy − sx·sy and P's own derivatives
// (the interpolants'), T = K / |K|²,
//   u_x = (T_x V') P,  u_y = (T_y V') P,  u_d = (−V)(T_x ∂x P + T_y ∂y P)
// — the curl of (T_y V P, −T_x V P, 0): divergence-free for the interpolants
// themselves (no derivative is interpolated separately), and u_d = 0 exactly
// where V = 0 (surface, floor). x / y outside one span of the world, or not
// finite, take the exact formulas; the current's point is never wrapped.
// ----------------------------------------------------------------------------
#include <cmath>
#include <cstdint>
#include <vector>
#include "../core/portable_math.hpp"

namespace oceanflow {

// ---- the eddies' radial factor ---------------------------------------------
inline constexpr int kRadialCells = 2048;
inline constexpr float kRadialPerUnit = 128.0f;   // nodes per unit of u
inline constexpr float kRadialCut = 16.0f;        // kRadialCells / kRadialPerUnit

struct RadialTable {
    float E[kRadialCells + 1];
    RadialTable() {
        for (int j = 0; j <= kRadialCells; ++j)   // e^{−u} as 1 / e^{u}: full relative precision
            E[j] = 1.0f / (evomath::expm1f((float)j * (1.0f / kRadialPerUnit)) + 1.0f);
    }
};
inline const RadialTable& radialTable() {
    static const RadialTable t;
    return t;
}

// g(u) into `g` for 0 <= u < 16; false (no contribution) otherwise, NaN included.
inline bool radial(const float* E, float u, float& g) {
    if (!(u >= 0.0f && u < kRadialCut)) return false;
    const float s = u * kRadialPerUnit;   // exact (a power of two), in [0, 2048)
    const int i = (int)s;
    const float t = s - (float)i;
    g = (E[i] + t * (E[i + 1] - E[i])) - E[kRadialCells];
    return true;
}

// ---- cubic Hermite on one cell ---------------------------------------------
// t in [0, 1]; a cell stores its start value v0, its rise dv = v1 − v0 and
// the end slopes m0, m1 per unit t:
//   p(t)  = v0 + h01(t)·dv + h10(t)·m0 + h11(t)·m1     (h00 = 1 − h01)
//   p'(t) = a(t)·dv + b(t)·m0 + c(t)·m1                 (d/dt)
// dv is worked out to full precision when the table is built (never as the
// difference of two rounded node values: the derivative would carry their
// rounding, ulp(v) / h — e.g. 8e-6 of the waves' horizontal speed at 64 cells
// per band, twice that at 128). Neighbouring cells then meet within ~1 ulp of
// the value, i.e. continuity to roundoff. The one evaluator every table uses,
// in this operation order.
struct Basis { float h10, h01, h11, a, b, c; };
inline Basis basis(float t) {
    const float tt = t * t;
    const float ttt = tt * t;
    return {(ttt - 2.0f * tt) + t, -2.0f * ttt + 3.0f * tt, ttt - tt,
            6.0f * t - 6.0f * tt, (3.0f * tt - 4.0f * t) + 1.0f, 3.0f * tt - 2.0f * t};
}
inline float hval(const Basis& B, float v0, float dv, float m0, float m1) {
    return ((v0 + B.h01 * dv) + B.h10 * m0) + B.h11 * m1;
}
inline float hder(const Basis& B, float dv, float m0, float m1) {   // d/dt
    return (B.a * dv + B.b * m0) + B.c * m1;
}

// ---- four lanes: the three wave trains side by side ---------------------------
// The wave tables are evaluated with the trains in the lanes of a 4-wide float
// vector (GCC / Clang vector extensions: SSE2 on x86-64, SIMD128 under
// -msimd128). Lane-wise +, −, × are IEEE single operations, each lane runs the
// same operation tree a scalar evaluation would, so the bits are the scalar
// ones in every build (no FMA: -ffp-contract=off, no relaxed SIMD); lane 3 is
// padding (zeros).
typedef float v4f __attribute__((vector_size(16)));
inline v4f splat(float x) { return v4f{x, x, x, x}; }
struct BasisV { v4f h10, h01, h11, a, b, c; };
inline BasisV basisV(float t) {
    const Basis B = basis(t);
    return {splat(B.h10), splat(B.h01), splat(B.h11), splat(B.a), splat(B.b), splat(B.c)};
}
inline v4f hval(const BasisV& B, v4f v0, v4f dv, v4f m0, v4f m1) {
    return ((v0 + B.h01 * dv) + B.h10 * m0) + B.h11 * m1;
}
inline v4f hder(const BasisV& B, v4f dv, v4f m0, v4f m1) {   // d/dt
    return (B.a * dv + B.b * m0) + B.c * m1;
}

// Three-point Gauss–Legendre on [x0, x1] (exact for quintics), in double.
template <class F>
double gauss3(double x0, double x1, F&& f) {
    const double m = 0.5 * (x0 + x1), r = 0.5 * (x1 - x0), o = r * 0.7745966692414834;   // sqrt(3/5)
    return r * ((5.0 / 9.0) * f(m - o) + (8.0 / 9.0) * f(m) + (5.0 / 9.0) * f(m + o));
}

// ---- the waves' depth profiles -----------------------------------------------
inline constexpr int kTrains = 3;
inline constexpr int kDepthCellsMin = 64;        // per piece
// The two band pieces get more: there V carries the taper's S''' = −12/b³, so
// with n cells the spline's derivative errs by ~1/n³ of the peak horizontal
// speed whatever b is (sampled, the preset's train 0: 64 cells 6.4e-6 units/s,
// 128 cells 1.4e-6 — the exact formula's own rounding level).
inline constexpr int kDepthCellsBand = 128;
inline constexpr double kDepthKCell = 0.04;      // k_max · cell width at most
inline constexpr double kDepthCellsMax = 16384;  // all four pieces; beyond: the exact formulas

// ---- the waves' horizontal factors -------------------------------------------
inline constexpr int kXCells = 512, kYCells = 256;
inline constexpr int kWaveModes[kTrains][2] = {{6, 0}, {2, 3}, {4, -2}};   // (m, n) over the world
inline constexpr double kTwoPi = 6.283185307179586;

// cos and sin of a double angle (|a| up to a few hundred), reduced in double
// by the nearest multiple of π/2 to |r| <= π/4, then evomath's float kernels:
// within ~1 ulp, the same bits native and wasm (IEEE double arithmetic).
inline void cosSin(double a, float& c, float& s) {
    constexpr double kPio2 = 1.57079632679489661923;
    const double q = std::floor(a / kPio2 + 0.5);
    const double r = a - q * kPio2;
    const float cr = evomath::detail::cosdf(r), sr = evomath::detail::sindf(r);
    switch ((((long long)q % 4) + 4) % 4) {
        case 0:  c = cr;  s = sr;  break;
        case 1:  c = -sr; s = cr;  break;
        case 2:  c = -cr; s = -sr; break;
        default: c = sr;  s = -cr; break;
    }
}

// cos / sin on one cell (Hermite cell form, see hval): start values, rises,
// slopes per unit t at both ends.
struct TrigCell { float c, dc, mc0, mc1, s, ds, ms0, ms1; };
// Cell j of cos / sin(2π·mode·u / N), u in [j, j + 1]: integer node phases
// (mode·j mod N), the rises by the half-angle products (full precision, not
// differences of rounded values), slopes w·(−sin, cos), w = 2π·mode / N.
inline TrigCell trigCell(int mode, int j, int N) {
    auto nodeAngle = [&](int jj) { return kTwoPi * (double)((((long long)mode * jj) % N + N) % N) / (double)N; };
    const double w = kTwoPi * (double)mode / (double)N, a0 = nodeAngle(j);
    float c0, s0, c1, s1, cm, sm, ch, sh;
    cosSin(a0, c0, s0);
    cosSin(nodeAngle(j + 1), c1, s1);
    cosSin(a0 + 0.5 * w, cm, sm);
    cosSin(0.5 * w, ch, sh);
    (void)ch;
    return {c0, (float)(-2.0 * (double)sm * (double)sh), (float)(-w * (double)s0), (float)(-w * (double)s1),
            s0, (float)(2.0 * (double)cm * (double)sh), (float)(w * (double)c0), (float)(w * (double)c1)};
}
// One table cell with the trains in the lanes (lane i = train i, lane 3 zero).
struct TrigV { v4f c, dc, mc0, mc1, s, ds, ms0, ms1; };
inline void setLane(TrigV& v, int i, const TrigCell& t) {
    v.c[i] = t.c; v.dc[i] = t.dc; v.mc0[i] = t.mc0; v.mc1[i] = t.mc1;
    v.s[i] = t.s; v.ds[i] = t.ds; v.ms0[i] = t.ms0; v.ms1[i] = t.ms1;
}
inline TrigCell getLane(const TrigV& v, int i) {
    return {v.c[i], v.dc[i], v.mc0[i], v.mc1[i], v.s[i], v.ds[i], v.ms0[i], v.ms1[i]};
}
struct XTable {   // the three trains' x factors
    TrigV cell[kXCells];
    XTable() {
        for (int j = 0; j < kXCells; ++j) {
            cell[j] = TrigV{};
            for (int i = 0; i < kTrains; ++i) setLane(cell[j], i, trigCell(kWaveModes[i][0], j, kXCells));
        }
    }
};
struct YTable {   // the y factors of trains 1, 2 (n != 0) at phase 0; lane 0 (train 0) zero
    TrigV cell[kYCells];
    YTable() {
        for (int j = 0; j < kYCells; ++j) {
            cell[j] = TrigV{};
            for (int i = 1; i < kTrains; ++i) setLane(cell[j], i, trigCell(kWaveModes[i][1], j, kYCells));
        }
    }
};
inline const XTable& xTable() { static const XTable t; return t; }
inline const YTable& yBaseTable() { static const YTable t; return t; }
// (c, s) rotated by the angle with cosine cr, sine sr: cos / sin(θ + ψ).
inline void rotate(float& c, float& s, float cr, float sr) {
    const float nc = c * cr - s * sr, ns = s * cr + c * sr;
    c = nc;
    s = ns;
}

// One depth cell, a train per lane: V at its start, its rise (∫ k·H over the
// cell), the slopes h·k·H at both ends.
struct DepthCell { v4f v, dv, m0, m1; };
struct DepthPiece {
    float lo = 0.0f;
    int n = 0;                    // cells
    double scale = 0.0;           // cells per unit depth (the cell coordinate)
    float dScale = 0.0f;          // the same as a float: dV/dd = dV/dt · dScale
    std::vector<DepthCell> cell;  // n
};

struct Eddy {
    float cx = 0.0f, cy = 0.0f;   // centre (CurrentField::advance's expressions)
    float signScale = 0.0f;       // sign · strength / core · e^{1/2}, as velocityAt forms it
};

// ============================================================================
// Glass walls (OceanConfig::walls): the wall-compatible flow's tables. Only
// OceanWorld's wall evaluators read anything below; the periodic tables above
// are untouched.
//
// Eddies: the streamfunction of one vortex, psi = a c² G(u), u = r² / 2c²,
// G(u) = −∫_u^16 g(s) ds with g the radial table's own factor (radial above).
// g is linear on each table cell, so G is its exact integral there (a
// quadratic): dG/du = g in every cell, and (−∂y psi, ∂x psi) is the vortex's
// velocity a g(u) (−dy, dx) exactly as the table gives it. The node values
// are accumulated from the cutoff down in double. G = 0 (and g = 0) from
// u = 16 on: C¹ there.
// ============================================================================
struct RadialIntTable {
    float G[kRadialCells + 1];
    RadialIntTable() {
        const float* E = radialTable().E;
        const double Ec = (double)E[kRadialCells];
        double acc = 0.0;   // G(u_j) = −Σ_{k >= j} ∫ over cell k
        G[kRadialCells] = 0.0f;
        for (int j = kRadialCells - 1; j >= 0; --j) {
            acc -= (0.5 * ((double)E[j] + (double)E[j + 1]) - Ec) / (double)kRadialPerUnit;
            G[j] = (float)acc;
        }
    }
};
inline const RadialIntTable& radialIntTable() {
    static const RadialIntTable t;
    return t;
}
// g(u) and G(u) for 0 <= u < 16; false (both 0: no contribution) otherwise.
// g is radial()'s value, bit for bit.
inline bool radialPsi(const float* E, const float* G, float u, float& g, float& Gu) {
    if (!(u >= 0.0f && u < kRadialCut)) return false;
    const float s = u * kRadialPerUnit;
    const int i = (int)s;
    const float t = s - (float)i;
    const float de = E[i + 1] - E[i];
    g = (E[i] + t * de) - E[kRadialCells];
    Gu = G[i] + (t * ((E[i] - E[kRadialCells]) + (0.5f * t) * de)) * (1.0f / kRadialPerUnit);
    return true;
}

// ---- double-precision cos / sin from IEEE operations only (table builds) ----
// Cody–Waite reduction by π/2 (two-part constant) to |r| <= π/4, then Taylor
// series through r^19 / r^20 (truncation < 1e-19 there): only +, −, ×, ÷ and
// floor, so every build gives the same bits (libm's cos / sin need not).
inline void cosSinD(double a, double& c, double& s) {
    constexpr double kP1 = 1.5707963267948966, kP2 = 6.123233995736766e-17;   // π/2 = kP1 + kP2
    const double q = std::floor(a / kP1 + 0.5);
    const double r = (a - q * kP1) - q * kP2, r2 = r * r;
    // sin r = r (1 − r²/(2·3) (1 − r²/(4·5) (1 − ...))), cos r = 1 − r²/(1·2) (1 − r²/(3·4) (...))
    double sn = 1.0, cs = 1.0;
    for (int k = 19; k >= 3; k -= 2) sn = 1.0 - r2 / ((double)k * (double)(k - 1)) * sn;
    sn = r * sn;
    for (int k = 20; k >= 2; k -= 2) cs = 1.0 - r2 / ((double)k * (double)(k - 1)) * cs;
    switch ((((long long)q % 4) + 4) % 4) {
        case 0:  c = cs;  s = sn;  break;
        case 1:  c = -sn; s = cs;  break;
        case 2:  c = -cs; s = -sn; break;
        default: c = sn;  s = -cs; break;
    }
}

// ---- quintic cells in monomial form: value, slope, curvature ----------------
// A cell holds p(t) = a0 + a1 t + a2 t² + a3 t³ + a4 t⁴ + a5 t⁵ (t in [0, 1]),
// the quintic Hermite interpolant of the node values, slopes and second
// derivatives at both ends, its coefficients solved in DOUBLE and stored as
// floats. p, p', p'' are evaluated from the same coefficients by Horner, each
// a sum of terms of the result's own size (the Hermite cell form
// v0 + h01·dv + ... formed p'' — O(w²) — out of O(w) terms and lost three
// digits to cancellation: 2e-4 of the vertical wave flow). One polynomial for
// all three, so a field built from p, p', p'' (the wall waves) is the curl of
// an interpolated potential: divergence-free for the interpolant itself.
struct Cell5V { v4f a0, a1, a2, a3, a4, a5; };   // a mode per lane
struct Eval5V { v4f p, d, s; };                   // value, d/dt, d²/dt²
inline Eval5V eval5(float tf, const Cell5V& c) {
    const v4f t = splat(tf);
    Eval5V e;
    e.p = ((((c.a5 * t + c.a4) * t + c.a3) * t + c.a2) * t + c.a1) * t + c.a0;
    e.d = (((splat(5.0f) * c.a5 * t + splat(4.0f) * c.a4) * t + splat(3.0f) * c.a3) * t + splat(2.0f) * c.a2) * t + c.a1;
    e.s = ((splat(20.0f) * c.a5 * t + splat(12.0f) * c.a4) * t + splat(6.0f) * c.a3) * t + splat(2.0f) * c.a2;
    return e;
}

// ---- the wall waves' modes (Neumann cosines) --------------------------------
// Each periodic train (m, n) (kWaveModes) becomes a QUADRATURE PAIR of
// standing modes, cos(μπx/W) cos(νπy/H): the primary (μa, νa) = (2|m|, 2|n|),
// the same wavelength and k as the periodic train, with cos(φ0 − phase), and
// its neighbour (μa + 1, νa ± 1) (νb = νa for n = 0, νa + 1 for n > 0, νa − 1
// for n < 0: the sign of n picks the diagonal) with sin(φ0 − phase). Each
// mode is a Neumann eigenfunction (zero normal derivative at x = 0, W and
// y = 0, H, odd μ / ν too), and each is the curl of its own vector potential,
// so the pair is divergence-free exactly and has no normal flow through any
// face; in the interior their relative phase advances across the tank, so the
// water orbits like the travelling wave it replaces, and near the glass it
// sloshes (a single standing mode would slosh everywhere). (The pairs follow
// the Gemini Argon answer q14; the per-mode construction is the walls design's
// and Argon's alike.) Modes: (12, 0) + (13, 0), (4, 6) + (5, 7), (8, 4) + (9, 3).
inline constexpr int wallModeA(int train, int axis) {
    return 2 * (kWaveModes[train][axis] < 0 ? -kWaveModes[train][axis] : kWaveModes[train][axis]);
}
inline constexpr int wallModeB(int train, int axis) {
    return axis == 0 ? wallModeA(train, 0) + 1
                     : (kWaveModes[train][1] > 0 ? wallModeA(train, 1) + 1
                                                 : kWaveModes[train][1] < 0 ? wallModeA(train, 1) - 1 : wallModeA(train, 1));
}
static_assert(wallModeA(0, 0) == 12 && wallModeB(0, 0) == 13 && wallModeB(0, 1) == 0 && wallModeB(1, 1) == 7 &&
                  wallModeB(2, 1) == 3,
              "the wall waves' quadrature pairs");

// cos(μπ·u/N) on cell j (u in [j, j + 1]) as a quintic: the node values,
// slopes −w·sin (per unit t) and second derivatives −w²·cos (per unit t²) at
// both ends, w = πμ/N, from the integer node phases (μ·j mod 2N half-turns)
// by cosSinD, all in double; then the coefficients (a0 = v0, a1 = m0,
// a2 = c0/2, and a3..a5 from the far end's three conditions) in double. The
// slopes at the tank's two ends (j = 0, j + 1 = N) are sin(0) = sin(μπ) = 0:
// pinned to exactly 0 (the evaluator pins the faces' slopes too, since the
// sum at t = 1 is the end slope only to rounding).
inline void wallCell(int mu, int j, int N, double a[6]) {
    auto nodeAngle = [&](int jj) { return kTwoPi * (double)(((long long)mu * jj) % (2LL * N)) / (2.0 * N); };
    const double w = kTwoPi * (double)mu / (2.0 * N);
    double c0, s0, c1, s1;
    cosSinD(nodeAngle(j), c0, s0);
    cosSinD(nodeAngle(j + 1), c1, s1);
    if (j == 0) s0 = 0.0;
    if (j + 1 == N) s1 = 0.0;
    const double m0 = -w * s0, m1 = -w * s1, k0 = -w * w * c0, k1 = -w * w * c1;
    const double R0 = ((c1 - c0) - m0) - 0.5 * k0, R1 = (m1 - m0) - k0, R2 = k1 - k0;
    a[0] = c0;
    a[1] = m0;
    a[2] = 0.5 * k0;
    a[3] = (10.0 * R0 - 4.0 * R1) + 0.5 * R2;
    a[4] = (-15.0 * R0 + 7.0 * R1) - R2;
    a[5] = (6.0 * R0 - 3.0 * R1) + 0.5 * R2;
}
inline void setLane5(Cell5V& c, int i, const double a[6]) {
    c.a0[i] = (float)a[0]; c.a1[i] = (float)a[1]; c.a2[i] = (float)a[2];
    c.a3[i] = (float)a[3]; c.a4[i] = (float)a[4]; c.a5[i] = (float)a[5];
}
// The x (axis 0, kXCells over W) or y (axis 1, kYCells over H) factors of
// every wall mode: lanes = trains (lane 3 zero), A = the primaries, B = the
// neighbours. A mode number 0 (no variation along the axis) is the constant
// 1 with zero slope and curvature, exactly.
struct WallAxisTable {
    int n = 0;
    std::vector<Cell5V> A, B;
    explicit WallAxisTable(int axis) {
        n = axis == 0 ? kXCells : kYCells;
        A.assign(n, Cell5V{});
        B.assign(n, Cell5V{});
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < kTrains; ++i)
                for (int ab = 0; ab < 2; ++ab) {
                    const int mu = ab == 0 ? wallModeA(i, axis) : wallModeB(i, axis);
                    Cell5V& c = (ab == 0 ? A : B)[j];
                    double a[6] = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0};   // mode 0: the constant 1
                    if (mu != 0) wallCell(mu, j, n, a);
                    setLane5(c, i, a);
                }
    }
};
inline const WallAxisTable& wallXTable() { static const WallAxisTable t(0); return t; }
inline const WallAxisTable& wallYTable() { static const WallAxisTable t(1); return t; }

// A BOUNDED cell coordinate for the wall tables: s = (v / L)·N (v / L is
// exactly 1 at v = L, so the far face is s = N exactly: the last cell at
// t = 1, never wrapped to the first); false outside [0, L] or not finite (the
// exact formulas then).
inline bool wallCellCoord(float v, double L, int N, int& j, float& t) {
    const double s = ((double)v / L) * (double)N;
    if (!(s >= 0.0 && s <= (double)N)) return false;
    j = (int)s;
    if (j > N - 1) j = N - 1;
    t = (float)(s - (double)j);
    return true;
}

struct FlowCache {
    // ---- the glass walls' scratch (OceanConfig::walls; set at init / when the
    // waves are prepared; read only by the wall evaluators) ----
    float wallBc = 1.0f;          // the current's taper band b_c = min(max(core, 1), W/4, H/4)
    std::vector<float> psiScale;  // per eddy: signScale · core² (the streamfunction's prefactor)
    v4f wInvK2 = {};              // 1 / k² per train (k = the primary's, the periodic train's)
    v4f wAmpA = {}, wAmpB = {};   // the pair's amplitude factors (see OceanWorld::prepareWaves)
    float wxS = 0.0f, wyS = 0.0f; // cells per unit: kXCells / W, kYCells / H (float)

    // ---- the eddy current (CurrentField Mode::Eddies, strength > 0) ----
    bool curOn = false;           // set at init: the Ocean's eddy evaluator is in use
    bool curAdvanced = false;     // CurrentField::advance ran since init (else its centres are 0)
    bool curReady = false;        // eddies[].cx / cy hold the centres for (curFrame, curAdvKey)
    bool curAdvKey = false;
    unsigned long long curFrame = 0;
    float inv2c2 = 0.0f;          // 1 / (2 core²), velocityAt's expression
    std::vector<float> vort;      // CurrentField::vortexState() at init (9 floats per vortex)
    std::vector<Eddy> eddies;

    // ---- the waves ----
    v4f Tx = {}, Ty = {};         // K / |K|² per lane (init: the world size)
    // Depth pieces, keyed on the bits of waveSpeed (the amplitudes), waveBand
    // (b) and D; rebuilt by OceanWorld::ensureFlow when a key moves.
    bool depthKeyed = false;      // the key below is set
    bool depthOn = false;         // the pieces are built (false: over the cap, the exact formulas)
    std::uint32_t keySpeed = 0, keyBand = 0, keyD = 0;
    float b = 0.0f, Dh = 0.0f, Dmb = 0.0f;   // the pieces' joins: b, D/2, D − b
    DepthPiece piece[4];
    // Horizontal (init: the world size): cells per unit, T in cell units.
    double xScale = 0.0, yScale = 0.0;          // kXCells / W, kYCells / H
    v4f TxS = {}, TyS = {};                     // T_x · xScale, T_y · yScale per lane
    // The phase factors, keyed on the bits of the three phases.
    bool phaseKeyed = false;
    std::uint64_t keyPhase[kTrains] = {};
    float cp[kTrains] = {}, sp[kTrains] = {};   // cos / sin(φ0 − phase)
    // The y factors at this phase (kYCells): trains 1, 2 rotated from the
    // static base; lane 0 is train 0's constant (cp[0], sp[0], no rise or
    // slope), so its y factor is that constant exactly (v0 + h·0 = v0).
    std::vector<TrigV> yRot;
};

}  // namespace oceanflow
