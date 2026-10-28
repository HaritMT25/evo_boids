#pragma once
#include <cstdint>

// ============================================================
// SENSORS — everything from biosim4 + learning_boids + boid-specific
// The genome wires whichever ones are useful; the rest get ignored.
// ============================================================
enum Sensor : uint8_t {
    // --- Position & boundaries (biosim4) ---
    LOC_X,                  // normalized X position in world
    LOC_Y,                  // normalized Y position in world
    BOUNDARY_DIST_X,        // X distance to nearest wall
    BOUNDARY_DIST_Y,        // Y distance to nearest wall
    BOUNDARY_DIST,          // distance to nearest edge overall

    // --- Own state ---
    OWN_VEL_X,              // own velocity X component
    OWN_VEL_Y,              // own velocity Y component
    OWN_SPEED,              // current speed (normalized)
    LAST_MOVE_DIR_X,        // direction of last movement X (biosim4)
    LAST_MOVE_DIR_Y,        // direction of last movement Y (biosim4)
    AGE,                    // own age (normalized)
    FOOD_RESERVES,          // own energy level
    OSCILLATOR,             // oscillating +-1 value (biosim4)
    RESPONSIVENESS,         // own responsiveness level (biosim4)

    // --- Boid neighbor sensors (for flocking) ---
    NEIGHBOR_COUNT,         // how many creatures nearby
    AVG_NEIGHBOR_DX,        // X offset toward avg neighbor pos (cohesion)
    AVG_NEIGHBOR_DY,        // Y offset toward avg neighbor pos (cohesion)
    AVG_NEIGHBOR_HEADING_X, // avg neighbor velocity X (alignment)
    AVG_NEIGHBOR_HEADING_Y, // avg neighbor velocity Y (alignment)
    NEAREST_NEIGHBOR_DIST,  // distance to closest creature (separation)

    // --- Directional population density (biosim4) ---
    POPULATION_DENSITY,     // population in neighborhood
    POPULATION_FWD,         // population in forward direction
    POPULATION_LR,          // population in left-right axis

    // --- Nearest neighbor attributes (learning_boids social) ---
    NEAREST_NEIGHBOR_AGE,   // age of nearest creature
    NEAREST_NEIGHBOR_FOOD,  // food reserves of nearest creature
    GENETIC_SIM_NEAREST,    // genetic similarity to nearest creature
    GENETIC_SIM_FWD,        // genetic similarity in forward direction (biosim4)

    // --- Food sensors ---
    NEAREST_FOOD_DX,        // X direction to nearest food
    NEAREST_FOOD_DY,        // Y direction to nearest food
    NEAREST_FOOD_DIST,      // distance to nearest food
    FOOD_DENSITY,           // amount of food in perception radius

    // --- Pheromone / signal sensors (biosim4) ---
    SIGNAL0,                // pheromone strength at current location
    SIGNAL0_FWD,            // pheromone strength in forward direction
    SIGNAL0_LR,             // pheromone strength in left-right axis

    // --- Probe sensors (biosim4) ---
    LONGPROBE_POP_FWD,      // long-range population probe forward

    // --- Current / flow (ocean currents) ---
    CURRENT_FWD,            // flow speed along own forward axis
    CURRENT_LR,             // flow speed along own left-right axis

    // --- Predator threat (Stage A predator) ---
    // Bearing to the NEAREST predator within own perception, relative to heading,
    // plus a proximity/urgency channel. All 0 when no predator is in range (so a
    // no-predator run just sees three dead inputs). Evasion cannot evolve if prey
    // can't perceive the threat, so these are mandatory for the predator stage.
    PREDATOR_FWD,           // +1 = predator dead ahead, -1 = directly behind
    PREDATOR_LR,            // +1 = predator to the right, -1 = to the left
    PREDATOR_PROX,          // 0 = far / none, -> 1 = right on top of me

    // --- Noise ---
    RANDOM_SENSOR,          // random noise input

#ifdef EVO_WORLD_OCEAN
    // --- Depth (the Ocean build only, src/ocean/; Classic never sees these) ---
    LOC_Z,                  // normalized depth, 0 = surface, 1 = floor
    OWN_VEL_Z,              // own vertical velocity (+ = sinking)
    NEAREST_FOOD_DZ,        // depth component of the unit vector to the nearest food
    AVG_NEIGHBOR_DZ,        // depth offset toward avg neighbor (cohesion)
    BOUNDARY_DIST_Z,        // distance to the nearer of surface and floor
#endif

    NUM_SENSES              // <<--- marker
};

// ============================================================
// ACTIONS — movement from biosim4 + social from learning_boids
// ============================================================
enum Action : uint8_t {
    // --- Movement (biosim4 + boid steering) ---
    STEER_X,                // lateral steering force
    STEER_Y,                // vertical steering force
    MOVE_FORWARD,           // continue in current direction
    MOVE_RANDOM,            // random movement impulse
    SPEED_ADJUST,           // accelerate / decelerate

    // --- Pheromone (biosim4) ---
    EMIT_SIGNAL0,           // emit pheromone at current location

    // --- Internal state (biosim4) ---
    SET_OSCILLATOR_PERIOD,  // adjust internal oscillator speed
    SET_RESPONSIVENESS,     // how strongly actions are applied

    // --- Social interactions (learning_boids) ---
    INTERACT_SHARE,         // willingness to share food
    INTERACT_REPRODUCE,     // willingness to reproduce
    INTERACT_FIGHT,         // willingness to fight
    INTERACT_IGNORE,        // willingness to ignore

#ifdef EVO_WORLD_OCEAN
    STEER_Z,                // vertical steering force (Ocean build only; + = dive)
#endif

    NUM_ACTIONS             // <<--- marker
};
