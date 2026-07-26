#pragma once
#include <array>
#include <string>
#include "world.hpp"
#include "sensors_actions.hpp"

// Per-sensor "wiring frequency": the fraction of the live population whose genome
// uses each sensor as a SOURCE in at least one connection. Sampled over time, this
// shows which inputs selection actually recruits as a run evolves — e.g. the
// PREDATOR_* and neighbour sensors lighting up once predation is on. Cheap: scanned
// only at log intervals, and only when --wiring-log is set.

namespace wiring {

// Names in Sensor-enum order (sensors_actions.hpp). The static_assert below fails
// the build if a sensor is added/removed without updating this table.
inline const std::array<const char*, NUM_SENSES> kSensorNames = {
    "LOC_X","LOC_Y","BOUNDARY_DIST_X","BOUNDARY_DIST_Y","BOUNDARY_DIST",
    "OWN_VEL_X","OWN_VEL_Y","OWN_SPEED","LAST_MOVE_DIR_X","LAST_MOVE_DIR_Y",
    "AGE","FOOD_RESERVES","OSCILLATOR","RESPONSIVENESS",
    "NEIGHBOR_COUNT","AVG_NEIGHBOR_DX","AVG_NEIGHBOR_DY",
    "AVG_NEIGHBOR_HEADING_X","AVG_NEIGHBOR_HEADING_Y","NEAREST_NEIGHBOR_DIST",
    "POPULATION_DENSITY","POPULATION_FWD","POPULATION_LR",
    "NEAREST_NEIGHBOR_AGE","NEAREST_NEIGHBOR_FOOD","GENETIC_SIM_NEAREST","GENETIC_SIM_FWD",
    "NEAREST_FOOD_DX","NEAREST_FOOD_DY","NEAREST_FOOD_DIST","FOOD_DENSITY",
    "SIGNAL0","SIGNAL0_FWD","SIGNAL0_LR","LONGPROBE_POP_FWD",
    "CURRENT_FWD","CURRENT_LR",
    "PREDATOR_FWD","PREDATOR_LR","PREDATOR_PROX","RANDOM_SENSOR",
};
static_assert(kSensorNames.size() == NUM_SENSES,
              "kSensorNames is out of sync with the Sensor enum");

// Fraction [0,1] of the population wiring each sensor as a source. 0 if no creatures.
inline void fractions(const World& world, float out[NUM_SENSES]) {
    for (int s = 0; s < NUM_SENSES; ++s) out[s] = 0.0f;
    const std::size_t pop = world.creatures.size();
    if (pop == 0) return;
    long counts[NUM_SENSES] = {};
    for (const Creature& c : world.creatures) {
        bool used[NUM_SENSES] = {};
        for (const Gene& g : c.nnet.connections)
            if (g.sourceType == SOURCE_SENSOR && g.sourceNum < NUM_SENSES) used[g.sourceNum] = true;
        for (int s = 0; s < NUM_SENSES; ++s) if (used[s]) ++counts[s];
    }
    for (int s = 0; s < NUM_SENSES; ++s) out[s] = (float)counts[s] / (float)pop;
}

inline std::string header() {
    std::string h = "step";
    for (int s = 0; s < NUM_SENSES; ++s) { h += ','; h += kSensorNames[s]; }
    return h;
}

}  // namespace wiring
