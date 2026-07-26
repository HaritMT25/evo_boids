#pragma once
#include <random>
#include <cstdint>

// Thread-local RNG adapted from biosim4
class RandomGen {
public:
    void seed(unsigned s) { engine_.seed(s); }
    void seed() { engine_.seed(std::random_device{}()); }

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
