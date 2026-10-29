#pragma once
// ----------------------------------------------------------------------------
// appearance_model — the Ocean fish's appearance genome (appearance schema 3):
// 31 skin-pattern genes (P1, pattern law 3), the two maternal layout phases,
// and 30 body-shape genes (M1). 63 binary32 floats per fish, in this order:
//
//   0..30   pattern genes (p1_patterns_v2_codex.md's table: base chroma x/y,
//           base value, countershading, hue shifts, balance, scale,
//           orientation x/y (double-angle), cross angle/weight, warp,
//           contrast, mark chroma x/y, mark value, coverage, density
//           gradient, outline, fold, countershade sharpness, eye bar,
//           shoulder spot, ocellus, fin margin)
//   31, 32  phaseU, phaseV (maternal: copied from parent A with a small drift)
//   33..62  morphology genes (m1_morphology_codex.md's table: depth, width,
//           cross-section, fullness, head, snout, peduncle, caudal outline /
//           aspect / area, dorsal, anal, pectoral, pelvic, barbels, eyes)
//
// COSMETIC. Nothing the simulation step reads depends on it: no sense,
// action, cost, vote or draw. It is rendered (web/ocean_scene.js) and
// inherited, nothing else. (Phase V — the eye sees appearance — and phase B —
// glow laid out by the pattern field — will read it later, each as its own
// switch.)
//
// Construction (founders and offspring) is the accepted prototype's
// (gallery v3, ocean_handoff/patterns_gallery/gallery_v3.html: pattern law 3,
// pattern inheritance / founder 3, morphology inheritance / founder 1),
// transcribed operation by operation: JS evaluates in doubles and stores each
// gene as binary32 (a Float32Array or Math.fround); this code does the same
// double operations in the same order and rounds at the same points, with the
// transcendentals of appearance_math.hpp. tests/ocean_tests.cpp checks it
// against vectors exported from the gallery's own law script (exact, every
// float).
//
//   founder:   P1 draws 0..32 (31 genes, then phaseU, phaseV), M1 draws 33..62
//   offspring: P1 draws 0..88 (25 group selectors, 31 prospective founder
//              alleles, 31 jitters, 2 phase drifts), M1 draws 89..159
//              (11 selectors, 30 prospective alleles, 30 jitters)
//   each group (25 pattern + 11 shape groups): mean of the two parents
//   (20%), copy A, copy B, or a new founder allele (novel probability p; at
//   the default p = .05: 20 / 37.5 / 37.5 / 5%); then a uniform jitter of
//   ±mutation on every gene, clamped (crossAngle wraps), and the three
//   2-vectors projected back onto the unit disk. Colour pairs average as the
//   saturation-weighted vector mean, the cross pair as a weighted axial vector.
//
// THE DRAW STREAM (not the simulation's rng). Every draw a fish's appearance
// uses comes from its own counter-based stream:
//     base  = mix64(worldKey ^ mix64(fishId + γ))
//     draw k = top 24 bits of mix64(base + (k + 1)·γ) × 2^-24   (SplitMix64)
// keyed by the world (worldKey: set at OceanWorld::init from a COPY of the
// seeded engine — its first two outputs — so it is a function of the world
// seed and the engine itself does not advance) and by the fish's unique id;
// draw k of fish i never depends on any other fish, on the order of births,
// or on the simulation's rng. So appearance on or off, any mutation setting,
// the simulation's every state and draw are the same (tests: neutrality;
// harness: the app_off_* twins). Slots are fixed: an offspring's M1 genes
// use draws 89..159 whether or not P1 is on.
// ----------------------------------------------------------------------------
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "appearance_math.hpp"

