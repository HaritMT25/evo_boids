// undulator.cpp — tier-2 physics go/no-go harness.
//
// Standalone. NOT in src/core, NOT linked into the sim, NOT built by CMake:
//   g++ -O2 -std=c++20 -ffp-contract=off tools/undulator.cpp -o undulator
//   ./undulator --tune --converge --grid --cost
//
// The single question it answers is whether undulatory locomotion is physically
// REACHABLE in a tier-2-style chain body, asked BEFORE paying for a format break
// to find out.
//
// VERDICT AS MEASURED (full write-up in the research log's tier-2 entry):
//   Q1 reachable?   YES. ~0.29 body-lengths/s at the reference gait, and it
//                   converges (0.2708 -> 0.2948 as substeps go 2 -> 128).
//   Q2 climbable?   YES. At phase lag k >= 0.5 rad/joint there is a broad smooth
//                   plateau of 0.2-0.38 BL/s across most of the amplitude /
//                   frequency plane — not an isolated spike.
//   Q3 affordable?  NO at 20-cell fidelity, and this is the actual blocker. A
//                   well-tracked 20-cell gait costs ~38 us per creature per step
//                   on this machine today (~46 us in an earlier session; the us
//                   figures move ~+-20% between sessions, the physics does not)
//                   against a ~1.5 us whole-creature baseline: ~25-30x the entire
//                   current per-creature budget, not the ~5% originally
//                   estimated. That estimate assumed ~100 float ops and no
//                   substepping, and it is muscle stiffness that forces the
//                   substeps. The blocker is fidelity-dependent, not absolute:
//                   the cheapest configuration that still swims — 8 cells, 2
//                   substeps, kSpring 600 — costs 0.96 us, about 2x total
//                   per-creature cost, at 0.081 BL/s.
//
// The trap this harness exists to avoid: its first version reported "no thrust
// anywhere on the grid", which reads exactly like an honest negative result. It
// was a sign error in the muscle force triple (see the comment there) that folded
// every joint to +-pi. The gait diagnostic in run() and the falsification tests
// below are what turn a plausible NO-GO into a checkable one.
//
// Model
//   * N point masses joined by distance constraints (a chain, fixed topology).
//   * Muscles are INTERNAL angular springs at each interior joint, driving the
//     joint toward a travelling-wave target curvature  phi*(j,t) = A sin(wt - k j).
//     The three-particle force triple sums to zero by construction, so muscles
//     apply no net force to the body. This is the whole point: any forward motion
//     must come from the fluid, not from the actuator.
//   * Anisotropic drag per segment: resistance normal to the segment is CN/CT
//     times the tangential resistance. That anisotropy is the entire physical
//     basis of undulatory thrust.
//   * Semi-implicit Euler + Gauss-Seidel constraint relaxation (PBD-style), with
//     velocities rebuilt from the projected positions.
//
// Falsification tests run first, because a harness that reports thrust for the
// wrong reason is worse than no harness:
//   T1  zero drag           => centre of mass must not translate (internal forces only)
//   T2  isotropic drag      => no meaningful thrust (anisotropy is necessary)
//   T3  anisotropic drag    => thrust (anisotropy is sufficient)
//   T4  reversed wave       => thrust reverses (it tracks the wave, not a bias)
//
// usage: undulator [--grid] [--cost] [--steps N]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>
#include <chrono>

struct V2 { double x = 0, y = 0; };
static inline V2 operator+(V2 a, V2 b) { return {a.x + b.x, a.y + b.y}; }
static inline V2 operator-(V2 a, V2 b) { return {a.x - b.x, a.y - b.y}; }
static inline V2 operator*(V2 a, double s) { return {a.x * s, a.y * s}; }
static inline double dot(V2 a, V2 b) { return a.x * b.x + a.y * b.y; }
static inline double cross(V2 a, V2 b) { return a.x * b.y - a.y * b.x; }
static inline double len(V2 a) { return std::sqrt(dot(a, a)); }
static inline V2 perp(V2 a) { return {-a.y, a.x}; }

