#pragma once
// ----------------------------------------------------------------------------
// appearance_eval — the deterministic CPU reference of pattern law 3: the
// unlit (intrinsic) skin colour of a fish at a material site, from its
// appearance genome (appearance_model.hpp). It mirrors the accepted
// prototype's CPU law (gallery v3: decode, fieldF, markOf, skinMat; the
// page's mirror is web/ocean_appearance.js), so a later sense can sample a
// fish's appearance (phase V: the eye) and a later glow can use the same
// field as its layout (phase B: fieldOpacity) without the renderer.
//
// Nothing in the simulation calls it yet. It is pure double arithmetic with
// the vendored transcendentals (appmath cos / sin / atan2, evomath's musl
// expm1 for exp), so its results are the same native and in wasm; the unit
// checks hold it to the gallery's own CPU law within 1e-6 (the GPU is held
// to the same law within the gallery's K12 tolerance, p99.9 <= .002).
//
// Material sites (P1's charts): u along the body (0 snout root .. 1 peduncle;
// below 0 the snout, above 1 the tail), v back (0) to belly (1), z the rest
// height in body radii (landmarks), lambda = body length / radius.
// ----------------------------------------------------------------------------
#include <algorithm>
#include <cmath>
#include "appearance_model.hpp"
#include "../core/portable_math.hpp"

