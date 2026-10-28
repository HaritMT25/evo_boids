#pragma once
// ----------------------------------------------------------------------------
// ocean_flow — the E1 flow's evaluation cache (OceanWorld::flowAt while
// flowE1()). Ocean-only: CurrentField (src/core/current.hpp) and Classic are
// untouched; the cache is derived scratch (tools/ocean_fingerprint.hpp does
// not hash it: everything in it is rebuilt from hashed state — the configs,
// the world size, the CurrentField's vortices, the frame — before it is read).
//
// The eddy current: OceanWorld copies the vortices once at init through
// CurrentField::vortexState() (Ocean never calls restoreVortexState), with the
// strength / core it built the field with, and recomputes the centres for each
// advanced frame with CurrentField::advance's own expressions. Before the first
// advance after init CurrentField's centres are all 0 (init only sizes them),
// and so are these. The velocity is CurrentField::velocityAt's sum, in the same
// vortex order with the same prefactor (sign · strength / core · e^{1/2}) and
// raw p − centre differences (no minimum image: the current is not periodic),
// but e^{−u}, u = (dx² + dy²) / (2 core²), comes from a table (radial below):
//   g(u) = lerp(E, u) − E[2048] for finite 0 <= u < 16, else 0 (the vortex
//   adds nothing), E[j] = e^{−j/128} (2048 intervals, the portable expm1f).
// Subtracting the last node makes g continuous at the cutoff; the field stays
// (−dy, dx)·g(r² / 2c²), a radial multiplier: divergence-free for any g.
// Error per vortex <= ~8.7e-6 · strength units/s (interpolation 7.63e-6·S at
// u = 1/2, the cutoff / endpoint shift 1.05e-6·S at u = 16; tests/ocean_tests.cpp
// flow_tables).
// ----------------------------------------------------------------------------
#include <cstdint>
#include <vector>
#include "../core/portable_math.hpp"

namespace oceanflow {

// ---- the eddies' radial factor ---------------------------------------------
inline constexpr int kRadialCells = 2048;
inline constexpr float kRadialPerUnit = 128.0f;   // nodes per unit of u
inline constexpr float kRadialCut = 16.0f;        // kRadialCells / kRadialPerUnit

struct RadialTable {
    float E[kRadialCells + 1];
    RadialTable() {
        for (int j = 0; j <= kRadialCells; ++j)   // e^{−u} as 1 / e^{u}: full relative precision
            E[j] = 1.0f / (evomath::expm1f((float)j * (1.0f / kRadialPerUnit)) + 1.0f);
    }
};
inline const RadialTable& radialTable() {
    static const RadialTable t;
    return t;
}

// g(u) into `g` for 0 <= u < 16; false (no contribution) otherwise, NaN included.
inline bool radial(const float* E, float u, float& g) {
    if (!(u >= 0.0f && u < kRadialCut)) return false;
    const float s = u * kRadialPerUnit;   // exact (a power of two), in [0, 2048)
    const int i = (int)s;
    const float t = s - (float)i;
    g = (E[i] + t * (E[i + 1] - E[i])) - E[kRadialCells];
    return true;
}

struct Eddy {
    float cx = 0.0f, cy = 0.0f;   // centre (CurrentField::advance's expressions)
    float signScale = 0.0f;       // sign · strength / core · e^{1/2}, as velocityAt forms it
};

struct FlowCache {
    // ---- the eddy current (CurrentField Mode::Eddies, strength > 0) ----
    bool curOn = false;           // set at init: the Ocean's eddy evaluator is in use
    bool curAdvanced = false;     // CurrentField::advance ran since init (else its centres are 0)
    bool curReady = false;        // eddies[].cx / cy hold the centres for (curFrame, curAdvKey)
    bool curAdvKey = false;
    unsigned long long curFrame = 0;
    float inv2c2 = 0.0f;          // 1 / (2 core²), velocityAt's expression
    std::vector<float> vort;      // CurrentField::vortexState() at init (9 floats per vortex)
    std::vector<Eddy> eddies;
};

}  // namespace oceanflow
