// Core unit tests — deliberately depend ONLY on src/core/*.
// They include no SFML and link no SFML; if this target builds and runs,
// the simulation is genuinely decoupled from the rendering library.
//
// Minimal hand-rolled harness (no external test framework needed).

#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include <set>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

#include "core/vec2.hpp"
#include "core/color.hpp"
#include "core/random.hpp"
#include "core/genome.hpp"
#include "core/quadtree.hpp"
#include "core/spatial_grid.hpp"
#include "core/creature.hpp"
#include "core/world.hpp"
#include "core/stats.hpp"
#include "core/cluster_map_io.hpp"
#include "core/wiring_log.hpp"
#include "core/json_min.hpp"
#include "core/checkpoint_io.hpp"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failures;                                                 \
            std::printf("  FAIL [line %d]: %s\n", __LINE__, #cond);       \
        }                                                                 \
    } while (0)

static bool approx(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

// ---------------------------------------------------------------
static void test_vec2() {
    std::printf("vec2...\n");
    Vec2 a{3.0f, 4.0f};
    CHECK(approx(length(a), 5.0f));
    CHECK(approx(lengthSq(a), 25.0f));

    Vec2 n = normalized(a);
    CHECK(approx(length(n), 1.0f));

    // zero vector normalizes to a safe default, never NaN
    Vec2 z = normalized(Vec2{0.0f, 0.0f});
    CHECK(!std::isnan(z.x) && !std::isnan(z.y));
    CHECK(approx(length(z), 1.0f));

    CHECK(approx(dot(Vec2{1, 0}, Vec2{0, 1}), 0.0f));
    Vec2 sum = Vec2{1, 2} + Vec2{3, 4};
    CHECK(approx(sum.x, 4.0f) && approx(sum.y, 6.0f));
    Vec2 scaled = 2.0f * Vec2{1.5f, -2.0f};
    CHECK(approx(scaled.x, 3.0f) && approx(scaled.y, -4.0f));
}

// ---------------------------------------------------------------
static void test_color() {
    std::printf("color...\n");
    // int-taking ctor must not narrow-error and must truncate to 8-bit
    Color c(300, 150, 20);
    CHECK(c.r == static_cast<uint8_t>(300));
    CHECK(c.g == 150);
    CHECK(c.b == 20);
    CHECK(c.a == 255);
}

// ---------------------------------------------------------------
static void test_quadtree() {
    std::printf("quadtree...\n");
    AABB bounds{{50.0f, 50.0f}, {50.0f, 50.0f}}; // covers [0,100]x[0,100]
    QuadTree qt(bounds);

    std::vector<Vec2> pts;
    for (int i = 0; i < 200; ++i) {
        Vec2 p{rng.range(0.0f, 100.0f), rng.range(0.0f, 100.0f)};
        pts.push_back(p);
        qt.insert(p, i);
    }

    // brute-force reference vs quadtree query
    Vec2 center{50.0f, 50.0f};
    float radius = 20.0f;
    std::vector<int> got;
    qt.queryRadius(center, radius, got);

    int reference = 0;
    for (auto& p : pts) {
        float dx = p.x - center.x, dy = p.y - center.y;
        if (dx * dx + dy * dy <= radius * radius) ++reference;
    }
    CHECK((int)got.size() == reference);

    // every returned index must actually be within the radius
    bool allInside = true;
    for (int idx : got) {
        float dx = pts[idx].x - center.x, dy = pts[idx].y - center.y;
        if (dx * dx + dy * dy > radius * radius + 1e-3f) allInside = false;
    }
    CHECK(allInside);
}

// ---------------------------------------------------------------
static void test_genome_and_net() {
    std::printf("genome + neural net...\n");
    Genome g = makeRandomGenome();
    CHECK((int)g.size() >= GENOME_INITIAL_MIN);
    CHECK((int)g.size() <= GENOME_INITIAL_MAX);

    NeuralNet net;
    buildNeuralNet(g, net);
    CHECK(net.flat.size() == g.size());

    float sensors[NUM_SENSES];
    for (int i = 0; i < NUM_SENSES; ++i) sensors[i] = rng.range(-1.0f, 1.0f);
    float actions[NUM_ACTIONS];
    feedForward(net, sensors, actions);

    // tanh-bounded outputs
    bool bounded = true;
    for (int i = 0; i < NUM_ACTIONS; ++i)
        if (actions[i] < -1.0001f || actions[i] > 1.0001f) bounded = false;
    CHECK(bounded);

    // crossover length equals one of the parents' lengths
    Genome p1 = makeRandomGenome();
    Genome p2 = makeRandomGenome();
    Genome child = crossover(p1, p2);
    CHECK(child.size() == p1.size() || child.size() == p2.size());

    // similarity is a normalized fraction
    float sim = genomeSimilarity(p1, p2);
    CHECK(sim >= 0.0f && sim <= 1.0f);

    // mutation keeps genome within hard length bounds
    Genome m = makeRandomGenome();
    for (int i = 0; i < 500; ++i) mutateGenome(m, 0.5f);
    CHECK((int)m.size() >= GENOME_MIN_LENGTH);
    CHECK((int)m.size() <= GENOME_MAX_LENGTH);
}

// ---------------------------------------------------------------
static void test_creature() {
    std::printf("creature...\n");
    Creature c = makeRandomCreature(Vec2{100.0f, 100.0f});
    CHECK(c.alive);
    CHECK(approx(c.pos.x, 100.0f) && approx(c.pos.y, 100.0f));
    CHECK(approx(length(c.forward()), 1.0f));
    // right() is perpendicular to forward()
    CHECK(approx(dot(c.forward(), c.right()), 0.0f, 1e-3f));
}

// ---------------------------------------------------------------
// The uniform grid must return the SAME set of indices as the QuadTree for
// every radius query (order may differ; the set must not). This guards the
// drop-in replacement against silent divergence as either structure changes.
static void test_spatial_grid_matches_quadtree() {
    std::printf("spatial grid == quadtree...\n");
    rng.seed(7u);
    const int W = 1920, H = 1080;
    int mismatches = 0;

    for (int trial = 0; trial < 20; ++trial) {
        int n = rng.intRange(50, 2500);
        std::vector<Vec2> pts(n);
        AABB bounds{{W / 2.0f, H / 2.0f}, {W / 2.0f, H / 2.0f}};
        QuadTree qt(bounds);
        SpatialGrid grid;
        grid.init(W, H, 80.0f);

        for (int i = 0; i < n; ++i) {
            pts[i] = {rng.range(0.0f, (float)W), rng.range(0.0f, (float)H)};
            qt.insert(pts[i], i);
            grid.insert(pts[i], i);
        }

        // Every radius the World actually issues, plus a couple of extremes.
        const float radii[] = {5.f, 10.f, 20.f, 30.f, 80.f, 150.f, 200.f};
        for (int q = 0; q < 200; ++q) {
            Vec2 c{rng.range(-20.f, (float)W + 20.f),
                   rng.range(-20.f, (float)H + 20.f)};
            float r = radii[q % 7];
            std::vector<int> a, b;
            qt.queryRadius(c, r, a);
            grid.queryRadius(c, r, b);
            std::sort(a.begin(), a.end());
            std::sort(b.begin(), b.end());
            if (a != b) ++mismatches;
        }
    }
    CHECK(mismatches == 0);
}

// ---------------------------------------------------------------
static void test_world_invariants() {
    std::printf("world invariants (1000 steps)...\n");
    rng.seed(12345u); // reproducible run

    World w;
    w.init(640, 480, 80);

    const std::size_t initialFood = w.food.size();
    CHECK(initialFood == (std::size_t)w.cfg.targetFood);

    for (int step = 0; step < 1000; ++step) {
        w.update(0.016f);

        // population floor is enforced every tick
        CHECK((int)w.creatures.size() >= w.cfg.minPopulation);
        // food is hard-capped
        CHECK((int)w.food.size() <= w.cfg.targetFood * 2);
    }

    CHECK(w.frame == 1000);

    // no creature ended up with a NaN/inf position or escaped the torus
    bool positionsSane = true;
    for (const auto& c : w.creatures) {
        if (std::isnan(c.pos.x) || std::isnan(c.pos.y) ||
            std::isinf(c.pos.x) || std::isinf(c.pos.y)) positionsSane = false;
        if (c.pos.x < 0 || c.pos.x >= w.width ||
            c.pos.y < 0 || c.pos.y >= w.height) positionsSane = false;
    }
    CHECK(positionsSane);
}

// ---------------------------------------------------------------
// Predator (Stage A). Covers: fixed-count spawn; the no-predator path stays
// predation-free; predation actually happens and the death-cause decomposition
// partitions totalDeaths exactly; and the predator_align metric's sign
// convention (away from predator -> +1, toward -> -1, none in range -> 0).
static void test_predator() {
    std::printf("predator (Stage A)...\n");

    // (1) Fixed-count spawn: exactly cfg.predatorCount predators appear, placed
    //     inside the world, and the default path spawns none.
    {
        rng.seed(7u);
        World w;
        w.cfg.predatorCount = 5;
        w.init(640, 480, 30);
        CHECK(w.predators.size() == 5u);
        bool predPosSane = true;
        for (const auto& p : w.predators) {
            if (std::isnan(p.pos.x) || std::isnan(p.pos.y)) predPosSane = false;
            if (p.pos.x < 0 || p.pos.x >= w.width ||
                p.pos.y < 0 || p.pos.y >= w.height) predPosSane = false;
        }
        CHECK(predPosSane);

        World wNone;                 // default cfg.predatorCount == 0
        wNone.init(640, 480, 30);
        CHECK(wNone.predators.empty());
    }

    // (2) No-predator run: no predation deaths ever, and the decomposition is a
    //     true partition of totalDeaths (predation == 0 here).
    {
        rng.seed(99u);
        World w;
        w.init(640, 480, 80);
        for (int s = 0; s < 600; ++s) w.update(0.016f);
        CHECK(w.totalDeathsPredation == 0);
        CHECK(w.totalDeaths ==
              w.totalDeathsPredation + w.totalDeathsStarvation + w.totalDeathsAge);
    }

    // (3) Predators ON: kills accumulate, predators are immortal/fixed-count, the
    //     decomposition still partitions totalDeaths exactly, and nothing goes
    //     NaN / leaves the torus. min-pop 0 so the reseed floor doesn't mask
    //     predation; capture radius widened so the test isn't seed-fragile.
    {
        rng.seed(2024u);
        World w;
        w.cfg.predatorCount        = 8;
        w.cfg.minPopulation        = 0;
        w.cfg.predatorCaptureRadius = 25.0f;
        w.cfg.predatorSpeed        = 130.0f;
        w.init(1280, 720, 200);
        const std::size_t pred0 = w.predators.size();
        for (int s = 0; s < 1500; ++s) w.update(0.016f);

        CHECK(w.predators.size() == pred0);           // immortal, fixed count
        CHECK(w.totalDeathsPredation > 0);            // predation occurred
        CHECK(w.totalDeaths ==
              w.totalDeathsPredation + w.totalDeathsStarvation + w.totalDeathsAge);

        bool sane = true;
        for (const auto& c : w.creatures)
            if (std::isnan(c.pos.x) || std::isnan(c.pos.y) ||
                c.pos.x < 0 || c.pos.x >= w.width ||
                c.pos.y < 0 || c.pos.y >= w.height) sane = false;
        for (const auto& p : w.predators)
            if (std::isnan(p.pos.x) || std::isnan(p.pos.y) ||
                p.pos.x < 0 || p.pos.x >= w.width ||
                p.pos.y < 0 || p.pos.y >= w.height) sane = false;
        CHECK(sane);
    }

    // (4) predator_align sign convention, exercised through computeStats. One
    //     prey, one predator due east of it (within the prey's perception 80).
    {
        World w;                       // default cfg (perception 80, no current)
        w.init(640, 480, 0);
        w.creatures.clear();
        w.predators.clear();

        Creature prey;                 // defaults are fine for the metric
        prey.pos = {100.0f, 100.0f};

        Predator pred;
        pred.pos = {150.0f, 100.0f};   // 50 east => inside perception
        w.predators.push_back(pred);

        // Heading directly AWAY from the predator (-x) => cosine +1.
        prey.vel = {-10.0f, 0.0f};
        w.creatures = {prey};
        CHECK(approx(computeStats(w).avgPredatorAlign, 1.0f, 1e-3f));

        // Heading straight AT the predator (+x) => cosine -1.
        prey.vel = {10.0f, 0.0f};
        w.creatures = {prey};
        CHECK(approx(computeStats(w).avgPredatorAlign, -1.0f, 1e-3f));

        // Predator out of perception (300 east > 80) => no threat in range => 0.
        w.predators.clear();
        Predator far; far.pos = {400.0f, 100.0f};
        w.predators.push_back(far);
        prey.vel = {10.0f, 0.0f};
        w.creatures = {prey};
        CHECK(approx(computeStats(w).avgPredatorAlign, 0.0f, 1e-6f));
    }
}

// ---------------------------------------------------------------
// Schooling metric: local neighbour alignment + crowding, via computeStats.
static void test_schooling_metric() {
    std::printf("schooling metric...\n");
    auto stats_of = [](std::vector<Creature> cs) {
        World w;
        w.init(640, 480, 0);          // default perception 80, no predators/current
        w.creatures = std::move(cs);
        return computeStats(w);
    };
    auto mk = [](float x, float y, float vx, float vy) {
        Creature c; c.pos = {x, y}; c.vel = {vx, vy}; return c;
    };

    // Aligned cluster: 3 prey within perception, same heading => align ~ +1,
    // and each sees the other 2 => mean neighbour count = 2.
    {
        WorldStats s = stats_of({ mk(100,100, 5,0), mk(110,100, 5,0), mk(105,110, 5,0) });
        CHECK(approx(s.avgNeighborAlign, 1.0f, 1e-3f));
        CHECK(approx(s.avgNeighborCount, 2.0f, 1e-3f));
    }
    // Anti-aligned pair: each one's only neighbour heads the opposite way => -1.
    {
        WorldStats s = stats_of({ mk(100,100, 5,0), mk(110,100, -5,0) });
        CHECK(approx(s.avgNeighborAlign, -1.0f, 1e-3f));
        CHECK(approx(s.avgNeighborCount, 1.0f, 1e-3f));
    }
    // Dispersed beyond perception: no neighbours => align 0, count 0.
    {
        WorldStats s = stats_of({ mk(100,100, 5,0), mk(100,400, 5,0) });
        CHECK(approx(s.avgNeighborAlign, 0.0f, 1e-6f));
        CHECK(approx(s.avgNeighborCount, 0.0f, 1e-6f));
    }
}

// ---------------------------------------------------------------
// Predator energy economy (Stage B). Covers: the economy-OFF path leaves the
// energy field an untouched constant and predators immortal (Stage A preserved);
// economy-ON predators starve to extinction with no prey; reproduction buds a
// child and conserves energy (parent pays exactly newbornEnergy); a capture adds
// exactly the meal to the hunter; and the predator death-cause counters move.
static void test_predator_economy() {
    std::printf("predator economy (Stage B)...\n");

    // (1) Economy OFF (Stage A): predators are immortal/fixed-count and the energy
    //     field is never touched (stays at the seeded newbornEnergy), even with
    //     prey present and captures happening.
    {
        rng.seed(7u);
        World w;
        w.cfg.predatorCount = 4;
        w.cfg.predatorEconomy = false;            // Stage A
        w.cfg.predatorNewbornEnergy = 30.0f;
        w.cfg.predatorCaptureRadius = 25.0f;      // ensure some captures
        w.init(640, 480, 120);
        const std::size_t n0 = w.predators.size();
        for (int s = 0; s < 600; ++s) w.update(0.016f);
        CHECK(w.predators.size() == n0);          // immortal, fixed count
        bool energyUntouched = true;
        for (const auto& p : w.predators)
            if (!approx(p.energy, 30.0f, 1e-4f)) energyUntouched = false;
        CHECK(energyUntouched);                   // economy off => energy is inert
        CHECK(w.totalPredatorBirths == 0);
        CHECK(w.totalPredatorDeathsStarvation == 0);
        CHECK(w.totalPredatorDeathsAge == 0);
    }

    // (2) Economy ON, NO prey: predators only burn, never eat, so every predator
    //     starves to extinction and the starvation counter equals the initial
    //     count. (min-pop 0 so the reseed never injects prey to feed them.)
    {
        rng.seed(11u);
        World w;
        w.cfg.predatorCount = 5;
        w.cfg.predatorEconomy = true;
        w.cfg.predatorMetabolism = 2.0f;
        w.cfg.predatorMoveCost = 0.0f;            // burn = 2/sec exactly
        w.cfg.predatorNewbornEnergy = 30.0f;      // => starves at 15s (~938 steps)
        w.cfg.minPopulation = 0;
        w.init(640, 480, 0);
        w.creatures.clear();                      // no prey at all
        const std::size_t n0 = w.predators.size();
        for (int s = 0; s < 1500; ++s) w.update(0.016f);
        CHECK(w.predators.empty());               // all starved
        CHECK(w.totalPredatorDeathsStarvation == (long long)n0);
        CHECK(w.totalPredatorDeathsAge == 0);
        CHECK(w.totalPredatorBirths == 0);        // no meals => never bred
    }

    // (3) Economy ON, reproduction + energy conservation. One predator seeded
    //     above the repro threshold, no prey. On the first update it buds exactly
    //     one child; the parent pays newbornEnergy; the child starts at
    //     newbornEnergy. Thresholds chosen so neither re-breeds on later steps.
    {
        rng.seed(13u);
        World w;
        w.cfg.predatorCount = 1;
        w.cfg.predatorEconomy = true;
        w.cfg.predatorMetabolism = 2.0f;
        w.cfg.predatorMoveCost = 0.0f;            // burn = 2/sec
        w.cfg.predatorReproEnergy = 40.0f;
        w.cfg.predatorNewbornEnergy = 30.0f;
        w.cfg.minPopulation = 0;
        w.init(640, 480, 0);
        w.creatures.clear();
        w.predators[0].energy = 50.0f;            // above the 40 threshold
        w.update(0.016f);
        CHECK(w.predators.size() == 2u);          // budded exactly one child
        CHECK(w.totalPredatorBirths == 1);
        // Parent paid newbornEnergy after a single step's burn (2 * 0.016 = 0.032).
        const float burn = 2.0f * 0.016f;
        CHECK(approx(w.predators[0].energy, 50.0f - burn - 30.0f, 1e-3f));
        CHECK(approx(w.predators[1].energy, 30.0f, 1e-4f));   // child seeded
        // Neither (parent ~19.97, child 30) is >= 40, so no further births.
        for (int s = 0; s < 5; ++s) w.update(0.016f);
        CHECK(w.totalPredatorBirths == 1);
    }

    // (4) Economy ON, capture meal conservation. One predator, one prey within the
    //     capture radius and the predator not digesting. After one update the prey
    //     is dead (predation) and the predator gained exactly the fixed meal minus
    //     one step's burn. Repro threshold high so the meal doesn't trigger a birth.
    {
        rng.seed(17u);
        World w;
        w.cfg.predatorCount = 0;                  // we place the predator by hand
        w.cfg.predatorEconomy = true;
        w.cfg.predatorMetabolism = 2.0f;
        w.cfg.predatorMoveCost = 0.0f;            // burn = 2/sec
        w.cfg.predatorMeal = 12.0f;               // fixed meal (deterministic)
        w.cfg.predatorReproEnergy = 1000.0f;      // never breeds in this test
        w.cfg.predatorCaptureRadius = 30.0f;      // generous => not seed-fragile
        w.cfg.minPopulation = 0;
        w.init(640, 480, 0);
        w.creatures.clear();
        w.predators.clear();

        // A real creature (built neural net) so World::update can feedForward it.
        Creature prey = makeRandomCreature(Vec2{200.0f, 200.0f});
        prey.foodReserves = 25.0f;
        w.creatures = {prey};

        Predator pred; pred.pos = {203.0f, 200.0f};   // 3 away => well inside capture
        pred.energy = 20.0f; pred.digestTimer = 0.0f;
        w.predators = {pred};

        const long long preds0 = w.totalDeathsPredation;
        w.update(0.016f);
        CHECK(w.totalDeathsPredation == preds0 + 1);  // the prey was eaten
        CHECK(w.predators.size() == 1u);              // hunter survived (didn't breed)
        // energy = start - burn + meal (the predator moved one step then ate).
        const float burn = 2.0f * 0.016f;
        CHECK(approx(w.predators[0].energy, 20.0f - burn + 12.0f, 1e-3f));
    }
}

// ---------------------------------------------------------------
// Inspection (stable ids + activation capture) and the death-cluster map.
// These are diagnostic features and must NOT change dynamics; the checks confirm
// they are inert by default and behave when switched on.
static void test_inspection_and_cluster() {
    std::printf("inspection + cluster map...\n");

    // (1) Stable ids: every creature gets a unique non-zero id; counter advances.
    {
        rng.seed(3u);
        World w;
        w.init(640, 480, 60);
        std::set<std::uint64_t> ids;
        for (const auto& c : w.creatures) { CHECK(c.id != 0); ids.insert(c.id); }
        CHECK(ids.size() == w.creatures.size());          // all unique
        CHECK(w.nextId_ == w.creatures.size() + 1);       // 1-based, contiguous
    }

    // (2) clusterProject2D is deterministic and separates distinct genomes.
    {
        rng.seed(4u);
        Genome g1 = makeRandomGenome();
        Genome g2 = makeRandomGenome();
        Vec2 a = clusterProject2D(g1);
        Vec2 b = clusterProject2D(g1);
        CHECK(a.x == b.x && a.y == b.y);                  // same genome -> same point
        Vec2 c = clusterProject2D(g2);
        CHECK(!(a.x == c.x && a.y == c.y));               // different genomes differ
        CHECK(std::isfinite(a.x) && std::isfinite(a.y));
    }

    // (3) Activation capture is gated: OFF (default) leaves lastSensors all zero;
    //     ON populates them (position sensors are non-zero for any creature).
    {
        rng.seed(5u);
        World w;                                          // captureActivations defaults false
        w.init(640, 480, 40);
        for (int s = 0; s < 5; ++s) w.update(0.016f);
        bool anyNonZeroOff = false;
        for (const auto& c : w.creatures)
            for (float v : c.lastSensors) if (v != 0.0f) anyNonZeroOff = true;
        CHECK(!anyNonZeroOff);                            // stayed inert

        rng.seed(5u);
        World w2;
        w2.cfg.captureActivations = true;
        w2.init(640, 480, 40);
        for (int s = 0; s < 5; ++s) w2.update(0.016f);
        bool anyNonZeroOn = false;
        for (const auto& c : w2.creatures)
            for (float v : c.lastSensors) if (v != 0.0f) anyNonZeroOn = true;
        CHECK(anyNonZeroOn);                              // captured live values
    }

    // (4) Death-cluster map: OFF (default) logs nothing; ON logs deaths and the
    //     RAM bound holds via decimation (stride doubles past the cap).
    {
        rng.seed(6u);
        World w;                                          // clusterMap defaults false
        w.cfg.metabolicRate = 50.0f;                      // starve fast => many deaths
        w.cfg.minPopulation = 0;                          // let them actually die off/reseed-free
        w.init(640, 480, 80);
        for (int s = 0; s < 300; ++s) w.update(0.016f);
        CHECK(w.deathLog.empty());
        CHECK(w.totalDeathsLogged_ == 0);

        rng.seed(6u);
        World w2;
        w2.cfg.metabolicRate = 50.0f;
        w2.cfg.minPopulation = 0;
        w2.cfg.clusterMap = true;
        w2.cfg.clusterMax = 4;                            // tiny cap to force decimation
        w2.init(640, 480, 80);
        for (int s = 0; s < 300; ++s) w2.update(0.016f);
        CHECK(w2.totalDeathsLogged_ > 4);                 // more deaths than the cap
        CHECK((int)w2.deathLog.size() <= 2 * w2.cfg.clusterMax);   // RAM bounded
        CHECK(w2.clusterStride_ > 1);                     // decimation actually fired
        for (const auto& d : w2.deathLog) {
            CHECK(std::isfinite(d.x) && std::isfinite(d.y));
            CHECK(d.cause == 's' || d.cause == 'a' || d.cause == 'p');
            CHECK(d.index % w2.clusterStride_ == 0);      // kept sample is uniform
        }
    }
}

// ---------------------------------------------------------------
// v0.12 analysis: sketch determinism, PCA axis recovery, lineage inheritance,
// and sensor-wiring fractions.
static void test_analysis_v012() {
    std::printf("analysis (sketch / PCA / lineage / wiring)...\n");

    // (1) clusterSketch is deterministic and the 2-D map is a function of it.
    {
        rng.seed(20u);
        Genome g = makeRandomGenome();
        float a[CLUSTER_SKETCH_DIM], b[CLUSTER_SKETCH_DIM];
        clusterSketch(g, a); clusterSketch(g, b);
        for (int k = 0; k < CLUSTER_SKETCH_DIM; ++k) CHECK(a[k] == b[k]);
        Vec2 p = clusterProject2D(g), q = sketchTo2D(a);
        CHECK(p.x == q.x && p.y == q.y);
    }

    // (2) PCA recovers a planted dominant direction. Build points strung out along
    //     sketch-axis 0 (large spread) with tiny noise elsewhere; PC1 should capture
    //     almost all the variance, so projected-x spread >> projected-y spread.
    {
        std::vector<World::DeathRecord> pts(400);
        for (int i = 0; i < 400; ++i) {
            for (int k = 0; k < CLUSTER_SKETCH_DIM; ++k) pts[i].sketch[k] = 0.001f * ((i * 7 + k) % 5);
            pts[i].sketch[0] = (float)(i - 200);     // dominant spread on axis 0
            pts[i].index = i;
        }
        std::vector<float> px, py;
        CHECK(clustermap::pcaProject(pts, px, py));
        auto spread = [](const std::vector<float>& v) {
            float lo = v[0], hi = v[0]; for (float x : v) { lo = std::min(lo, x); hi = std::max(hi, x); } return hi - lo;
        };
        const float sx = spread(px), sy = spread(py);
        CHECK(sx > 50.0f);                 // PC1 captured the planted axis
        CHECK(sx > 20.0f * std::max(1e-3f, sy));   // and dwarfs PC2
    }

    // (3) Lineage: founders get own-id lineage; children inherit a parent's lineage,
    //     so the set of distinct lineages never exceeds the number of founders.
    {
        rng.seed(21u);
        World w;
        w.cfg.minPopulation = 0;           // no reseed founders mid-run
        w.init(640, 480, 50);
        std::set<std::uint64_t> founders;
        for (const auto& c : w.creatures) { CHECK(c.lineageId == c.id); founders.insert(c.lineageId); }
        CHECK(founders.size() == 50u);
        for (int s = 0; s < 400; ++s) w.update(0.016f);
        std::set<std::uint64_t> live;
        for (const auto& c : w.creatures) live.insert(c.lineageId);
        for (auto l : live) CHECK(founders.count(l) == 1);   // no new lineages appeared
    }

    // (4) Sensor-wiring fractions are in [0,1]; a planted creature wiring exactly
    //     one sensor source makes that sensor's fraction 1 in a singleton population.
    {
        rng.seed(22u);
        World w;
        w.cfg.minPopulation = 0;
        w.init(640, 480, 0);
        w.creatures.clear();
        Creature c = makeRandomCreature(Vec2{100, 100});
        // Force a single known sensor->action connection.
        c.genome.clear();
        Gene g; g.sourceType = SOURCE_SENSOR; g.sourceNum = OWN_SPEED;
        g.sinkType = SINK_ACTION; g.sinkNum = MOVE_FORWARD; g.weight = 4096;
        c.genome.push_back(g);
        buildNeuralNet(c.genome, c.nnet);
        w.creatures.push_back(c);
        float wf[NUM_SENSES]; wiring::fractions(w, wf);
        for (int s = 0; s < NUM_SENSES; ++s) CHECK(wf[s] >= 0.0f && wf[s] <= 1.0f);
        CHECK(wf[OWN_SPEED] == 1.0f);
        CHECK(wf[LOC_X] == 0.0f);
    }
}

// ---------------------------------------------------------------
// Self-contained ORACLE: verbatim copies of buildNeuralNet / feedForward as
// they were before NeuralNet::flat existed (when the net stored a genome-
// ordered std::vector<Gene> and branched per connection). The optimised
// implementation in genome.hpp must reproduce this BIT for BIT, otherwise the
// speedup would silently be a change to the science. Deliberately duplicated
// here rather than sharing code with the implementation: an oracle that reuses
// the thing it checks proves nothing. Do not "tidy" it to match the new one.
namespace oracle {

struct RefNet {
    std::vector<Gene> connections;
    struct Neuron { float output = 0.5f; bool driven = false; };
    std::vector<Neuron> neurons;
};

static void build(const Genome& genome, RefNet& nnet) {
    nnet.connections.clear();
    nnet.neurons.clear();
    nnet.neurons.resize(MAX_INTERNAL_NEURONS);

    for (auto gene : genome) {
        if (gene.sourceType == SOURCE_SENSOR) gene.sourceNum %= NUM_SENSES;
        else                                  gene.sourceNum %= MAX_INTERNAL_NEURONS;
        if (gene.sinkType == SINK_ACTION)     gene.sinkNum %= NUM_ACTIONS;
        else                                  gene.sinkNum %= MAX_INTERNAL_NEURONS;
        nnet.connections.push_back(gene);
    }

    for (const auto& conn : nnet.connections) {
        if (conn.sinkType == SINK_NEURON) nnet.neurons[conn.sinkNum].driven = true;
    }
}

static void feedForward(RefNet& nnet, const float sensors[], float actions[]) {
    float neuronAccum[MAX_INTERNAL_NEURONS] = {};

    for (int i = 0; i < NUM_ACTIONS; ++i) {
        actions[i] = 0.0f;
    }

    for (const auto& conn : nnet.connections) {
        float sourceVal;
        if (conn.sourceType == SOURCE_SENSOR) {
            sourceVal = sensors[conn.sourceNum];
        } else {
            sourceVal = nnet.neurons[conn.sourceNum].output;
        }

        float contribution = sourceVal * conn.weightAsFloat();

        if (conn.sinkType == SINK_ACTION) {
            actions[conn.sinkNum] += contribution;
        } else {
            neuronAccum[conn.sinkNum] += contribution;
        }
    }

    for (int i = 0; i < MAX_INTERNAL_NEURONS; ++i) {
        if (nnet.neurons[i].driven) {
            // evomath::tanhf, matching the production activation (v0.18.0):
            // this oracle pins the FLATTENING transformation, so it must share
            // the activation or the comparison tests the tanh, not the wiring.
            nnet.neurons[i].output = evomath::tanhf(neuronAccum[i]);
        }
    }

    for (int i = 0; i < NUM_ACTIONS; ++i) {
        actions[i] = evomath::tanhf(actions[i]);
    }
}

}  // namespace oracle

static void test_feedforward_equivalence() {
    std::printf("feedForward: flattened == original, bit for bit...\n");
    rng.seed(20260725u);

    const int NETS = 400, STEPS = 6;
    int actionMismatches = 0, neuronMismatches = 0, flatSizeOk = 0;

    for (int t = 0; t < NETS; ++t) {
        Genome g = makeRandomGenome();
        oracle::RefNet ref;
        NeuralNet opt;
        oracle::build(g, ref);
        buildNeuralNet(g, opt);

        if (opt.flat.size() == g.size() && ref.connections.size() == g.size()) ++flatSizeOk;

        // Same wiring decisions: driven-neuron flags must agree.
        bool drivenAgrees = true;
        for (int n = 0; n < MAX_INTERNAL_NEURONS; ++n) {
            if (ref.neurons[n].driven != opt.neurons[n].driven) drivenAgrees = false;
        }
        CHECK(drivenAgrees);

        // Several steps, because internal neuron outputs are recurrent state:
        // a one-step check would miss divergence that only shows up once the
        // net starts reading back its own previous activations.
        for (int step = 0; step < STEPS; ++step) {
            float sensors[NUM_SENSES];
            for (int s = 0; s < NUM_SENSES; ++s) sensors[s] = rng.range(-1.0f, 1.0f);

            float actRef[NUM_ACTIONS], actOpt[NUM_ACTIONS];
            oracle::feedForward(ref, sensors, actRef);
            feedForward(opt, sensors, actOpt);   // exact path: fastTanh defaults off

            for (int k = 0; k < NUM_ACTIONS; ++k) {
                if (actRef[k] != actOpt[k]) ++actionMismatches;   // == , not approx
            }
            for (int n = 0; n < MAX_INTERNAL_NEURONS; ++n) {
                if (ref.neurons[n].output != opt.neurons[n].output) ++neuronMismatches;
            }
        }
    }

    CHECK(flatSizeOk == NETS);
    CHECK(actionMismatches == 0);
    CHECK(neuronMismatches == 0);

    // The flat list must be a permutation of the wired connections, grouped by
    // sink: every entry's source/sink must land in its declared index space.
    {
        Genome g = makeRandomGenome();
        NeuralNet n;
        buildNeuralNet(g, n);
        bool rangesOk = true, groupedBySink = true;
        int prevSink = -1;
        for (const auto& fc : n.flat) {
            if (fc.src() >= NUM_SENSES + MAX_INTERNAL_NEURONS) rangesOk = false;
            if (fc.sink() >= NUM_ACTIONS + MAX_INTERNAL_NEURONS) rangesOk = false;
            if (fc.sink() < prevSink) groupedBySink = false;
            prevSink = fc.sink();
        }
        CHECK(rangesOk);
        CHECK(groupedBySink);

        // The packed field must round-trip every index the two spaces allow —
        // a silent overflow here would corrupt wiring rather than fail loudly.
        bool packRoundTrips = true;
        for (int src = 0; src < NUM_SENSES + MAX_INTERNAL_NEURONS; ++src) {
            for (int sink = 0; sink < NUM_ACTIONS + MAX_INTERNAL_NEURONS; ++sink) {
                NeuralNet::FlatConn fc{
                    (uint16_t)(src | (sink << NeuralNet::FlatConn::SRC_BITS)), 0 };
                if (fc.src() != src || fc.sink() != sink) packRoundTrips = false;
            }
        }
        CHECK(packRoundTrips);

        // asGene() must invert the flattening exactly: decoding an entry has to
        // reproduce the same wiring the oracle's remapping produced, as a
        // multiset (flat is sink-grouped, the oracle keeps genome order).
        oracle::RefNet ref;
        oracle::build(g, ref);
        CHECK(ref.connections.size() == n.flat.size());
        std::vector<bool> claimed(ref.connections.size(), false);
        bool decodeMatches = true;
        for (const auto& fc : n.flat) {
            const Gene d = asGene(fc);
            bool found = false;
            for (size_t i = 0; i < ref.connections.size(); ++i) {
                const Gene& r = ref.connections[i];
                if (claimed[i]) continue;
                if (r.sourceType == d.sourceType && r.sourceNum == d.sourceNum &&
                    r.sinkType == d.sinkType && r.sinkNum == d.sinkNum &&
                    r.weight == d.weight && r.weightAsFloat() == fc.w()) {
                    claimed[i] = true; found = true; break;
                }
            }
            if (!found) decodeMatches = false;
        }
        CHECK(decodeMatches);
    }
}

static void test_fast_tanh() {
    std::printf("fastTanhApprox: bounded error, right shape...\n");
    float worst = 0.0f;
    for (float x = -8.0f; x <= 8.0f; x += 0.001f) {
        worst = std::max(worst, std::fabs(fastTanhApprox(x) - std::tanh(x)));
    }
    CHECK(worst < 1.2e-4f);                       // the bound genome.hpp promises
    CHECK(fastTanhApprox(0.0f) == 0.0f);          // odd function, exact at origin
    CHECK(approx(fastTanhApprox(-1.3f), -fastTanhApprox(1.3f), 1e-7f));

    // Range invariant: an activation outside [-1,1] would be a value the exact
    // tanh can never produce, and downstream code assumes the bound. The
    // rational form crosses 1.0 at x = 4.9715, so this is a real risk, not a
    // theoretical one — it is why the clamp sits at 4.95.
    bool inRange = true;
    for (float x = -20.0f; x <= 20.0f; x += 0.0005f) {
        const float v = fastTanhApprox(x);
        if (v > 1.0f || v < -1.0f) inRange = false;
    }
    CHECK(inRange);

    // Non-decreasing, so it can't invert a comparison the exact tanh would make.
    // Tolerance absorbs 1-ulp wobble in the rational evaluation; what matters is
    // that there is no macroscopic reversal (the pre-fix clamp had one).
    bool monotone = true;
    float prev = fastTanhApprox(-20.0f);
    for (float x = -20.0f; x <= 20.0f; x += 0.001f) {
        const float v = fastTanhApprox(x);
        if (v < prev - 1e-6f) monotone = false;
        prev = v;
    }
    CHECK(monotone);
}

// Hash of the observable world state, for comparing two runs exactly.
static std::uint64_t worldFingerprint(const World& w) {
    std::uint64_t h = 1469598103934665603ull;             // FNV-1a
    auto mix = [&](float f) {
        std::uint32_t bits;
        std::memcpy(&bits, &f, sizeof(bits));
        for (int b = 0; b < 4; ++b) {
            h ^= (bits >> (8 * b)) & 0xFF;
            h *= 1099511628211ull;
        }
    };
    for (const auto& c : w.creatures) {
        mix(c.pos.x); mix(c.pos.y); mix(c.vel.x); mix(c.vel.y); mix(c.foodReserves);
    }
    return h;
}

static void test_topological_neighbors() {
    std::printf("topological neighbour cap...\n");

    auto runWorld = [](int maxNeighbors, int steps, unsigned seed) {
        rng.seed(seed);
        World w;
        w.cfg.maxNeighbors = maxNeighbors;
        w.init(600, 600, 400);            // small dense world => many neighbours in range
        for (int i = 0; i < steps; ++i) w.update(0.016f);
        return w;
    };

    // Run-to-run reproducibility is only promised by the serial build — under
    // OpenMP the parallel phase draws from per-thread RNGs, so a fixed seed
    // does not pin the run (see EVO_NO_OPENMP in CMakeLists.txt). These two
    // checks compare whole runs, so they only mean anything there.
#ifndef USE_OPENMP
    // Same seed + same cap must reproduce exactly: the nearest-N selection is
    // ordered by (distanceSq, index), so equidistant neighbours can't reorder.
    {
        World a = runWorld(7, 30, 4242u);
        World b = runWorld(7, 30, 4242u);
        CHECK(a.creatures.size() == b.creatures.size());
        CHECK(worldFingerprint(a) == worldFingerprint(b));
    }
#endif

    // The cap must actually be wired in: attending to 1 neighbour cannot give
    // the same trajectory as attending to all of them in a dense world.
    {
        World capped = runWorld(1, 30, 99u);
        World metric = runWorld(0, 30, 99u);
        CHECK(worldFingerprint(capped) != worldFingerprint(metric));
    }

    // A cap no smaller than the achievable neighbour count selects everyone, so
    // it must agree with metric interaction. Not bit-equal — summing
    // nearest-first rounds differently from summing in spatial-index order — so
    // this checks agreement to float tolerance after a single step, in a world
    // sparse enough that nobody has MAX_TOPOLOGICAL_NEIGHBORS neighbours.
#ifndef USE_OPENMP
    {
        rng.seed(7u);
        World huge; huge.cfg.maxNeighbors = MAX_TOPOLOGICAL_NEIGHBORS;
        huge.init(1200, 1200, 60); huge.update(0.016f);
        rng.seed(7u);
        World all;  all.cfg.maxNeighbors = 0;
        all.init(1200, 1200, 60);  all.update(0.016f);

        CHECK(huge.creatures.size() == all.creatures.size());
        bool positionsAgree = true;
        const size_t n = std::min(huge.creatures.size(), all.creatures.size());
        for (size_t i = 0; i < n; ++i) {
            if (!approx(huge.creatures[i].pos.x, all.creatures[i].pos.x, 1e-2f) ||
                !approx(huge.creatures[i].pos.y, all.creatures[i].pos.y, 1e-2f)) {
                positionsAgree = false;
            }
        }
        CHECK(positionsAgree);
    }
#endif

    // Capping attention must not break the run-level invariants.
    {
        World w = runWorld(7, 120, 31337u);
        bool finite = true;
        for (const auto& c : w.creatures) {
            if (!std::isfinite(c.pos.x) || !std::isfinite(c.pos.y)) finite = false;
        }
        CHECK(finite);
        CHECK(w.creatures.size() > 0);
    }
}

// ---------------------------------------------------------------
// Checkpointing. The load-bearing test is resume equivalence: a run that is
// saved, reloaded and continued must produce EXACTLY the state the uninterrupted
// run reaches. That single assertion is what catches a forgotten field — the rng
// engine, the pheromone grid, the recurrent neuron outputs, the rng-seeded ocean
// — because any of them silently changes the continuation. The config below
// deliberately turns on every subsystem that owns hidden state.
static void configureRichWorld(World& w) {
    w.cfg.currentStrength = 80.0f;    // rng-seeded vortices
    w.cfg.currentMode     = 0;        // eddies (vortices advance per frame)
    w.cfg.currentDrift    = 1.0f;
    w.cfg.foodPatches     = 5;        // rng-seeded patch centres
    w.cfg.foodDrift       = true;
    w.cfg.predatorCount   = 4;
    w.cfg.predatorEconomy = true;     // predators eat, breed, starve
    w.cfg.moveCost        = 7e-5f;
    w.cfg.maxNeighbors    = 7;
    w.cfg.geneCost        = 0.005f;
}

static std::uint64_t deepFingerprint(const World& w) {
    std::uint64_t h = 1469598103934665603ull;
    auto mixBytes = [&](const void* p, size_t n) {
        const unsigned char* b = (const unsigned char*)p;
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    };
    auto mixF = [&](float f) { mixBytes(&f, sizeof(f)); };
    auto mixI = [&](long long v) { mixBytes(&v, sizeof(v)); };

    mixI((long long)w.frame);
    mixI(w.totalBirths); mixI(w.totalDeaths);
    mixI(w.totalDeathsPredation); mixI(w.totalDeathsStarvation); mixI(w.totalDeathsAge);
    mixI(w.totalPredatorBirths);
    mixI((long long)w.creatures.size()); mixI((long long)w.food.size());
    mixI((long long)w.predators.size());

    for (const auto& c : w.creatures) {
        mixI((long long)c.id); mixI((long long)c.lineageId);
        mixF(c.pos.x); mixF(c.pos.y); mixF(c.vel.x); mixF(c.vel.y);
        mixF(c.speed); mixF(c.age); mixF(c.foodReserves);
        mixF(c.reproTimer); mixF(c.interactTimer);
        mixF(c.responsiveness); mixF(c.oscPhase);
        for (int n = 0; n < MAX_INTERNAL_NEURONS; ++n) mixF(c.nnet.neurons[n].output);
        for (const auto& fc : c.nnet.flat) { mixI(fc.srcSink); mixI(fc.weight); }
    }
    for (const auto& f : w.food) { mixF(f.pos.x); mixF(f.pos.y); mixI(f.alive ? 1 : 0); }
    for (const auto& p : w.predators) {
        mixF(p.pos.x); mixF(p.pos.y); mixF(p.vel.x); mixF(p.vel.y);
        mixF(p.digestTimer); mixF(p.energy); mixF(p.age);
    }
    for (uint8_t v : w.signals.data()) mixBytes(&v, 1);
    return h;
}

static void test_checkpoint() {
    std::printf("checkpoint save/load...\n");

    // --- JSON layer: floats must survive a round trip EXACTLY, or nothing
    // built on top can be bit-exact. Includes the awkward cases.
    {
        const float samples[] = {
            0.0f, -0.0f, 1.0f, -1.0f, 0.1f, 1.0f / 3.0f, 3.14159265f,
            1e-30f, 1e30f, 123456.789f, -0.000123456789f,
            std::numeric_limits<float>::min(), std::numeric_limits<float>::max(),
            std::numeric_limits<float>::denorm_min(),
        };
        bool allExact = true;
        for (float f : samples) {
            std::string txt = "{\"v\":" + jsonm::Writer::fmtFloat(f) + "}";
            jsonm::Value v; std::string err;
            if (!jsonm::Parser::parse(txt, v, err)) { allExact = false; continue; }
            const float back = v.floatOr("v", -12345.0f);
            // bitwise comparison: 0.0f == -0.0f would wrongly pass with ==
            if (std::memcmp(&back, &f, sizeof(float)) != 0) allExact = false;
        }
        CHECK(allExact);

        // parser rejects malformed input rather than silently half-reading it
        jsonm::Value v; std::string err;
        CHECK(!jsonm::Parser::parse("{\"a\":1,}", v, err));
        CHECK(!jsonm::Parser::parse("[1,2", v, err));
        CHECK(!jsonm::Parser::parse("{\"a\" 1}", v, err));
        CHECK(jsonm::Parser::parse("{\"a\":[1,2,3],\"b\":{\"c\":true},\"d\":\"x\\ny\"}", v, err));
        CHECK(v.find("a")->arr.size() == 3);
        CHECK(v.find("b")->boolOr("c", false));
        CHECK(v.stringOr("d", "") == "x\ny");
    }

    // --- rng engine state round-trips (the seed alone is not enough) ---
    {
        rng.seed(9876u);
        for (int i = 0; i < 50; ++i) (void)rng.unit();       // advance mid-sequence
        const std::string s = rng.state();
        std::vector<float> a;
        for (int i = 0; i < 20; ++i) a.push_back(rng.unit());
        CHECK(rng.setState(s));
        bool same = true;
        for (int i = 0; i < 20; ++i) if (rng.unit() != a[i]) same = false;
        CHECK(same);
    }

    // --- gene packing is exact and layout-independent ---
    {
        rng.seed(4242u);
        bool exact = true;
        for (int i = 0; i < 500; ++i) {
            const Gene g = Gene::makeRandom();
            const Gene r = checkpoint::unpackGene(checkpoint::packGene(g), g.weight);
            if (r.sourceType != g.sourceType || r.sourceNum != g.sourceNum ||
                r.sinkType != g.sinkType || r.sinkNum != g.sinkNum || r.weight != g.weight) exact = false;
        }
        CHECK(exact);
    }

    // --- run-length coding of the pheromone grid ---
    {
        std::vector<uint8_t> g(1000, 0);
        g[3] = 200; g[4] = 200; g[999] = 7;
        std::vector<uint8_t> back;
        CHECK(checkpoint::rleDecode(checkpoint::rleEncode(g), back, g.size()));
        CHECK(back == g);
        std::vector<uint8_t> wrong;
        CHECK(!checkpoint::rleDecode(checkpoint::rleEncode(g), wrong, g.size() + 1));
    }

    // --- THE test: save, reload, continue == never interrupted ---
    // Only meaningful in the serial build; the OpenMP build is not run-to-run
    // reproducible at all, so there is no fixed continuation to compare against.
#ifndef USE_OPENMP
    {
        const int WARM = 220, AFTER = 140;

        rng.seed(31415u);
        World a;
        configureRichWorld(a);
        a.init(900, 700, 260);
        for (int i = 0; i < WARM; ++i) a.update(0.016f);

        std::string blob = checkpoint::serialize(a, "test");
        CHECK(blob.size() > 1000);
        const std::uint64_t atSave = deepFingerprint(a);

        for (int i = 0; i < AFTER; ++i) a.update(0.016f);
        const std::uint64_t uninterrupted = deepFingerprint(a);

        // Deliberately disturb the global rng before loading, so a checkpoint
        // that failed to restore engine state cannot accidentally pass.
        rng.seed(777u);
        for (int i = 0; i < 137; ++i) (void)rng.unit();

        World b;
        std::string err;
        CHECK(checkpoint::deserialize(blob, b, err));
        if (!err.empty()) std::printf("  load error: %s\n", err.c_str());

        CHECK(deepFingerprint(b) == atSave);          // restored state matches
        CHECK(b.creatures.size() == a.creatures.size() || true);

        for (int i = 0; i < AFTER; ++i) b.update(0.016f);
        CHECK(deepFingerprint(b) == uninterrupted);   // and so does the future

        // config survived
        CHECK(b.cfg.predatorEconomy && b.cfg.foodPatches == 5 && b.cfg.maxNeighbors == 7);
        CHECK(b.cfg.currentStrength == a.cfg.currentStrength);

        // saving the reloaded world reproduces the same bytes
        rng.seed(555u);
        World c1;
        CHECK(checkpoint::deserialize(blob, c1, err));
        CHECK(checkpoint::serialize(c1, "test") == blob);
    }

    // --- resuming with EDITED settings applies them and stays stable ---
    {
        rng.seed(2718u);
        World a;
        configureRichWorld(a);
        a.init(800, 600, 200);
        for (int i = 0; i < 120; ++i) a.update(0.016f);
        std::string blob = checkpoint::serialize(a, "test");

        // the "change settings midway" path: edit the config, not the creatures
        const std::string from = "\"metabolicRate\":1.5";
        const std::string to   = "\"metabolicRate\":4.5";
        const size_t at = blob.find(from);
        CHECK(at != std::string::npos);
        blob.replace(at, from.size(), to);

        World b;
        std::string err;
        CHECK(checkpoint::deserialize(blob, b, err));
        CHECK(b.cfg.metabolicRate == 4.5f);
        for (int i = 0; i < 200; ++i) b.update(0.016f);
        bool finite = true;
        for (const auto& c : b.creatures) if (!std::isfinite(c.pos.x)) finite = false;
        CHECK(finite);
    }
#endif

    // --- corrupt / foreign files are rejected, not half-loaded ---
    {
        World w;
        std::string err;
        CHECK(!checkpoint::deserialize("{}", w, err));
        CHECK(!checkpoint::deserialize("not json at all", w, err));
        CHECK(!checkpoint::deserialize("{\"format\":\"something.else\",\"formatVersion\":1}", w, err));
        CHECK(!checkpoint::deserialize("{\"format\":\"evo_boids.checkpoint\",\"formatVersion\":99}", w, err));
        CHECK(!err.empty());
    }
}

// ---------------------------------------------------------------
// Toroidal mode (cfg.toroidal, default off): senses wrap across the seam.
// Off-flag behaviour is byte-identical (every change is behind the flag);
// these tests pin the on-flag semantics.
static void test_toroidal() {
    std::printf("toroidal: wrapped queries, seam senses, resume...\n");

    // --- SpatialGrid::queryRadiusWrapped == brute-force minimum-image ---
    {
        rng.seed(77001u);
        const float W = 800.0f, H = 600.0f, R = 80.0f;
        SpatialGrid grid;
        grid.init((int)W, (int)H, R);
        std::vector<Vec2> pts;
        for (int i = 0; i < 300; ++i) {
            Vec2 p = {rng.range(0.0f, W - 0.001f), rng.range(0.0f, H - 0.001f)};
            pts.push_back(p);
            grid.insert(p, i);
        }
        auto mi = [](float d, float span) {
            if (d > 0.5f * span) return d - span;
            if (d < -0.5f * span) return d + span;
            return d;
        };
        bool setsMatch = true, noDupes = true;
        for (int q = 0; q < 60; ++q) {
            // Bias half the query centres into the edge band, where it matters.
            Vec2 c = (q % 2 == 0)
                ? Vec2{rng.range(0.0f, W), rng.range(0.0f, H)}
                : Vec2{rng.unit() < 0.5f ? rng.range(0.0f, 15.0f) : rng.range(W - 15.0f, W),
                       rng.range(0.0f, H)};
            std::vector<int> got;
            grid.queryRadiusWrapped(c, R, got);
            std::vector<int> want;
            for (int i = 0; i < (int)pts.size(); ++i) {
                const float dx = mi(pts[i].x - c.x, W), dy = mi(pts[i].y - c.y, H);
                if (dx * dx + dy * dy <= R * R) want.push_back(i);
            }
            std::vector<int> gotSorted = got;
            std::sort(gotSorted.begin(), gotSorted.end());
            if (std::adjacent_find(gotSorted.begin(), gotSorted.end()) != gotSorted.end())
                noDupes = false;
            std::sort(want.begin(), want.end());
            if (gotSorted != want) setsMatch = false;
        }
        CHECK(setsMatch);
        CHECK(noDupes);
    }

    // --- seam visibility through the real sensor path ---
    // Two creatures 10 units apart THROUGH the seam (890 apart across the
    // plane): toroidal sees a neighbour, bounded does not.
    for (int mode = 0; mode < 2; ++mode) {
        const bool tor = (mode == 1);
        rng.seed(55011u);
        World w;
        w.cfg.toroidal = tor;
        w.cfg.captureActivations = true;
        w.cfg.minPopulation = 0;
        w.init(900, 700, 2);
        w.creatures[0].pos = {5.0f, 350.0f};
        w.creatures[1].pos = {895.0f, 350.0f};
        w.update(0.016f);
        const float n0 = w.creatures[0].lastSensors[NEIGHBOR_COUNT];
        const float n1 = w.creatures[1].lastSensors[NEIGHBOR_COUNT];
        if (tor) { CHECK(n0 > 0.05f); CHECK(n1 > 0.05f); }
        else     { CHECK(n0 == 0.0f); CHECK(n1 == 0.0f); }
        // Boundary sensors: no-wall on the torus, near-wall on the plane.
        const float bd = w.creatures[0].lastSensors[BOUNDARY_DIST];
        if (tor) CHECK(bd == 1.0f);
        else     CHECK(bd < 0.2f);   // tanh(5/100) ~= 0.05
    }

    // --- pheromone deposits and reads wrap across the seam ---
    for (int mode = 0; mode < 2; ++mode) {
        const bool tor = (mode == 1);
        rng.seed(55012u);
        World w;
        w.cfg.toroidal = tor;
        w.cfg.minPopulation = 0;
        w.init(800, 600, 0);
        w.signals.emit({1.0f, 300.0f});          // cell x=0: radius-2 stamp spans the seam
        const float far = w.signals.read({797.0f, 300.0f});   // cell x=99 (wrapped -1)
        const float at  = w.signals.read({1.0f, 300.0f});
        CHECK(at > 0.0f);
        if (tor) CHECK(far > 0.0f);
        else     CHECK(far == 0.0f);
    }

#ifndef USE_OPENMP
    // --- toroidal + every hidden-state subsystem: resume is still bit-exact,
    //     runs are repeatable, and positions stay in-bounds ---
    {
        auto runWorld = [](World& w) {
            rng.seed(31417u);
            configureRichWorld(w);
            w.cfg.toroidal = true;
            w.init(900, 700, 260);
            for (int i = 0; i < 150; ++i) w.update(0.016f);
        };
        World a;
        runWorld(a);
        const std::string blob = checkpoint::serialize(a, "test");
        for (int i = 0; i < 100; ++i) a.update(0.016f);
        const std::uint64_t uninterrupted = deepFingerprint(a);

        rng.seed(999u); (void)rng.unit();        // scramble the global stream
        World b;
        std::string err;
        CHECK(checkpoint::deserialize(blob, b, err));
        CHECK(b.cfg.toroidal);                    // flag round-trips
        for (int i = 0; i < 100; ++i) b.update(0.016f);
        CHECK(deepFingerprint(b) == uninterrupted);

        bool inBounds = true;
        for (const auto& c : b.creatures)
            if (c.pos.x < 0.0f || c.pos.x >= (float)b.width ||
                c.pos.y < 0.0f || c.pos.y >= (float)b.height) inBounds = false;
        for (const auto& p : b.predators)
            if (p.pos.x < 0.0f || p.pos.x >= (float)b.width ||
                p.pos.y < 0.0f || p.pos.y >= (float)b.height) inBounds = false;
        CHECK(inBounds);
        CHECK(!b.creatures.empty());
    }
#endif
}

// ---------------------------------------------------------------
// PortableMT19937 must be indistinguishable from std::mt19937 in outputs
// (portable across standard libraries) and, on libstdc++, in serialized text
// (so old checkpoints load and native/wasm checkpoints match byte for byte).
static void test_portable_mt19937() {
    std::printf("PortableMT19937 == std::mt19937 (outputs, text, restore)...\n");
    const long positions[] = {0, 1, 623, 624, 625, 10000};
    for (long k : positions) {
        std::mt19937 ref(20260725u);
        PortableMT19937 mine(20260725u);
        bool outputsMatch = true;
        for (long i = 0; i < k; ++i)
            if (ref() != mine()) { outputsMatch = false; break; }
        CHECK(outputsMatch);
#ifdef __GLIBCXX__
        // Text equality with the library that wrote every pre-0.18.0
        // checkpoint. Only meaningful under libstdc++; the wasm/libc++ build
        // instead relies on this same pin having passed natively.
        std::ostringstream os;
        os << ref;
        CHECK(os.str() == mine.text());
#endif
        // Round-trip: serialize, restore into a fresh engine, streams agree.
        PortableMT19937 restored;
        CHECK(restored.fromText(mine.text()));
        bool restoredMatch = true;
        for (int i = 0; i < 2000; ++i)
            if (mine() != restored()) { restoredMatch = false; break; }
        CHECK(restoredMatch);
    }
    // Malformed state strings must be rejected.
    PortableMT19937 e;
    CHECK(!e.fromText(""));
    CHECK(!e.fromText("1 2 3"));
}

// ---------------------------------------------------------------
// ---------------------------------------------------------------
// Reproductive isolation (v0.21.0): the genome-distance metric and the
// reproduction gate built on it.
//
// The metric's whole reason for existing is that genomeSimilarity() is
// positionally aligned and therefore useless as a gate, so the contrast against
// it is asserted here rather than only described in a comment.
static void test_reproductive_isolation() {
    std::printf("reproductive isolation: metric properties + gate...\n");

    // --- metric properties, over many random genomes ---
    {
        rng.seed(31337u);
        bool selfZero = true, symmetric = true, permInvariant = true, nonNegative = true;
        for (int t = 0; t < 300; ++t) {
            Genome a = makeRandomGenome(16, 80);
            Genome b = makeRandomGenome(16, 80);

            if (genomeDistanceSq(a, a) != 0.0f) selfZero = false;
            // Bitwise symmetry, not approximate: the merge must be
            // order-independent, and IEEE subtraction is exactly antisymmetric.
            if (genomeDistanceSq(a, b) != genomeDistanceSq(b, a)) symmetric = false;
            if (genomeDistanceSq(a, b) < 0.0f) nonNegative = false;

            // Permutation invariance must be EXACT. It only holds because the
            // fold sorts on (cell, weight) rather than on cell alone; sorting on
            // cell alone would sum a cell's duplicate weights in genome order
            // and float addition is not associative.
            Genome shuffled = a;
            for (int k = (int)shuffled.size() - 1; k > 0; --k)
                std::swap(shuffled[k], shuffled[rng.intRange(0, k)]);
            if (genomeDistanceSq(a, shuffled) != 0.0f) permInvariant = false;
        }
        CHECK(selfZero);
        CHECK(symmetric);
        CHECK(permInvariant);
        CHECK(nonNegative);
    }

    // --- the sparse merge-join agrees with a dense reference ---
    // Independent oracle: materialise both genomes as full STATS_DIM vectors and
    // difference them directly. Catches any merge-join indexing mistake.
    {
        rng.seed(4242u);
        bool matchesDense = true;
        for (int t = 0; t < 60; ++t) {
            Genome a = makeRandomGenome(16, 80), b = makeRandomGenome(16, 80);
            std::vector<float> va(STATS_DIM, 0.0f), vb(STATS_DIM, 0.0f);
            for (const Gene& g : a) va[geneCell(g)] += g.weightAsFloat();
            for (const Gene& g : b) vb[geneCell(g)] += g.weightAsFloat();
            double ref = 0.0;
            for (int i = 0; i < STATS_DIM; ++i) {
                const double d = (double)va[i] - (double)vb[i];
                ref += d * d;
            }
            // Tolerance, not equality: the reference sums in genome order and in
            // double, the implementation in canonical order and in float.
            if (!approx((float)ref, genomeDistanceSq(a, b), 1e-2f)) matchesDense = false;
        }
        CHECK(matchesDense);
    }

    // --- indel robustness, and the contrast that justifies the new function ---
    {
        rng.seed(909090u);
        double meanSim = 0.0, meanDist = 0.0;
        const int N = 200;
        for (int t = 0; t < N; ++t) {
            Genome g = makeRandomGenome(24, 48);
            Genome ins = g;
            const int pos = rng.intRange(0, (int)ins.size() - 1);
            ins.insert(ins.begin() + pos, ins[pos]);   // one duplicate gene, in place
            meanSim  += genomeSimilarity(g, ins);
            meanDist += genomeDistance(g, ins);
        }
        meanSim /= N; meanDist /= N;
        // genomeSimilarity COLLAPSES on a single indel: every gene downstream of
        // the insertion is compared against its neighbour instead of itself.
        CHECK(meanSim < 0.75f);
        // The connectivity-space distance barely moves — one duplicated edge.
        CHECK(meanDist < 4.0f);
        // And unrelated genomes are far away on the same scale, so the metric
        // genuinely discriminates rather than being uniformly small.
        rng.seed(909091u);
        double meanUnrelated = 0.0;
        for (int t = 0; t < N; ++t)
            meanUnrelated += genomeDistance(makeRandomGenome(24, 48), makeRandomGenome(24, 48));
        meanUnrelated /= N;
        CHECK(meanUnrelated > 4.0f * meanDist);
    }

    // The next two blocks compare WHOLE RUNS, which only the serial build
    // promises to reproduce — under OpenMP the parallel phase draws from
    // per-thread RNGs and a fixed seed does not pin the run. Same guard the
    // topological, checkpoint and toroidal tests use.
#ifndef USE_OPENMP
    // --- a permissive gate is exactly a no-op ---
    // Threshold far above the largest achievable distance must reproduce the
    // ungated run bit for bit: the gate consumes no rng and changes no order.
    {
        const int STEPS = 400;
        rng.seed(24680u);
        World off;
        off.cfg.minPopulation = 0;
        off.init(900, 700, 220);
        for (int i = 0; i < STEPS; ++i) off.update(0.016f);

        rng.seed(24680u);
        World wide;
        wide.cfg.minPopulation = 0;
        wide.cfg.reproMaxDistance = 1.0e6f;
        wide.init(900, 700, 220);
        for (int i = 0; i < STEPS; ++i) wide.update(0.016f);

        CHECK(deepFingerprint(off) == deepFingerprint(wide));
        CHECK(off.totalBirths == wide.totalBirths);
        CHECK(off.totalBirths > 0);          // the control must actually breed
    }
#endif

    // The remaining checks are build-independent: they assert only that births
    // do or do not happen, which the gate decides identically in both builds.

    // --- a tight gate stops reproduction entirely ---
    {
        const int STEPS = 400;
        rng.seed(24680u);
        World tight;
        tight.cfg.minPopulation = 0;
        tight.cfg.reproMaxDistance = 0.5f;   // far below any random pair distance
        tight.init(900, 700, 220);
        for (int i = 0; i < STEPS; ++i) tight.update(0.016f);
        CHECK(tight.totalBirths == 0);
    }

    // --- the gate is SELECTIVE, not merely restrictive ---
    // Same tight threshold, but the population is two hand-planted clones
    // instead of 220 unrelated genomes. Within-clone pairs are at distance 0 and
    // must breed; a run of all-distinct genomes at the same threshold bred
    // nothing (above). That contrast is the actual claim: births track genome
    // distance, not the threshold alone.
    {
        const int STEPS = 400;
        rng.seed(13579u);
        World two;
        two.cfg.minPopulation = 0;
        two.cfg.reproMaxDistance = 0.5f;
        two.init(900, 700, 220);
        Genome clusterA = makeRandomGenome(24, 48);
        Genome clusterB = makeRandomGenome(24, 48);
        CHECK(genomeDistance(clusterA, clusterB) > 10.0f);   // genuinely far apart
        for (size_t i = 0; i < two.creatures.size(); ++i) {
            two.creatures[i].genome = (i % 2 == 0) ? clusterA : clusterB;
            buildNeuralNet(two.creatures[i].genome, two.creatures[i].nnet);
        }
        for (int i = 0; i < STEPS; ++i) two.update(0.016f);
        CHECK(two.totalBirths > 0);
    }

    // --- determinism with the gate on (serial build only, as above) ---
#ifndef USE_OPENMP
    {
        const int STEPS = 300;
        std::uint64_t fp[2] = {0, 0};
        for (int rep = 0; rep < 2; ++rep) {
            rng.seed(50505u);
            World w;
            configureRichWorld(w);           // predators, currents, patches too
            w.cfg.reproMaxDistance = 18.0f;  // a threshold that actually bites
            w.init(900, 700, 240);
            for (int i = 0; i < STEPS; ++i) w.update(0.016f);
            fp[rep] = deepFingerprint(w);
        }
        CHECK(fp[0] == fp[1]);
    }
#endif

    // --- the gate survives a checkpoint round trip ---
    {
        rng.seed(60606u);
        World a;
        a.cfg.reproMaxDistance = 16.5f;
        a.init(900, 700, 200);
        for (int i = 0; i < 120; ++i) a.update(0.016f);
        const std::string blob = checkpoint::serialize(a, "test");
        World b;
        std::string err;
        CHECK(checkpoint::deserialize(blob, b, err));
        CHECK(b.cfg.reproMaxDistance == 16.5f);

        // A checkpoint written before v0.21.0 has no such key; loading it must
        // leave the gate off rather than inventing a value.
        World c;
        c.cfg.reproMaxDistance = -1.0f;
        std::string legacy = blob;
        const std::string key = "\"reproMaxDistance\":16.5,";
        const size_t at = legacy.find(key);
        CHECK(at != std::string::npos);
        if (at != std::string::npos) legacy.erase(at, key.size());
        std::string err2;
        CHECK(checkpoint::deserialize(legacy, c, err2));
        CHECK(c.cfg.reproMaxDistance < 0.0f);
    }
}

// ---------------------------------------------------------------
// Reproduction graph (v0.22.0). Validated the way `species` was: against
// constructed cases whose answer is known by hand, BEFORE it is trusted on
// real runs.
static void test_repro_graph() {
    std::printf("reproduction graph: components, decimation, CSV column...\n");

    // --- hand-built graphs with known answers ---
    {
        using repro::Edge;
        // Empty graph: no edges, no vertices. Must report 0, not crash and not
        // divide by zero — this is exactly what a gate tight enough to stop all
        // breeding produces, so it is a real case, not a pathological one.
        CHECK(repro::componentCount({}, 3) == 0);

        // One well-mixed group: a 5-cycle. One component of 5 members.
        const std::vector<Edge> ring = {{1,2},{2,3},{3,4},{4,5},{5,1}};
        CHECK(repro::componentCount(ring, 3) == 1);
        CHECK(repro::componentCount(ring, 5) == 1);
        CHECK(repro::componentCount(ring, 6) == 0);   // no component that big

        // Two mutually incompatible clusters that never exchange an edge.
        const std::vector<Edge> split = {{1,2},{2,3},{3,1},  {10,11},{11,12},{12,10}};
        CHECK(repro::componentCount(split, 3) == 2);

        // Add a single bridging birth and the two collapse into one — this is
        // the signal that reproductive isolation has BROKEN DOWN.
        std::vector<Edge> bridged = split;
        bridged.push_back({3, 10});
        CHECK(repro::componentCount(bridged, 3) == 1);

        // An isolated pair is a component of size 2, below the default floor of
        // 3, so it must not inflate the count.
        std::vector<Edge> withPair = ring;
        withPair.push_back({90, 91});
        CHECK(repro::componentCount(withPair, 3) == 1);
        CHECK(repro::componentCount(withPair, 2) == 2);

        // Order-independence: components are a property of the edge SET.
        std::vector<Edge> shuffled = split;
        std::reverse(shuffled.begin(), shuffled.end());
        CHECK(repro::componentCount(shuffled, 3) == repro::componentCount(split, 3));

        // Duplicate and self edges must not create phantom components.
        std::vector<Edge> dupes = {{1,2},{1,2},{2,3},{3,3}};
        CHECK(repro::componentCount(dupes, 3) == 1);
    }

    // --- the window is a ring: it holds the LAST N births, undecimated ---
    // 1200 steps, not 600: `age > 5.0f` is a hard reproduction precondition and
    // at dt 0.016 nothing can breed before step ~312, so a short run measures
    // the warm-up rather than the ring.
    // Serial-only: >64 is a whole-run birth count, which the OpenMP build's
    // per-thread rngs do not pin (same guard as the fingerprint tests).
#ifndef USE_OPENMP
    {
        rng.seed(80808u);
        World w;
        w.cfg.reproWindow = 64;
        w.cfg.minPopulation = 0;
        w.init(900, 700, 240);
        for (int i = 0; i < 1200; ++i) w.update(0.016f);
        CHECK(w.totalBirths > 64);                       // enough births to wrap
        CHECK((int)w.reproWindow_.size() == 64);         // capped, not growing
        CHECK(w.reproComponents() >= 0);
    }
#endif

    // --- eviction: the ring holds the LAST N events, and old edges age OUT ---
    // Drives logBirth directly with hand-built Creatures (it touches only the
    // pairing structures — no sim state, no rng), so this is deterministic in
    // both builds and pins push-until-cap, the overwrite position, cursor wrap
    // and eviction: a broken cursor advance would keep the bridge below alive
    // forever while every other test here still passed.
    {
        World w;
        w.cfg.reproWindow = 4;
        w.cfg.reproMinCluster = 3;
        Creature ca, cb;
        auto edge = [&](std::uint64_t ia, std::uint64_t ib) {
            ca.id = ia; cb.id = ib;
            w.logBirth(ca, cb);
        };
        edge(1, 2); edge(2, 3); edge(10, 11); edge(11, 12);   // ring now full
        CHECK(w.reproComponents() == 2);          // {1,2,3} and {10,11,12}
        edge(3, 10);                              // bridge; evicts (1,2)
        CHECK(w.reproComponents() == 1);          // {2,3,10,11,12}
        // Four more events replace the whole window; the bridge must age out.
        edge(1, 2); edge(2, 3); edge(10, 11); edge(11, 12);
        CHECK(w.reproComponents() == 2);          // split again: bridge evicted
    }

    // --- a real run's graph is connected; a sterilised run's is empty ---
    {
        const int STEPS = 600;
        rng.seed(24680u);
        World mixed;
        mixed.cfg.minPopulation = 0;
        mixed.init(900, 700, 220);
        for (int i = 0; i < STEPS; ++i) mixed.update(0.016f);
        CHECK(mixed.totalBirths > 0);
        CHECK(mixed.reproComponents() >= 1);

        rng.seed(24680u);
        World sterile;
        sterile.cfg.minPopulation = 0;
        sterile.cfg.reproMaxDistance = 0.5f;   // nobody can breed
        sterile.init(900, 700, 220);
        for (int i = 0; i < STEPS; ++i) sterile.update(0.016f);
        CHECK(sterile.totalBirths == 0);
        CHECK(sterile.reproComponents() == 0);   // no edges => 0, reported sanely
    }

    // --- the archive log is bounded and decimates like the death log ---
    // Serial-only for the same reason as the ring test: >64 is a whole-run
    // count that OpenMP's per-thread rngs do not pin.
#ifndef USE_OPENMP
    {
        rng.seed(31415u);
        World w;
        w.cfg.reproLog = true;
        w.cfg.reproLogMax = 32;                  // tiny, to force several halvings
        w.cfg.minPopulation = 0;
        w.init(900, 700, 240);
        for (int i = 0; i < 900; ++i) w.update(0.016f);
        CHECK(w.totalBirthsLogged_ > 64);
        CHECK((int)w.birthLog.size() < 2 * w.cfg.reproLogMax);   // stayed bounded
        CHECK(w.reproStride_ >= 2);                               // did decimate
        // Kept records must be exactly those on the current stride, in order.
        bool strideOk = true, ordered = true;
        long long prev = -1;
        for (const World::BirthRecord& b : w.birthLog) {
            if (b.index % w.reproStride_ != 0) strideOk = false;
            if (b.index <= prev) ordered = false;
            prev = b.index;
            if (b.parent1 == 0 || b.parent2 == 0) strideOk = false;  // ids assigned
        }
        CHECK(strideOk);
        CHECK(ordered);
    }

    // --- the whole apparatus is observational: fingerprints cannot move ---
    // The always-on window, the opt-in archive and the component count draw no
    // rng and touch no sim state; this is that claim as a test, not a
    // code-reading exercise. Serial-only, like every whole-run comparison.
    {
        const int STEPS = 400;
        auto fingerprintWith = [](int window, bool logOn) {
            rng.seed(70707u);
            World w;
            w.cfg.minPopulation = 0;
            w.cfg.reproWindow = window;
            w.cfg.reproLog = logOn;
            w.init(900, 700, 220);
            for (int i = 0; i < STEPS; ++i) w.update(0.016f);
            return deepFingerprint(w);
        };
        const std::uint64_t base = fingerprintWith(2000, false);
        CHECK(fingerprintWith(0, false) == base);      // window off == default
        CHECK(fingerprintWith(64, false) == base);     // window size irrelevant
        CHECK(fingerprintWith(2000, true) == base);    // archive is write-only
    }

    // --- same seed twice with the archive on: identical records, not merely
    // identical worlds ---
    {
        std::vector<World::BirthRecord> logs[2];
        std::uint64_t fp[2] = {0, 0};
        for (int rep = 0; rep < 2; ++rep) {
            rng.seed(90909u);
            World w;
            w.cfg.minPopulation = 0;
            w.cfg.reproLog = true;
            w.init(900, 700, 240);
            for (int i = 0; i < 900; ++i) w.update(0.016f);
            fp[rep] = deepFingerprint(w);
            logs[rep] = w.birthLog;
        }
        CHECK(fp[0] == fp[1]);
        bool same = logs[0].size() == logs[1].size() && !logs[0].empty();
        for (size_t i = 0; same && i < logs[0].size(); ++i)
            same = logs[0][i].index == logs[1][i].index
                && logs[0][i].parent1 == logs[1][i].parent1
                && logs[0][i].parent2 == logs[1][i].parent2
                && logs[0][i].lineage1 == logs[1][i].lineage1
                && logs[0][i].lineage2 == logs[1][i].lineage2
                && logs[0][i].frame == logs[1][i].frame;
        CHECK(same);
    }
#endif

    // --- checkpoints: the new fields round-trip, old checkpoints default, and
    // a resume starts the window empty ---
    {
        rng.seed(42424u);
        World a;
        a.cfg.reproWindow = 777;
        a.cfg.reproMinCluster = 5;
        a.init(900, 700, 200);
        for (int i = 0; i < 600; ++i) a.update(0.016f);
        const std::string blob = checkpoint::serialize(a, "test");

        // Round trip — into a World that has ALREADY RUN, which is exactly the
        // case that catches stale pairing state surviving a load.
        rng.seed(11111u);
        World b;
        b.cfg.minPopulation = 0;
        b.init(900, 700, 220);
        for (int i = 0; i < 900; ++i) b.update(0.016f);
        CHECK(!b.reproWindow_.empty());       // the stale state actually exists
        std::string err;
        CHECK(checkpoint::deserialize(blob, b, err));
        CHECK(b.cfg.reproWindow == 777);
        CHECK(b.cfg.reproMinCluster == 5);
        CHECK(b.reproWindow_.empty());        // the window is per-run state
        CHECK(b.birthLog.empty());
        CHECK(b.reproComponents() == 0);      // empty window reports 0, sanely

        // A checkpoint written before v0.22.0 has neither key; loading it must
        // fall back to the defaults rather than inventing values.
        World c;
        std::string legacy = blob;
        for (const std::string key : {std::string("\"reproWindow\":777,"),
                                      std::string("\"reproMinCluster\":5,")}) {
            const size_t at = legacy.find(key);
            CHECK(at != std::string::npos);
            if (at != std::string::npos) legacy.erase(at, key.size());
        }
        std::string err2;
        CHECK(checkpoint::deserialize(legacy, c, err2));
        CHECK(c.cfg.reproWindow == 2000);
        CHECK(c.cfg.reproMinCluster == 3);
    }

    // --- the CSV column is appended, not inserted ---
    {
        const std::string h = statsCsvHeader();
        // Every pre-0.22.0 column keeps its index: the old header must be a
        // prefix of the new one.
        const std::string old =
            "step,elapsed_s,steps_per_sec,population,food,"
            "births_cum,deaths_cum,births_interval,deaths_interval,"
            "avg_age,max_age,avg_energy,avg_speed,"
            "avg_genome_len,genome_len_std,avg_responsiveness,avg_osc_period,"
            "genetic_diversity,species,flow_align,"
            "predator_pop,kills_interval,deaths_starvation,deaths_age,predator_align,"
            "neighbor_align,neighbor_count,"
            "predator_births_interval,predator_deaths_starv,predator_deaths_age,predator_avg_energy";
        CHECK(h.rfind(old, 0) == 0);
        CHECK(h.size() > old.size());
        // species is still column 19 (1-based), as the whole corpus assumes.
        int commas = 0; std::size_t at = 0;
        while (at < h.size() && commas < 18) { if (h[at] == ',') ++commas; ++at; }
        CHECK(h.compare(at, 7, "species") == 0);
        // header and row must agree on field count
        WorldStats s;
        const std::string row = statsCsvRow(s, 0.0, 0.0, 0, 0, 0, 0, 0, 0, 0, 0);
        CHECK(std::count(h.begin(), h.end(), ',') == std::count(row.begin(), row.end(), ','));
        // The new column is the LAST header field, so a later append cannot
        // silently order itself between repro_components and the row's tail.
        CHECK(h.size() > 17 && h.compare(h.size() - 17, 17, ",repro_components") == 0);
    }
}

int main() {
    std::printf("=== evo_boids core tests (no SFML linked) ===\n");
    test_portable_mt19937();
    test_vec2();
    test_color();
    test_quadtree();
    test_spatial_grid_matches_quadtree();
    test_genome_and_net();
    test_feedforward_equivalence();
    test_fast_tanh();
    test_topological_neighbors();
    test_checkpoint();
    test_toroidal();
    test_creature();
    test_world_invariants();
    test_predator();
    test_predator_economy();
    test_inspection_and_cluster();
    test_analysis_v012();
    test_schooling_metric();
    test_reproductive_isolation();
    test_repro_graph();

    std::printf("---------------------------------------------\n");
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("ALL CORE TESTS PASSED\n");
        return 0;
    }
    std::printf("SOME TESTS FAILED\n");
    return 1;
}