namespace appearance {

struct PatternDecoded {
    double bHue = 0, bSat = 0, bVal = 0, cs = 0, L = 0, ax = 0, hd = 0, bal = 0, f = 0, a = 0;
    double n0x = 1, n0y = 0, n1x = 1, n1y = 0, w = 0, A = 0, warp = 0, contrast = 0;
    double mark[3] = {0, 0, 0};
    double cut = 0, dg = 0, outline = 0, fold = 0, hband = 0, wcs = 0;
    double ebS = 0, ebW = 0, shS = 0, shR = 0, ocS = 0, ocR = 0, ocRing = 0, ocW = 0, fm = 0;
    double Tx = 0, Ty = 0, lam = 1;
};

namespace eval_detail {
inline constexpr double TAU = 6.283185307179586, PI = 3.141592653589793;
inline constexpr double kIsoA = 0.364;   // law 3: .42·sqrt(6/8)
// The eight detuned unit waves (law 3): directions in degrees, fixed phases;
// each component is the binary32 rounding of cos / sin of the angle, as the
// gallery (and the shader's literals) have them.
inline constexpr double kWaveDeg[8] = {3.8, 18.8, 53.9, 69.3, 90.4, 107.5, 142.9, 162.4};
inline constexpr double kWavePh[8] = {0, 0.137, 0.311, 0.503, 0.677, 0.839, 0.229, 0.953};
struct Waves { double kx[8], ky[8]; };
inline const Waves& waves() {
    static const Waves W = [] {
        Waves w{};
        for (int k = 0; k < 8; ++k) {
            w.kx[k] = (double)(float)appmath::cos(kWaveDeg[k] * PI / 180);
            w.ky[k] = (double)(float)appmath::sin(kWaveDeg[k] * PI / 180);
        }
        return w;
    }();
    return W;
}
inline double S3(double a, double b, double x) {
    double t = (x - a) / (b - a);
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return t * t * (3 - 2 * t);
}
inline double clamp01(double x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
inline double Edz(double g) { return std::max(0.0, (g - 0.40) / 0.60); }
inline double wrap01(double x) { const double y = x - std::floor(x); return (y >= 1 || y == 0) ? 0 : y; }
inline double expd(double x) { return evomath::detail::expm1_impl(x) + 1.0; }
inline void hsv(double h, double s, double v, double out[3]) {
    for (int c = 0; c < 3; ++c) {
        double x = 6 * h + (c == 0 ? 0 : (c == 1 ? 4 : 2));
        x = x - 6 * std::floor(x / 6);
        double q = std::fabs(x - 3) - 1;
        q = q < 0 ? 0 : (q > 1 ? 1 : q);
        out[c] = v * (1 + s * (q - 1));
    }
}
inline void chroma(double gx, double gy, double& hue, double& sat) {
    const double x = 2 * gx - 1, y = 2 * gy - 1, cc = x * x + y * y;
    hue = cc == 0 ? 0 : wrap01(appmath::atan2(y, x) / TAU);
    sat = std::min(1.0, std::sqrt(cc));
}
// P1's half-height h(u) (radiusZ), linear in u.
inline double profileH(double u) {
    static const double PU[8] = {-0.20, -0.10, 0.04, 0.20, 0.42, 0.64, 0.84, 1.00};
    static const double PZ[8] = {0, 0.42, 0.74, 0.84, 0.72, 0.52, 0.33, 0.20};
    if (u <= PU[0]) return PZ[0];
    for (int i = 1; i < 8; ++i)
        if (u < PU[i]) return PZ[i - 1] + (PZ[i] - PZ[i - 1]) * (u - PU[i - 1]) / (PU[i] - PU[i - 1]);
    return PZ[7];
}
inline double Dm(double d, double r) { return 1 - S3(r - 0.005, r + 0.005, d); }
}  // namespace eval_detail

// lambda = body length / body radius: 20·size·elongation / (4.6·(1 + .4 tanh(5(size − 1)))),
// the tank's radius curve (web/ocean_scene.js bodyScale).
inline double lambdaOf(double size, double elong) {
    return 20.0 * size * elong / (4.6 * (1.0 + 0.4 * evomath::tanh(5.0 * (size - 1.0))));
}

inline PatternDecoded decodePattern(const Genome& G, double lambda) {
    using namespace eval_detail;
    const float* g = G.g;
    PatternDecoded D;
    chroma(g[0], g[1], D.bHue, D.bSat);
    D.bVal = g[2];
    D.cs = g[3]; D.L = 0.15 + 0.70 * g[4]; D.ax = g[5]; D.hd = g[6]; D.bal = g[7];
    D.f = 1.5 * expd(2.0794415416798359 * g[8]);
    const double ox = 2.0 * g[9] - 1, oy = 2.0 * g[10] - 1, oo = ox * ox + oy * oy;
    D.a = std::min(1.0, std::sqrt(oo));
    const double th = oo == 0 ? 0 : 0.5 * appmath::atan2(oy, ox);
    D.n0x = appmath::cos(th); D.n0y = appmath::sin(th);
    const double del = PI * g[11];
    D.n1x = appmath::cos(th + del); D.n1y = appmath::sin(th + del);
    const double w = g[12], a = D.a;
    D.w = w;
    const double coh = 1 - S3(0.02, 0.20, std::fabs(appmath::sin(del)));
    const double q0 = kIsoA * (1 - a), q1 = 0.95 * a;
    D.A = std::sqrt(q0 * q0 + q1 * q1 * (1 + w * w + 2 * w * coh) / ((1 + w) * (1 + w)));
    D.warp = g[13];
    D.contrast = std::max(0.0, 2.0 * g[14] - 1);
    double mh, ms;
    chroma(g[15], g[16], mh, ms);
    hsv(mh, ms, g[17], D.mark);
    D.cut = -0.15 + 1.30 * g[18]; D.dg = g[19]; D.outline = g[20]; D.fold = g[21];
    D.hband = 0.025 + 0.18 * (1.0 - g[20]) * (1.0 - g[20]);
    D.wcs = 0.35 * expd(-3.2188758248682006 * g[22]);
    D.ebS = Edz(g[23]); D.ebW = 0.08 + 0.42 * g[24]; D.shS = Edz(g[25]); D.shR = 0.08 + 0.22 * g[26];
    D.ocS = Edz(g[27]); D.ocR = 0.12 + 0.34 * g[28]; D.ocRing = g[29]; D.ocW = 0.04 + 0.08 * g[29]; D.fm = Edz(g[30]);
    D.Tx = 0.35 * appmath::sin(TAU * g[kPhaseU]);
    D.Ty = 0.25 * appmath::sin(TAU * g[kPhaseV]);
    D.lam = lambda;
    return D;
}

// The shared field F at (u, v) (one value per site, before the threshold).
inline double patternFieldValue(const PatternDecoded& D, double u, double v) {
    using namespace eval_detail;
    const double vf = v + ((0.5 - std::fabs(v - 0.5)) - v) * D.fold;
    const double x = D.f * (2 * u - 1 - D.Tx), y = D.f * (vf - 0.5 - D.Ty);
    double zx = x, zy = y;
    if (D.warp != 0) {
        zx += 0.65 * D.warp * appmath::sin(TAU * (0.31 * x + 0.23 * y + 0.17));
        zy += 0.65 * D.warp * appmath::sin(TAU * (-0.19 * x + 0.29 * y + 0.43));
    }
    double N = 0, C = 0;
    if (D.a < 1) {
        const Waves& W = waves();
        for (int k = 0; k < 8; ++k) N += appmath::cos(TAU * (W.kx[k] * zx + W.ky[k] * zy + kWavePh[k]));
        N /= 8;
    }
    if (D.a > 0) {
        C = appmath::cos(TAU * (D.n0x * zx + D.n0y * zy));
        if (D.w != 0) C = (C + D.w * appmath::cos(TAU * (D.n1x * zx + D.n1y * zy))) / (1 + D.w);
    }
    return (1 - D.a) * N + D.a * C;
}

// The field's opacity at (u, v): where the marks are, 0..1 (contrast x coverage
// x the thresholded field). Phase B's glow layout.
inline double fieldOpacity(const PatternDecoded& D, double u, double v) {
    using namespace eval_detail;
    const double vc = clamp01(v);
    const double F = patternFieldValue(D, u, v);
    const double b = clamp01(D.bal + 0.5 * (2 * D.dg - 1) * (vc - 0.5));
    const double t = D.A * (1 - 2 * b), aa = 0.005;
    const double fill = S3(t - aa, t + aa, F);
    double mark = fill;
    if (D.outline != 0) {
        const double hb = D.hband;
        const double band = S3(t - hb - aa, t - hb + aa, F) - S3(t + hb - aa, t + hb + aa, F);
        mark = fill + (std::max(0.0, band) - fill) * D.outline;
    }
    return D.contrast * (1 - S3(D.cut - 0.05, D.cut + 0.05, vc)) * mark;
}

// The unlit skin colour (RGB reflectance in [0, 1]) at a body site (u, v, z);
// fin 0 = body (landmarks), 1 / 2 / 3 / 5 / 6 = a fin at margin distance dMar.
inline void skinRGB(const PatternDecoded& D, double u, double v, double z, int fin, double dMar, double out[3]) {
    using namespace eval_detail;
    const double vc = clamp01(v);
    const double fo = fieldOpacity(D, u, v);
    const double headW = 1 - S3(0.10, 0.25, u);
    const double H = wrap01(D.bHue + 0.25 * (2 * D.ax - 1) * clamp01(u) + 0.50 * (2 * D.hd - 1) * headW);
    double b[3], p[3], bt[3];
    hsv(H, D.bSat, D.bVal, b);
    hsv(H, 0.12 * D.bSat, 0.97, p);
    hsv(H, 0.18 * D.bSat, 0.65 + 0.35 * D.bVal, bt);
    const double s = D.cs, below = S3(D.L - D.wcs, D.L + D.wcs, vc);
    for (int c = 0; c < 3; ++c) {
        const double dors = b[c] * (1 - 0.5 * s), bel = b[c] + (bt[c] - b[c]) * 0.85 * s;
        const double col = dors + (bel - dors) * below;
        out[c] = col + (D.mark[c] - col) * fo;
    }
    if (fin == 0) {
        const double lam = D.lam;
        double op = D.ebS * Dm(std::fabs(lam * (u - 0.03)), D.ebW);
        if (op > 0) for (int c = 0; c < 3; ++c) out[c] += (D.mark[c] - out[c]) * op;
        const double sx = lam * (u - 0.24), sz = z - 0.10 * profileH(0.24);
        op = D.shS * Dm(std::sqrt(sx * sx + sz * sz), D.shR);
        if (op > 0) for (int c = 0; c < 3; ++c) out[c] += (D.mark[c] - out[c]) * op;
        const double qx = lam * (u - 0.72), dO = std::sqrt(qx * qx + z * z);
        op = D.ocS * D.ocRing * std::max(0.0, Dm(dO, D.ocR + D.ocW) - Dm(dO, D.ocR));
        if (op > 0) for (int c = 0; c < 3; ++c) out[c] += (p[c] - out[c]) * op;
        op = D.ocS * Dm(dO, D.ocR);
        if (op > 0) for (int c = 0; c < 3; ++c) out[c] += (D.mark[c] - out[c]) * op;
    } else if (fin == 1 || fin == 2 || fin == 3 || fin == 5 || fin == 6) {
        for (int c = 0; c < 3; ++c) out[c] += (p[c] - out[c]) * 0.12;
        const double op = D.fm * Dm(dMar, 0.08);
        for (int c = 0; c < 3; ++c) out[c] += (p[c] - out[c]) * op;
    }
}

// The body flank chart: (u, v) with z = h(u)(1 − 2v) (the gallery's flankAt).
inline void flankRGB(const PatternDecoded& D, double u, double v, double out[3]) {
    skinRGB(D, u, v, eval_detail::profileH(u) * (1 - 2 * v), 0, 1e3, out);
}

}  // namespace appearance
