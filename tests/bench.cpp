// Core-only throughput + phase-profile benchmark (no SFML). For each population it
// runs several seeds and reports the mean and spread of the phase split, so the
// proportions aren't a single-seed artifact. Phase profiler must be on.
#include <cstdio>
#include <chrono>
#include <vector>
#include <algorithm>
#include "core/world.hpp"

struct Split { double build, par, serial, gather, ff, apply, stepUs, sps; };

static Split run1(int pop, int steps, int W, int H, unsigned seed) {
    rng.seed(seed);
    World w; w.cfg.profile = true; w.init(W, H, pop);
    for (int i = 0; i < 200; ++i) w.update(0.016f);     // warm up
    w.prof = World::PhaseProfile{};                       // reset after warmup
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < steps; ++i) w.update(0.016f);
    auto t1 = std::chrono::steady_clock::now();
    const auto& p = w.prof;
    const double parCpu = p.gatherNs + p.ffNs + p.applyNs;
    auto wpct = [&](double v){ return 100.0 * v / p.totalNs; };
    auto cpct = [&](double v){ return 100.0 * v / parCpu; };
    double secs = std::chrono::duration<double>(t1 - t0).count();
    return { wpct(p.buildNs), wpct(p.parWallNs), wpct(p.serialNs),
             cpct(p.gatherNs), cpct(p.ffNs), cpct(p.applyNs),
             p.totalNs / p.steps / 1000.0, steps / secs };
}

static void report(const char* tag, int pop, int steps, int W, int H, std::vector<unsigned> seeds) {
    std::vector<Split> r; for (unsigned s : seeds) r.push_back(run1(pop, steps, W, H, s));
    auto stat = [&](double Split::*f, double& mean, double& lo, double& hi) {
        mean = 0; lo = 1e9; hi = -1e9;
        for (auto& x : r) { double v = x.*f; mean += v; lo = std::min(lo, v); hi = std::max(hi, v); }
        mean /= r.size();
    };
    double m, lo, hi;
    std::printf("%s  pop=%d  (%zu seeds, %d steps)\n", tag, pop, seeds.size(), steps);
    stat(&Split::sps, m, lo, hi);    std::printf("   throughput   %7.0f steps/s  [%.0f..%.0f]\n", m, lo, hi);
    stat(&Split::build, m, lo, hi);  std::printf("   build         %5.1f%%  [%.1f..%.1f]\n", m, lo, hi);
    stat(&Split::par, m, lo, hi);    std::printf("   parallel      %5.1f%%  [%.1f..%.1f]\n", m, lo, hi);
    stat(&Split::serial, m, lo, hi); std::printf("   serial        %5.1f%%  [%.1f..%.1f]\n", m, lo, hi);
    std::printf("   -- within parallel (CPU share) --\n");
    stat(&Split::gather, m, lo, hi); std::printf("   sensor-gather %5.1f%%  [%.1f..%.1f]\n", m, lo, hi);
    stat(&Split::ff, m, lo, hi);     std::printf("   feedForward   %5.1f%%  [%.1f..%.1f]\n", m, lo, hi);
    stat(&Split::apply, m, lo, hi);  std::printf("   apply/metab   %5.1f%%  [%.1f..%.1f]\n", m, lo, hi);
    std::printf("\n");
}

int main(int argc, char** argv) {
    int steps = (argc > 1) ? std::atoi(argv[1]) : 1200;
    std::vector<unsigned> seeds = {1, 7, 42, 101, 2025};
    std::printf("=== phase profile, multi-seed (single-thread sandbox) ===\n\n");
    report("[small] ", 300,  steps, 1920, 1080, seeds);
    report("[medium]", 800,  steps, 1920, 1080, seeds);
    report("[large] ", 2000, steps, 1920, 1080, seeds);
    return 0;
}
