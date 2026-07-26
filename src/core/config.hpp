#pragma once
#include <string>
#include <sstream>
#include "genome.hpp"  // GENOME_* defaults — single source of truth for bounds

// ----------------------------------------------------------------------------
// SimConfig — runtime-tunable simulation parameters.
//
// Every default below equals the value that used to be hard-coded, so a run
// with no flags behaves exactly as before. World holds one of these (World::cfg)
// and reads from it instead of compile-time constants, so these can be set from
// the command line without recompiling.
//
// Deliberately NOT exposed (structural, not tuning knobs): NUM_SENSES /
// NUM_ACTIONS (sensor/action enums) and MAX_INTERNAL_NEURONS (sizes the neural
// net and the stats connectivity grid at compile time).
// ----------------------------------------------------------------------------
struct SimConfig {
    // --- World topology ---
    // Toroidal perception (opt-in, off = byte-identical historical behaviour).
    // Positions have ALWAYS wrapped; this makes the SENSES wrap too: neighbour/
    // food/predator queries and offsets use minimum-image distances across the
    // seam, the pheromone grid deposits and reads wrap, the long probe looks
    // through the seam, and the boundary sensors report "no wall anywhere"
    // (a torus has no walls). Without it the seam is a sensory cliff — an
    // edge-band creature perceives roughly HALF its true neighbours (measured:
    // 3.0 seen vs 5.8 physically within range), which breeds wall-hugging
    // phenotypes. Currents stay non-toroidal (the ocean is a bounded basin;
    // a wrapped traveller feels a small velocity discontinuity at the seam).
    bool toroidal = false;

    // --- Population ---
    int minPopulation = 50;   // reseed floor: random creatures injected below this
    int maxPopulation = 0;    // hard birth cap (0 = unlimited)

    // --- Food ---
    int   targetFood    = 600;   // baseline food count (hard cap is 2x this)
    float foodSpawnBase = 3.0f;  // mean food spawned per step
    float foodSpawnAmp  = 2.0f;  // sinusoidal abundance swing around the base
    float foodEnergy    = 5.0f;  // energy gained per food eaten
    float pickupRadius  = 10.0f; // distance within which food is eaten

    // --- Energy / lifespan (selection pressure) ---
    float metabolicRate = 1.5f;   // energy burned per second (fixed, per creature)
    float geneCost      = 0.0f;   // extra energy/sec per gene (parsimony pressure; 0 = off)
    float moveCost      = 0.0f;   // kinetic cost: extra energy/sec = moveCost * speed^2
                                  // (0 = off => speed is free, reproducing prior behavior;
                                  //  >0 makes thrust expensive, so there's an interior
                                  //  optimal cruise speed and riding a current can pay)
    float maxAge        = 120.0f; // death age
    float reproEnergy   = 20.0f;  // energy each parent needs to reproduce
    float newbornEnergy = 25.0f;  // starting energy of a child

    // --- Sensing / interaction ranges ---
    float perceptionRadius = 80.0f; // neighbour/food sensing radius (also grid cell)
    float interactRadius   = 20.0f; // social-interaction range

    // Topological interaction cap: attend to at most the N NEAREST neighbours
    // inside perceptionRadius rather than every one of them. 0 = off = metric
    // interaction (attend to all), which is the historical behaviour and is
    // preserved byte-for-byte.
    //
    // WHY THIS IS A MODEL IMPROVEMENT, NOT JUST A SPEED KNOB: real schooling and
    // flocking animals interact topologically, not metrically. Ballerini et al.
    // (PNAS 2008) showed starlings track a fixed ~6-7 nearest neighbours
    // regardless of flock density, and fish studies find an even smaller
    // effective set — Katz et al. (PNAS 2011) and Herbert-Read et al. (PNAS
    // 2011) both find golden shiner / mosquitofish motion is explained largely
    // by the 1-2 nearest neighbours. A metric rule makes a fish in a dense
    // school average over ~150 neighbours, which no real animal does (and which
    // is what makes dense worlds expensive here).
    //
    // 7 is the recommended setting (--topological): the canonical Ballerini
    // number, and the conservative end of the fish range, so it constrains
    // attention without discarding the schooling signal. Set 1-4 to test the
    // stronger fish-specific claim.
    //
    // Only the per-neighbour INTERACTION terms are capped (average neighbour
    // offset/heading, fore-aft and left-right population balance). The density
    // percepts NEIGHBOR_COUNT and POPULATION_DENSITY still see the true count
    // inside the radius, since crowding is something a fish genuinely senses
    // and evolution should keep access to it.
    int maxNeighbors = 0;

