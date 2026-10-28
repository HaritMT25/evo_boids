// ocean_eye_block.cpp — one Ocean build behind a tiny C API, so that several
// builds (plain / eye, with or without -DEVO_OCEAN_PROF) can be timed in ONE
// process, in lockstep: tools/ocean_eye_lockstep.cpp loads each as a shared
// object (dlopen), tools/ocean_eye_lockstep.js each as a wasm module (node).
// Interleaving the variants every few steps puts them under the same clock
// speed, which matters on a laptop whose turbo / thermal state and host
// scheduling (WSL vCPUs) move the absolute step time by up to 2x within a minute.
//
// Each handle is a world, its snapshot (taken after init and the optional
// evolution) and its OWN rng state, swapped in around every blk_run, so two
// worlds stepped alternately in one module do not share draws.
//
//   native: g++ -O3 -std=c++20 -ffp-contract=off -DEVO_WORLD_OCEAN [-DEVO_OCEAN_EYE] -I src
//           -shared -fPIC -fvisibility=hidden -fno-gnu-unique -Wl,-Bsymbolic
//           tools/ocean_eye_block.cpp -o eye.so
//   wasm:   em++ (same) -msimd128 --no-entry -sMODULARIZE=1 -sENVIRONMENT=node
//           -sALLOW_MEMORY_GROWTH=1 tools/ocean_eye_block.cpp -o eye_blk.js
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "ocean_fingerprint.hpp"   // ofp::initWorld: the page's config path

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define BLK extern "C" EMSCRIPTEN_KEEPALIVE
#else
#define BLK extern "C" __attribute__((visibility("default")))
#endif

namespace {

struct Slot {
    OceanWorld snap, w;
    RandomGen rsnap, r;
    double fishSteps = 0;
};
std::vector<std::unique_ptr<Slot>> g_slots;

// World presets (no strings across the C API): 0 = the page's defaults; 1 = a
// 6.25x-area world with 6.25x the food (the evolved ~3000-fish workload).
const char* presetJson(int p) {
    if (p == 1)
        return R"({"targetFood":15000,"foodSpawnBase":18.75,"foodSpawnAmp":12.5,"minPopulation":312})";
    return nullptr;
}

}  // namespace

BLK int blk_has_eye() { return kOceanHasEye ? 1 : 0; }
BLK int blk_has_prof() {
#ifdef EVO_OCEAN_PROF
    return 1;
#else
    return 0;
#endif
}

// A new world: init (seed, size, preset), then `evolve` steps with the eye
// set to evolveEye; the result is the handle's snapshot. -1 on a refused config.
BLK int blk_new(unsigned seed, int pop, int W, int H, int preset, int evolve, int evolveEye) {
    auto s = std::make_unique<Slot>();
    std::string err;
    if (!ofp::initWorld(s->w, seed, W, H, 240.0f, pop, presetJson(preset), err)) {
        std::printf("blk_new refused: %s\n", err.c_str());
        return -1;
    }
    if (evolve > 0) {
        s->w.ocfg.eye = kOceanHasEye ? evolveEye : 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < evolve; ++i) s->w.update(0.016f);
        std::printf("blk_new: evolved %d steps (eye %d, %s build) in %.1f s: pop=%zu births=%lld reseeds=%lld\n",
                    evolve, s->w.ocfg.eye, kOceanHasEye ? "eye" : "plain",
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(),
                    s->w.creatures.size(), s->w.totalBirths, s->w.totalReseeds);
        std::fflush(stdout);
        s->w.ocfg.eye = 0;
    }
    s->snap = s->w;
    s->rsnap = rng;
    s->r = rng;
    g_slots.push_back(std::move(s));
    return (int)g_slots.size() - 1;
}

// Another handle on the same snapshot (the A/B and A/A partners).
BLK int blk_clone(int h) {
    g_slots.push_back(std::make_unique<Slot>(*g_slots[h]));
    return (int)g_slots.size() - 1;
}

// Back to the snapshot, with the eye set (0 in a plain build). The world is
// rebuilt in fresh allocations (copy, then the old buffers go), so its memory
// placement varies from rep to rep instead of favouring one variant.
BLK void blk_reset(int h, int eye) {
    Slot& s = *g_slots[h];
    s.w = OceanWorld(s.snap);
    s.r = s.rsnap;
    s.w.ocfg.eye = kOceanHasEye ? eye : 0;
    s.fishSteps = 0;
#ifdef EVO_OCEAN_PROF
    s.w.oprof = {};
#endif
}

// n steps of this world under its own rng; returns the wall seconds.
BLK double blk_run(int h, int n) {
    Slot& s = *g_slots[h];
    rng = s.r;
    double fishSteps = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) {
        fishSteps += (double)s.w.creatures.size();
        s.w.update(0.016f);
    }
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    s.fishSteps += fishSteps;
    s.r = rng;
    return sec;
}

BLK double blk_fish_steps(int h) { return g_slots[h]->fishSteps; }
BLK void blk_clear_counts(int h) {
    g_slots[h]->fishSteps = 0;
#ifdef EVO_OCEAN_PROF
    g_slots[h]->w.oprof = {};
#endif
}

// Profiler sums since the last reset / clear (0 without -DEVO_OCEAN_PROF):
// k = 0 build, 1 fish phase, 2 serial, 3 total, 4 sensors, 5 feedForward,
// 6 move + metabolism (ns), 7 steps, 8 fish-steps.
BLK double blk_prof(int h, int k) {
#ifdef EVO_OCEAN_PROF
    const auto& p = g_slots[h]->w.oprof;
    const double v[9] = {p.buildNs, p.fishPhaseNs, p.serialNs, p.totalNs, p.sensNs, p.ffNs, p.moveNs,
                         (double)p.steps, (double)p.fishSteps};
    return (k >= 0 && k < 9) ? v[k] : 0.0;
#else
    (void)h; (void)k;
    return 0.0;
#endif
}
