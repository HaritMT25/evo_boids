#pragma once
// ----------------------------------------------------------------------------
// ocean_config — the Ocean world's own settings, kept OUT of SimConfig so
// Classic's CLI and checkpoint keys stay what they are (PLAN_ecosystem §14.1).
// OceanWorld holds one beside WorldCommon::cfg. readOceanConfig takes the same
// flat JSON object the shim hands checkpoint::readConfig, so one cfgJson can
// carry SimConfig keys and these together.
//
// applyOceanDefaults is the Ocean preview's starting point for the SimConfig
// half (PLAN_ocean_preview §6: 3-D contact rates fall, so the ranges grow and
// the food stock grows; the spawn rate is unchanged). The shim and the probe
// both call it before reading any JSON, so the page gets the tuned world
// without passing anything.
//
// E1 physics (PLAN_ecosystem §4, §5.1-5.2; the reviewed E1 design): waves,
// depth-weighted deep currents, the light field (display only), depth costs
// as multiples of metabolism, and the size laws (drift, fights, mouth). Every
// E1 knob defaults OFF, and with all of them off the world is bit-identical to
// the preview's (oceanE1AtDefaults; the harness's OFF-identity cases).
//
// Every knob has a finite RANGE (oceanConfigError): no accepted config can
// make the state non-finite. The ranges are wide — every harness case and
// unit-test world is inside them — but bounded, e.g. kBuoy = -1e37 used to be
// accepted and overflowed vz to inf - inf = NaN within 50 steps.
// ----------------------------------------------------------------------------
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include "../core/config.hpp"
#include "../core/json_min.hpp"
#include "../core/checkpoint_io.hpp"   // checkpoint::readConfig: SimConfig's JSON keys

struct OceanConfig {
    float depth        = 240.0f;  // D: sea depth (0 = surface). oc_init's depth argument.
    float kBuoy        = 1.0f;    // buoyancy pull toward bodyBuoy·D, per second. Plan
                                  // start 0.5; 1.0 tuned (ocean_probe, 9 seeds x 30k):
                                  // both pass, 1.0 sweeps NEAREST_FOOD_DZ -> STEER_Z
                                  // more often (leaving neutral depth costs steering)
    float buoyMutation = 0.05f;   // half-width of the newborn's bodyBuoy draw
    float pitchMax     = 0.5f;    // |vz| ceiling (vz is a pitch: depth rate = vz·speed)
    float vDrag        = 2.0f;    // vertical drag, 1/s
    float wallBand     = 12.0f;   // soft push within this distance of surface / floor
    // Depth costs, after kernel::stepMetabolism, evaluated only when one is != 0.
    // H = |depth − bodyBuoy·D| / D (off neutral), P = depth / D (pressure).
    //   depthCostScaled false (the preview's): energy/s per unit H / P
    //   depthCostScaled true  (E1): dimensionless multiples of the fish's own
    //     metabolism M = metabolicRate·massFactor (bodySize):
    //     foodReserves −= dt·M·(cHold·H + cPress·P)
    float cHold        = 0.0f;
    float cPress       = 0.0f;
    // Where food appears in depth:
    //   0 = uniform over [0, D]
    //   1 = per patch: every food patch (SimConfig::foodPatches > 0) gets its own
    //       depth at init; food lands within ±foodDepthSpread of it (uniform if
    //       there are no patches)
    int   foodDepthMode   = 0;
    float foodDepthSpread = 30.0f;

