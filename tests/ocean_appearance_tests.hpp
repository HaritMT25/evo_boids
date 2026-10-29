#pragma once
// ocean_appearance_tests.hpp — the appearance genome's unit checks
// (src/ocean/appearance_model.hpp), included by tests/ocean_tests.cpp just
// before main (namespace ot: Test, g_passed / g_failed are that file's).
//
//   appear_gallery   the C++ construction reproduces gallery v3's own law
//                    script: its exported construction vectors (4 founders,
//                    4 offspring with their 160 draws and 36 branches), every
//                    float exact, and the hashes of long runs (founders, then
//                    random mating under default / zero / max / mixed
//                    settings: ~400k offspring), from
//                    tests/ocean_appearance_vectors.txt (tools/
//                    appearance_vectors.mjs)
//   appear_stream    the per-fish stream: the 2^-24 lattice, uniform, a pure
//                    function of (world key, id, k); construction never
//                    touches `rng`
//   appear_build     draw-to-slot assignment, the four branches, axial /
//                    perpendicular orientation means, cross-pair means,
//                    opposed-chroma cancellation, identical-parent bit
//                    identity, maternal phases, clamp / wrap / projection,
//                    branch rates at the default settings
//   appear_owner     the side array through births, deaths, predators,
//                    compaction and reseeding; a direct append / reorder is
//                    detected; attachFounderAppearance keeps an append bound
//   appear_neutral   appearance on vs off, and different inheritance
//                    settings: every simulation section and the rng equal at
//                    every step (pairing, predators, E1, the eye build's eye)
//   appear_divers    no collapse of variety under random mating: the median
//                    spread ratio (gene SD / founder SD) stays >= .50 at
//                    generation 8 and >= .35 at generation 40
//   appear_hash      one ulp in any stored float, an owner id, a knob: the
//                    app section changes, the simulation sections do not
//   appear_eval      the C++ reference of the pattern law (appearance_eval.hpp)
//                    against the gallery's CPU law at 3,072 flank sites
//   appear_shim      oc_creature_appearance's rows, the live-key rules and
//                    the refused values (with the shim only)
#include <fstream>
#include <map>
#include <sstream>
#include "../src/ocean/appearance_model.hpp"
#include "../src/ocean/appearance_eval.hpp"

// Private state for the hash-sensitivity check (the fingerprint header's peek).
namespace ofp::peek {
struct OwAppTest { friend constexpr auto member(OwAppTest) noexcept; };
template struct Expose<OwAppTest, &OceanWorld::app_>;
}  // namespace ofp::peek

