// ocean_probe.cpp — headless OceanWorld run for tuning the Ocean preview:
// population, births vs reseeds, the 8-bin depth histogram, and how many fish
// wire a depth sensor into STEER_Z. Same defaults as the page (wasm/ocean_wasm.cpp):
// applyOceanDefaults + OceanConfig{}, then --cfg on top.
//
//   g++ -O3 -std=c++20 -ffp-contract=off -DEVO_WORLD_OCEAN -I src tools/ocean_probe.cpp -o ocean_probe
//   ./ocean_probe --seed 1 --steps 30000 [--w 1920 --h 1080 --depth 240 --pop 300]
//                 [--every 5000] [--e1] [--cfg '{"kBuoy":0.3,"foodPatches":6}']
//   --e1: the E1 preset (ocean_config.hpp kE1PresetJson) first, --cfg on top.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "ocean/ocean_config.hpp"
#include "ocean/ocean_world.hpp"

namespace {

void report(const OceanWorld& w, double secs) {
    double mean, sd, hist[ocean::kDepthBins];
    w.depthStats(mean, sd, hist);
    double bs = 0, bs2 = 0, avz = 0, off = 0;
    for (const Fish& f : w.creatures) {
        bs += f.bodyBuoy; bs2 += (double)f.bodyBuoy * f.bodyBuoy;
        avz += std::abs(f.vz);
        off += std::abs(f.depth - f.bodyBuoy * w.D);
    }
    const double n = (double)w.creatures.size();
    const double bm = n ? bs / n : 0, bsd = n ? std::sqrt(std::max(0.0, bs2 / n - bm * bm)) : 0;
    double fz = 0;
    for (const OceanFood& f : w.food) fz += f.depth;
    std::printf("f=%6llu pop=%4zu food=%5zu pred=%2zu births=%7lld deaths=%7lld reseeds=%5lld"
                " | z %5.1f±%5.1f buoy %.2f±%.2f |vz| %.3f off %5.1f foodz %5.1f | hist",
                w.frame, w.creatures.size(), w.food.size(), w.predators.size(), w.totalBirths,
                w.totalDeaths, w.totalReseeds, mean, sd, bm, bsd, n ? avz / n : 0.0, n ? off / n : 0.0,
                w.food.empty() ? 0.0 : fz / (double)w.food.size());
    for (double h : hist) std::printf(" %4.1f", 100.0 * h);
    std::printf(" | %.0f st/s\n", secs > 0 ? (double)w.frame / secs : 0.0);
}

}  // namespace

int main(int argc, char** argv) {
    unsigned seed = 1;
    long steps = 30000, every = 5000;
    int W = 1920, H = 1080, pop = 300;
    double depth = 240.0;
    const char* cfgJson = nullptr;
    bool e1 = false;
    for (int i = 1; i < argc; ++i) {
        auto is = [&](const char* k) { return std::strcmp(argv[i], k) == 0 && i + 1 < argc; };
        if (!std::strcmp(argv[i], "--e1")) e1 = true;
        else if (is("--seed")) seed = (unsigned)std::strtoul(argv[++i], nullptr, 10);
        else if (is("--steps")) steps = std::strtol(argv[++i], nullptr, 10);
        else if (is("--every")) every = std::strtol(argv[++i], nullptr, 10);
        else if (is("--w")) W = std::atoi(argv[++i]);
        else if (is("--h")) H = std::atoi(argv[++i]);
        else if (is("--depth")) depth = std::atof(argv[++i]);
        else if (is("--pop")) pop = std::atoi(argv[++i]);
        else if (is("--cfg")) cfgJson = argv[++i];
        else { std::fprintf(stderr, "unknown or incomplete argument: %s\n", argv[i]); return 2; }
    }

    SimConfig sc;
    applyOceanDefaults(sc);
    OceanConfig oc;
    std::string err;
    if (e1 && !readOceanJson(kE1PresetJson, sc, oc, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
    if (!readOceanJson(cfgJson, sc, oc, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
    oc.depth = (float)depth;
    if (!oceanConfigValid(oc)) { std::fprintf(stderr, "bad ocean config: %s\n", oceanConfigError(oc).c_str()); return 2; }
    if (const std::string se = oceanSimConfigError(sc); !se.empty()) { std::fprintf(stderr, "bad config for the Ocean: %s\n", se.c_str()); return 2; }

    rng.seed(seed);
    OceanWorld w;
    w.cfg = sc;
    w.ocfg = oc;
    w.init(W, H, (float)depth, pop);
    std::printf("seed=%u %dx%dx%.0f pop=%d e1=%d cfg=%s\n", seed, W, H, depth, pop, (int)e1, cfgJson ? cfgJson : "{}");

    const auto t0 = std::chrono::steady_clock::now();
    auto secs = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    for (long s = 1; s <= steps; ++s) {
        w.update(0.016f);
        if (every > 0 && s % every == 0) report(w, secs());
    }

    // Wiring: which depth sensors reach STEER_Z (directly / through hidden neurons).
    const int depthSensors[] = {LOC_Z, OWN_VEL_Z, NEAREST_FOOD_DZ, AVG_NEIGHBOR_DZ, BOUNDARY_DIST_Z};
    std::printf("wiring into STEER_Z (direct / any path) of %zu fish:", w.creatures.size());
    for (int s : depthSensors) {
        int direct = 0, path = 0;
        for (const Fish& f : w.creatures) {
            direct += ocean::sensorWiresAction(f.nnet, s, STEER_Z);
            path   += ocean::sensorReachesAction(f.nnet, s, STEER_Z);
        }
        std::printf(" %s %d/%d", ocean::kSensorNames[s], direct, path);
    }
    int keyPath = 0, keyDirect = 0, anySteerZ = 0;
    for (const Fish& f : w.creatures) {
        keyPath   += ocean::sensorReachesAction(f.nnet, LOC_Z, STEER_Z) ||
                     ocean::sensorReachesAction(f.nnet, NEAREST_FOOD_DZ, STEER_Z);
        keyDirect += ocean::sensorWiresAction(f.nnet, LOC_Z, STEER_Z) ||
                     ocean::sensorWiresAction(f.nnet, NEAREST_FOOD_DZ, STEER_Z);
        bool any = false;
        for (const auto& fc : f.nnet.flat) any |= (fc.sink() == STEER_Z);
        anySteerZ += any;
    }
    std::printf("\n");

    double mean, sd, hist[ocean::kDepthBins];
    w.depthStats(mean, sd, hist);
    double maxBin = 0;
    for (double h : hist) maxBin = std::max(maxBin, h);
    std::printf("RESULT seed=%u steps=%ld pop=%zu births=%lld reseeds=%lld deaths=%lld"
                " zmean=%.1f zsd=%.1f maxbin=%.1f%% steerZ_any=%d locz_or_fooddz_to_steerZ=%d(direct %d)"
                " secs=%.1f\n",
                seed, steps, w.creatures.size(), w.totalBirths, w.totalReseeds, w.totalDeaths,
                mean, sd, 100.0 * maxBin, anySteerZ, keyPath, keyDirect, secs());
    return 0;
}
