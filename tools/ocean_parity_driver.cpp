// ocean_parity_driver.cpp — the Ocean world's native-vs-wasm determinism oracle
// (PLAN_ecosystem.md §12, E0 exit test "native == wasm fingerprints").
//
// Runs OceanWorld headless and prints, every K steps, an exact fingerprint of
// the whole state (tools/ocean_fingerprint.hpp: every fish's AgentCore fields,
// genome, net, pos/vel/depth/vz/bodyBuoy/body trail, food incl. depth,
// predators incl. depth/pitch, the pheromone grid, the current, all counters
// and nextId_, the config, and the rng engine's state; floats by bit pattern).
// Build the SAME file natively and with Emscripten, run both with the same
// arguments and byte-compare stdout: equal output == bit-identical runs.
//
//   g++ -O3 -std=c++20 -ffp-contract=off -DEVO_WORLD_OCEAN -I src tools/ocean_parity_driver.cpp -o opd
//   em++ -O3 -std=c++20 -ffp-contract=off -msimd128 -DEVO_WORLD_OCEAN -I src
//        tools/ocean_parity_driver.cpp -o opd.js -sALLOW_MEMORY_GROWTH=1 -sNODERAWFS=1 -sEXIT_RUNTIME=1
//   ./opd 42 3000 > a.txt && node opd.js 42 3000 > b.txt && cmp a.txt b.txt
//
// wasm/ocean_parity.sh runs the case matrix.
//
// The world is built the way the page builds it (oc_init in wasm/ocean_wasm.cpp:
// applyOceanDefaults, the cfg JSON, the shim's clamps; ofp::initWorld), so a
// cfg JSON means here what it means to oc_init.
//
// Usage: ocean_parity_driver SEED STEPS [options]
//   --every K          fingerprint every K steps (default 100; step 0 and the
//                      last step are always printed)
//   --w W --h H        world size (default 1920 x 1080)
//   --depth D          sea depth (default 240)
//   --pop N            founders (default 300)
//   --cfg JSON         flat SimConfig + OceanConfig keys, as oc_init takes them
//   --set NAME=V@S     before step S+1 (after S steps), change one live knob the
//                      way oc_set_param does (repeatable)
//   --dump PATH        write the full state as text (tools/ocean_fingerprint.hpp
//                      dumpState) at --dump-step (default: the last step)
//   --dump-step S      which step to dump (0 = right after init)
//   --capture          captureActivations on: every fish's 46 sensors and 13
//                      actions of its last step are hashed too (not a cfg JSON key)
//   --trace            fingerprint EVERY step (divergence bisection)
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "ocean_fingerprint.hpp"

