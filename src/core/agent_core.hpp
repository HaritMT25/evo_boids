#pragma once
// ----------------------------------------------------------------------------
// agent_core — everything about a creature that does not depend on the world's
// geometry: identity and provenance, energy and age, the timers, the biosim4
// internal state, the heritable body parameters, and the genome with the
// network built from it.
//
// A world adds the geometric part (position, heading, body trail) by deriving
// from AgentCore — the 2-D Creature does (creature.hpp) — so every field keeps
// its name (c.genome, c.age, c.bodySize) and no caller changes. The steps both
// kinds of world run the same way are in kernel.hpp; the world-level records
// (counters, logs, ids) are in world_common.hpp.
//
// NUM_SENSES / NUM_ACTIONS come from sensors_actions.hpp, so this struct and
// the kernel take the sensor list of the build that compiles them; a world
// with a different list is a different build.
// ----------------------------------------------------------------------------
#include <cstdint>
#include "portable_math.hpp"
#include "random.hpp"
#include "color.hpp"
#include "genome_color.hpp"
#include "genome.hpp"
#include "sensors_actions.hpp"

struct AgentCore {
    bool alive = true;
    std::uint64_t id = 0;  // stable identity (assigned by WorldCommon on creation), so a
                           // click-selected creature can be tracked as the vector
                           // is reordered by death/reproduction. 0 = unassigned.
    std::uint64_t lineageId = 0;  // founder id of this creature's maternal line:
                           // own id for a spawned/reseeded founder, else inherited
                           // from parent1 at birth. Lets the cluster map colour by
                           // clade and lets selective sweeps be measured. Dynamics-
                           // neutral (integer copy only; never touches the rng).
    std::uint32_t generation = 0;  // 0 for a spawned/reseeded founder, else
                           // max(source records' generation) + 1 at birth.
                           // Generations overlap (continuous birth and death),
                           // so this is a depth count, not a synchronised epoch.
                           // Display-only and dynamics-neutral, like lineageId.
    float speed = 80.0f;
    float age = 0.0f;
    float foodReserves = 30.0f;
    float reproTimer = 0.0f;
    float interactTimer = 0.0f;  // cooldown between social interactions

    // biosim4 internal state
    float responsiveness = 0.5f;  // 0..1, scales action outputs
    float oscPeriod = 34.0f;      // oscillator period
    float oscPhase = 0.0f;        // current oscillator phase

    // --- Tier-1 body model (v0.23.0; see body_model.hpp) -------------------
    // Heritable morphology, held OUTSIDE the neural genome on purpose so the
    // body can vary independently of the wiring. Both default to 1.0, which
    // makes every derived factor exactly 1.0 and every apply site bit-identical
    // to the pre-0.23 code. They only ever move when SimConfig::bodyModel is on
    // (during a run the inheritance step, kernel::inheritFromPair, is the sole
    // writer — a checkpoint load restores them; a founder or a reseeded
    // creature keeps the default).
    float bodySize  = 1.0f;   // mass / scale proxy, clamped to [0.5, 2.0]
    float bodyElong = 1.0f;   // elongation,         clamped to [0.5, 2.0]

    Genome genome;
    NeuralNet nnet;

    // Last neural I/O, captured each step ONLY when SimConfig.captureActivations
    // is on (i.e. when there's a window to inspect them; off in headless so it
    // costs nothing). The hidden-neuron outputs already persist in nnet.neurons[];
    // sensors and actions are otherwise transient, so the live overlay needs these.
    float lastSensors[NUM_SENSES] = {};
    float lastActions[NUM_ACTIONS] = {};

    // Visual (colour only — no rendering dependency)
    Color color;

    // The geometry-free part of a world's init(). A world calls it AFTER its own
    // position/heading draws: the 2-D Creature::init draws vel's two components
    // first and the oscillator phase here second, and that order is the random
    // stream every archived run was made with.
    void initCore() {
        oscPhase = rng.range(0.0f, 6.28f);

        buildNeuralNet(genome, nnet);

        // Locality-preserving colour from the genome (see genome_color.hpp):
        // similar genomes -> similar colours, so lineages drift smoothly.
        color = genomeColor(genome);
    }

    float getOscillator() const {
        return evomath::sinf(oscPhase);
    }
};
