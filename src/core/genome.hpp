#pragma once
#include <cstdint>
#include <vector>
#include <cmath>
#include <algorithm>
#include "sensors_actions.hpp"
#include "random.hpp"

// Max number of internal (hidden) neurons per creature
constexpr int MAX_INTERNAL_NEURONS = 12;

// Genome length bounds — evolution can grow/shrink genomes within this range
constexpr int GENOME_MIN_LENGTH = 16;
constexpr int GENOME_MAX_LENGTH = 80;
constexpr int GENOME_INITIAL_MIN = 24;
constexpr int GENOME_INITIAL_MAX = 48;

// --- Gene: one connection in the neural net ---
// Adapted from biosim4. Each gene encodes: source -> sink with a weight.
// Source can be a sensor or internal neuron.
// Sink can be an action or internal neuron.
constexpr uint8_t SOURCE_SENSOR = 1;
constexpr uint8_t SOURCE_NEURON = 0;
constexpr uint8_t SINK_ACTION = 1;
constexpr uint8_t SINK_NEURON = 0;

struct Gene {
    uint16_t sourceType : 1;  // SENSOR or internal NEURON
    uint16_t sourceNum  : 7;
    uint16_t sinkType   : 1;  // ACTION or internal NEURON
    uint16_t sinkNum    : 7;
    int16_t  weight;

    float weightAsFloat() const { return weight / 8192.0f; }

    static Gene makeRandom() {
        Gene g;
        g.sourceType = rng.uint(1);
        g.sourceNum  = rng.uint(127);
        g.sinkType   = rng.uint(1);
        g.sinkNum    = rng.uint(127);
        g.weight     = static_cast<int16_t>(rng.uint(0xFFFF) - 0x8000);
        return g;
    }
};

using Genome = std::vector<Gene>;

// --- NeuralNet: derived from genome ---
// Free-topology: no fixed layers, any source can connect to any sink.
struct NeuralNet {
    struct Neuron {
        float output = 0.5f;
        bool driven = false;  // undriven neurons have fixed output
    };

    std::vector<Gene> connections;
    std::vector<Neuron> neurons;
};

// --- Genome operations ---

inline Genome makeRandomGenome(int initMin = GENOME_INITIAL_MIN,
                               int initMax = GENOME_INITIAL_MAX) {
    int length = rng.intRange(initMin, initMax);
    Genome genome;
    genome.reserve(length);
    for (int i = 0; i < length; ++i) {
        genome.push_back(Gene::makeRandom());
    }
    return genome;
}

// Build neural net wiring from genome
// Adapted from biosim4's createWiringFromGenome
inline void buildNeuralNet(const Genome& genome, NeuralNet& nnet) {
    nnet.connections.clear();
    nnet.neurons.clear();
    nnet.neurons.resize(MAX_INTERNAL_NEURONS);

    // Remap gene source/sink numbers to valid indices
    for (auto gene : genome) {
        if (gene.sourceType == SOURCE_SENSOR) {
            gene.sourceNum %= NUM_SENSES;
        } else {
            gene.sourceNum %= MAX_INTERNAL_NEURONS;
        }

        if (gene.sinkType == SINK_ACTION) {
            gene.sinkNum %= NUM_ACTIONS;
        } else {
            gene.sinkNum %= MAX_INTERNAL_NEURONS;
        }

        nnet.connections.push_back(gene);
    }

    // Mark neurons that receive input as "driven"
    for (const auto& conn : nnet.connections) {
        if (conn.sinkType == SINK_NEURON) {
            nnet.neurons[conn.sinkNum].driven = true;
        }
    }
}