namespace {

struct LiveSet { std::string name; double value; long step; };

bool parseSet(const char* s, LiveSet& out) {
    const char* eq = std::strchr(s, '=');
    const char* at = std::strrchr(s, '@');
    if (!eq || !at || at < eq) return false;
    out.name.assign(s, eq - s);
    out.value = std::strtod(eq + 1, nullptr);
    out.step = std::strtol(at + 1, nullptr, 10);
    return !out.name.empty() && out.step >= 0;
}

// oc_set_param's path: the value through "%.9g" into a one-key JSON, read onto
// copies of both configs, clamped, assigned.
bool applySet(OceanWorld& w, const LiveSet& s) {
    char num[40];
    std::snprintf(num, sizeof num, "%.9g", s.value);
    const std::string json = "{\"" + s.name + "\":" + num + "}";
    SimConfig sc = w.cfg;
    OceanConfig oc = w.ocfg;
    std::string err;
    if (!readOceanJson(json.c_str(), sc, oc, err)) return false;
    ofp::clampConfigLikeShim(sc);
    if (!oceanConfigValid(oc)) return false;
    w.cfg = sc;
    w.ocfg = oc;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s SEED STEPS [--every K] [--w W --h H --depth D --pop N]"
                             " [--cfg JSON] [--set NAME=V@STEP] [--dump PATH --dump-step S] [--trace]\n",
                     argv[0]);
        return 2;
    }
    {   // Locale guard: the config text in the fingerprint assumes C-locale %.9g.
        char buf[16];
        std::snprintf(buf, sizeof buf, "%.9g", 0.5);
        if (std::strcmp(buf, "0.5") != 0) { std::fprintf(stderr, "FATAL: non-C locale (%s)\n", buf); return 3; }
    }

    const unsigned seed = (unsigned)std::strtoul(argv[1], nullptr, 10);
    const long steps = std::strtol(argv[2], nullptr, 10);
    long every = 100, dumpStep = -1;
    int W = 1920, H = 1080, pop = 300;
    double depth = 240.0;
    const char* cfgJson = nullptr;
    const char* dumpPath = nullptr;
    bool capture = false;
    std::vector<LiveSet> sets;
    for (int i = 3; i < argc; ++i) {
        auto is = [&](const char* k) { return std::strcmp(argv[i], k) == 0 && i + 1 < argc; };
        if (is("--every")) every = std::strtol(argv[++i], nullptr, 10);
        else if (is("--w")) W = std::atoi(argv[++i]);
        else if (is("--h")) H = std::atoi(argv[++i]);
        else if (is("--depth")) depth = std::atof(argv[++i]);
        else if (is("--pop")) pop = std::atoi(argv[++i]);
        else if (is("--cfg")) cfgJson = argv[++i];
        else if (is("--dump")) dumpPath = argv[++i];
        else if (is("--dump-step")) dumpStep = std::strtol(argv[++i], nullptr, 10);
        else if (is("--set")) {
            LiveSet s;
            if (!parseSet(argv[++i], s)) { std::fprintf(stderr, "bad --set %s (NAME=VALUE@STEP)\n", argv[i]); return 2; }
            sets.push_back(s);
        }
        else if (!std::strcmp(argv[i], "--trace")) every = 1;
        else if (!std::strcmp(argv[i], "--capture")) capture = true;
        else { std::fprintf(stderr, "unknown or incomplete argument: %s\n", argv[i]); return 2; }
    }
    if (every <= 0) every = steps > 0 ? steps : 1;
    if (dumpStep < 0) dumpStep = steps;

    OceanWorld w;
    std::string err;
    if (!ofp::initWorld(w, seed, W, H, (float)depth, pop, cfgJson, err)) {
        std::fprintf(stderr, "config refused: %s\n", err.c_str());
        return 4;
    }
    w.cfg.captureActivations = capture;
    std::printf("ocean_parity seed=%u steps=%ld every=%ld world=%dx%dx%.9g pop=%d capture=%d cfg=%s\n",
                seed, steps, every, W, H, depth, pop, (int)capture, cfgJson ? cfgJson : "{}");
    for (const LiveSet& s : sets) std::printf("set %s=%.9g @%ld\n", s.name.c_str(), s.value, s.step);

    auto dumpNow = [&] {
        std::FILE* f = std::fopen(dumpPath, "w");
        if (!f) { std::fprintf(stderr, "cannot write %s\n", dumpPath); return; }
        ofp::dumpState(w, f);
        std::fclose(f);
    };

    // Non-finite state is a failure even when both builds agree (see ofp::countNonFinite).
    long nonFiniteMax = 0, nonFiniteFirst = -1;
    auto print = [&](long step) {
        std::fputs(ofp::fingerprintLine("fp", step, w, ofp::fingerprint(w)).c_str(), stdout);
        const long nf = ofp::countNonFinite(w);
        if (nf > 0 && nonFiniteFirst < 0) nonFiniteFirst = step;
        nonFiniteMax = std::max(nonFiniteMax, nf);
    };

    print(0);
    if (dumpPath && dumpStep == 0) dumpNow();

    const auto t0 = std::chrono::steady_clock::now();
    for (long s = 0; s < steps; ++s) {
        for (const LiveSet& ls : sets)
            if (ls.step == s && !applySet(w, ls)) {
                std::fprintf(stderr, "--set %s refused\n", ls.name.c_str());
                return 4;
            }
        w.update(0.016f);
        const long done = s + 1;
        if (done % every == 0 || done == steps)
            print(done);
        if (dumpPath && done == dumpStep) dumpNow();
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    double mean, sd, hist[ocean::kDepthBins];
    w.depthStats(mean, sd, hist);
    std::printf("final frame=%llu pop=%zu food=%zu pred=%zu births=%lld deaths=%lld reseeds=%lld"
                " predBirths=%lld nonfinite_max=%ld nonfinite_from=%ld zmean=%.6f zsd=%.6f hist",
                w.frame, w.creatures.size(), w.food.size(), w.predators.size(), w.totalBirths,
                w.totalDeaths, w.totalReseeds, w.totalPredatorBirths, nonFiniteMax, nonFiniteFirst, mean, sd);
    for (double h : hist) std::printf(" %.4f", h);
    std::printf("\n");
    std::fflush(stdout);
    std::fprintf(stderr, "# %ld steps, %.2f s, %.0f steps/s, pop %zu\n", steps, secs,
                 steps / (secs > 0 ? secs : 1e-9), w.creatures.size());
    return 0;
}