    // ---- E1 physics: every knob below defaults OFF -------------------------
    // Waves: three fixed deep-water trains, modes (6,0) (2,3) (4,−2) over the
    // world, amplitude fractions .5/.3/.2 of waveSpeed, phases 0/1/2 rad, ω =
    // sqrt(waveGravity·k); tapered to zero normal flow at surface and floor
    // over b = min(waveBand, D/4). No randomness.
    float waveSpeed   = 0.0f;     // orbital speed scale, units/s (0 = no waves)
    float waveBand    = 32.0f;    // taper band (units)
    float waveGravity = 20.0f;    // dispersion constant (ω² = g·k)
    // The current (SimConfig current*) as a deep current: horizontal flow ×
    // f(d) = smoothstep(2d/D): still at the surface, full from mid-depth down.
    bool  deepCurrent = false;
    // Light (display only in E1: the page's depth shading). L(d) = L0·exp(−kWater·d).
    bool  lightEnabled = false;
    float L0          = 1.0f;
    float kWater      = 0.006f;   // 1/unit
    bool  depthCostScaled = false;  // see cHold / cPress
    // Size laws (L = bodySize): the flow moves a fish by min(1, 1/L) of itself;
    // fight strength × L²; a fish eats food only if food.size <= kMouth·L.
    bool  sizeDrift   = false;
    bool  sizeFight   = false;
    bool  mouthGate   = false;
    float kMouth      = 2.0f;
};

// True when every E1 knob holds its default (OFF) value, bit for bit. The
// fingerprint hashes the E1 knobs only when this is false, so a preview config
// hashes exactly as it did before E1.
inline bool oceanE1AtDefaults(const OceanConfig& c) {
    const OceanConfig d;
    auto same = [](float a, float b) { return std::memcmp(&a, &b, sizeof a) == 0; };
    return same(c.waveSpeed, d.waveSpeed) && same(c.waveBand, d.waveBand) &&
           same(c.waveGravity, d.waveGravity) && c.deepCurrent == d.deepCurrent &&
           c.lightEnabled == d.lightEnabled && same(c.L0, d.L0) && same(c.kWater, d.kWater) &&
           c.depthCostScaled == d.depthCostScaled && c.sizeDrift == d.sizeDrift &&
           c.sizeFight == d.sizeFight && c.mouthGate == d.mouthGate && same(c.kMouth, d.kMouth);
}

namespace ocean_cfg_detail {
// A bool key: true / false, or the numbers 0 / 1 (oc_set_param's "%.9g" text).
// Anything else under the key is an error. Absent = unchanged.
inline bool readBool(const jsonm::Value& o, const char* k, bool& out, std::string* err) {
    const jsonm::Value* v = o.find(k);
    if (!v) return true;
    if (v->type == jsonm::Value::Type::Bool) { out = v->b; return true; }
    if (v->type == jsonm::Value::Type::Number && (v->num == 0.0 || v->num == 1.0)) {
        out = (v->num == 1.0);
        return true;
    }
    if (err) *err = std::string(k) + " must be true / false (or 0 / 1)";
    return false;
}
}  // namespace ocean_cfg_detail

// Every key defaults to the value already in `c` (the checkpoint reader's rule).
// `o` is the flat config object itself, not a {"config": ...} wrapper. "depth"
// is not read: oc_init's argument sets it. Returns false (err set) when a bool
// key holds something other than true / false / 0 / 1; ranges are
// oceanConfigError's.
inline bool readOceanConfig(const jsonm::Value& o, OceanConfig& c, std::string* err = nullptr) {
    if (!o.isObject()) return true;
    c.kBuoy           = o.floatOr("kBuoy", c.kBuoy);
    c.buoyMutation    = o.floatOr("buoyMutation", c.buoyMutation);
    c.pitchMax        = o.floatOr("pitchMax", c.pitchMax);
    c.vDrag           = o.floatOr("vDrag", c.vDrag);
    c.wallBand        = o.floatOr("wallBand", c.wallBand);
    c.cHold           = o.floatOr("cHold", c.cHold);
    c.cPress          = o.floatOr("cPress", c.cPress);
    // Only 0 and 1 are modes; any other number is kept as -1, which the
    // validator refuses (an (int) cast of 1e300 would be undefined).
    if (const jsonm::Value* v = o.find("foodDepthMode"); v && v->type == jsonm::Value::Type::Number)
        c.foodDepthMode = (v->num == 0.0) ? 0 : (v->num == 1.0) ? 1 : -1;
    c.foodDepthSpread = o.floatOr("foodDepthSpread", c.foodDepthSpread);
    // E1
    c.waveSpeed   = o.floatOr("waveSpeed", c.waveSpeed);
    c.waveBand    = o.floatOr("waveBand", c.waveBand);
    c.waveGravity = o.floatOr("waveGravity", c.waveGravity);
    c.L0          = o.floatOr("L0", c.L0);
    c.kWater      = o.floatOr("kWater", c.kWater);
    c.kMouth      = o.floatOr("kMouth", c.kMouth);
    using ocean_cfg_detail::readBool;
    return readBool(o, "deepCurrent", c.deepCurrent, err) &&
           readBool(o, "lightEnabled", c.lightEnabled, err) &&
           readBool(o, "depthCostScaled", c.depthCostScaled, err) &&
           readBool(o, "sizeDrift", c.sizeDrift, err) &&
           readBool(o, "sizeFight", c.sizeFight, err) &&
           readBool(o, "mouthGate", c.mouthGate, err);
}

