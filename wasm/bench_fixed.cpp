// bench_fixed.cpp — step time at a FIXED creature count, one source for every
// target, so native and wasm numbers measure the same work:
//
//   native serial   g++ -O3 -std=c++20 -ffp-contract=off -I src wasm/bench_fixed.cpp
//   native OpenMP   ... -fopenmp -DUSE_OPENMP              (threads: OMP_NUM_THREADS)
//   wasm single     em++ ... ; node bench_fixed.js
//   wasm threaded   em++ ... -pthread -DEVO_POOL_THREADS -sPTHREAD_POOL_SIZE=7
//   (wasm/bench.sh builds and runs all of them)
//
// The population is pinned by setting the reseed floor and the birth cap both
// to N, so the count holds at N for the whole run. World 1920x1080, default
// config otherwise, activation capture on (as in the browser module).
//
// usage: bench_fixed N [threads] [warmup_steps] [measure_steps]
// prints: N, threads, mean live count, median ms/step over 5 blocks
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "core/world.hpp"
#ifdef USE_OPENMP
#include <omp.h>
#endif

int main(int argc, char** argv) {
    const int n       = argc > 1 ? std::atoi(argv[1]) : 1000;
    int threads       = argc > 2 ? std::atoi(argv[2]) : 1;
    const int warm    = argc > 3 ? std::atoi(argv[3]) : 300;
    const int measure = argc > 4 ? std::atoi(argv[4]) : 1000;
#if defined(USE_OPENMP)
    omp_set_num_threads(threads);
    threads = omp_get_max_threads();
#elif defined(EVO_POOL_THREADS)
    evo_par::Pool::instance().setThreads(threads);
    threads = evo_par::Pool::instance().threads();
#else
    threads = 1;
#endif

    rng.seed(42);
    World w;
    w.cfg.minPopulation = n;
    w.cfg.maxPopulation = n;
    w.cfg.captureActivations = true;
    w.init(1920, 1080, n);
    for (int i = 0; i < warm; ++i) w.update(0.016f);

    const int blocks = 5, per = std::max(1, measure / blocks);
    std::vector<double> msPerStep;
    double popSum = 0;
    for (int b = 0; b < blocks; ++b) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < per; ++i) w.update(0.016f);
        const auto t1 = std::chrono::steady_clock::now();
        msPerStep.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count() / per);
        popSum += (double)w.creatures.size();
    }
    std::sort(msPerStep.begin(), msPerStep.end());
    std::printf("N=%d threads=%d live=%.0f ms_per_step=%.4f (min %.4f max %.4f)\n",
                n, threads, popSum / blocks, msPerStep[blocks / 2], msPerStep.front(), msPerStep.back());
    std::fflush(stdout);
    std::exit(0);  // also ends the pthread pool under Emscripten (-sEXIT_RUNTIME)
}
