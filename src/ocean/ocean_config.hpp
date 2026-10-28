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
// ----------------------------------------------------------------------------
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
    float cHold        = 0.0f;    // energy/s per unit |depth − bodyBuoy·D| / D
    float cPress       = 0.0f;    // energy/s per unit depth / D (pressure)
    // Where food appears in depth:
    //   0 = uniform over [0, D]
    //   1 = per patch: every food patch (SimConfig::foodPatches > 0) gets its own
    //       depth at init; food lands within ±foodDepthSpread of it (uniform if
    //       there are no patches)
    int   foodDepthMode   = 0;
    float foodDepthSpread = 30.0f;
};

// Every key defaults to the value already in `c` (the checkpoint reader's rule).
// `o` is the flat config object itself, not a {"config": ...} wrapper. "depth"
// is not read: oc_init's argument sets it.
inline void readOceanConfig(const jsonm::Value& o, OceanConfig& c) {
    if (!o.isObject()) return;
    c.kBuoy           = o.floatOr("kBuoy", c.kBuoy);
    c.buoyMutation    = o.floatOr("buoyMutation", c.buoyMutation);
    c.pitchMax        = o.floatOr("pitchMax", c.pitchMax);
    c.vDrag           = o.floatOr("vDrag", c.vDrag);
    c.wallBand        = o.floatOr("wallBand", c.wallBand);
    c.cHold           = o.floatOr("cHold", c.cHold);
    c.cPress          = o.floatOr("cPress", c.cPress);
    c.foodDepthMode   = o.intOr("foodDepthMode", c.foodDepthMode);
    c.foodDepthSpread = o.floatOr("foodDepthSpread", c.foodDepthSpread);
}

// The preview's SimConfig starting point (tuned with tools/ocean_probe.cpp).
inline void applyOceanDefaults(SimConfig& c) {
    c.perceptionRadius = 100.0f;
    c.interactRadius   = 36.0f;
    c.pickupRadius     = 24.0f;
    c.targetFood       = 2400;
}

// A flat cfg JSON object (null / empty = nothing to change) into both configs:
// SimConfig keys through the checkpoint reader, Ocean keys through
// readOceanConfig. Returns false with `err` set on malformed JSON.
inline bool readOceanJson(const char* json, SimConfig& sc, OceanConfig& oc, std::string& err) {
    if (!json || !json[0]) return true;
    const std::string wrapped = std::string("{\"config\":") + json + "}";
    jsonm::Value root;
    if (!jsonm::Parser::parse(wrapped, root, err)) { err = "config parse error: " + err; return false; }
    checkpoint::readConfig(root, sc);
    if (const jsonm::Value* o = root.find("config")) readOceanConfig(*o, oc);
    return true;
}

// True when every knob is usable (finite, and >= 0 where a sign makes no sense).
inline bool oceanConfigValid(const OceanConfig& c) {
    auto fin = [](float v) { return v == v && v - v == 0.0f; };  // finite, NaN-safe
    return fin(c.depth) && c.depth > 0.0f && fin(c.kBuoy) && fin(c.buoyMutation) &&
           c.buoyMutation >= 0.0f && fin(c.pitchMax) && c.pitchMax > 0.0f &&
           fin(c.vDrag) && c.vDrag >= 0.0f && fin(c.wallBand) && c.wallBand >= 0.0f &&
           fin(c.cHold) && fin(c.cPress) && fin(c.foodDepthSpread) && c.foodDepthSpread >= 0.0f;
}
