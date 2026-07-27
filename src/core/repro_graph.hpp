#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

// ----------------------------------------------------------------------------
// The pairing graph — the INDEPENDENT grouping measure.
//
// Why this exists. The `species` column (stats.hpp) is a SimHash of the folded
// connectivity vector, and the match-distance gate (isolation.hpp) is a
// distance threshold in that SAME folded space. Using one to evaluate the other
// is circular: any gate defined by distance in a space, measured by a metric
// over that space, reports groups because you built them in. (The research log
// already flags the sibling trap: `Genome` COLOUR is a 3-D shadow of the
// `species` SimHash — the same vector — so it is not independent evidence
// either.)
//
// This measure is mechanism-level instead: an edge exists between two
// `Creature`s because doInteraction case 2 actually emitted a successor record
// from that pair. Nothing about it is computed from `Genome` space, so it
// cannot be circular with a `Genome`-space gate. Separation, if it is real,
// shows up here as the graph splitting into components that never exchange an
// edge.
//
// Read the two columns TOGETHER. The interesting result is precisely where they
// disagree — `species` can split while the graph stays connected (drift in the
// embedding with no change in who pairs with whom), and the graph can split
// while `species` stays at one (separation on a trait the SimHash is blind to).
// ----------------------------------------------------------------------------

namespace repro {

// One pairing event, reduced to the only thing the graph needs.
struct Edge {
    std::uint64_t a = 0, b = 0;   // Creature::id of the two records in the pair
};

// Number of connected components with at least `minMembers` members, over the
// graph whose vertices are the Creature ids appearing in `edges`.
//
// Union-find with path halving and union by size. O(E log E), dominated by the
// sort that maps sparse 64-bit ids onto dense indices.
//
// Determinism: connected components are a property of the edge SET, so the
// answer does not depend on the order edges are supplied or on which root a
// union picks. No rng, no floats.
//
// Degenerate cases are the ones worth stating, because a gate tight enough to
// stop all pairing produces them and the metric must stay readable rather than
// dividing by zero: no edges at all => 0 vertices => 0 components. A single
// disconnected pair is a component of size 2, so it is NOT counted at the
// default minMembers of 3 — that threshold is what keeps the metric from being
// dominated by one-off pairings.
inline int componentCount(const std::vector<Edge>& edges, int minMembers) {
    if (edges.empty()) return 0;

    // Dense-index the ids that actually appear.
    std::vector<std::uint64_t> ids;
    ids.reserve(edges.size() * 2);
    for (const Edge& e : edges) { ids.push_back(e.a); ids.push_back(e.b); }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

    const int n = (int)ids.size();
    std::vector<int> parent(n), size(n, 1);
    for (int i = 0; i < n; ++i) parent[i] = i;

    auto find = [&](int x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };
    auto index = [&](std::uint64_t id) {
        return (int)(std::lower_bound(ids.begin(), ids.end(), id) - ids.begin());
    };

    for (const Edge& e : edges) {
        int ra = find(index(e.a)), rb = find(index(e.b));
        if (ra == rb) continue;
        if (size[ra] < size[rb]) std::swap(ra, rb);
        parent[rb] = ra;
        size[ra] += size[rb];
    }

    int count = 0;
    for (int i = 0; i < n; ++i)
        if (find(i) == i && size[i] >= minMembers) ++count;
    return count;
}

}  // namespace repro