struct Params {
    int    N        = 20;      // cells
    double L        = 4.0;     // rest segment length (matches Creature::updateBody)
    double mass     = 1.0;
    double dt       = 0.016;   // the sim's fixed dt
    int    substeps = 16;      // physics substeps per sim step (stiff muscles need them)
    int    relax    = 4;       // constraint relaxation iterations per substep

    double A  = 0.45;          // wave amplitude   (rad of joint curvature)
    double w  = 12.0;          // wave frequency   (rad/s)
    double k  = 0.35;          // phase lag per joint (rad/joint)

    // Muscle stiffness must put the joint's natural frequency w_n = sqrt(k/I)
    // ABOVE the drive frequency w, or the joint cannot track its commanded
    // curvature and the body barely bends (measured: k=900 => w_n=5.3 rad/s
    // against a 12 rad/s drive => 27% of commanded amplitude and no thrust).
    // k=10000 gives w_n=17.7 rad/s and 96% tracking.
    double kSpring = 10000.0;  // muscle stiffness
    double kDamp   = 170.0;    // muscle damping (~15% of critical at kSpring)
    double CT      = 0.03;     // tangential drag coefficient
    double CN      = 0.60;     // normal drag coefficient  (CN/CT = anisotropy)
};

struct Chain {
    std::vector<V2> p, v;
    const Params& P;
    explicit Chain(const Params& params) : P(params) {
        p.resize(P.N); v.resize(P.N);
        for (int i = 0; i < P.N; ++i) p[i] = {i * P.L, 0.0};   // straight, along +x
    }

    V2 com() const {
        V2 c{};
        for (const V2& q : p) { c.x += q.x; c.y += q.y; }
        return c * (1.0 / P.N);
    }
    V2 momentum() const {
        V2 m{};
        for (const V2& u : v) { m.x += u.x * P.mass; m.y += u.y * P.mass; }
        return m;
    }
    // Angular momentum about the centre of mass. Muscles are internal, so this
    // must stay ~0 with drag off. Checking only linear momentum is not enough:
    // a force triple can sum to zero and still apply a net couple.
    double angularMomentum() const {
        const V2 c = com();
        V2 vc{};
        for (const V2& u : v) { vc.x += u.x; vc.y += u.y; }
        vc = vc * (1.0 / P.N);
        double l = 0.0;
        for (int i = 0; i < P.N; ++i) l += P.mass * cross(p[i] - c, v[i] - vc);
        return l;
    }