// The preview's SimConfig starting point (tuned with tools/ocean_probe.cpp).
inline void applyOceanDefaults(SimConfig& c) {
    c.perceptionRadius = 100.0f;
    c.interactRadius   = 36.0f;
    c.pickupRadius     = 24.0f;
    c.targetFood       = 2400;
}

// The E1 preset (tools/ocean_probe.cpp tuning, see the E1 report): waves, deep
// currents, light and depth costs on top of the preview defaults. A flat cfg
// JSON like any other; the page applies it with ?e1=1 (oc_e1_preset), the
// probe with --e1, and wasm/ocean_parity.sh runs it as a case (its copy is
// checked against this text).
inline constexpr const char* kE1PresetJson =
    R"({"waveSpeed":6,"deepCurrent":true,"currentStrength":5,"foodDrift":true,"lightEnabled":true,"depthCostScaled":true,"cHold":0.05,"cPress":0.03})";

// The int-typed SimConfig keys: checkpoint::readConfig reads each with
// jsonm::Value::intOr, a plain (int) cast of the JSON double, which is
// undefined behaviour outside int's range (x86 gives INT_MIN, wasm traps or
// saturates) and silently truncates a fraction ({"currentMode":0.5} was mode
// 0). readOceanJson refuses any such value before the reader sees it: the
// number must be a whole number in int's range. Must list exactly
// readConfig's intOr keys: wasm/ocean_parity.sh checks it.
inline constexpr const char* kSimConfigIntKeys[] = {
    "reproWindow", "reproMinCluster", "minPopulation", "maxPopulation", "targetFood",
    "maxNeighbors", "genomeMinLength", "genomeMaxLength", "genomeInitMin", "genomeInitMax",
    "currentMode", "currentEddies", "foodPatches", "predatorCount",
    "predatorMinPopulation", "predatorMaxPopulation",
};
inline bool isSimConfigIntKey(const std::string& k) {
    for (const char* i : kSimConfigIntKeys) if (k == i) return true;
    return false;
}
// x is a whole number in int's range (so (int)x is defined and exact). NaN and
// ±inf fail.
inline bool isIntValue(double x) {
    return x >= -2147483648.0 && x <= 2147483647.0 && std::trunc(x) == x;
}

