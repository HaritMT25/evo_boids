#pragma once
#include "genome.hpp"
#include "sensors_actions.hpp"

// Shared connectivity embedding: fold a genome into a fixed-length vector indexed
// by (source, sink) cell, mirroring how buildNeuralNet() wires the net. Used by
// BOTH the SimHash species metric (stats.hpp) and genome colouring
// (genome_color.hpp), so the two can never drift apart.

constexpr int STATS_SRC = NUM_SENSES  + MAX_INTERNAL_NEURONS;  // sources
constexpr int STATS_SNK = NUM_ACTIONS + MAX_INTERNAL_NEURONS;  // sinks
constexpr int STATS_DIM = STATS_SRC * STATS_SNK;

inline int geneCell(const Gene& g) {
    const int src = (g.sourceType == SOURCE_SENSOR)
        ? (g.sourceNum % NUM_SENSES)
        : (NUM_SENSES + (g.sourceNum % MAX_INTERNAL_NEURONS));
    const int snk = (g.sinkType == SINK_ACTION)
        ? (g.sinkNum % NUM_ACTIONS)
        : (NUM_ACTIONS + (g.sinkNum % MAX_INTERNAL_NEURONS));
    return src * STATS_SNK + snk;
}
