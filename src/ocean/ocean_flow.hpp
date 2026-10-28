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
// and so are these. The velocity is CurrentField::velocityAt's sum, term for
// term, in the same order.
// ----------------------------------------------------------------------------
#include <cstdint>
#include <vector>

namespace oceanflow {

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
