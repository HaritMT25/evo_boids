#pragma once
#include <cstddef>
#include "core/world.hpp"

// ============================================================
// Rendering / platform boundary.
//
// The simulation core knows nothing about this header. `main` programs
// against IRenderer, so the SFML backend (SfmlRenderer) is just one
// implementation — it can be swapped for another graphics library, or a
// headless/null renderer in tests, without touching the simulation.
//
// Every type crossing this boundary is SFML-free (Vec2/Color/World plus
// the plain-data structs below).
// ============================================================

// One frame's discrete, edge-triggered input. The backend translates raw
// window/keyboard events into these so `main` never sees an SFML event.
struct InputEvents {
    bool quit = false;           // window closed / quit requested
    bool togglePause = false;    // pause <-> run
    bool speedUp = false;        // increase steps-per-frame
    bool speedDown = false;      // decrease steps-per-frame
    bool toggleTurbo = false;    // headless burst mode <-> live rendering (T)
    bool saveCheckpoint = false; // write a checkpoint of the current state (S)
};

// Everything the renderer needs to draw the on-screen HUD. Plain data,
// assembled by the caller from the simulation state.
struct HudInfo {
    std::size_t population = 0;
    std::size_t food = 0;
    std::size_t predators = 0;
    unsigned long long frame = 0;
    int births = 0;
    int deaths = 0;
    float avgGenome = 0.0f;
    int simSpeed = 1;
    int fps = 0;
    bool paused = false;
    bool turbo = false;          // headless burst mode active
    float stepsPerSec = 0.0f;    // measured sim throughput while in turbo
};

class IRenderer {
public:
    virtual ~IRenderer() = default;

    // Is the output surface (window) still open?
    virtual bool isOpen() const = 0;

    // Seconds elapsed since the previous call — drives the sim timestep.
    virtual float frameDelta() = 0;

    // Pump platform events and return edge-triggered actions for this frame.
    virtual InputEvents poll() = 0;

    // Draw one complete frame: clear, world, HUD, then present.
    virtual void render(const World& world, const HudInfo& hud) = 0;
};