namespace ot {
namespace app {
using namespace appearance;
using Genome = appearance::Genome;   // (the neural genome is the global ::Genome)
inline std::uint32_t fb(float f) { std::uint32_t u; std::memcpy(&u, &f, 4); return u; }
inline float bf(std::uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
struct H32 {
    std::uint32_t a = 0x811c9dc5u, b = 0x01234567u;
    void add(std::uint32_t u) { a = (a ^ u) * 16777619u; b = b * 0x9E3779B1u + u; }
    std::string str() const { char s[24]; std::snprintf(s, sizeof s, "%08x%08x", a, b); return s; }
};
inline bool sameGenome(const Genome& x, const Genome& y) {
    for (int j = 0; j < kStride; ++j) if (fb(x.g[j]) != fb(y.g[j])) return false;
    return true;
}
inline bool simEqual(const ofp::Fingerprint& a, const ofp::Fingerprint& b) {
    for (auto s : ofp::kSections) if (a.*s != b.*s) return false;
    return a.all == b.all;
}
inline Genome randomGenome(RandomGen& g) {   // a founder from an MT stream (any valid genome would do)
    double r[kFounderDraws];
    for (double& x : r) x = g.unit();
    Genome G;
    founderFromDraws(r, kAll, G);
    return G;
}
}  // namespace app

inline void testAppearanceGallery() {
    using namespace app;
    Test t("appear_gallery");
    std::ifstream in("tests/ocean_appearance_vectors.txt");
    if (!t.expect((bool)in, "tests/ocean_appearance_vectors.txt not found (run from the repo root)")) return;
    std::string line;
    int nFounder = 0, nOff = 0, nRuns = 0, nGens = 0;
    long floats = 0;
    RandomGen founders;
    founders.seed(20261028);   // FL.constructionFixtures(): founders from MT(20261028)
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream is(line);
        std::string tag;
        is >> tag;
        auto rd = [&](float* f, int n) { for (int i = 0; i < n; ++i) { std::string h; is >> h; f[i] = bf((std::uint32_t)std::stoul(h, nullptr, 16)); } };
        if (tag == "G") {
            std::string kind;
            is >> kind;
            if (kind == "founder") {
                Genome want, got;
                rd(want.g, kStride);
                double r[kFounderDraws];
                for (double& x : r) x = founders.unit();
                founderFromDraws(r, kAll, got);
                for (int j = 0; j < kStride; ++j, ++floats)
                    t.expect(fb(got.g[j]) == fb(want.g[j]), "gallery founder %d gene %d: %08x, the gallery %08x", nFounder, j,
                             fb(got.g[j]), fb(want.g[j]));
                ++nFounder;
            } else {
                float c[5];
                rd(c, 5);
                const Inheritance inh{c[0], c[1], c[2], c[3], c[4]};
                Genome A, B, want, got;
                rd(A.g, kStride);
                rd(B.g, kStride);
                double D[kOffspringDraws];
                for (double& x : D) { long k; is >> k; x = (double)k * 5.9604644775390625e-8; }
                rd(want.g, kStride);
                int wm[36];
                for (int& m : wm) is >> m;
                std::uint8_t m1[kPatternGroups], m2[kMorphGroups];
                offspringFromDraws(A, B, D, inh, kAll, got, m1, m2);
                for (int j = 0; j < kStride; ++j, ++floats)
                    t.expect(fb(got.g[j]) == fb(want.g[j]), "gallery offspring %d gene %d: %08x, the gallery %08x", nOff, j,
                             fb(got.g[j]), fb(want.g[j]));
                for (int k = 0; k < 36; ++k)
                    t.expect((k < 25 ? m1[k] : m2[k - 25]) == wm[k], "gallery offspring %d group %d branch", nOff, k);
                ++nOff;
            }
        } else if (tag == "H") {
            std::string name;
            unsigned seed;
            int N, gens;
            is >> name >> seed >> N >> gens;
            float c[5];
            rd(c, 5);
            const Inheritance inh{c[0], c[1], c[2], c[3], c[4]};
            std::vector<std::string> want((std::size_t)gens + 1);
            for (auto& w : want) is >> w;
            RandomGen g;
            g.seed(seed);
            std::vector<Genome> pop((std::size_t)N), next((std::size_t)N);
            std::vector<std::uint8_t> modes((std::size_t)N * 36);
            auto hashPop = [&](bool withModes) {
                H32 h;
                for (int i = 0; i < N; ++i) {
                    for (float v : pop[(std::size_t)i].g) h.add(fb(v));
                    if (withModes) for (int k = 0; k < 36; ++k) h.add(modes[(std::size_t)i * 36 + k]);
                }
                return h.str();
            };
            for (auto& G : pop) G = randomGenome(g);
            t.expect(hashPop(false) == want[0], "run %s: the founders' hash differs", name.c_str());
            for (int gen = 1; gen <= gens; ++gen) {
                for (int i = 0; i < N; ++i) {
                    const std::uint32_t a = g.uint((std::uint32_t)N - 1), b = g.uint((std::uint32_t)N - 1);
                    double D[kOffspringDraws];
                    for (double& x : D) x = g.unit();
                    offspringFromDraws(pop[a], pop[b], D, inh, kAll, next[(std::size_t)i], &modes[(std::size_t)i * 36],
                                       &modes[(std::size_t)i * 36 + 25]);
                }
                pop.swap(next);
                ++nGens;
                if (!t.expect(hashPop(true) == want[(std::size_t)gen], "run %s generation %d: the hash differs", name.c_str(), gen)) break;
            }
            ++nRuns;
        }
    }
    t.expect(nFounder == 4 && nOff == 4 && nRuns >= 6, "the vector file is incomplete (%d founders, %d offspring, %d runs)",
             nFounder, nOff, nRuns);
    t.note("gallery vectors: %d founders + %d offspring, %ld floats exact; %d long runs, %d generations hash-equal",
           nFounder, nOff, floats, nRuns, nGens);
}

inline void testAppearanceStream() {
    using namespace app;
    Test t("appear_stream");
    // the lattice, uniform moments, and a pure function of (key, id, k)
    double s = 0, s2 = 0;
    long n = 0, offLattice = 0, outOfRange = 0;
    for (std::uint64_t id = 1; id <= 4000; ++id) {
        const std::uint64_t base = streamBase(0x1234567890ABCDEFull, id);
        for (unsigned k = 0; k < kOffspringDraws; ++k) {
            const double d = streamDraw(base, k);
            offLattice += (d * 16777216.0 != std::floor(d * 16777216.0));
            outOfRange += !(d >= 0.0 && d < 1.0);
            s += d; s2 += d * d; ++n;
        }
    }
    const double mean = s / n, var = s2 / n - mean * mean;
    t.expect(offLattice == 0 && outOfRange == 0, "draws off the 2^-24 lattice (%ld) or out of [0, 1) (%ld)", offLattice, outOfRange);
    t.expect(std::fabs(mean - 0.5) < 0.002 && std::fabs(var - 1.0 / 12.0) < 0.001, "draw moments %.5f %.5f", mean, var);
    // neighbouring ids and keys give unrelated streams (lag-0 correlation)
    double c = 0;
    for (std::uint64_t id = 1; id <= 20000; ++id)
        c += (streamDraw(streamBase(7, id), 0) - 0.5) * (streamDraw(streamBase(7, id + 1), 0) - 0.5);
    t.expect(std::fabs(c / 20000 * 12.0) < 0.05, "consecutive ids correlate: %.4f", c / 20000 * 12.0);
    t.expect(streamDraw(streamBase(1, 5), 3) != streamDraw(streamBase(2, 5), 3), "the key does not change the stream");
    // construction does not touch the simulation's rng
    rng.seed(77);
    const RandomGen before = rng;
    Genome G, Q;
    makeFounder(99, 5, kAll, G);
    makeOffspring(99, 6, G, G, Inheritance{}, kAll, Q);
    t.expect(sameEngine(before, rng), "makeFounder / makeOffspring advanced the simulation's rng");
    // the world key is read from a copy: init draws exactly what it drew before the key existed
    OceanWorld w1, w2;
    std::string err;
    ofp::initWorld(w1, 5, 1920, 1080, 240.0f, 300, R"({"appearance":3})", err);
    const RandomGen after1 = rng;
    ofp::initWorld(w2, 5, 1920, 1080, 240.0f, 300, R"({"appearance":0})", err);
    t.expect(sameEngine(after1, rng), "init with appearance on left the engine elsewhere than with it off");
    t.expect(w1.appearanceKey() == w2.appearanceKey() && w1.appearanceKey() != 0, "the world key depends on the flag");
    OceanWorld w3;
    ofp::initWorld(w3, 6, 1920, 1080, 240.0f, 300, nullptr, err);
    t.expect(w3.appearanceKey() != w1.appearanceKey(), "seeds 5 and 6 share a world key");
    // every founder record is the pure function of (key, id)
    bool pure = true;
    for (const auto& r : w1.appearanceRecords()) {
        Genome want;
        makeFounder(w1.appearanceKey(), r.ownerId, kAll, want);
        pure = pure && sameGenome(want, r.genome);
    }
    t.expect(pure && w1.appearanceRecords().size() == 300, "founder records are not makeFounder(key, id)");
}

inline void testAppearanceBuild() {
    using namespace app;
    Test t("appear_build");
    RandomGen g;
    g.seed(4242);
    // draw-to-slot: founder draw 33 + j moves only shape gene j (0 / 1 share draws 33, 34)
    {
        double r[kFounderDraws];
        for (double& x : r) x = g.unit();
        Genome base;
        founderFromDraws(r, kAll, base);
        int stray = 0;
        for (int k = 33; k < kFounderDraws; ++k) {
            double r2[kFounderDraws];
            std::memcpy(r2, r, sizeof r);
            r2[k] = r[k] < 0.5 ? r[k] + 0.37 : r[k] - 0.37;
            Genome G;
            founderFromDraws(r2, kAll, G);
            const int j = k - 33;
            for (int q = 0; q < kStride; ++q) {
                const bool may = (q == kMorphOffset + j) || (j < 2 && (q == kMorphOffset || q == kMorphOffset + 1));
                if (!may && fb(G.g[q]) != fb(base.g[q])) ++stray;
            }
        }
        // the two phases: draws 31, 32 exactly
        double r3[kFounderDraws];
        std::memcpy(r3, r, sizeof r);
        r3[31] = 0.125; r3[32] = 0.625;
        Genome P;
        founderFromDraws(r3, kAll, P);
        t.expect(stray == 0, "a shape draw moved %d other genes", stray);
        t.expect(P.g[kPhaseU] == 0.125f && P.g[kPhaseV] == 0.625f, "phases are not draws 31, 32");
        // flags: patterns off -> the neutral prefix; shapes off -> neutral shapes; slots unchanged
        Genome onlyShapes, onlyPatterns;
        founderFromDraws(r, kShapes, onlyShapes);
        founderFromDraws(r, kPatterns, onlyPatterns);
        bool ok = true;
        for (int q = 0; q < kStride; ++q) {
            const bool morph = q >= kMorphOffset;
            const float neutralQ = morph ? kNeutralMorph[q - kMorphOffset] : (q < kPatternGenes ? kNeutralPattern[q] : 0.0f);
            ok = ok && fb(onlyShapes.g[q]) == fb(morph ? base.g[q] : neutralQ);
            ok = ok && fb(onlyPatterns.g[q]) == fb(morph ? neutralQ : base.g[q]);
        }
        t.expect(ok, "a part switched off is not exactly neutral, or the other part moved");
    }
    // the four branches with zero jitter (jitter draw .5 gives exactly 0)
    {
        const Genome A = randomGenome(g), B = randomGenome(g);
        double D[kOffspringDraws];
        for (double& x : D) x = g.unit();
        for (int j = 0; j < kPatternGenes; ++j) D[56 + j] = 0.5;
        for (int j = 0; j < kMorphGenes; ++j) D[kP1OffspringDraws + kMorphGroups + kMorphGenes + j] = 0.5;
        Inheritance inh;   // defaults
        Genome fresh;
        {
            double r[kFounderDraws] = {};
            for (int j = 0; j < 31; ++j) r[j] = D[25 + j];
            for (int j = 0; j < 30; ++j) r[33 + j] = D[kP1OffspringDraws + kMorphGroups + j];
            founderFromDraws(r, kAll, fresh);
        }
        const double sel[4] = {0.1, 0.3, 0.7, 0.99};   // mean, copy A, copy B, novel
        for (int m = 0; m < 4; ++m) {
            for (int gi = 0; gi < kPatternGroups; ++gi) D[gi] = sel[m];
            for (int gi = 0; gi < kMorphGroups; ++gi) D[kP1OffspringDraws + gi] = sel[m];
            Genome Q;
            std::uint8_t m1[kPatternGroups], m2[kMorphGroups];
            offspringFromDraws(A, B, D, inh, kAll, Q, m1, m2);
            bool modesOk = true;
            for (auto x : m1) modesOk = modesOk && x == m;
            for (auto x : m2) modesOk = modesOk && x == m;
            t.expect(modesOk, "selector %.2f did not give branch %d everywhere", sel[m], m);
            int bad = 0;
            for (int j = 0; j < kStride; ++j) {
                if (j == kPhaseU || j == kPhaseV) continue;
                const bool vec = j <= 1 || j == 9 || j == 10 || j == 15 || j == 16;   // projected pairs
                float want;
                if (m == kCopyA) want = A.g[j];
                else if (m == kCopyB) want = B.g[j];
                else if (m == kNovel) want = fresh.g[j];
                else {
                    const bool special = j <= 2 || j == 11 || j == 12 || (j >= 15 && j <= 17);
                    if (special) continue;   // the colour and cross means: checked below
                    want = (float)(((double)A.g[j] + (double)B.g[j]) * 0.5);
                }
                if (vec && m != kMean) {   // copies of a valid vector are inside the disk: unchanged by projection
                    if (fb(Q.g[j]) != fb(want)) ++bad;
                } else if (fb(Q.g[j]) != fb(want)) ++bad;
            }
            t.expect(bad == 0, "branch %d: %d genes are not the branch's value", m, bad);
            t.expect(Q.g[kPhaseU] == A.g[kPhaseU] || std::fabs(Q.g[kPhaseU] - A.g[kPhaseU]) <= inh.phaseMutation + 1e-6f,
                     "phaseU is not A's (± the drift)");
        }
    }
    // orientation means: 90° and 270° stripes share an encoding; perpendicular normals cancel
    {
        Genome A = neutral(), B = neutral();
        A.g[9] = 0.0f; A.g[10] = 0.5f;   // horizontal stripes: θ = π/2 -> (cos 2θ, sin 2θ) = (−1, 0)
        B.g[9] = 1.0f; B.g[10] = 0.5f;   // vertical bars: θ = 0 -> (1, 0)
        double D[kOffspringDraws] = {};
        for (double& x : D) x = 0.5;
        for (int gi = 0; gi < kPatternGroups; ++gi) D[gi] = 0.1;   // every group: the mean
        Inheritance zero{0, 0, 0, 0, 0};
        Genome Q;
        offspringFromDraws(A, B, D, zero, kAll, Q);
        t.expect(Q.g[9] == 0.5f && Q.g[10] == 0.5f, "stripes x bars do not cancel to isotropy (%g, %g)", Q.g[9], Q.g[10]);
        offspringFromDraws(A, A, D, zero, kAll, Q);
        t.expect(Q.g[9] == 0.0f && Q.g[10] == 0.5f, "stripes x the same stripes moved");
        // the cross pair: an absent carrier (w 0) does not steer an active one
        Genome C = neutral(), E = neutral();
        C.g[11] = 0.25f; C.g[12] = 0.8f;
        E.g[11] = 0.9f;  E.g[12] = 0.0f;
        offspringFromDraws(C, E, D, zero, kAll, Q);
        t.expect(std::fabs(Q.g[11] - 0.25f) < 1e-6f && std::fabs(Q.g[12] - 0.4f) < 1e-6f, "absent x active cross pair: %g %g", Q.g[11], Q.g[12]);
        // perpendicular axial angles (a and a + 1/2 in the doubled angle) with equal weight cancel
        E.g[11] = 0.75f; E.g[12] = 0.8f;
        offspringFromDraws(C, E, D, zero, kAll, Q);
        t.expect(Q.g[11] == 0.0f && Q.g[12] == 0.0f, "perpendicular cross carriers do not cancel: %g %g", Q.g[11], Q.g[12]);
        // opposed chroma vectors cancel to grey; achromatic parents stay achromatic
        Genome Y = neutral(), N2 = neutral();
        Y.g[0] = 1.0f; Y.g[1] = 0.5f;    // hue 0, saturation 1
        N2.g[0] = 0.0f; N2.g[1] = 0.5f;  // hue 1/2, saturation 1
        offspringFromDraws(Y, N2, D, zero, kAll, Q);
        t.expect(Q.g[0] == 0.5f && Q.g[1] == 0.5f, "opposed chroma does not cancel: %g %g", Q.g[0], Q.g[1]);
        offspringFromDraws(neutral(), Y, D, zero, kAll, Q);
        t.expect(Q.g[0] == 1.0f && Q.g[1] == 0.5f, "an achromatic parent greys the chromatic one (satw keeps its hue): %g %g", Q.g[0], Q.g[1]);
    }
    // identical parents, every mutation knob 0: the child is the parent, bit for bit, in every branch
    {
        Inheritance zero{0, 0, 0, 0, 0};
        int differ = 0;
        for (int trial = 0; trial < 2000; ++trial) {
            const Genome A = randomGenome(g);
            double D[kOffspringDraws];
            for (double& x : D) x = g.unit();
            Genome Q;
            offspringFromDraws(A, A, D, zero, kAll, Q);
            differ += !sameGenome(A, Q);
        }
        t.expect(differ == 0, "%d of 2000 identical-parent children differ from the parent", differ);
    }
    // maternal phases: A's, ± phaseMutation (B's never)
    {
        Genome A = randomGenome(g), B = randomGenome(g);
        A.g[kPhaseU] = 0.2f; A.g[kPhaseV] = 0.7f;
        B.g[kPhaseU] = 0.9f; B.g[kPhaseV] = 0.1f;
        Inheritance inh;
        float worst = 0;
        for (int trial = 0; trial < 5000; ++trial) {
            double D[kOffspringDraws];
            for (double& x : D) x = g.unit();
            Genome Q;
            offspringFromDraws(A, B, D, inh, kAll, Q);
            worst = std::max({worst, std::fabs(Q.g[kPhaseU] - 0.2f), std::fabs(Q.g[kPhaseV] - 0.7f)});
        }
        t.expect(worst <= inh.phaseMutation + 1e-6f, "a phase moved %.5f from parent A's (drift %.3f)", worst, inh.phaseMutation);
    }
    // the largest settings: every gene stays valid (clamp, crossAngle wrap, disk projection)
    {
        Inheritance mx{0.25f, 0.25f, 0.25f, 0.25f, 0.25f};
        std::vector<Genome> pop(256);
        for (auto& G : pop) G = randomGenome(g);
        long invalid = 0, wraps = 0;
        for (int gen = 0; gen < 40; ++gen) {
            std::vector<Genome> next(pop.size());
            for (auto& Q : next) {
                const Genome& A = pop[g.uint(255)];
                const Genome& B = pop[g.uint(255)];
                double D[kOffspringDraws];
                for (double& x : D) x = g.unit();
                offspringFromDraws(A, B, D, mx, kAll, Q);
                invalid += !genomeValid(Q);
                wraps += (std::fabs(Q.g[11] - A.g[11]) > 0.5f);
            }
            pop.swap(next);
        }
        t.expect(invalid == 0, "%ld children at the largest settings hold an out-of-range gene", invalid);
        t.expect(wraps > 0, "crossAngle never wrapped at the largest settings");
    }
    // branch rates at the defaults: 20 / 37.5 / 37.5 / 5 % per group (4-sigma binomial bands)
    {
        long cnt[4] = {}, mcnt[4] = {};
        const int trials = 20000;
        for (int trial = 0; trial < trials; ++trial) {
            double D[kOffspringDraws];
            for (double& x : D) x = g.unit();
            Genome Q;
            std::uint8_t m1[kPatternGroups], m2[kMorphGroups];
            const Genome A = neutral();
            offspringFromDraws(A, A, D, Inheritance{}, kAll, Q, m1, m2);
            for (auto x : m1) ++cnt[x];
            for (auto x : m2) ++mcnt[x];
        }
        const double want[4] = {0.20, 0.375, 0.375, 0.05};
        bool ok = true;
        std::string rates;
        for (int m = 0; m < 4; ++m) {
            for (int part = 0; part < 2; ++part) {
                const double nn = part ? (double)trials * kMorphGroups : (double)trials * kPatternGroups;
                const double got = (part ? mcnt[m] : cnt[m]) / nn;
                const double sd = std::sqrt(want[m] * (1 - want[m]) / nn);
                ok = ok && std::fabs(got - want[m]) < 4 * sd;
                char b[48];
                std::snprintf(b, sizeof b, "%s%.4f", (m || part) ? " " : "", got);
                rates += b;
            }
        }
        t.expect(ok, "branch rates off the design: %s", rates.c_str());
        t.note("branch rates (pattern, shape) mean / copy A / copy B / novel:%s", rates.c_str());
    }
}

inline void testAppearanceOwner() {
    using namespace app;
    Test t("appear_owner");
    OceanWorld w;
    std::string err;
    // births, predation, the minPopulation reseed
    const char* cfg = R"({"predatorCount":8,"predatorEconomy":true,"minPopulation":260,"interactCooldown":0.5,"reproCooldown":2})";
    t.expect(ofp::initWorld(w, 23, 1920, 1080, 240.0f, 300, cfg, err), "init: %s", err.c_str());
    long bad = 0;
    long long lastBirths = 0;
    for (int s = 0; s < 4000; ++s) {
        w.update(0.016f);
        if (!w.appearanceConsistent()) ++bad;
        lastBirths = w.totalBirths;
    }
    const auto& st = w.appearanceStats();
    t.expect(bad == 0, "the side array went out of step %ld times", bad);
    t.expect(w.totalBirths > 0 && w.totalDeathsPredation > 0, "the case reached no births (%lld) or predation (%lld)",
             w.totalBirths, w.totalDeathsPredation);
    t.expect((long long)st.offspring == lastBirths, "offspring records %llu != births %lld", (unsigned long long)st.offspring, lastBirths);
    t.expect((long long)st.founders == 300 + w.totalReseeds, "founder records %llu != 300 + reseeds %lld",
             (unsigned long long)st.founders, w.totalReseeds);
    t.note("4000 steps: %lld births, %lld predator deaths, %lld reseeds, %zu records",
           w.totalBirths, w.totalDeathsPredation, w.totalReseeds, w.appearanceRecords().size());
    // a direct append, without the helper: detected at the next step, never re-bound
    {
        OceanWorld v;
        ofp::initWorld(v, 3, 1920, 1080, 240.0f, 50, nullptr, err);
        v.creatures.push_back(v.creatures.back());
        v.creatures.back().id = 999999;
        v.update(0.016f);
        t.expect(v.appearanceBroken() && v.appearanceRecords().empty() && v.appearanceFlags() == 0,
                 "a direct append was not detected");
    }
    // a same-count reorder: detected too
    {
        OceanWorld v;
        ofp::initWorld(v, 3, 1920, 1080, 240.0f, 50, nullptr, err);
        std::swap(v.creatures[3], v.creatures[7]);
        t.expect(!v.appearanceConsistent(), "a same-count reorder looks consistent");
        v.update(0.016f);
        t.expect(v.appearanceBroken(), "a same-count reorder was not detected");
    }
    // attachFounderAppearance keeps a direct append bound (the tests' addFish)
    {
        OceanWorld v;
        ofp::initWorld(v, 3, 1920, 1080, 240.0f, 50, nullptr, err);
        Fish f = v.creatures.back();
        v.creatures.push_back(f);
        v.markFounder(v.creatures.back());
        const RandomGen before = rng;
        v.attachFounderAppearance();
        t.expect(sameEngine(before, rng), "attachFounderAppearance drew from rng");
        v.update(0.016f);
        t.expect(!v.appearanceBroken() && v.appearanceConsistent(), "an attached append is not consistent");
    }
    // init again on the same object: the side array is rebuilt from scratch (ids
    // continue, as WorldCommon::nextId_ does: each record is still the pure
    // function of the key and its owner's id), the counters restart, and the
    // key is the seed's, whatever the object held before
    {
        OceanWorld a, b;
        ofp::initWorld(a, 31, 1920, 1080, 240.0f, 120, nullptr, err);
        for (int s = 0; s < 300; ++s) a.update(0.016f);
        ofp::initWorld(a, 31, 1920, 1080, 240.0f, 120, nullptr, err);
        ofp::initWorld(b, 31, 1920, 1080, 240.0f, 120, nullptr, err);
        bool pure = a.appearanceConsistent() && a.appearanceRecords().size() == 120;
        for (const auto& r : a.appearanceRecords()) {
            Genome want;
            makeFounder(a.appearanceKey(), r.ownerId, kAll, want);
            pure = pure && sameGenome(want, r.genome);
        }
        t.expect(pure && a.appearanceKey() == b.appearanceKey(), "a re-init does not rebuild the side array from the key");
        t.expect(a.appearanceStats().offspring == 0 && a.appearanceStats().founders == 120, "a re-init keeps the old counters");
    }
}

inline void testAppearanceNeutral() {
    using namespace app;
    Test t("appear_neutral");
    struct Case { const char* name; unsigned seed; int pop; const char* on; const char* off; int steps; };
    const Case cases[] = {
        {"pairing", 29, 300, R"({"reproMaxDistance":16,"reproSoftWidth":5,"reproGracePeriod":30,"bodyModel":true})",
                             R"({"reproMaxDistance":16,"reproSoftWidth":5,"reproGracePeriod":30,"bodyModel":true,"appearance":0})", 1500},
        {"predators", 42, 300, R"({"predatorCount":6,"predatorEconomy":true,"predatorMinPopulation":3,"predatorInheritance":true})",
                               R"({"predatorCount":6,"predatorEconomy":true,"predatorMinPopulation":3,"predatorInheritance":true,"appearance":0})", 1500},
        {"e1", 13, 300, R"({"waveSpeed":6,"deepCurrent":true,"currentStrength":40,"foodDrift":true,"sizeDrift":true,"mouthGate":true,"bodyModel":true})",
                        R"({"waveSpeed":6,"deepCurrent":true,"currentStrength":40,"foodDrift":true,"sizeDrift":true,"mouthGate":true,"bodyModel":true,"appearance":0})", 1200},
        {"settings", 7, 300, R"({"patternMutation":0.25,"patternNovelProbability":0.25,"morphologyMutation":0,"patternPhaseMutation":0.25})",
                             R"({"patternMutation":0,"patternNovelProbability":0,"morphologyNovelProbability":0.25,"appearance":1})", 1500},
#ifdef EVO_OCEAN_EYE
        {"eye", 17, 300, R"({"eye":1,"eyeCode":7,"predatorCount":2})", R"({"eye":1,"eyeCode":7,"predatorCount":2,"appearance":0})", 1200},
#endif
    };
    for (const Case& c : cases) {
        // each world in turn from the same seed (the fingerprint's rng section reads the global engine)
        auto run = [&](const char* cfg, std::vector<ofp::Fingerprint>& fps, long long& births) {
            OceanWorld w;
            std::string err;
            if (!t.expect(ofp::initWorld(w, c.seed, 1920, 1080, 240.0f, c.pop, cfg, err), "%s: %s", c.name, err.c_str())) return;
            fps.push_back(ofp::fingerprint(w));
            for (int s = 0; s < c.steps; ++s) { w.update(0.016f); fps.push_back(ofp::fingerprint(w)); }
            births = w.totalBirths;
        };
        std::vector<ofp::Fingerprint> fa, fbv;
        long long ba = 0, bb = 0;
        run(c.on, fa, ba);
        run(c.off, fbv, bb);
        long first = -1;
        bool appDiffers = false;
        for (std::size_t s = 0; s < fa.size() && s < fbv.size(); ++s) {
            if (first < 0 && !simEqual(fa[s], fbv[s])) first = (long)s;
            appDiffers = appDiffers || fa[s].app != fbv[s].app;
        }
        t.expect(fa.size() == fbv.size() && first < 0, "%s: a simulation section differs from step %ld", c.name, first);
        t.expect(appDiffers, "%s: the two worlds' appearance never differed (the case tests nothing)", c.name);
        t.expect(ba > 0 && ba == bb, "%s: births %lld vs %lld", c.name, ba, bb);
        t.note("%s: %d steps, %lld births; all 9 sections, all and rng equal at every step", c.name, c.steps, ba);
    }
}

inline void testAppearanceDiversity() {
    using namespace app;
    Test t("appear_divers");
    const int N = 4096;
    double worst8 = 9, worst40 = 9, mworst8 = 9, mworst40 = 9;
    for (unsigned seed : {1u, 2u, 3u}) {
        std::vector<Genome> pop((std::size_t)N), next((std::size_t)N);
        for (int i = 0; i < N; ++i) makeFounder(seed * 7919u, (std::uint64_t)i + 1, kAll, pop[(std::size_t)i]);
        auto sds = [&](std::vector<double>& out) {
            out.assign(kStride, 0.0);
            for (int j = 0; j < kStride; ++j) {
                double m = 0, v = 0;
                for (const auto& G : pop) m += G.g[j];
                m /= N;
                for (const auto& G : pop) v += (G.g[j] - m) * (G.g[j] - m);
                out[(std::size_t)j] = std::sqrt(v / N);
            }
        };
        std::vector<double> sd0, sd;
        sds(sd0);
        RandomGen pick;
        pick.seed(seed);
        std::uint64_t nextId = N + 1;
        for (int gen = 1; gen <= 40; ++gen) {
            for (int i = 0; i < N; ++i)
                makeOffspring(seed * 7919u, nextId++, pop[pick.uint(N - 1)], pop[pick.uint(N - 1)], Inheritance{}, kAll,
                              next[(std::size_t)i]);
            pop.swap(next);
            if (gen != 8 && gen != 40) continue;
            sds(sd);
            std::vector<double> rp, rm;
            for (int j = 0; j < kPatternGenes; ++j) if (j != 11 && sd0[(std::size_t)j] > 0) rp.push_back(sd[(std::size_t)j] / sd0[(std::size_t)j]);
            for (int j = kMorphOffset; j < kStride; ++j) if (sd0[(std::size_t)j] > 0) rm.push_back(sd[(std::size_t)j] / sd0[(std::size_t)j]);
            std::sort(rp.begin(), rp.end());
            std::sort(rm.begin(), rm.end());
            auto med = [](const std::vector<double>& r) { const std::size_t n = r.size(); return n % 2 ? r[n / 2] : 0.5 * (r[n / 2 - 1] + r[n / 2]); };
            if (gen == 8) { worst8 = std::min(worst8, med(rp)); mworst8 = std::min(mworst8, med(rm)); }
            else { worst40 = std::min(worst40, med(rp)); mworst40 = std::min(mworst40, med(rm)); }
        }
    }
    t.expect(worst8 >= 0.50 && worst40 >= 0.35, "pattern median spread ratio %.3f (gen 8), %.3f (gen 40)", worst8, worst40);
    t.expect(mworst8 >= 0.50 && mworst40 >= 0.35, "shape median spread ratio %.3f (gen 8), %.3f (gen 40)", mworst8, mworst40);
    t.note("median spread ratio, worst of 3 seeds x 4096: patterns %.3f (gen 8) %.3f (gen 40); shapes %.3f %.3f (gallery v3: .734 / .609)",
           worst8, worst40, mworst8, mworst40);
}

inline void testAppearanceHash() {
    using namespace app;
    Test t("appear_hash");
    OceanWorld w;
    std::string err;
    ofp::initWorld(w, 9, 1920, 1080, 240.0f, 40, nullptr, err);
    for (int s = 0; s < 50; ++s) w.update(0.016f);
    const ofp::Fingerprint f0 = ofp::fingerprint(w);
    t.expect(f0.appShown, "appearance on by default is not shown");
    auto& recs = ofp::field<ofp::peek::OwAppTest>(w);
    long missed = 0, leaked = 0;
    for (std::size_t i = 0; i < recs.size(); i += 7)
        for (int j = 0; j < kStride; ++j) {
            float& v = recs[i].genome.g[j];
            const float old = v;
            v = std::nextafter(v, v < 0.5f ? 1.0f : 0.0f);
            const ofp::Fingerprint f1 = ofp::fingerprint(w);
            missed += f1.app == f0.app;
            leaked += !simEqual(f0, f1);
            v = old;
        }
    t.expect(missed == 0, "%ld one-ulp gene changes left the app hash unchanged", missed);
    t.expect(leaked == 0, "%ld appearance changes moved a simulation section", leaked);
    recs[3].ownerId ^= 1;
    t.expect(ofp::fingerprint(w).app != f0.app, "an owner id change is not hashed");
    recs[3].ownerId ^= 1;
    w.ocfg.morphologyMutation = std::nextafter(w.ocfg.morphologyMutation, 1.0f);
    const ofp::Fingerprint f2 = ofp::fingerprint(w);
    t.expect(f2.app != f0.app && f2.cfg == f0.cfg && f2.all == f0.all, "a knob is not hashed in app only");
    t.expect(ofp::fingerprint(w) == f2 && ofp::fingerprint(w).app == f2.app, "fingerprinting is not pure");
}

inline void testAppearanceEval() {
    using namespace app;
    Test t("appear_eval");
    std::ifstream in("tests/ocean_appearance_vectors.txt");
    if (!t.expect((bool)in, "tests/ocean_appearance_vectors.txt not found")) return;
    std::vector<Genome> founders;
    RandomGen g;
    g.seed(777);
    std::string line;
    double worst = 0, sum = 0;
    long n = 0, nonFinite = 0;
    while (std::getline(in, line)) {
        if (line.size() < 2 || line[0] != 'E' || line[1] != ' ') continue;
        std::istringstream is(line.substr(2));
        int i;
        double lam, u, v, want[3];
        is >> i >> lam >> u >> v >> want[0] >> want[1] >> want[2];
        while ((int)founders.size() <= i) founders.push_back(randomGenome(g));
        const PatternDecoded D = decodePattern(founders[(std::size_t)i], lam);
        double got[3];
        flankRGB(D, u, v, got);
        for (int c = 0; c < 3; ++c) {
            if (!std::isfinite(got[c])) ++nonFinite;
            const double e = std::fabs(got[c] - want[c]);
            worst = std::max(worst, e);
            sum += e;
        }
        ++n;
    }
    t.expect(n == 3072, "%ld evaluator samples (want 3072)", n);
    t.expect(nonFinite == 0 && worst <= 1e-6, "the C++ pattern law is %.3g from the gallery's (max), %ld non-finite", worst, nonFinite);
    t.note("C++ pattern law 3 vs the gallery's CPU law: %ld sites, max |RGB error| %.2g, mean %.2g", n, worst, sum / (3.0 * (double)(n ? n : 1)));
}

#ifndef OCEAN_TESTS_NO_SHIM
inline void testAppearanceShim() {
    using namespace app;
    Test t("appear_shim");
    t.expect(oc_init(11, 1920, 1080, 240.0, 200, nullptr) == 1, "oc_init: %s", oc_last_error());
    oc_step(400);
    oc_sync();
    const int n = oc_creature_count();
    t.expect(oc_appearance_abi() == 3 && oc_appearance_schema() == 3 && oc_appearance_stride() == 63 &&
             oc_appearance_pattern_genes() == 31 && oc_appearance_morph_genes() == 30 && oc_appearance_morph_offset() == 33 &&
             oc_pattern_law() == 3 && oc_morphology_law() == 2 && oc_morphology_chart_law() == 1, "the layout constants");
    t.expect(oc_appearance_flags() == 3 && oc_appearance_count() == n && oc_creature_appearance() != nullptr, "the default export (%d of %d)",
             oc_appearance_count(), n);
    const float* rows = oc_creature_appearance();
    long wrong = 0;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < kStride; ++j)
            wrong += fb(rows[(std::size_t)i * kStride + j]) != fb(g_world->appearanceRecords()[(std::size_t)i].genome.g[j]);
    t.expect(wrong == 0, "%ld exported floats differ from the records", wrong);
    // the family colours are the same with appearance off
    std::vector<std::uint8_t> col(oc_creature_colors(), oc_creature_colors() + 3 * n);
    t.expect(oc_init(11, 1920, 1080, 240.0, 200, R"({"appearance":0})") == 1, "oc_init off: %s", oc_last_error());
    oc_step(400);
    oc_sync();
    t.expect(oc_appearance_count() == 0 && oc_creature_appearance() == nullptr && oc_appearance_flags() == 0, "off still exports");
    t.expect(n == oc_creature_count() && std::memcmp(col.data(), oc_creature_colors(), col.size()) == 0, "the colours change with appearance");
    // live keys: the knobs in [0, .25]; appearance itself only as it is
    t.expect(oc_set_param("appearance", 0) == 1, "the unchanged master key is not a no-op: %s", oc_last_error());
    t.expect(oc_set_param("appearance", 3) == 0, "a live appearance change was accepted");
    t.expect(oc_init(11, 1920, 1080, 240.0, 200, nullptr) == 1, "oc_init: %s", oc_last_error());
    t.expect(oc_set_param("patternMutation", 0.1) == 1 && g_world->ocfg.patternMutation == 0.1f, "patternMutation 0.1 refused");
    t.expect(oc_set_param("morphologyNovelProbability", 0.25) == 1, "morphologyNovelProbability 0.25 refused: %s", oc_last_error());
    t.expect(oc_set_param("patternNovelProbability", 0.26) == 0, "patternNovelProbability 0.26 accepted");
    t.expect(oc_set_param("patternPhaseMutation", 1e39) == 0, "patternPhaseMutation 1e39 accepted");
    t.expect(oc_set_param("appearance", 2.5) == 0, "appearance 2.5 accepted");
    for (const char* bad : {R"({"appearance":4})", R"({"appearance":-1})", R"({"appearance":1.5})",
                            R"({"patternMutation":0.3})", R"({"morphologyMutation":-0.01})", R"({"patternNovelProbability":1e39})"})
        t.expect(oc_init(11, 1920, 1080, 240.0, 50, bad) == 0, "oc_init accepted %s", bad);
    t.expect(oc_init(11, 1920, 1080, 240.0, 50, R"({"appearance":1})") == 1 && oc_appearance_flags() == 1, "appearance 1 (patterns only)");
    oc_sync();
    bool neutralShapes = true;
    for (int i = 0; i < oc_appearance_count(); ++i)
        for (int j = 0; j < kMorphGenes; ++j)
            neutralShapes = neutralShapes && oc_creature_appearance()[(std::size_t)i * kStride + kMorphOffset + j] == kNeutralMorph[j];
    t.expect(neutralShapes, "patterns-only fish have shapes");
}
#endif

}  // namespace ot