// A flat cfg JSON object (null / empty = nothing to change) into both configs:
// SimConfig keys through the checkpoint reader, Ocean keys through
// readOceanConfig. Returns false with `err` set on malformed JSON, an int key
// whose number is not a whole number in int's range (checked before the
// reader casts it, every occurrence of the key), or a bool key that is not a
// bool.
inline bool readOceanJson(const char* json, SimConfig& sc, OceanConfig& oc, std::string& err) {
    if (!json || !json[0]) return true;
    const std::string wrapped = std::string("{\"config\":") + json + "}";
    jsonm::Value root;
    if (!jsonm::Parser::parse(wrapped, root, err)) { err = "config parse error: " + err; return false; }
    if (const jsonm::Value* o = root.find("config"); o && o->isObject())
        for (const auto& kv : o->obj)
            if (kv.second.type == jsonm::Value::Type::Number && isSimConfigIntKey(kv.first) &&
                !isIntValue(kv.second.num)) {
                char buf[160];
                std::snprintf(buf, sizeof buf, "%s must be a whole number that fits an int (got %.17g)",
                              kv.first.c_str(), kv.second.num);
                err = buf;
                return false;
            }
    checkpoint::readConfig(root, sc);
    if (const jsonm::Value* o = root.find("config"))
        if (!readOceanConfig(*o, oc, &err)) return false;
    return true;
}

// "" when every knob is inside its range, else which one is not and why. The
// ranges keep every product the step forms finite: e.g. kBuoy·(bodyBuoy·D −
// depth) ≤ 1e3·D, vz·vDrag·dt ≤ (pitchMax + 16)·1e4·dt before the clamp, the
// wave flow ≤ waveSpeed·(1 + 1.5/(b·k)) with b ≥ 1.
inline std::string oceanConfigError(const OceanConfig& c) {
    const struct { const char* name; float v, lo, hi; } r[] = {
        {"depth", c.depth, 10.0f, 100000.0f},
        {"kBuoy", c.kBuoy, -1000.0f, 1000.0f},
        {"buoyMutation", c.buoyMutation, 0.0f, 1.0f},
        {"pitchMax", c.pitchMax, 0.001f, 100.0f},
        {"vDrag", c.vDrag, 0.0f, 10000.0f},
        {"wallBand", c.wallBand, 0.0f, 100000.0f},
        {"cHold", c.cHold, 0.0f, 100.0f},
        {"cPress", c.cPress, 0.0f, 100.0f},
        {"foodDepthSpread", c.foodDepthSpread, 0.0f, 1000000.0f},
        {"waveSpeed", c.waveSpeed, 0.0f, 100.0f},
        {"waveBand", c.waveBand, 1.0f, 100000.0f},
        {"waveGravity", c.waveGravity, 0.01f, 10000.0f},
        {"L0", c.L0, 0.0f, 100.0f},
        {"kWater", c.kWater, 0.0f, 1.0f},
        {"kMouth", c.kMouth, 0.01f, 100.0f},
    };
    for (const auto& e : r)
        if (!(e.v >= e.lo && e.v <= e.hi)) {   // NaN fails both
            char buf[160];
            std::snprintf(buf, sizeof buf, "%s must be in [%g, %g] (got %g)", e.name, (double)e.lo,
                          (double)e.hi, (double)e.v);
            return buf;
        }
    if (c.foodDepthMode != 0 && c.foodDepthMode != 1) return "foodDepthMode must be 0 or 1";
    return "";
}

// True when every knob is usable (oceanConfigError is empty).
inline bool oceanConfigValid(const OceanConfig& c) { return oceanConfigError(c).empty(); }

