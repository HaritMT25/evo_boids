#pragma once
#include <cmath>
#include <array>
#include <vector>
#include <random>
#include "connectivity.hpp"
#include "color.hpp"

// Project the (source,sink) connectivity vector onto 3 fixed random directions
// (a 3-component cousin of the 16-bit species SimHash) -> RGB. Projection is
// linear, so a small mutation shifts colour proportionally -> lineages drift
// smoothly. Directions are seeded once with a LOCAL rng (the global `rng` stream
// is untouched) so colours are stable and comparable across runs.
// Caveat: 3-D is lossy, so similar colour does NOT guarantee similar genome.
inline Color genomeColor(const Genome& genome) {
    static const std::array<std::vector<float>, 3> planes = [] {
        std::mt19937 g(0x5EEDC010u);
        std::normal_distribution<float> nd(0.0f, 1.0f);
        std::array<std::vector<float>, 3> p;
        for (auto& row : p) { row.assign(STATS_DIM, 0.0f); for (float& x : row) x = nd(g); }
        return p;
    }();

    double proj[3] = {0.0, 0.0, 0.0};
    for (const Gene& gene : genome) {
        const int   idx = geneCell(gene);
        const float w   = gene.weightAsFloat();
        proj[0] += w * planes[0][idx];
        proj[1] += w * planes[1][idx];
        proj[2] += w * planes[2][idx];
    }

    // tanh squash to [40,235] (vivid, no pure black/white). K keeps tanh
    // near-linear so the mapping stays smooth; raise toward ~0.1 for more vivid.
    constexpr float K = 0.06f;
    auto chan = [](double v) {
        return static_cast<int>(std::lround(137.5 + 97.5 * std::tanh(K * v)));
    };
    return Color(chan(proj[0]), chan(proj[1]), chan(proj[2]));
}
