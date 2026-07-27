// parity_driver.cpp — the native-vs-wasm bit-for-bit determinism oracle.
//
// Runs the core sim headless (no SFML, no filesystem) and prints the final
// checkpoint JSON to stdout. Build the SAME file with the native toolchain and
// with Emscripten, run both with the same arguments, and byte-compare:
//
//   g++ -O3 -std=c++20 -ffp-contract=off -I src wasm/parity_driver.cpp -o parity_native
//   em++ -O3 -std=c++20 -ffp-contract=off -I src wasm/parity_driver.cpp -o parity_wasm.js
//   ./parity_native 42 2000 > a.json && node parity_wasm.js 42 2000 > b.json && cmp a.json b.json
//
// The checkpoint is a canonical encoding of the ENTIRE world (creatures with
// neuron state, predators, food, pheromone grid, rng engine state, config), so
// byte equality of two checkpoints == bit-for-bit identical simulations.
// wasm/parity.sh runs the full matrix.
//
// Usage: parity_driver SEED STEPS [--rich] [--tri] [--toroidal]
//                      [--repro-max-distance F] [--engine-version STR] [--trace N]
//   --rich   turn on every hidden-state subsystem (mirrors the resume test's
//            configureRichWorld: eddies, food patches + drift, 4 predators with
//            economy, move cost, topological cap 7, gene cost)
//   --tri    triangular motion (exercises atan2f)
//   --repro-max-distance  exercise the genetic reproduction gate (isolation.hpp):
//            a sort + merge-join over folded genomes on the reproduction path
//   --trace  print "frame fingerprint" to stderr every N steps (divergence bisection)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <chrono>
#include "core/world.hpp"
#include "core/checkpoint_io.hpp"
#include "core/random.hpp"

// Compact whole-state fingerprint for bisecting the first divergent step.
// (The checkpoint bytes are the real oracle; this is just cheap per-step.)
static uint64_t fingerprint(const World& w) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) {
        const unsigned char* b = (const unsigned char*)p;
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    };
    mix(&w.frame, sizeof w.frame);
    size_t nc = w.creatures.size(), nf = w.food.size(), np = w.predators.size();
    uint64_t counts[3] = {nc, nf, np};
    mix(counts, sizeof counts);
    for (const auto& c : w.creatures) {
        mix(&c.pos, sizeof c.pos);
        mix(&c.vel, sizeof c.vel);
        mix(&c.foodReserves, sizeof c.foodReserves);
    }
    for (const auto& p : w.predators) {
        mix(&p.pos, sizeof p.pos);
        mix(&p.vel, sizeof p.vel);
    }
    if (!w.signals.data().empty())
        mix(w.signals.data().data(), w.signals.data().size());
    return h;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s SEED STEPS [--rich] [--tri] [--engine-version STR] [--trace N]\n", argv[0]);
        return 2;
    }
    // Locale guard: checkpoint bytes assume C-locale float formatting.
    {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%.9g", 0.5);
        if (std::strcmp(buf, "0.5") != 0) {
            std::fprintf(stderr, "FATAL: non-C locale float formatting (%s)\n", buf);
            return 3;
        }
    }

    const unsigned seed = (unsigned)std::strtoul(argv[1], nullptr, 10);
    const long steps = std::strtol(argv[2], nullptr, 10);
    bool rich = false, tri = false, tor = false;
    float reproMaxDistance = -1.0f;
    const char* engineVersion = "parity";
    long traceEvery = 0;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--rich")) rich = true;
        else if (!std::strcmp(argv[i], "--tri")) tri = true;
        else if (!std::strcmp(argv[i], "--toroidal")) tor = true;
        else if (!std::strcmp(argv[i], "--repro-max-distance") && i + 1 < argc)
            reproMaxDistance = (float)std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--engine-version") && i + 1 < argc) engineVersion = argv[++i];
        else if (!std::strcmp(argv[i], "--trace") && i + 1 < argc) traceEvery = std::strtol(argv[++i], nullptr, 10);
        else { std::fprintf(stderr, "unknown arg %s\n", argv[i]); return 2; }
    }

    // Mirror the CLI's deterministic headless path exactly (main.cpp): seed,
    // then cfg BEFORE init, then N x update(0.016f).
    rng.seed(seed);
    World world;
    if (rich) {
        world.cfg.currentStrength = 80.0f;
        world.cfg.currentMode     = 0;
        world.cfg.currentDrift    = 1.0f;
        world.cfg.foodPatches     = 5;
        world.cfg.foodDrift       = true;
        world.cfg.predatorCount   = 4;
        world.cfg.predatorEconomy = true;
        world.cfg.moveCost        = 7e-5f;
        world.cfg.maxNeighbors    = 7;
        world.cfg.geneCost        = 0.005f;
    }
    if (tri) world.cfg.triangularMotion = true;
    if (tor) world.cfg.toroidal = true;
    world.cfg.reproMaxDistance = reproMaxDistance;
    world.init(1920, 1080, 300);

    const auto t0 = std::chrono::steady_clock::now();
    for (long i = 0; i < steps; ++i) {
        world.update(0.016f);
        if (traceEvery > 0 && (i + 1) % traceEvery == 0)
            std::fprintf(stderr, "%ld %016llx\n", i + 1, (unsigned long long)fingerprint(world));
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "# %ld steps, %.2f s, %.0f steps/s, pop %zu\n",
                 steps, secs, steps / (secs > 0 ? secs : 1e-9), world.creatures.size());

    const std::string blob = checkpoint::serialize(world, engineVersion);
    std::fwrite(blob.data(), 1, blob.size(), stdout);
    return 0;
}