    void step(double t, double sdt) {
        std::vector<V2> f(P.N, V2{});

        // ---- muscles: internal angular springs at interior joints ----
        for (int j = 1; j + 1 < P.N; ++j) {
            V2 u = p[j] - p[j - 1];
            V2 q = p[j + 1] - p[j];
            const double lu = len(u), lq = len(q);
            if (lu < 1e-9 || lq < 1e-9) continue;
            const double phi = std::atan2(cross(u, q), dot(u, q));
            const double target = P.A * std::sin(P.w * t - P.k * j);

            // angular rate, from the endpoints' velocities about the joint
            const double dphi = (cross(u, v[j - 1] - v[j]) / (lu * lu))
                              + (cross(q, v[j + 1] - v[j]) / (lq * lq));
            const double torque = P.kSpring * (target - phi) - P.kDamp * dphi;

            // Realise the torque as a force triple that is zero-sum in BOTH force
            // and torque, so a muscle can neither translate nor spin the body.
            //
            // Both end forces carry the SAME sign. Pushing p[j-1] along +perp(u)
            // decreases the heading of u, and pushing p[j+1] along +perp(q)
            // increases the heading of q; since phi = theta_q - theta_u, both
            // increase phi. Taking moments about p[j] gives -tau from the first
            // and +tau from the second, which cancel.
            //
            // Getting this wrong is not a subtle error: with the second sign
            // flipped, the moments ADD to -2 tau, the joint is driven by positive
            // feedback instead of tracking its target, and every joint saturates
            // at +-pi (the chain folds on itself). The gait diagnostic in run()
            // exists because that failure otherwise looks exactly like an honest
            // "undulation produces no thrust" result.
            const V2 fa = perp(u) * (torque / (lu * lu));
            const V2 fb = perp(q) * (torque / (lq * lq));
            f[j - 1] = f[j - 1] + fa;
            f[j + 1] = f[j + 1] + fb;
            f[j]     = f[j] - fa - fb;
        }

        // ---- anisotropic fluid drag, per segment ----
        for (int i = 0; i + 1 < P.N; ++i) {
            V2 d = p[i + 1] - p[i];
            const double sl = len(d);
            if (sl < 1e-9) continue;
            const V2 tHat = d * (1.0 / sl);
            const V2 nHat = perp(tHat);
            const V2 vs = (v[i] + v[i + 1]) * 0.5;
            const V2 fd = tHat * (-P.CT * dot(vs, tHat) * sl)
                        + nHat * (-P.CN * dot(vs, nHat) * sl);
            f[i]     = f[i] + fd * 0.5;
            f[i + 1] = f[i + 1] + fd * 0.5;
        }

        // ---- semi-implicit Euler ----
        const std::vector<V2> prev = p;
        for (int i = 0; i < P.N; ++i) {
            v[i] = v[i] + f[i] * (sdt / P.mass);
            p[i] = p[i] + v[i] * sdt;
        }

        // ---- distance constraints (Gauss-Seidel), symmetric so momentum is kept ----
        for (int it = 0; it < P.relax; ++it) {
            for (int i = 0; i + 1 < P.N; ++i) {
                V2 d = p[i + 1] - p[i];
                const double dl = len(d);
                if (dl < 1e-12) continue;
                const double corr = (dl - P.L) / dl * 0.5;
                const V2 c = d * corr;
                p[i]     = p[i] + c;
                p[i + 1] = p[i + 1] - c;
            }
        }

        // ---- rebuild velocities from the projected positions ----
        for (int i = 0; i < P.N; ++i) v[i] = (p[i] - prev[i]) * (1.0 / sdt);
    }
};

// Net displacement along the body axis, in body lengths per second, signed so
// that POSITIVE means head-first.
//
// The chain is laid out with the head p[0] at the origin and the body extending
// toward +x, so head-first travel is the -x direction. The drive
// A sin(wt - k j) sends the curvature wave from head to tail (increasing j), and
// an undulatory swimmer moves OPPOSITE to the direction its body wave travels —
// so a correct model must report -x motion here. Getting this backwards makes a
// working swimmer look like a failure, which is exactly what it did before the
// convention was pinned down.
//
// achievedAmp / targetAmp is the diagnostic that decides whether a zero-thrust
// result means "undulation does not produce thrust" or the far more likely
// "the body never undulated". Without it a NO-GO verdict is worthless.
struct Result {
    double bodyLenPerSec = 0, driftY = 0, comSpeed = 0;
    double achievedAmp = 0;   // max |joint angle| over the measured window (rad)
    double tailExcursion = 0; // peak-to-peak lateral travel of the tail, in body lengths
    bool   blewUp = false;
};

static Result run(Params P, int simSteps, bool zeroDrag = false, bool isotropic = false) {
    if (zeroDrag) { P.CT = 0.0; P.CN = 0.0; }
    if (isotropic) { P.CN = P.CT; }
    Chain c(P);
    const V2 start = c.com();
    double t = 0.0;
    const double sdt = P.dt / P.substeps;
    const double bodyLen = (P.N - 1) * P.L;
    Result r;
    double tailMin = 1e30, tailMax = -1e30;

    for (int s = 0; s < simSteps; ++s) {
        for (int k = 0; k < P.substeps; ++k) { c.step(t, sdt); t += sdt; }
        // Measure the gait over the back half, once transients have decayed.
        if (s > simSteps / 2) {
            const int j = P.N / 2;                       // a mid-body joint
            V2 u = c.p[j] - c.p[j - 1], q = c.p[j + 1] - c.p[j];
            if (len(u) > 1e-9 && len(q) > 1e-9)
                r.achievedAmp = std::max(r.achievedAmp,
                                         std::fabs(std::atan2(cross(u, q), dot(u, q))));
            const double tailY = c.p[P.N - 1].y - c.com().y;
            tailMin = std::min(tailMin, tailY);
            tailMax = std::max(tailMax, tailY);
        }
    }
    const V2 end = c.com();
    const double seconds = simSteps * P.dt;
    if (!std::isfinite(end.x) || !std::isfinite(end.y)) { r.blewUp = true; return r; }
    r.bodyLenPerSec = -(end.x - start.x) / bodyLen / seconds;   // -x is head-first
    r.driftY = (end.y - start.y) / bodyLen;
    r.comSpeed = len(end - start) / bodyLen / seconds;
    r.tailExcursion = (tailMax - tailMin) / bodyLen;
    return r;
}

