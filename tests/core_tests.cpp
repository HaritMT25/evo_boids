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
    CHECK(net.connections.size() == g.size());

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
int main() {
    std::printf("=== evo_boids core tests (no SFML linked) ===\n");
    test_vec2();
    test_color();
    test_quadtree();
    test_spatial_grid_matches_quadtree();
    test_genome_and_net();
    test_creature();
    test_world_invariants();
    test_predator();
    test_predator_economy();
    test_inspection_and_cluster();
    test_analysis_v012();
    test_schooling_metric();

    std::printf("---------------------------------------------\n");
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("ALL CORE TESTS PASSED\n");
        return 0;
    }
    std::printf("SOME TESTS FAILED\n");
    return 1;
}