// Feed sensor values through the neural net, return action levels
inline void feedForward(NeuralNet& nnet, const float sensors[], float actions[]) {
    // Reset neuron accumulators (stack-allocated; MAX_INTERNAL_NEURONS is a
    // compile-time constant, so no per-call heap allocation, and being a
    // local it is automatically private to each OpenMP thread).
    float neuronAccum[MAX_INTERNAL_NEURONS] = {};

    // Zero action outputs
    for (int i = 0; i < NUM_ACTIONS; ++i) {
        actions[i] = 0.0f;
    }

    // First pass: accumulate inputs to neurons and actions
    for (const auto& conn : nnet.connections) {
        // Get source value
        float sourceVal;
        if (conn.sourceType == SOURCE_SENSOR) {
            sourceVal = sensors[conn.sourceNum];
        } else {
            sourceVal = nnet.neurons[conn.sourceNum].output;
        }

        // Apply weight and send to sink
        float contribution = sourceVal * conn.weightAsFloat();

        if (conn.sinkType == SINK_ACTION) {
            actions[conn.sinkNum] += contribution;
        } else {
            neuronAccum[conn.sinkNum] += contribution;
        }
    }

    // Apply activation function to internal neurons
    for (int i = 0; i < MAX_INTERNAL_NEURONS; ++i) {
        if (nnet.neurons[i].driven) {
            nnet.neurons[i].output = std::tanh(neuronAccum[i]);
        }
    }

    // Apply tanh to action outputs too
    for (int i = 0; i < NUM_ACTIONS; ++i) {
        actions[i] = std::tanh(actions[i]);
    }
}

// --- Reproduction ---

inline Genome crossover(const Genome& parent1, const Genome& parent2) {
    // Pick genome length from one parent randomly
    size_t len = rng.uint(1) ? parent1.size() : parent2.size();
    Genome child;
    child.reserve(len);

    for (size_t i = 0; i < len; ++i) {
        // For each gene slot, pick from either parent
        if (rng.uint(1) == 0 && i < parent1.size()) {
            child.push_back(parent1[i]);
        } else if (i < parent2.size()) {
            child.push_back(parent2[i]);
        } else if (i < parent1.size()) {
            child.push_back(parent1[i]);
        } else {
            child.push_back(Gene::makeRandom());
        }
    }
    return child;
}

inline void mutateGenome(Genome& genome, float rate = 0.01f,
                         int minLen = GENOME_MIN_LENGTH,
                         int maxLen = GENOME_MAX_LENGTH) {
    // Weight mutations and rewiring
    for (auto& gene : genome) {
        if (rng.unit() < rate) {
            // Mutate the weight
            gene.weight += static_cast<int16_t>(rng.intRange(-1000, 1000));
        }
        if (rng.unit() < rate * 0.5f) {
            // Rarely rewire a connection entirely
            gene = Gene::makeRandom();
        }
    }

    // Gene insertion — genome can grow
    if (rng.unit() < rate * 2.0f && (int)genome.size() < maxLen) {
        int pos = rng.intRange(0, (int)genome.size());
        genome.insert(genome.begin() + pos, Gene::makeRandom());
    }

    // Gene deletion — genome can shrink
    if (rng.unit() < rate * 2.0f && (int)genome.size() > minLen) {
        int pos = rng.intRange(0, (int)genome.size() - 1);
        genome.erase(genome.begin() + pos);
    }

    // Gene duplication — copy an existing gene to a new position (rare)
    if (rng.unit() < rate * 0.5f && (int)genome.size() < maxLen) {
        int src = rng.intRange(0, (int)genome.size() - 1);
        Gene dup = genome[src];
        // Slightly mutate the duplicate's weight
        dup.weight += static_cast<int16_t>(rng.intRange(-500, 500));
        genome.push_back(dup);
    }
}

inline float genomeSimilarity(const Genome& g1, const Genome& g2) {
    size_t minLen = std::min(g1.size(), g2.size());
    if (minLen == 0) return 0.0f;

    int matches = 0;
    for (size_t i = 0; i < minLen; ++i) {
        if (g1[i].sourceType == g2[i].sourceType &&
            g1[i].sinkType == g2[i].sinkType) {
            matches++;
        }
    }
    return static_cast<float>(matches) / static_cast<float>(minLen);
}