    // --- Cooldowns ---
    float reproCooldown    = 3.0f;
    float interactCooldown = 2.0f;

    // --- Movement ---
    float minSpeed = 30.0f;
    float maxSpeed = 150.0f;
    // Triangular action space (opt-in; off = original decoupled steer/speed model).
    // When on, the net commands a forward DRIVE and a TURN, and the feasible turn is
    // throttled by drive: turnEff = turn*(1 - drive). This realises the triangular
    // (forward, turn) feasible region of Malik/Singh/Harpaz/Engert/Rajan 2025
    // (arXiv:2510.03699, Fig 1d / S2.1): "larger forward speeds permit only smaller
    // turns" — apex at max drive (strike, no turn), base at zero drive (free turn).
    // NOTE the paper's clean strike/reorient bout split emerges under their FULL
    // constraint set (this coupling + linear hinge speed/turn costs + binocular sensing
    // + vergence cost) trained with PPO; the coupling alone is necessary, not sufficient,
    // and our setting is evolutionary not RL — so bimodality is not guaranteed here yet.
    // Their model is bout-based (125 ms); ours integrates heading continuously per step.
    // For their exact triangle (near-zero forward during a reorient) pair with --min-speed 0.
    // Drive maps to speed in [minSpeed, maxSpeed]; turn to heading change up to maxTurnRate.
    bool  triangularMotion = false;
    float maxTurnRate = 4.0f;   // max heading change (rad/s) at zero forward drive

    // --- Genome ---
    int   genomeMinLength = GENOME_MIN_LENGTH;   // hard lower bound on gene count
    int   genomeMaxLength = GENOME_MAX_LENGTH;   // hard upper bound on gene count
    int   genomeInitMin   = GENOME_INITIAL_MIN;  // initial gene-count range (low)
    int   genomeInitMax   = GENOME_INITIAL_MAX;  // initial gene-count range (high)
    float mutationMultiplier = 1.0f;             // scales the per-birth mutation rate

    // --- Currents (ocean flow; off by default) ---
    float currentStrength = 0.0f;   // peak flow speed, world units/sec (0 = off)
    int   currentMode     = 0;      // 0 = eddies (moving vortices), 1 = double-gyre
    int   currentEddies   = 5;      // [eddies] number of vortices ("eyes")
    float currentCore     = 150.0f; // [eddies] eddy radius (peak speed at this distance)
    float currentDrift    = 1.0f;   // how fast the field evolves (0 = static)
    float currentGyreEps  = 0.25f;  // [gyre] how far the dividing line oscillates

    // --- Spatial food (off by default => uniform spawn, static food) ---
    int   foodPatches     = 0;      // number of food patches (0 = uniform random)
    float foodPatchRadius = 120.0f; // spread of food around each patch centre
    bool  foodDrift       = false;  // advect food with the current (forms filaments)

    // --- Predator (Stage A: scripted, immortal, fixed-count; OFF by default) ---
    // predatorCount 0 => no predators => the no-predator baseline (unchanged
    // dynamics aside from the bumped NUM_SENSES, which is the expected fresh
    // genome baseline). All of these are the Stage A tuning knobs: the goal is
    // to find the speed/radius/cooldown window where evasion appears in the prey
    // AND neither side goes extinct (see context §9).
    int   predatorCount         = 0;      // number of predators (0 = none)
    float predatorPerception    = 150.0f; // predator's prey-detection radius. LARGER than
                                          // the prey's --perception (80) => ambush
                                          // asymmetry; set equal for symmetric sensing.
    float predatorSpeed         = 110.0f; // predator cruise speed. Prey calm cruise is ~82
                                          // under move-cost 7e-5 (so a predator out-cruises
                                          // a foraging prey), but prey can sprint to
                                          // --max-speed (150) to flee at a quadratic energy
                                          // cost => a real chase/escape tension.
    float predatorCaptureRadius = 14.0f;  // contact distance at which a prey is caught
                                          // (a touch larger than the food --pickup-radius).
    float predatorCooldown      = 3.0f;   // digestion cooldown (s) after a capture: caps
                                          // each predator's kill rate (first anti-spiral knob).