int main(int argc, char** argv) {
    bool doGrid = false, doCost = false, doTune = false, doConverge = false;
    int steps = 1200;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--grid")) doGrid = true;
        else if (!std::strcmp(argv[i], "--tune")) doTune = true;
        else if (!std::strcmp(argv[i], "--converge")) doConverge = true;
        else if (!std::strcmp(argv[i], "--cost")) doCost = true;
        else if (!std::strcmp(argv[i], "--steps") && i + 1 < argc) steps = std::atoi(argv[++i]);
    }

    Params P;
    std::printf("=== undulator harness: %d cells, dt %.3f x %d substeps, %d relax iters ===\n",
                P.N, P.dt, P.substeps, P.relax);
    std::printf("    default drive A=%.2f rad, w=%.1f rad/s, k=%.2f rad/joint; CN/CT=%.0f\n\n",
                P.A, P.w, P.k, P.CN / P.CT);

    // ---------------- falsification tests ----------------
    std::printf("--- falsification tests (these decide whether to believe anything below) ---\n");
    {
        Params Q = P;
        Chain c(Q);
        const V2 start = c.com();
        Q.CT = Q.CN = 0.0;
        Chain c0(Q);
        double t = 0; const double sdt = Q.dt / Q.substeps;
        for (int s = 0; s < steps; ++s)
            for (int k = 0; k < Q.substeps; ++k) { c0.step(t, sdt); t += sdt; }
        const V2 d = c0.com() - start;
        const V2 mom = c0.momentum();
        const double L = c0.angularMomentum();
        const bool okLin = std::fabs(d.x) < 1e-6 && std::fabs(d.y) < 1e-6;
        // Linear momentum is conserved to machine precision because the force
        // triple sums to zero identically. Angular momentum is only conserved in
        // the CONTINUOUS limit: the triple is torque-free at the instant it is
        // evaluated, but semi-implicit Euler moves the particles within the step.
        // So the residual below is not pass/fail on its own — --converge is the
        // real test, and it shows |L| falling toward zero as the timestep shrinks
        // while the thrust converges. Reported here for visibility only.
        const double Lscale = 2.0 * P.mass * P.L * P.L * P.A * P.w;
        std::printf("T1 zero drag        : com moved (%.2e, %.2e), |p|=%.2e  -> %s\n",
                    d.x, d.y, len(mom),
                    okLin ? "PASS (muscles apply no net force)"
                          : "FAIL (muscles push on nothing!)");
        std::printf("   angular residual : |L| = %.2e (%.2f x I*w*A) — discretisation, not a\n"
                    "                      modelling error; run --converge to see it vanish\n",
                    std::fabs(L), std::fabs(L) / Lscale);
    }
    {
        Result iso = run(P, steps, false, true);
        std::printf("T2 isotropic drag   : %+.4f body-lengths/s  -> %s\n", iso.bodyLenPerSec,
                    std::fabs(iso.bodyLenPerSec) < 0.05 ? "PASS (no thrust without anisotropy)"
                                                        : "FAIL (thrust from nowhere)");
    }
    Result base = run(P, steps);
    std::printf("T3 anisotropic drag : %+.4f body-lengths/s  -> %s\n", base.bodyLenPerSec,
                base.bodyLenPerSec > 0.05 ? "PASS (undulation produces thrust)"
                                          : "FAIL / NO-GO (no net forward motion)");
    // The check that tells you WHICH kind of failure T3 is.
    std::printf("   gait diagnostic  : target joint amp %.3f rad, ACHIEVED %.3f rad (%.0f%%), "
                "tail excursion %.3f BL%s\n",
                P.A, base.achievedAmp, 100.0 * base.achievedAmp / P.A, base.tailExcursion,
                base.blewUp ? "  [DIVERGED]" : "");
    if (base.achievedAmp < 0.25 * P.A)
        std::printf("   >> the body is barely bending: a zero-thrust result here says nothing\n"
                    "      about undulation, only that this parameter set is over-damped.\n");
    {
        Params R = P; R.k = -P.k;            // wave travels head-ward instead
        Result rev = run(R, steps);
        std::printf("T4 reversed wave    : %+.4f body-lengths/s  -> %s\n", rev.bodyLenPerSec,
                    (rev.bodyLenPerSec * base.bodyLenPerSec < 0)
                        ? "PASS (direction tracks the wave)"
                        : "SUSPECT (thrust sign did not follow the wave)");
    }
    std::printf("\n");

    // ---------------- muscle authority ----------------
    // A body that cannot reach its commanded curvature is not undulating, and
    // its thrust says nothing about whether undulation works. The angular spring
    // is a driven oscillator: its response collapses as (w_n/w)^2 once the drive
    // frequency passes the joint's natural frequency w_n = sqrt(kSpring / I),
    // I ~ 2 m L^2. So sweep stiffness against drag and find where the gait is
    // actually realised BEFORE reading any thrust number.
    if (doTune) {
        std::printf("--- muscle authority: does the body reach its commanded curvature? ---\n");
        const double I = 2.0 * P.mass * P.L * P.L;
        std::printf("    joint inertia I ~ %.0f, drive w = %.1f rad/s\n", I, P.w);
        std::printf("%10s %8s | %s\n", "kSpring", "w_n", "  CN=0.30        CN=0.60        CN=1.20");
        std::printf("%10s %8s | %s\n", "", "", "  amp%  BL/s    amp%  BL/s    amp%  BL/s");
        for (double ks : {900.0, 3000.0, 10000.0, 30000.0, 100000.0, 300000.0}) {
            std::printf("%10.0f %8.1f |", ks, std::sqrt(ks / I));
            for (double cn : {0.30, 0.60, 1.20}) {
                Params Q = P;
                Q.kSpring = ks;
                Q.kDamp = 0.15 * 2.0 * std::sqrt(ks * I);   // 15% of critical
                Q.CN = cn;
                Q.substeps = 16;                            // stability headroom
                Result r = run(Q, steps);
                if (r.blewUp) std::printf("   DIVERGED     ");
                else std::printf("  %4.0f %+6.3f  ", 100.0 * r.achievedAmp / Q.A, r.bodyLenPerSec);
            }
            std::printf("\n");
        }
        std::printf("\n");
    }

    // ---------------- landscape ----------------
    if (doGrid) {
        std::printf("--- Q2: is the fitness landscape climbable? sweep A, w, k ---\n");
        std::printf("(each cell = body-lengths/sec; evolution can climb a smooth gradient, not an isolated spike)\n\n");
        const double As[] = {0.15, 0.30, 0.45, 0.60, 0.75, 0.90};
        const double ws[] = {4.0, 8.0, 12.0, 16.0, 20.0, 24.0};
        const double ks[] = {0.10, 0.20, 0.35, 0.50, 0.65, 0.80};

        for (double kk : ks) {
            std::printf("k = %.2f rad/joint\n      ", kk);
            for (double ww : ws) std::printf("  w=%-6.1f", ww);
            std::printf("\n");
            for (double aa : As) {
                std::printf("A=%.2f", aa);
                for (double ww : ws) {
                    Params Q = P; Q.A = aa; Q.w = ww; Q.k = kk;
                    std::printf("  %+7.3f", run(Q, steps).bodyLenPerSec);
                }
                std::printf("\n");
            }
            std::printf("\n");
        }
    }

    // ---------------- convergence ----------------
    // Does the model converge as the timestep shrinks? If the spurious angular
    // momentum falls toward zero and the thrust settles, the leak is
    // discretisation and the thrust is real. If thrust wanders with substeps,
    // the number is an integration artifact and means nothing.
    if (doConverge) {
        std::printf("--- convergence: is the thrust a physical result or an integrator artifact? ---\n");
        std::printf("%9s %12s %12s %10s %10s\n", "substeps", "|L| no-drag", "L/Iw scale", "amp%", "BL/s");
        const double Lscale = 2.0 * P.mass * P.L * P.L * P.A * P.w;
        for (int ss : {2, 4, 8, 16, 32, 64, 128}) {
            Params Q = P; Q.substeps = ss;
            // no-drag angular momentum leak
            Params Z = Q; Z.CT = Z.CN = 0.0;
            Chain c0(Z);
            double t = 0; const double sdt = Z.dt / Z.substeps;
            for (int s = 0; s < steps; ++s)
                for (int k = 0; k < Z.substeps; ++k) { c0.step(t, sdt); t += sdt; }
            const double L = std::fabs(c0.angularMomentum());
            Result r = run(Q, steps);
            std::printf("%9d %12.3e %12.4f %10.0f %10.4f\n",
                        ss, L, L / Lscale, 100.0 * r.achievedAmp / Q.A, r.bodyLenPerSec);
        }
        std::printf("\n");
    }

    // ---------------- cost ----------------
    if (doCost) {
        // Cost is dominated by substeps x relax x cells, and substeps is set by
        // muscle stiffness, which is set by drive frequency. So the useful
        // question is not "what does one configuration cost" but "what is the
        // CHEAPEST configuration that still swims" — quote a number from a
        // config that does not swim and the whole estimate is meaningless.
        std::printf("--- Q3: cost vs capability. Only a configuration that SWIMS may be quoted. ---\n");
        std::printf("%6s %9s %7s %8s %10s %9s %9s\n",
                    "cells", "substeps", "relax", "kSpring", "w (rad/s)", "BL/s", "us/step");
        struct Cfg { int n, ss, relax; double ks, w; };
        const Cfg cfgs[] = {
            {20, 16, 4, 10000, 12.0},   // the reference gait
            {20,  8, 4,  4000,  8.0},
            {20,  4, 3,  1500,  4.0},
            {20,  2, 2,   600,  4.0},
            {12,  4, 3,  1500,  4.0},
            { 8,  4, 3,  1500,  4.0},
            { 8,  2, 2,   600,  4.0},
            { 6,  2, 2,   600,  4.0},
        };
        for (const Cfg& cf : cfgs) {
            Params Q = P;
            Q.N = cf.n; Q.substeps = cf.ss; Q.relax = cf.relax;
            Q.kSpring = cf.ks; Q.w = cf.w;
            Q.kDamp = 0.15 * 2.0 * std::sqrt(cf.ks * 2.0 * Q.mass * Q.L * Q.L);
            // Shorter bodies need a longer per-joint phase lag to fit a wave on.
            Q.k = 0.35 * 20.0 / cf.n;
            const Result r = run(Q, steps);

            Chain c(Q);
            double t = 0; const double sdt = Q.dt / Q.substeps;
            for (int i = 0; i < 500; ++i) { c.step(t, sdt); t += sdt; }
            const int reps = 20000;
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < reps; ++i)
                for (int s = 0; s < Q.substeps; ++s) { c.step(t, sdt); t += sdt; }
            const double us = std::chrono::duration<double>(
                                  std::chrono::steady_clock::now() - t0).count() / reps * 1e6;
            std::printf("%6d %9d %7d %8.0f %10.1f %9.3f %9.2f%s\n",
                        cf.n, cf.ss, cf.relax, cf.ks, cf.w, r.bodyLenPerSec, us,
                        r.bodyLenPerSec < 0.05 ? "   <- does not swim" : "");
        }
        std::printf("\n  Sim baseline is ~1.5 us per creature per step (measured, research log).\n"
                    "  A row costing X us adds X/1.5 = that many TIMES the entire current\n"
                    "  per-creature budget — the plan's ~5%% estimate assumed ~100 float ops\n"
                    "  and no substepping, which the stiffness requirement rules out.\n");
    }
    return 0;
}
