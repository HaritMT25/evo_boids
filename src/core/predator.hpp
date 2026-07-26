#pragma once
#include <cmath>
#include "vec2.hpp"

// ============================================================
// Predator — a scripted hunter (no genome / neural net; that is Stage C).
//
//   * Stage A (cfg.predatorEconomy == false): immortal, fixed-count. It never
//     starves and never reproduces, so the ONLY thing under test is the chase
//     kinematics — predator speed vs prey speed, capture radius, and the
//     digestion cooldown. The energy/age fields below are simply unused.
//   * Stage B (cfg.predatorEconomy == true): the energy economy turns on. A
//     predator gains `energy` by capturing prey, burns metabolism + a kinetic
//     move-cost each second, reproduces above a threshold and starves below 0
//     (and optionally ages out). predator_pop becomes time-varying, giving the
//     coupled (Lotka-Volterra-ish) oscillator — see evo_boids context §9/§11.
//
// The hunt + the economy (nearest-prey query, steering, capture, burn, birth,
// death) live in World::updatePredators so they can reuse the creature spatial
// grid and the world's bookkeeping; this struct only holds the per-predator
// state that the hunt advances.
// ============================================================
struct Predator {
    Vec2  pos;
    Vec2  vel = {1.0f, 0.0f};  // heading (kept ~unit; speed is cfg.predatorSpeed)
    float digestTimer = 0.0f;  // seconds until it can capture again (>0 => digesting)

    // --- Stage B energy economy (only read/written when cfg.predatorEconomy) ---
    float energy = 0.0f;       // food energy; <0 => starves. Seeded to
                               // cfg.predatorNewbornEnergy at birth/spawn.
    float age    = 0.0f;       // seconds alive; only matters if predatorMaxAge>0.
    bool  alive  = true;       // cleared on death; the dead are erased after the
                               // hunt loop (mirrors the prey remove_if).

    // Forward unit vector (mirrors Creature::forward()).
    Vec2 forward() const {
        const float len = std::sqrt(vel.x * vel.x + vel.y * vel.y);
        if (len < 1e-3f) return {1.0f, 0.0f};
        return {vel.x / len, vel.y / len};
    }
};
