#pragma once
#include <random>
#include <cstdint>
#include <string>
#include <sstream>

// Thread-local RNG adapted from biosim4
class RandomGen {
public:
    void seed(unsigned s) { engine_.seed(s); }
    void seed() { engine_.seed(std::random_device{}()); }

    // --- Engine state, for checkpointing ---
    // A resumed run must continue the SAME random sequence, so the seed alone is
    // not enough once a run has advanced — the engine's full internal state has
    // to travel with the checkpoint. std::mt19937's stream operators emit the
    // state as space-separated integers in a format the standard specifies, so
    // this round-trips exactly (and across standard libraries).
    //
    // The distributions above are constructed fresh per call and hold no state
    // between calls, so the engine is the only thing that needs saving.
    std::string state() const {
        std::ostringstream os;
        os << engine_;
        return os.str();
    }
    bool setState(const std::string& s) {
        std::istringstream is(s);
        is >> engine_;
        return !is.fail();
    }

    uint32_t uint(uint32_t max = UINT32_MAX) {
        return std::uniform_int_distribution<uint32_t>(0, max)(engine_);
    }

    float unit() {
        return std::uniform_real_distribution<float>(0.0f, 1.0f)(engine_);
    }

    float range(float lo, float hi) {
        return std::uniform_real_distribution<float>(lo, hi)(engine_);
    }

    int intRange(int lo, int hi) {
        return std::uniform_int_distribution<int>(lo, hi)(engine_);
    }

private:
    std::mt19937 engine_{std::random_device{}()};
};

// Global thread-local RNG instance
inline thread_local RandomGen rng;