    // --- Predator energy economy (Stage B: predators starve / reproduce; OFF by
    // default => Stage A immortal/fixed-count is preserved byte-for-byte). When
    // predatorEconomy is true, a predator gains energy per capture, burns
    // metabolism + a kinetic move-cost per second, buds a child above
    // predatorReproEnergy and dies below 0 energy (and optionally past
    // predatorMaxAge). The whole point is the coupled oscillator: prey crash ->
    // predators starve -> predator decline -> prey rebound -> predators recover.
    // TUNE meal/metabolism/repro on real runs until NEITHER population's trough
    // hits zero (there is deliberately no predator reseed floor — extinction is
    // an observable failure mode, not something to mask). See context §9/§11.
    // NOTE: Stage B adds NO sensors, so NUM_SENSES is unchanged (41) and 0.9.x
    // predator runs stay comparable. (Genome/neural predators are Stage C.)
    bool  predatorEconomy       = false;  // master switch: false => Stage A (immortal)
    float predatorMetabolism    = 2.0f;   // predator fixed energy burn/sec (prey: 1.5)
    float predatorMoveCost      = -1.0f;  // predator kinetic cost/sec = coeff*speed^2.
                                          // <0 => reuse the prey --move-cost coefficient;
                                          // >=0 => predator-specific coefficient.
    float predatorReproEnergy   = 60.0f;  // energy needed to bud a child
    float predatorNewbornEnergy = 30.0f;  // child's starting energy (and the initial
                                          // predators' energy); the parent pays this on
                                          // each birth (energy-conserving budding).
    float predatorMeal          = -1.0f;  // energy gained per capture. <0 => absorb the
                                          // victim's foodReserves (the lean: carnivory is
                                          // calorie-dense, and the spiral-prone lever to
                                          // tune); >=0 => a fixed meal value (gentler).
    float predatorMaxAge        = 0.0f;   // predator death age. 0 => immortal-until-starved
                                          // (the lean); >0 => predators also die of old age.

    // --- Inspection & analysis (visual/diagnostic; do not affect dynamics) ---
    // Capture each creature's last sensor inputs / action outputs every step so a
    // click-selected creature's neural net can be drawn "firing live". Off by
    // default (zero cost in headless); main turns it on when there is a window.
    bool captureActivations = false;
    // Death-cluster map: log a 2-D projection of each dead creature's network and
    // write <name>.deaths.csv + <name>.cluster.svg at end of run. Off by default
    // (keeps default runs byte-for-byte and allocation-free). See world.hpp.
    bool clusterMap   = false;
    int  clusterMax   = 20000;  // max points kept in RAM (uniform decimation above this)
    int  clusterEpoch = 1000;   // deaths per colour band ("changes gradient every N")
    bool clusterPca   = false;  // map axes = PCA over the death sketches (per-run,
                                // variance-aligned) instead of the fixed random map.
    int  clusterColor = 0;      // 0 = colour by epoch (time); 1 = colour by lineage (clade)
    // Per-sensor wiring frequency over time -> <name>.wiring.csv (one row per log
    // interval: fraction of the population whose genome wires each sensor as a
    // source). Headless only, off by default. Reveals which sensors selection
    // recruits as the run evolves (e.g. predator/neighbour sensors under predation).
    bool wiringLog = false;
    // Phase profiler: accumulate per-phase timings in World and print at end of run
    // (build / parallel{gather,feedForward,apply} / serial). Diagnostic only.
    bool profile = false;