// The Ocean's own ranges for SimConfig keys whose extreme FINITE values break
// the Ocean step (SimConfig itself is Classic's and stays as it is). "" when
// every one is inside, else which one is not and why. Checked wherever an
// Ocean config is accepted: oc_init / oc_set_param (wasm/ocean_wasm.cpp
// configOk), the harness's world builder (tools/ocean_fingerprint.hpp
// buildConfigs) and its live --set, the probe.
//   * The current: currentStrength in [0, 1000] u/s (cases use <= 120; 0 =
//     off), currentEddies in [0, 100], currentCore in [0, 1e5] (the field uses
//     max(core, 1)), currentDrift in [-100, 100], currentGyreEps in [-1, 1],
//     currentMode 0 (eddies) or 1 (gyre). The flow is then at most
//     currentEddies·currentStrength (eddies; each vortex peaks at strength) or,
//     per component, currentStrength·max(1, 2(H/W)(1 + 6|eps|)) (gyre, in a
//     W x H world), and the field's clock
//     (0.0003·drift·frame) stays finite. currentStrength 1e20 used to be
//     accepted: food moved ~1e17 per step and OceanWorld::wrapCoord's loops
//     never ended (wrapCoord is constant-time now too).
//   * foodPatchRadius in [0, 1e5]: a toroidal patch's food is placed at
//     wrapCoord(centre ± R); 1e30 sent that to the same endless loop.
// And the keys whose value reaches an (int) conversion while stepping, where
// an out-of-range value is undefined behaviour (and differs native / wasm):
//   * foodSpawnBase, foodSpawnAmp in [-1e4, 1e4] (defaults 3, 2): each step
//     spawns (int)(base + amp·sin(·)) times; 1e20 was accepted (finite).
//   * targetFood in [0, 1e6]: the spawn cap is targetFood·2 (int arithmetic),
//     and init lays out targetFood items.
//   * maxSpeed <= 2000 and predatorSpeed in [0, 2000] (defaults 150, 110): a
//     fish moves at most 2·maxSpeed·dt per step (speedFactor <= 2), a
//     predator 2·predatorSpeed·dt — at dt 0.016 under 64, less than the
//     smallest world (100), so the single add / subtract wraps keep positions
//     in the world, and SpatialGrid's (int)(position / cell) stays in range.
//   * every radius (perceptionRadius, interactRadius, pickupRadius,
//     predatorPerception, predatorCaptureRadius) <= 1e6: SpatialGrid casts
//     (centre ± radius) / cell to int, cell = perceptionRadius >= 1 (a
//     toroidal world already holds them under min(W, H) / 2; radiusError).
inline std::string oceanSimConfigError(const SimConfig& c) {
    const struct { const char* name; float v, lo, hi; } r[] = {
        {"currentStrength", c.currentStrength, 0.0f, 1000.0f},
        {"currentCore", c.currentCore, 0.0f, 100000.0f},
        {"currentDrift", c.currentDrift, -100.0f, 100.0f},
        {"currentGyreEps", c.currentGyreEps, -1.0f, 1.0f},
        {"foodPatchRadius", c.foodPatchRadius, 0.0f, 100000.0f},
        {"foodSpawnBase", c.foodSpawnBase, -10000.0f, 10000.0f},
        {"foodSpawnAmp", c.foodSpawnAmp, -10000.0f, 10000.0f},
        {"maxSpeed", c.maxSpeed, 0.0f, 2000.0f},
        {"predatorSpeed", c.predatorSpeed, 0.0f, 2000.0f},
        {"perceptionRadius", c.perceptionRadius, 0.0f, 1000000.0f},
        {"interactRadius", c.interactRadius, 0.0f, 1000000.0f},
        {"pickupRadius", c.pickupRadius, 0.0f, 1000000.0f},
        {"predatorPerception", c.predatorPerception, 0.0f, 1000000.0f},
        {"predatorCaptureRadius", c.predatorCaptureRadius, 0.0f, 1000000.0f},
    };
    for (const auto& e : r)
        if (!(e.v >= e.lo && e.v <= e.hi)) {   // NaN fails both
            char buf[160];
            std::snprintf(buf, sizeof buf, "%s must be in [%g, %g] (got %g)", e.name, (double)e.lo,
                          (double)e.hi, (double)e.v);
            return buf;
        }
    if (c.currentMode != 0 && c.currentMode != 1) return "currentMode must be 0 (eddies) or 1 (gyre)";
    if (c.currentEddies < 0 || c.currentEddies > 100) return "currentEddies must be in [0, 100]";
    if (c.targetFood < 0 || c.targetFood > 1000000) return "targetFood must be in [0, 1000000]";
    return "";
}
