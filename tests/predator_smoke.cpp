// Predator integration smoke test (NO SFML) — exercises the same core+stats path
// the headless runner uses, with predators ON, and asserts the run-level
// invariants the unit tests can only check in miniature. Two harnesses:
//
//   Stage A (cfg.predatorEconomy == false): immortal, fixed-count. Asserts the
//     pop floor holds, nothing goes NaN / leaves the torus, predation happens,
//     predator_align stays in [-1,1], deaths partition exactly, and the predator
//     count never changes (immortal).
//
//   Stage B (cfg.predatorEconomy == true): the energy economy. Asserts the same
//     prey-side invariants PLUS the Stage-B ones: predator_pop is always >= 0 and
//     finite, predator energy never goes NaN, the predator death decomposition is
//     consistent (births and starvation/age counters only ever grow), and over
//     the run BOTH predator births AND predator deaths actually occur (the economy
//     is live, not inert). It does NOT assert non-extinction — predator extinction
//     is a legitimate, observable Stage-B outcome (there is no predator reseed
//     floor), so the harness only checks the bookkeeping stays sound through it.
//
// It also prints the predator CSV columns so the output can be eyeballed.

#include <cstdio>
#include <cmath>
#include "core/world.hpp"
#include "core/stats.hpp"

namespace {

int g_fails = 0;
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}

// Run a world for `steps`, sampling every `logEvery`, accumulating the shared
// prey-side invariants. Returns the final WorldStats. `econ` selects Stage A/B.
struct RunResult {
    bool floorHeld = true, sane = true, alignInRange = true;
    // Stage A only:
    bool countFixed = true;
    // Stage B only:
    bool predPopNonNeg = true, predEnergyFinite = true, countersMonotone = true;
    long long lastBirths = 0, lastStarv = 0, lastAge = 0;
};

void runWorld(const char* tag, bool econ, int steps, int logEvery) {
    rng.seed(42u);
    World w;
    w.cfg.minPopulation = 50;        // prey reseed floor (default)
    w.cfg.moveCost      = 0.00007f;
    w.cfg.minSpeed      = 0.0f;
    w.cfg.geneCost      = 0.01f;
    w.cfg.predatorCount = 6;         // the recommended starting count
    if (econ) {
        w.cfg.predatorEconomy   = true;
        // A balanced-band economy (fixed meal so it can't explode on fat prey).
        // This is a SMOKE config (short run), not a tuned oscillator — see the
        // Stage-B notes; tuning is a real-run, replicate task.
        w.cfg.predatorMeal       = 14.0f;
        w.cfg.predatorMetabolism = 2.0f;
        w.cfg.predatorReproEnergy = 60.0f;
    }
    w.init(1920, 1080, 300);

    std::printf("=== predator integration smoke (%s) ===\n", tag);
    std::printf("%s\n", statsCsvHeader().c_str());

    const std::size_t pred0 = w.predators.size();
    RunResult r;
    bool predBornEver = false, predDiedEver = false;

    for (int s = 1; s <= steps; ++s) {
        w.update(0.016f);

        if ((int)w.creatures.size() < w.cfg.minPopulation) r.floorHeld = false;
        if (!econ && w.predators.size() != pred0)          r.countFixed = false;

        // Per-step Stage-B bookkeeping sanity (cheap).
        if (econ) {
            for (const auto& p : w.predators)
                if (std::isnan(p.energy) || std::isinf(p.energy)) r.predEnergyFinite = false;
            if (w.totalPredatorBirths < r.lastBirths ||
                w.totalPredatorDeathsStarvation < r.lastStarv ||
                w.totalPredatorDeathsAge < r.lastAge) r.countersMonotone = false;
            r.lastBirths = w.totalPredatorBirths;
            r.lastStarv  = w.totalPredatorDeathsStarvation;
            r.lastAge    = w.totalPredatorDeathsAge;
            if (w.totalPredatorBirths > 0) predBornEver = true;
            if (w.totalPredatorDeathsStarvation + w.totalPredatorDeathsAge > 0) predDiedEver = true;
        }

        if (s % logEvery == 0) {
            const WorldStats st = computeStats(w);
            for (const auto& c : w.creatures)
                if (std::isnan(c.pos.x) || std::isnan(c.pos.y) ||
                    c.pos.x < 0 || c.pos.x >= w.width ||
                    c.pos.y < 0 || c.pos.y >= w.height) r.sane = false;
            for (const auto& p : w.predators)
                if (std::isnan(p.pos.x) || std::isnan(p.pos.y) ||
                    p.pos.x < 0 || p.pos.x >= w.width ||
                    p.pos.y < 0 || p.pos.y >= w.height) r.sane = false;
            if (st.avgPredatorAlign < -1.0001f || st.avgPredatorAlign > 1.0001f)
                r.alignInRange = false;
            if (st.predatorPop < 0) r.predPopNonNeg = false;

            std::printf("step %-8d pop %5d pred %4d kills+ ?  pE %7.1f  bornEver=%d diedEver=%d\n",
                        s, st.population, st.predatorPop,
                        econ ? st.predatorAvgEnergy : 0.0f,
                        (int)predBornEver, (int)predDiedEver);
        }
    }

    const bool identity =
        (w.totalDeaths ==
         w.totalDeathsPredation + w.totalDeathsStarvation + w.totalDeathsAge);
    const bool predationHappened = (w.totalDeathsPredation > 0);

    std::printf("---------------------------------------------\n");
    std::printf("final(%s): pop=%zu pred=%zu kills=%lld starv=%lld age=%lld | "
                "predBirths=%lld predStarv=%lld predAge=%lld\n",
                tag, w.creatures.size(), w.predators.size(),
                w.totalDeathsPredation, w.totalDeathsStarvation, w.totalDeathsAge,
                w.totalPredatorBirths, w.totalPredatorDeathsStarvation, w.totalPredatorDeathsAge);

    check(r.floorHeld,         "prey min-population floor held every tick");
    check(r.sane,              "all positions finite and inside the torus");
    check(predationHappened,   "predation occurred (totalKills > 0)");
    check(r.alignInRange,      "predator_align stayed within [-1, 1]");
    check(identity,            "deaths == predation + starvation + age (exact partition)");
    if (!econ) {
        check(r.countFixed,    "predator count stayed fixed (Stage A: immortal)");
    } else {
        check(r.predPopNonNeg,     "predator_pop stayed >= 0");
        check(r.predEnergyFinite,  "predator energy stayed finite (no NaN/inf)");
        check(r.countersMonotone,  "predator birth/death counters never decreased");
        check(predBornEver,        "Stage B: predator reproduction occurred (economy live)");
        check(predDiedEver,        "Stage B: predator death occurred (economy live)");
    }
    std::printf("\n");
}

}  // namespace

int main() {
    const int steps    = 12000;   // long enough for Stage B births AND deaths
    const int logEvery = 3000;

    runWorld("Stage A: immortal/fixed-count", /*econ=*/false, steps, logEvery);
    runWorld("Stage B: energy economy",       /*econ=*/true,  steps, logEvery);

    if (g_fails == 0) { std::printf("SMOKE OK\n"); return 0; }
    std::printf("SMOKE FAILED (%d)\n", g_fails);
    return 1;
}