namespace appearance {

// ---- schema and versions -----------------------------------------------------
inline constexpr int kAbi = 3;                   // oc_creature_appearance's layout
inline constexpr int kSchema = 3;                // 63 floats: P1 31 + 2 phases + M1 30
inline constexpr int kPatternSchema = 2;         // the 33-float P1 prefix's layout
inline constexpr int kPatternLaw = 3;            // gallery v3: eight waves, contrast dead zone .5
inline constexpr int kPatternInheritance = 3;    // colour pairs: saturation-weighted mean
inline constexpr int kPatternFounder = 3;        // contrast founder remap .775 + .225(r − .25)/.75
inline constexpr int kMorphLaw = 2;              // M1 law 1 + gallery v3's recommendations (render only)
inline constexpr int kMorphInheritance = 1;
inline constexpr int kMorphFounder = 1;
inline constexpr int kMorphChart = 1;

inline constexpr int kPatternGenes = 31;
inline constexpr int kPhaseU = 31, kPhaseV = 32;
inline constexpr int kMorphOffset = 33;
inline constexpr int kMorphGenes = 30;
inline constexpr int kStride = 63;

inline constexpr int kFounderDraws = 63;         // P1 33, then M1 30
inline constexpr int kOffspringDraws = 160;      // P1 89, then M1 71
inline constexpr int kP1FounderDraws = 33;
inline constexpr int kP1OffspringDraws = 89;
inline constexpr int kPatternGroups = 25;
inline constexpr int kMorphGroups = 11;

// OceanConfig::appearance bits.
inline constexpr int kPatterns = 1;   // P1: the pattern genes and phases (else they stay neutral)
inline constexpr int kShapes = 2;     // M1: the body-shape genes (else neutral)
inline constexpr int kAll = kPatterns | kShapes;

struct Genome {
    float g[kStride];
};
static_assert(sizeof(Genome) == 252, "the appearance genome is 63 binary32 floats");

// One per fish while appearance is on, in creature order, bound to its owner.
struct alignas(8) Record {
    std::uint64_t ownerId;
    Genome genome;
};
static_assert(sizeof(Record) == 264, "an appearance record is the owner id + 63 floats + padding");

// Inheritance settings (OceanConfig: patternMutation, patternPhaseMutation,
// patternNovelProbability, morphologyMutation, morphologyNovelProbability).
struct Inheritance {
    float patternMutation = 0.025f;
    float phaseMutation   = 0.030f;   // law 3 (P1 v2 had .010)
    float patternNovel    = 0.050f;
    float morphMutation   = 0.025f;
    float morphNovel      = 0.050f;
};

// Which inheritance branch each group took (statistics; never read by the step).
enum Mode : std::uint8_t { kMean = 0, kCopyA = 1, kCopyB = 2, kNovel = 3 };

// A world's construction counters: records built, and how often each branch
// was taken per group (pattern and shape groups summed). Statistics only.
struct Stats {
    std::uint64_t founders = 0, offspring = 0;
    std::uint64_t pattern[4] = {}, morph[4] = {};   // [Mode]
    void count(const std::uint8_t* modes, const std::uint8_t* mmodes, int flags) {
        ++offspring;
        if (flags & kPatterns) for (int k = 0; k < kPatternGroups; ++k) ++pattern[modes[k] & 3];
        if (flags & kShapes) for (int k = 0; k < kMorphGroups; ++k) ++morph[mmodes[k] & 3];
    }
};

// ---- the neutral genome ----------------------------------------------------------
inline constexpr float kNeutralPattern[kPatternGenes] = {
    0.5f, 0.5f, 0.65f, 0.0f, 0.5f, 0.5f, 0.5f, 0.5f, 0.30f, 0.5f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f,
    0.5f, 0.08f, 1.0f, 0.5f, 0.0f, 0.0f, 0.5f, 0.0f, 0.5f, 0.0f, 0.5f, 0.0f, 0.5f, 0.0f, 0.0f};
inline constexpr float kNeutralMorph[kMorphGenes] = {
    0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f,
    0.0f, 0.25f, 0.4f, 0.7f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 0.5f, 0.5f, 0.5f};

inline void setNeutralPattern(Genome& G) {
    for (int j = 0; j < kPatternGenes; ++j) G.g[j] = kNeutralPattern[j];
    G.g[kPhaseU] = 0.0f;
    G.g[kPhaseV] = 0.0f;
}
inline void setNeutralMorph(Genome& G) {
    for (int j = 0; j < kMorphGenes; ++j) G.g[kMorphOffset + j] = kNeutralMorph[j];
}
inline Genome neutral() {
    Genome G;
    setNeutralPattern(G);
    setNeutralMorph(G);
    return G;
}

// ---- the per-fish draw stream ------------------------------------------------------
inline constexpr std::uint64_t kGolden = 0x9E3779B97F4A7C15ull;
inline std::uint64_t mix64(std::uint64_t z) {   // SplitMix64's finalizer
    z ^= z >> 30; z *= 0xBF58476D1CE4E5B9ull;
    z ^= z >> 27; z *= 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
inline std::uint64_t streamBase(std::uint64_t worldKey, std::uint64_t fishId) {
    return mix64(worldKey ^ mix64(fishId + kGolden));
}
// Draw k (0-based) of the stream with this base: a multiple of 2^-24 in [0, 1),
// the same lattice as RandomGen::unit().
inline double streamDraw(std::uint64_t base, unsigned k) {
    const std::uint64_t z = mix64(base + (std::uint64_t)(k + 1u) * kGolden);
    return (double)(std::uint32_t)(z >> 40) * 5.9604644775390625e-8;
}
inline void streamDraws(std::uint64_t worldKey, std::uint64_t fishId, double* r, int n) {
    const std::uint64_t base = streamBase(worldKey, fishId);
    for (int k = 0; k < n; ++k) r[k] = streamDraw(base, (unsigned)k);
}

// ---- construction (gallery v3's law script, operation by operation) --------------
namespace detail {
inline constexpr double TAU = 6.283185307179586;
inline constexpr double PI = 3.141592653589793;     // JS Math.PI
inline constexpr double kTiny = 9.5367431640625e-7; // 2^-20
inline float f32(double x) { return (float)x; }

inline double Sat(double r) {
    return r < 0.30 ? 0.0 : (r < 0.90 ? 0.25 + 0.55 * (r - 0.30) / 0.60 : 0.80 + 0.20 * (r - 0.90) / 0.10);
}
inline double Val(double r) {
    return r < 0.15 ? 0.05 + 0.20 * r / 0.15 : (r < 0.85 ? 0.35 + 0.50 * (r - 0.15) / 0.70 : 0.88 + 0.12 * (r - 0.85) / 0.15);
}
inline double Lmk(double r, double t) { return r < t ? 0.40 * r / t : 0.40 + 0.60 * std::sqrt((r - t) / (1 - t)); }
// C(h, s): a hue/saturation pair as the stored chroma (or orientation) vector.
inline void Cenc(double h, double s, float* out) {
    out[0] = f32(0.5 + 0.5 * s * appmath::cos(TAU * h));
    out[1] = f32(0.5 + 0.5 * s * appmath::sin(TAU * h));
}
// wrap01 of a value first rounded to binary32 (the gallery's wrap01f).
inline float wrap01f(double x) {
    const float xf = f32(x);
    const float y = f32((double)xf - std::floor((double)xf));
    return (y >= 1.0f || y == 0.0f) ? 0.0f : y;
}

// P1 founder 3: 31 genes from r[0..30] (the gallery's founderGenesInto).
inline void patternFounderGenes(const double* r, float* out) {
    Cenc(r[0], Sat(r[1]), out + 0);
    const double g2 = (double)f32(Val(r[2]));
    out[2] = (float)g2;
    out[3] = f32(r[3] < 0.25 ? 0.0 : 0.30 + 0.65 * (r[3] - 0.25) / 0.75);
    out[4] = f32(0.35 + 0.30 * r[4]);
    out[5] = f32(r[5] < 0.85 ? 0.5 : 0.5 + 0.12 * (2 * (r[5] - 0.85) / 0.15 - 1));
    out[6] = f32(r[6] < 0.80 ? 0.5 : 0.5 + 0.20 * (2 * (r[6] - 0.80) / 0.20 - 1));
    out[7] = f32(0.12 + 0.76 * r[7]);
    out[8] = f32(r[8] < 0.95 ? 0.05 + 0.65 * r[8] / 0.95 : 0.85 + 0.15 * (r[8] - 0.95) / 0.05);
    const double r9 = r[9], r10 = r[10];
    double th;
    if (r9 < 0.40) th = PI / 2 + 0.04 * PI * (2 * r9 / 0.40 - 1);
    else if (r9 < 0.80) th = 0.04 * PI * (2 * (r9 - 0.40) / 0.40 - 1);
    else th = PI * (r9 - 0.80) / 0.20;
    const double an = r10 < 0.35 ? 0.0 : (r10 < 0.50 ? 0.20 + 0.50 * (r10 - 0.35) / 0.15 : 0.95 + 0.05 * (r10 - 0.50) / 0.50);
    Cenc(th / PI, an, out + 9);
    out[11] = f32(r[11]);
    out[12] = f32(r[12] < 0.85 ? 0.0 : 0.20 + 0.80 * (r[12] - 0.85) / 0.15);
    if (r[13] < 0.65) out[13] = 0.0f;
    else { const double x = (r[13] - 0.65) / 0.35; out[13] = f32(x * x); }   // Math.pow(x, 2) == x·x on every draw
    out[14] = f32(r[14] < 0.25 ? 0.0 : 0.775 + 0.225 * (r[14] - 0.25) / 0.75);
    Cenc(r[15], Sat(r[16]), out + 15);
    out[17] = f32(r[17] < 0.70 ? 0.015 + 0.20 * (r[17] / 0.70) * g2 : 0.65 + 0.35 * (r[17] - 0.70) / 0.30);
    out[18] = f32(r[18] < 0.20 ? 0.28 + 0.30 * r[18] / 0.20 : 1.0);
    out[19] = f32(r[19] < 0.85 ? 0.5 : 0.25 + 0.50 * (r[19] - 0.85) / 0.15);
    out[20] = f32(r[20] < 0.85 ? 0.0 : 0.35 + 0.65 * (r[20] - 0.85) / 0.15);
    out[21] = f32(r[21] < 0.94 ? 0.0 : 0.70 + 0.30 * (r[21] - 0.94) / 0.06);
    out[22] = f32(0.25 + 0.55 * r[22]);
    out[23] = f32(Lmk(r[23], 0.90));
    out[24] = f32(0.20 + 0.60 * r[24]);
    out[25] = f32(Lmk(r[25], 0.90));
    out[26] = f32(0.25 + 0.50 * r[26]);
    out[27] = f32(Lmk(r[27], 0.90));
    out[28] = f32(0.35 + 0.50 * r[28]);
    out[29] = f32(r[29] < 0.50 ? 0.0 : r[29]);
    out[30] = f32(Lmk(r[30], 0.75));
}

// M1 founder 1: 30 genes from r[0..29] (morphFounderInto).
inline double Nf(double r) { return 0.5 + 0.35 * appmath::cubeLattice(2 * r - 1); }
inline double Bf(double r) { return 0.15 + 0.70 * r; }
inline double Df(double r) { return r < 0.85 ? 0.30 + 0.40 * r / 0.85 : (r - 0.85) / 0.15; }
inline void morphFounderGenes(const double* r, float* out) {
    const double a = 2 * r[0] - 1, b = 2 * r[1] - 1;
    out[0] = f32(0.5 + 0.28 * a + 0.16 * b);
    out[1] = f32(0.5 + 0.28 * a - 0.16 * b);
    for (int j = 2; j < kMorphGenes; ++j) {
        const double x = r[j];
        double v;
        switch (j) {
        case 2: case 3: case 4: case 5: case 11: case 17: case 19: case 20: case 22: case 29: v = Nf(x); break;
        case 6: case 10: case 12: v = Df(x); break;
        case 7: case 8: case 13: case 14: case 21: case 24: case 25: case 27: v = Bf(x); break;
        case 9: case 28: v = x; break;
        case 15: v = x < 0.70 ? 0.0 : (x - 0.70) / 0.30; break;
        case 16: v = 0.10 + 0.45 * x; break;
        case 18: v = 0.55 + 0.35 * x; break;
        case 23: v = 0.05 + 0.45 * x; break;
        default: /* 26 */ v = x < 0.80 ? 0.20 * x / 0.80 : 0.20 + 0.80 * (x - 0.80) / 0.20; break;
        }
        out[j] = f32(v);
    }
}

// The 25 pattern groups and 11 morphology groups, in draw order.
inline constexpr std::uint8_t kPGroupStart[kPatternGroups + 1] = {
    0, 3, 4, 5, 6, 7, 8, 9, 11, 13, 14, 15, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
inline constexpr std::uint8_t kPGroupGenes[kPatternGenes] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30};
inline constexpr std::uint8_t kMGroupStart[kMorphGroups + 1] = {0, 4, 6, 8, 9, 12, 16, 19, 23, 26, 29, 30};
// (both group lists are runs of consecutive genes: group k is genes [start[k], start[k+1]))

// The cross pair's mean (crossMeanInto, t = ½): weighted axial vector.
inline void crossMean(const float* A, const float* B, float* Q) {
    const double t = 0.5;
    const double wA = A[12], wB = B[12], aA = A[11], aB = B[11];
    const float cx = f32((1 - t) * wA * appmath::cos(TAU * aA) + t * wB * appmath::cos(TAU * aB));
    const float cy = f32((1 - t) * wA * appmath::sin(TAU * aA) + t * wB * appmath::sin(TAU * aB));
    const float L = f32(std::sqrt((double)cx * cx + (double)cy * cy));
    if ((double)L <= kTiny) { Q[12] = 0.0f; Q[11] = 0.0f; }
    else {
        Q[12] = f32(std::fmin(1.0, (double)L));
        Q[11] = wrap01f(appmath::atan2((double)cy, (double)cx) / TAU);
    }
}
// A colour group's mean (colourBlendInto, t = ½, "satw"): the saturation-weighted
// vector mean of the chroma pair j, j+1; the value j+2 by A + ½(B − A).
inline void colourMean(const float* A, const float* B, float* Q, int j) {
    const double t = 0.5;
    const double ax = f32(2.0 * A[j] - 1), ay = f32(2.0 * A[j + 1] - 1);
    const double bx = f32(2.0 * B[j] - 1), by = f32(2.0 * B[j + 1] - 1);
    double cx, cy;
    const double sa = f32(std::sqrt(ax * ax + ay * ay)), sb = f32(std::sqrt(bx * bx + by * by));
    const double wa = f32((1 - t) * sa), wb = f32(t * sb), ws = f32(wa + wb);
    if (ws > kTiny) { cx = f32((wa * ax + wb * bx) / ws); cy = f32((wa * ay + wb * by) / ws); }
    else { cx = 0.0; cy = 0.0; }
    Q[j] = f32(0.5 + (double)f32(0.5 * cx));
    Q[j + 1] = f32(0.5 + (double)f32(0.5 * cy));
    Q[j + 2] = f32((double)A[j + 2] + t * ((double)B[j + 2] - (double)A[j + 2]));
}
// projInto: a stored 2-vector back onto the unit disk.
inline void project(float* Q, int o) {
    float x = f32(2.0 * Q[o] - 1), y = f32(2.0 * Q[o + 1] - 1);
    const float r2 = f32((double)f32((double)x * x) + (double)f32((double)y * y));
    if (r2 > 1.0f) {
        const float L = f32(std::sqrt((double)r2));
        x = f32((double)x / L);
        y = f32((double)y / L);
        Q[o] = f32(0.5 + (double)f32(0.5 * (double)x));
        Q[o + 1] = f32(0.5 + (double)f32(0.5 * (double)y));
    }
}
inline int selectMode(double s, double p) {
    const double mp = 0.20, cA = mp + (1 - mp - p) / 2, cB = 1 - p;
    return s < mp ? kMean : (s < cA ? kCopyA : (s < cB ? kCopyB : kNovel));
}
}  // namespace detail

// A founder's genome from its 63 draws (r[0..62]); flags = which parts are on.
inline void founderFromDraws(const double* r, int flags, Genome& G) {
    if (flags & kPatterns) {
        detail::patternFounderGenes(r, G.g);
        G.g[kPhaseU] = detail::f32(r[31]);
        G.g[kPhaseV] = detail::f32(r[32]);
    } else {
        setNeutralPattern(G);
    }
    if (flags & kShapes) detail::morphFounderGenes(r + 33, G.g + kMorphOffset);
    else setNeutralMorph(G);
}

// An offspring's genome from parents A (the maternal source: lineage and
// phases) and B and its 160 draws (r[0..159]). modes / mmodes (optional)
// receive each group's branch.
inline void offspringFromDraws(const Genome& GA, const Genome& GB, const double* D, const Inheritance& cfg,
                               int flags, Genome& Q, std::uint8_t* modes = nullptr, std::uint8_t* mmodes = nullptr) {
    using namespace detail;
    const float* A = GA.g;
    const float* B = GB.g;
    float* q = Q.g;
    if (flags & kPatterns) {
        float fresh[kPatternGenes];
        patternFounderGenes(D + 25, fresh);
        const double p = cfg.patternNovel;
        for (int gi = 0; gi < kPatternGroups; ++gi) {
            const int j0 = kPGroupStart[gi], j1 = kPGroupStart[gi + 1];
            const int mode = selectMode(D[gi], p);
            if (modes) modes[gi] = (std::uint8_t)mode;
            bool same = true;
            for (int j = j0; j < j1; ++j) if (A[j] != B[j]) { same = false; break; }
            if (mode == kCopyA || (mode == kMean && same)) for (int j = j0; j < j1; ++j) q[j] = A[j];
            else if (mode == kCopyB) for (int j = j0; j < j1; ++j) q[j] = B[j];
            else if (mode == kNovel) for (int j = j0; j < j1; ++j) q[j] = fresh[j];
            else if (gi == 8) crossMean(A, B, q);
            else if (gi == 0 || gi == 11) colourMean(A, B, q, j0);
            else for (int j = j0; j < j1; ++j) q[j] = f32(((double)A[j] + (double)B[j]) * 0.5);
        }
        const float m = cfg.patternMutation, m2 = f32(2.0 * m);
        for (int j = 0; j < kPatternGenes; ++j) {
            const float jit = f32(-(double)m + (double)f32((double)m2 * D[56 + j]));
            float x = f32((double)q[j] + (double)jit);
            if (j == 11) x = wrap01f(x);
            else x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
            q[j] = x;
        }
        project(q, 0);
        project(q, 9);
        project(q, 15);
        const float pm = cfg.phaseMutation, pm2 = f32(2.0 * pm);
        q[kPhaseU] = wrap01f((double)A[kPhaseU] + (double)f32(-(double)pm + (double)f32((double)pm2 * D[87])));
        q[kPhaseV] = wrap01f((double)A[kPhaseV] + (double)f32(-(double)pm + (double)f32((double)pm2 * D[88])));
    } else {
        setNeutralPattern(Q);
        if (modes) for (int gi = 0; gi < kPatternGroups; ++gi) modes[gi] = 0xFF;
    }
    if (flags & kShapes) {
        const double* M = D + kP1OffspringDraws;
        const float* Am = A + kMorphOffset;
        const float* Bm = B + kMorphOffset;
        float* qm = q + kMorphOffset;
        float fresh[kMorphGenes];
        morphFounderGenes(M + kMorphGroups, fresh);
        const double p = cfg.morphNovel;
        for (int gi = 0; gi < kMorphGroups; ++gi) {
            const int j0 = kMGroupStart[gi], j1 = kMGroupStart[gi + 1];
            const int mode = selectMode(M[gi], p);
            if (mmodes) mmodes[gi] = (std::uint8_t)mode;
            bool same = true;
            for (int j = j0; j < j1; ++j) if (Am[j] != Bm[j]) { same = false; break; }
            if (mode == kCopyA || (mode == kMean && same)) for (int j = j0; j < j1; ++j) qm[j] = Am[j];
            else if (mode == kCopyB) for (int j = j0; j < j1; ++j) qm[j] = Bm[j];
            else if (mode == kNovel) for (int j = j0; j < j1; ++j) qm[j] = fresh[j];
            else for (int j = j0; j < j1; ++j) qm[j] = f32(((double)Am[j] + (double)Bm[j]) * 0.5);
        }
        const float m = cfg.morphMutation, m2 = f32(2.0 * m);
        for (int j = 0; j < kMorphGenes; ++j) {
            const float x = f32((double)qm[j] + (double)f32(-(double)m + (double)f32((double)m2 * M[kMorphGroups + kMorphGenes + j])));
            qm[j] = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
        }
    } else {
        setNeutralMorph(Q);
        if (mmodes) for (int gi = 0; gi < kMorphGroups; ++gi) mmodes[gi] = 0xFF;
    }
}

// The engine's entry points: the same functions, fed by the fish's own stream.
inline void makeFounder(std::uint64_t worldKey, std::uint64_t fishId, int flags, Genome& G) {
    double r[kFounderDraws];
    streamDraws(worldKey, fishId, r, kFounderDraws);
    founderFromDraws(r, flags, G);
}
inline void makeOffspring(std::uint64_t worldKey, std::uint64_t childId, const Genome& A, const Genome& B,
                          const Inheritance& cfg, int flags, Genome& Q, std::uint8_t* modes = nullptr,
                          std::uint8_t* mmodes = nullptr) {
    double r[kOffspringDraws];
    streamDraws(worldKey, childId, r, kOffspringDraws);
    offspringFromDraws(A, B, r, cfg, flags, Q, modes, mmodes);
}

// Every stored float finite and in range: [0, 1] (crossAngle and the phases
// [0, 1)), the chroma / orientation vectors within the unit disk up to the
// design's rounding allowance (|v|² <= 1 + 2^-20).
inline bool genomeValid(const Genome& G) {
    for (int j = 0; j < kStride; ++j) {
        const float v = G.g[j];
        if (!(v >= 0.0f && v <= 1.0f)) return false;
        if ((j == 11 || j == kPhaseU || j == kPhaseV) && !(v < 1.0f)) return false;
    }
    for (int o : {0, 9, 15}) {
        const double x = 2.0 * G.g[o] - 1, y = 2.0 * G.g[o + 1] - 1;
        if (x * x + y * y > 1.0 + 9.5367431640625e-7) return false;
    }
    return true;
}

}  // namespace appearance
