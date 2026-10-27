#pragma once
// ----------------------------------------------------------------------------
// body_model — tier-1 morphology (v0.23.0, opt-in via SimConfig::bodyModel).
//
// Two heritable per-creature parameters, Creature::bodySize and
// Creature::bodyElong, that make size and shape modulate the EXISTING
// speed / turn / metabolism model. No new physics, no new sensor, no format
// break. They are a SEPARATE parameter vector, deliberately NOT derived from
// the neural genome: attribution ("did the body or the brain change?") and
// future body-gating both need the body to vary independently of the wiring.
//
// The three factors below are plain float arithmetic — no transcendentals, no
// rng, no branches — so they are identical on every target the determinism
// contract covers (native serial == wasm).
//
// EXACTNESS AT THE DEFAULT, which is what makes the flag free when it is off:
// both parameters default to 1.0f, so speedFactor and turnFactor evaluate
// 2.0f / (1.0f + 1.0f) = 2.0f / 2.0f = 1.0f and massFactor is 1.0f. Multiplying
// an IEEE-754 float by exactly 1.0f returns that float unchanged (no rounding,
// no flush), so every apply site in world.hpp computes bit-for-bit what it
// computed before the body model existed. That is an argument, not a proof —
// the proof is the §6.1 byte-identity harness, which runs the pre-change and
// post-change binaries step for step and compares fingerprints.
//
// RNG: none of this draws. The ONLY draws the body model ever makes are the two
// per insertion in kernel::inheritFromPair, and only when cfg.bodyModel is true —
// see the comment there. That is what keeps the off-path random stream, and
// therefore every run in the existing corpus, untouched.
// ----------------------------------------------------------------------------
#include "agent_core.hpp"

// Clamp range shared by the inheritance step and the tests. 1.0 (the default)
// sits at the geometric middle: half-size to double-size, and the same for
// elongation. Wider bounds are a knob we do not have yet on purpose — an
// unbounded random walk would eventually park the whole population at an
// extreme and the factors would stop meaning anything.
inline constexpr float BODY_PARAM_MIN = 0.5f;
inline constexpr float BODY_PARAM_MAX = 2.0f;

// Bigger body => slower. 2/(1+size): 4/3 at the small extreme, 2/3 at the
// large one, exactly 1 at the default.
//
// NOTE what this does to the meaning of cfg.maxSpeed: it becomes the ceiling of
// a DEFAULT body, not a hard cap on the world. A body at the small rail gets a
// ceiling of 1.333 * maxSpeed (200 at the stock 150), and a body at the large
// rail gets 0.667 * maxSpeed (100). That is the intended trade — a small body is
// fast and cheap but the same metabolism/mass coupling gives it nothing else —
// but it means the OWN_SPEED sensor, which stays normalised by cfg.maxSpeed and
// is deliberately NOT rescaled per body, can read above 1.0 for a small creature
// and is capped well below 1.0 for a large one. That asymmetry is information
// the brain can evolve to use, which is why the sensor is left alone.
inline float speedFactor(const AgentCore& c) { return 2.0f / (1.0f + c.bodySize); }

// Longer body => worse turning. Same shape, driven by elongation. Only bites
// under --triangular-motion, which is the only mode that uses maxTurnRate.
inline float turnFactor(const AgentCore& c) { return 2.0f / (1.0f + c.bodyElong); }

// Metabolism scales linearly with size: a body twice as large costs twice as
// much to run. This is the term that pays for the speed the other two cost.
inline float massFactor(const AgentCore& c) { return c.bodySize; }
