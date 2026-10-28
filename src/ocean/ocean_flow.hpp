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
struct XCell { TrigCell t[kTrains]; };       // the three trains at one x cell
struct YCell { TrigCell t[kTrains - 1]; };   // trains 1, 2 (n != 0) at one y cell
struct XTable {
    XCell cell[kXCells];
    XTable() {
        for (int j = 0; j < kXCells; ++j)
            for (int i = 0; i < kTrains; ++i) cell[j].t[i] = trigCell(kWaveModes[i][0], j, kXCells);
    }
};
struct YTable {
    YCell cell[kYCells];
    YTable() {
        for (int j = 0; j < kYCells; ++j)
            for (int i = 1; i < kTrains; ++i) cell[j].t[i - 1] = trigCell(kWaveModes[i][1], j, kYCells);
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

// One depth cell, per train: V at its start, its rise (∫ k·H over the cell),
// the slopes h·k·H at both ends.
struct DepthCell { float v[kTrains], dv[kTrains], m0[kTrains], m1[kTrains]; };
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

struct FlowCache {
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
    float Tx[kTrains] = {}, Ty[kTrains] = {};   // K / |K|² (init: the world size)
    // Depth pieces, keyed on the bits of waveSpeed (the amplitudes), waveBand
    // (b) and D; rebuilt by OceanWorld::ensureFlow when a key moves.
    bool depthKeyed = false;      // the key below is set
    bool depthOn = false;         // the pieces are built (false: over the cap, the exact formulas)
    std::uint32_t keySpeed = 0, keyBand = 0, keyD = 0;
    float b = 0.0f, Dh = 0.0f, Dmb = 0.0f;   // the pieces' joins: b, D/2, D − b
    DepthPiece piece[4];
    // Horizontal (init: the world size): cells per unit, T in cell units.
    double xScale = 0.0, yScale = 0.0;          // kXCells / W, kYCells / H
    float TxS[kTrains] = {}, TyS[kTrains] = {}; // T_x · xScale, T_y · yScale
    // The phase factors, keyed on the bits of the three phases.
    bool phaseKeyed = false;
    std::uint64_t keyPhase[kTrains] = {};
    float cp[kTrains] = {}, sp[kTrains] = {};   // cos / sin(φ0 − phase)
    std::vector<YCell> yRot;                    // trains 1, 2 at this phase (kYCells)
};

}  // namespace oceanflow