    // --- Numerics ---
    // Swap std::tanh for a rational approximation in the neural net (see
    // fastTanhApprox in genome.hpp). std::tanh is >half of feedForward's cost
    // because every creature makes MAX_INTERNAL_NEURONS + NUM_ACTIONS calls per
    // step no matter how small its genome, so this is the single largest
    // remaining speedup — but it CHANGES THE NUMBERS (|error| <= 1.0e-4 per
    // activation, which a chaotic sim amplifies into different trajectories).
    //
    // Off by default: runs must stay comparable with the existing corpus. Turn
    // it on for exploratory sweeps where throughput matters more than
    // continuity, and treat the results as a separate numerical lineage —
    // record it in the run sidecar (dump() does) and re-confirm any headline
    // finding with it off. It does NOT bias the dynamics (the approximation is
    // symmetric and monotone), but individual runs will not match.
    bool fastTanh = false;

    // Human-readable dump for run logs and the per-run sidecar file.
    std::string dump() const {
        const std::string maxPop =
            (maxPopulation > 0) ? std::to_string(maxPopulation) : std::string("unlimited");
        std::ostringstream o;
        o << "toroidal="          << (toroidal ? "on (wrap-around senses)" : "off (bounded plane)") << "\n"
          << "minPopulation="     << minPopulation
          << " maxPopulation="    << maxPop << "\n"
          << "targetFood="        << targetFood
          << " foodSpawnBase="    << foodSpawnBase
          << " foodSpawnAmp="     << foodSpawnAmp
          << " foodEnergy="       << foodEnergy
          << " pickupRadius="     << pickupRadius << "\n"
          << "metabolicRate="     << metabolicRate
          << " geneCost="         << geneCost
          << " moveCost="         << moveCost
          << " maxAge="           << maxAge
          << " reproEnergy="      << reproEnergy
          << " newbornEnergy="    << newbornEnergy << "\n"
          << "perceptionRadius="  << perceptionRadius
          << " interactRadius="   << interactRadius
          << " maxNeighbors="     << (maxNeighbors > 0
                                      ? std::to_string(maxNeighbors) + " (topological)"
                                      : std::string("unlimited (metric)"))
          << " reproCooldown="    << reproCooldown
          << " interactCooldown=" << interactCooldown << "\n"
          << "fastTanh="          << (fastTanh ? "on (approx activations)" : "off (std::tanh)") << "\n"
          << "minSpeed="          << minSpeed
          << " maxSpeed="         << maxSpeed << "\n"
          << "triangularMotion="  << (triangularMotion ? "on" : "off")
          << " maxTurnRate="      << maxTurnRate << "\n"
          << "genomeLength="      << genomeMinLength << ".." << genomeMaxLength
          << " genomeInit="       << genomeInitMin << ".." << genomeInitMax
          << " mutationMultiplier=" << mutationMultiplier << "\n"
          << "currentStrength="   << currentStrength
          << " currentMode="      << (currentMode == 1 ? "gyre" : "eddies")
          << " currentEddies="    << currentEddies
          << " currentCore="      << currentCore
          << " currentDrift="     << currentDrift
          << " currentGyreEps="   << currentGyreEps << "\n"
          << "foodPatches="       << foodPatches
          << " foodPatchRadius="  << foodPatchRadius
          << " foodDrift="        << (foodDrift ? "on" : "off") << "\n"
          << "predatorCount="     << predatorCount
          << " predatorPerception=" << predatorPerception
          << " predatorSpeed="    << predatorSpeed
          << " predatorCaptureRadius=" << predatorCaptureRadius
          << " predatorCooldown=" << predatorCooldown << "\n"
          << "predatorEconomy="   << (predatorEconomy ? "on" : "off")
          << " predatorMetabolism=" << predatorMetabolism
          << " predatorMoveCost=" << (predatorMoveCost < 0.0f
                                      ? std::string("reuse(") + std::to_string(moveCost) + ")"
                                      : std::to_string(predatorMoveCost))
          << " predatorReproEnergy=" << predatorReproEnergy
          << " predatorNewbornEnergy=" << predatorNewbornEnergy
          << " predatorMeal="     << (predatorMeal < 0.0f
                                      ? std::string("absorb-reserves")
                                      : std::to_string(predatorMeal))
          << " predatorMaxAge="   << (predatorMaxAge > 0.0f
                                      ? std::to_string(predatorMaxAge)
                                      : std::string("immortal"));
        return o.str();
    }
};
