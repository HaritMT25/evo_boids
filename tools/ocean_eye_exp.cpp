// ocean_eye_exp.cpp — the eye-only experiment (eye build): can fish live on the
// compound eye alone? One seeded run of one sense arm, with the ecology and the
// wiring logged every K steps to a CSV, and at the end a frozen-genome
// behavioural test; or --bench: the four sense arms timed interleaved on one
// snapshot (the cost question).
//
// Arms (OceanConfig eye / eyeOnly; every other setting from --cfg, identical):
//   A  labeled senses, eye off      (eye 0, eyeOnly 0)
//   B  labeled senses + eye         (eye 1, eyeOnly 0)
//   C  eye only                     (eye 1, eyeOnly 1)
//   D  blind                        (eye 0, eyeOnly 1)
//
//   g++ -O3 -std=c++20 -ffp-contract=off -DEVO_WORLD_OCEAN -DEVO_OCEAN_EYE -I src
//       tools/ocean_eye_exp.cpp -o ocean_eye_exp
//   ./ocean_eye_exp --arm C --seed 1 --steps 100000 --every 10000 --cfg '{...}' --out run_C_s1 [--behav]
//   ./ocean_eye_exp --bench --evolve 30000 --evolve-arm B --cfg '{...}' --reps 7 --warm 20 --steps 200
//
// Outputs: PREFIX.csv (one row per interval), PREFIX_behav.csv (with --behav).
//
// Wiring (topology only): "eye -> move" = some EYE_* input reaches a movement
// action (STEER_X/Y/Z, MOVE_FORWARD, SPEED_ADJUST) directly / through any
// chain of hidden neurons. Each interval also scores a random-genome baseline:
// for every live fish a random genome of the SAME length (its own rng stream,
// the world's rng is saved and restored, so the run is unchanged).
//
// Behavioural test (--behav, at the end): every live fish's genome, frozen
// (fresh network state), alone in a quiet eye-only world (no labels: the fish
// can only SEE), level at mid-depth, for 8 headings x {nothing, one food item
// 50 away in each of the 6 sectors, one predator 60 away in each sector}; the
// scene is held fixed for 3 steps (so sensor -> hidden -> action paths act) and
// the 3rd step's actions are read. The same for a random genome of the same
// length in the same body. Indices (action units, + = adaptive):
//   food_h   lateral steer toward food right minus left (body frame)
//   food_v   STEER_Z toward food: food below minus food above
//   food_spd SPEED_ADJUST with food ahead minus with nothing
//   pred_h   lateral steer away from a predator (left minus right)
//   pred_v   STEER_Z away: predator above minus below
//   pred_spd SPEED_ADJUST with a predator (mean of the 6 sectors) minus nothing
//   react_food / react_pred  mean L1 change of (STEER_X, STEER_Y, STEER_Z,
//            SPEED_ADJUST) vs nothing (does seeing anything change behaviour?)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "ocean_fingerprint.hpp"   // ofp::initWorld: the page's config path

#ifndef EVO_OCEAN_EYE
#error "ocean_eye_exp needs the eye build: -DEVO_OCEAN_EYE"
#endif

namespace {

using clk = std::chrono::steady_clock;
constexpr float kDt = 0.016f;

struct ArmDef { char name; int eye, eyeOnly; };
constexpr ArmDef kArms[] = {{'A', 0, 0}, {'B', 1, 0}, {'C', 1, 1}, {'D', 0, 1}};
const ArmDef* findArm(char c) {
    for (const ArmDef& a : kArms) if (a.name == c) return &a;
    return nullptr;
}

constexpr int kMoveActs[] = {STEER_X, STEER_Y, STEER_Z, MOVE_FORWARD, SPEED_ADJUST};
constexpr std::uint32_t kMoveMask = (1u << STEER_X) | (1u << STEER_Y) | (1u << STEER_Z) |
                                    (1u << MOVE_FORWARD) | (1u << SPEED_ADJUST);

// Actions reached from any source in `srcs` (a predicate on the source index):
// direct wires, and any path through hidden neurons. Bitmasks over actions.
template <class Pred>
void reachFrom(const NeuralNet& nn, Pred srcs, std::uint32_t& direct, std::uint32_t& any) {
    bool hid[MAX_INTERNAL_NEURONS] = {};
    direct = any = 0;
    for (const auto& fc : nn.flat) {
        const int s = fc.src(), k = fc.sink();
        if (s < NUM_SENSES && srcs(s) && k < NUM_ACTIONS) direct |= 1u << k;
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto& fc : nn.flat) {
            const int s = fc.src(), k = fc.sink();
            const bool on = (s < NUM_SENSES) ? srcs(s) : hid[s - NUM_SENSES];
            if (!on) continue;
            if (k < NUM_ACTIONS) any |= 1u << k;
            else if (!hid[k - NUM_ACTIONS]) { hid[k - NUM_ACTIONS] = true; changed = true; }
        }
    }
}

struct Wiring {
    double fish = 0;
    double eyeAny = 0, eyeDirect = 0, eyeConns = 0, eyeAbsW = 0;   // eye -> move
    double eyeShareNum = 0, sensorConns = 0;                        // eye share of sensor conns
    double eyeVertDirect = 0;   // EYE_UP/DOWN_* -> STEER_Z direct (vertical use)
    double foodMatched = 0;     // NEAREST_FOOD_DX->STEER_X | DY->STEER_Y | DZ->STEER_Z, w > 0
    double predMove = 0;        // PREDATOR_* -> move, any path
    double labMove = 0;         // any labeled sense -> move, any path
    double genomeLen = 0;
    void add(const NeuralNet& nn, std::size_t glen) {
        fish += 1;
        genomeLen += (double)glen;
        std::uint32_t d, a;
        reachFrom(nn, [](int s) { return ocean::isEyeSense(s); }, d, a);
        eyeAny += (a & kMoveMask) != 0;
        eyeDirect += (d & kMoveMask) != 0;
        bool matched = false, vert = false;
        for (const auto& fc : nn.flat) {
            const int s = fc.src(), k = fc.sink();
            if (s >= NUM_SENSES) continue;
            sensorConns += 1;
            const bool eye = ocean::isEyeSense(s);
            eyeShareNum += eye;
            if (eye && k < NUM_ACTIONS && ((kMoveMask >> k) & 1u)) {
                eyeConns += 1;
                eyeAbsW += std::abs(fc.w());
            }
            if (k == STEER_Z && s >= EYE_UP_PROX && s <= EYE_DOWN_CON) vert = true;
            if (fc.weight > 0 && ((s == NEAREST_FOOD_DX && k == STEER_X) || (s == NEAREST_FOOD_DY && k == STEER_Y) ||
                                  (s == NEAREST_FOOD_DZ && k == STEER_Z)))
                matched = true;
        }
        eyeVertDirect += vert;
        foodMatched += matched;
        reachFrom(nn, [](int s) { return s == PREDATOR_FWD || s == PREDATOR_LR || s == PREDATOR_PROX; }, d, a);
        predMove += (a & kMoveMask) != 0;
        reachFrom(nn, [](int s) { return ocean::isLabeledSense(s); }, d, a);
        labMove += (a & kMoveMask) != 0;
    }
};

// A random genome of length len from the private stream `r` (the global rng is
// what Gene::makeRandom draws from: swap it in and out).
Genome randomGenomeOfLength(RandomGen& r, std::size_t len) {
    const RandomGen keep = rng;
    rng = r;
    Genome g;
    g.reserve(len);
    for (std::size_t i = 0; i < len; ++i) g.push_back(Gene::makeRandom());
    r = rng;
    rng = keep;
    return g;
}

void scoreWiring(const OceanWorld& w, RandomGen& baseRng, Wiring& evo, Wiring& rnd) {
    for (const Fish& f : w.creatures) {
        if (!f.alive) continue;
        evo.add(f.nnet, f.genome.size());
        const Genome g = randomGenomeOfLength(baseRng, f.genome.size());
        NeuralNet nn;
        buildNeuralNet(g, nn);
        rnd.add(nn, g.size());
    }
}

// ---- the behavioural test ------------------------------------------------------
struct Behav {
    double n = 0;
    double food_h = 0, food_v = 0, food_spd = 0, pred_h = 0, pred_v = 0, pred_spd = 0;
    double react_food = 0, react_pred = 0;
    double pos_food_h = 0, pos_food_v = 0, pos_pred_spd = 0, pos_pred_v = 0;   // fraction of fish with index > 0.05
};

// What one fish does (its 3rd-step actions) in a held scene. kind: 0 nothing,
// 1 food, 2 predator; sector 0..5 = ahead, behind, left, right, up, down.
struct Probe {
    const OceanWorld& tmpl;
    RandomGen sceneRng;
    void act(const Fish& base, Vec2 heading, int kind, int sector, float out[NUM_ACTIONS]) {
        OceanWorld w = tmpl;
        const Vec2 P = {0.5f * (float)w.width, 0.5f * (float)w.height};
        const float Z = 0.5f * w.D;
        const Vec2 rht = {-heading.y, heading.x};
        Vec2 off = {0, 0};
        float dz = 0;
        const float dist = (kind == 2) ? 60.0f : 50.0f;
        switch (sector) {
            case 0: off = {heading.x * dist, heading.y * dist}; break;
            case 1: off = {-heading.x * dist, -heading.y * dist}; break;
            case 2: off = {-rht.x * dist, -rht.y * dist}; break;
            case 3: off = {rht.x * dist, rht.y * dist}; break;
            case 4: dz = -dist; break;
            case 5: dz = dist; break;
        }
        w.creatures.clear();
        w.creatures.push_back(base);
        w.food.clear();
        w.predators.clear();
        const RandomGen keep = rng;
        for (int s = 0; s < 3; ++s) {
            Fish& f = w.creatures[0];
            f.pos = P; f.vel = heading; f.lastMoveDir = heading; f.depth = Z; f.vz = 0.0f;
            f.foodReserves = 25.0f;
            if (kind == 1) {
                w.food.clear();
                OceanFood fd;
                fd.pos = {P.x + off.x, P.y + off.y};
                fd.depth = Z + dz;
                fd.eyeCon = ocean::eyeContrast(ocean::kEyeFoodBright, fd.depth);
                w.food.push_back(fd);
            } else if (kind == 2) {
                w.predators.clear();
                OceanPredator p;
                p.pos = {P.x + off.x, P.y + off.y};
                p.depth = Z + dz;
                p.vel = {-heading.x, -heading.y};
                p.digestTimer = 1e9f;
                w.predators.push_back(p);
            }
            rng = sceneRng;   // the same RANDOM_SENSOR / MOVE_RANDOM draws in every scene
            w.update(kDt);
        }
        rng = keep;
        std::memcpy(out, w.creatures[0].lastActions, sizeof(float) * NUM_ACTIONS);
    }
};

// One fish's indices, averaged over 8 headings. lat = steer along the body's
// right axis (world-frame STEER_X/Y projected; triangular motion: STEER_X turns).
void behaveOne(Probe& pr, const Fish& base, bool triangular, double idx[8]) {
    for (int k = 0; k < 8; ++k) idx[k] = 0;
    constexpr int H = 8;
    for (int h = 0; h < H; ++h) {
        const float a = 6.2831853f * (float)h / H + 0.3f;
        const Vec2 hd = {std::cos(a), std::sin(a)};
        const Vec2 rht = {-hd.y, hd.x};
        auto lat = [&](const float* A) { return triangular ? A[STEER_X] : A[STEER_X] * rht.x + A[STEER_Y] * rht.y; };
        auto l1 = [&](const float* A, const float* B) {
            return std::abs(A[STEER_X] - B[STEER_X]) + std::abs(A[STEER_Y] - B[STEER_Y]) +
                   std::abs(A[STEER_Z] - B[STEER_Z]) + std::abs(A[SPEED_ADJUST] - B[SPEED_ADJUST]);
        };
        float none[NUM_ACTIONS], fd[6][NUM_ACTIONS], pd[6][NUM_ACTIONS];
        pr.act(base, hd, 0, 0, none);
        for (int s = 0; s < 6; ++s) { pr.act(base, hd, 1, s, fd[s]); pr.act(base, hd, 2, s, pd[s]); }
        idx[0] += lat(fd[3]) - lat(fd[2]);
        idx[1] += fd[5][STEER_Z] - fd[4][STEER_Z];
        idx[2] += fd[0][SPEED_ADJUST] - none[SPEED_ADJUST];
        idx[3] += lat(pd[2]) - lat(pd[3]);
        idx[4] += pd[4][STEER_Z] - pd[5][STEER_Z];
        double ps = 0, rf = 0, rp = 0;
        for (int s = 0; s < 6; ++s) {
            ps += pd[s][SPEED_ADJUST] - none[SPEED_ADJUST];
            rf += l1(fd[s], none);
            rp += l1(pd[s], none);
        }
        idx[5] += ps / 6; idx[6] += rf / 6; idx[7] += rp / 6;
    }
    for (int k = 0; k < 8; ++k) idx[k] /= H;
}

void addBehav(Behav& b, const double idx[8]) {
    b.n += 1;
    b.food_h += idx[0]; b.food_v += idx[1]; b.food_spd += idx[2];
    b.pred_h += idx[3]; b.pred_v += idx[4]; b.pred_spd += idx[5];
    b.react_food += idx[6]; b.react_pred += idx[7];
    b.pos_food_h += idx[0] > 0.05; b.pos_food_v += idx[1] > 0.05;
    b.pos_pred_spd += idx[5] > 0.05; b.pos_pred_v += idx[4] > 0.05;
}

void writeBehavRow(std::FILE* f, const char* arm, unsigned seed, const char* who, const Behav& b) {
    const double n = b.n > 0 ? b.n : 1;
    std::fprintf(f, "%s,%u,%s,%.0f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.4f,%.4f,%.4f,%.4f\n", arm, seed, who, b.n,
                 b.food_h / n, b.food_v / n, b.food_spd / n, b.pred_h / n, b.pred_v / n, b.pred_spd / n,
                 b.react_food / n, b.react_pred / n, b.pos_food_h / n, b.pos_food_v / n, b.pos_pred_v / n,
                 b.pos_pred_spd / n);
}

void behaviouralTest(const OceanWorld& w, const char* arm, unsigned seed, const std::string& path) {
    // A quiet eye-only world with the run's own motion settings.
    OceanWorld tmpl;
    tmpl.cfg = w.cfg;
    tmpl.cfg.targetFood = 0; tmpl.cfg.foodSpawnBase = 0; tmpl.cfg.foodSpawnAmp = 0;
    tmpl.cfg.minPopulation = 0; tmpl.cfg.maxPopulation = 0;
    tmpl.cfg.predatorCount = 0; tmpl.cfg.predatorEconomy = false; tmpl.cfg.predatorMinPopulation = 0;
    tmpl.cfg.metabolicRate = 0; tmpl.cfg.moveCost = 0; tmpl.cfg.geneCost = 0; tmpl.cfg.maxAge = 1e9f;
    tmpl.cfg.currentStrength = 0; tmpl.cfg.foodPatches = 0; tmpl.cfg.foodDrift = false;
    tmpl.cfg.captureActivations = true;
    tmpl.ocfg = w.ocfg;
    tmpl.ocfg.eye = 1;
    tmpl.ocfg.eyeOnly = 1;
    const RandomGen keep = rng;
    rng.seed(seed * 7919u + 17u);
    tmpl.init(w.width, w.height, w.D, 0);
    Probe pr{tmpl, rng};
    RandomGen genRng;
    genRng.seed(seed * 104729u + 3u);
    Behav evo, rnd;
    double idx[8];
    const bool tri = w.cfg.triangularMotion;
    for (const Fish& f0 : w.creatures) {
        if (!f0.alive) continue;
        Fish f = f0;
        buildNeuralNet(f.genome, f.nnet);   // frozen genome, fresh network state
        behaveOne(pr, f, tri, idx);
        addBehav(evo, idx);
        f.genome = randomGenomeOfLength(genRng, f0.genome.size());
        buildNeuralNet(f.genome, f.nnet);
        behaveOne(pr, f, tri, idx);
        addBehav(rnd, idx);
    }
    rng = keep;
    std::FILE* out = std::fopen(path.c_str(), "w");
    if (!out) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); return; }
    std::fprintf(out, "arm,seed,genomes,n,food_h,food_v,food_spd,pred_h,pred_v,pred_spd,react_food,react_pred,"
                      "frac_food_h,frac_food_v,frac_pred_v,frac_pred_spd\n");
    writeBehavRow(out, arm, seed, "evolved", evo);
    writeBehavRow(out, arm, seed, "random", rnd);
    std::fclose(out);
}

double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return (n % 2) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

}  // namespace

int main(int argc, char** argv) {
    unsigned seed = 1;
    long steps = 100000, every = 10000, evolve = 30000;
    int W = 1920, H = 1080, pop = 300, reps = 7, warm = 20, bsteps = 200;
    double depth = 240.0;
    const char* cfgJson = nullptr;
    std::string out = "run", armArg = "A", evolveArm = "B";
    bool behav = false, bench = false;
    for (int i = 1; i < argc; ++i) {
        auto is = [&](const char* k) { return std::strcmp(argv[i], k) == 0 && i + 1 < argc; };
        if (is("--seed")) seed = (unsigned)std::strtoul(argv[++i], nullptr, 10);
        else if (is("--steps")) steps = std::strtol(argv[++i], nullptr, 10);
        else if (is("--every")) every = std::strtol(argv[++i], nullptr, 10);
        else if (is("--w")) W = std::atoi(argv[++i]);
        else if (is("--h")) H = std::atoi(argv[++i]);
        else if (is("--pop")) pop = std::atoi(argv[++i]);
        else if (is("--depth")) depth = std::atof(argv[++i]);
        else if (is("--cfg")) cfgJson = argv[++i];
        else if (is("--out")) out = argv[++i];
        else if (is("--arm")) armArg = argv[++i];
        else if (is("--evolve")) evolve = std::strtol(argv[++i], nullptr, 10);
        else if (is("--evolve-arm")) evolveArm = argv[++i];
        else if (is("--reps")) reps = std::atoi(argv[++i]);
        else if (is("--warm")) warm = std::atoi(argv[++i]);
        else if (is("--bsteps")) bsteps = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--behav")) behav = true;
        else if (!std::strcmp(argv[i], "--bench")) bench = true;
        else { std::fprintf(stderr, "unknown or incomplete argument: %s\n", argv[i]); return 2; }
    }

    OceanWorld w;
    std::string err;
    if (!ofp::initWorld(w, seed, W, H, (float)depth, pop, cfgJson, err)) {
        std::fprintf(stderr, "config refused: %s\n", err.c_str());
        return 4;
    }

    if (bench) {
        // Evolve under one arm, then time all four arms on that snapshot,
        // interleaved, the start rotating with the rep.
        const ArmDef* ea = findArm(evolveArm.empty() ? 'B' : evolveArm[0]);
        if (!ea) { std::fprintf(stderr, "bad --evolve-arm\n"); return 2; }
        w.ocfg.eye = ea->eye; w.ocfg.eyeOnly = ea->eyeOnly;
        const auto t0 = clk::now();
        for (long s = 0; s < evolve; ++s) w.update(kDt);
        std::printf("bench: evolved %ld steps (arm %c) in %.1f s: pop=%zu pred=%zu food=%zu\n", evolve, ea->name,
                    std::chrono::duration<double>(clk::now() - t0).count(), w.creatures.size(), w.predators.size(),
                    w.food.size());
        const OceanWorld snap = w;
        const RandomGen rsnap = rng;
        constexpr int V = 4;
        std::vector<double> ms[V], fishSteps[V];
        for (int r = -1; r < reps; ++r) {
            for (int j = 0; j < V; ++j) {
                const int vi = (r + 1 + j) % V;
                OceanWorld x = snap;
                rng = rsnap;
                x.ocfg.eye = kArms[vi].eye; x.ocfg.eyeOnly = kArms[vi].eyeOnly;
                for (int s = 0; s < warm; ++s) x.update(kDt);
                double fs = 0;
                const auto a = clk::now();
                for (int s = 0; s < bsteps; ++s) { fs += (double)x.creatures.size(); x.update(kDt); }
                const double sec = std::chrono::duration<double>(clk::now() - a).count();
                if (r < 0) continue;
                ms[vi].push_back(1e3 * sec / bsteps);
                fishSteps[vi].push_back(1e6 * sec / fs);
            }
        }
        for (int vi = 0; vi < V; ++vi) {
            std::vector<double> rat;
            for (int r = 0; r < reps; ++r) rat.push_back(ms[vi][r] / ms[0][r]);
            std::printf("bench arm=%c median_ms=%.4f min=%.4f max=%.4f us_per_fish_step=%.3f ratio_vs_A=%.4f [%.4f %.4f]\n",
                        kArms[vi].name, median(ms[vi]), *std::min_element(ms[vi].begin(), ms[vi].end()),
                        *std::max_element(ms[vi].begin(), ms[vi].end()), median(fishSteps[vi]), median(rat),
                        *std::min_element(rat.begin(), rat.end()), *std::max_element(rat.begin(), rat.end()));
        }
        return 0;
    }

    const ArmDef* arm = findArm(armArg.empty() ? 'A' : armArg[0]);
    if (!arm) { std::fprintf(stderr, "bad --arm (A, B, C or D)\n"); return 2; }
    w.ocfg.eye = arm->eye;
    w.ocfg.eyeOnly = arm->eyeOnly;
    if (!oceanConfigValid(w.ocfg)) { std::fprintf(stderr, "invalid ocean config\n"); return 4; }
    const char armName[2] = {arm->name, 0};

    std::FILE* csv = std::fopen((out + ".csv").c_str(), "w");
    if (!csv) { std::fprintf(stderr, "cannot write %s.csv\n", out.c_str()); return 3; }
    std::fprintf(csv,
                 "arm,seed,step,pop,pop_mean,births,births_int,reseeds,reseeds_int,deaths_pred_int,deaths_starve_int,"
                 "deaths_age_int,food_eaten_int,food,food_mean,pred,pred_mean,pred_births,pred_births_int,pred_reseeds,"
                 "pred_starved,eat_per_fish_s,predation_per_fish_s,births_per_s,reseeds_per_1k,kills_per_pred_s,"
                 "clearance,clearance_norm,speed_mean,reserves_mean,depth_mean,depth_sd,genome_len,ms_per_step,"
                 "us_per_fish_step,eye_any,eye_direct,eye_conns,eye_absw,eye_share,eye_vert,food_matched,pred_move,"
                 "lab_move,rnd_eye_any,rnd_eye_direct,rnd_eye_conns,rnd_eye_absw,rnd_eye_share,rnd_eye_vert,"
                 "rnd_food_matched,rnd_pred_move,rnd_lab_move\n");
    std::printf("arm %c (eye %d, eyeOnly %d) seed %u steps %ld cfg %s\n", arm->name, arm->eye, arm->eyeOnly, seed,
                steps, cfgJson ? cfgJson : "{}");

    RandomGen wiringRng;
    wiringRng.seed(seed * 2654435761u + 11u);
    const double volume = (double)W * H * depth;
    const double pickup = w.cfg.pickupRadius;
    long long lastBirths = 0, lastReseeds = 0, lastPred = 0, lastStarve = 0, lastAge = 0, lastEaten = 0,
              lastPredBirths = 0;
    double sumPop = 0, sumFood = 0, sumPreds = 0, fishSteps = 0;
    auto tInt = clk::now();
    const auto tRun = clk::now();
    for (long s = 1; s <= steps; ++s) {
        fishSteps += (double)w.creatures.size();
        w.update(kDt);
        sumPop += (double)w.creatures.size();
        sumFood += (double)w.food.size();
        sumPreds += (double)w.predators.size();
        if (s % every != 0 && s != steps) continue;

        const double secsWall = std::chrono::duration<double>(clk::now() - tInt).count();
        const long n = (s % every == 0) ? every : (s % every);
        const double simSecs = n * (double)kDt;
        const double popMean = sumPop / n, foodMean = sumFood / n, predMean = sumPreds / n;
        const long long births = w.totalBirths - lastBirths, reseeds = w.totalReseeds - lastReseeds,
                        dPred = w.totalDeathsPredation - lastPred, dStarve = w.totalDeathsStarvation - lastStarve,
                        dAge = w.totalDeathsAge - lastAge, eaten = w.totalFoodEaten - lastEaten,
                        pBirths = w.totalPredatorBirths - lastPredBirths;
        double resv = 0, zs = 0, zs2 = 0, spd = 0, glen = 0;
        for (const Fish& f : w.creatures) {
            resv += f.foodReserves; zs += f.depth; zs2 += (double)f.depth * f.depth; spd += f.speed;
            glen += (double)f.genome.size();
        }
        const double np = w.creatures.empty() ? 1.0 : (double)w.creatures.size();
        const double zm = zs / np, zsd = std::sqrt(std::max(0.0, zs2 / np - zm * zm));
        const double eatPerFish = popMean > 0 ? eaten / (popMean * simSecs) : 0.0;
        const double density = foodMean / volume;
        const double clearance = density > 0 ? eatPerFish / density : 0.0;          // volume searched / s
        const double sweep = 3.14159265 * pickup * pickup * (spd / np);            // a straight swimmer's
        Wiring we, wr;
        scoreWiring(w, wiringRng, we, wr);
        const double fe = we.fish > 0 ? we.fish : 1, fr = wr.fish > 0 ? wr.fish : 1;
        std::fprintf(csv,
                     "%c,%u,%ld,%zu,%.2f,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%zu,%.1f,%zu,%.3f,%lld,%lld,%lld,%lld,"
                     "%.6f,%.6f,%.4f,%.4f,%.5f,%.1f,%.4f,%.2f,%.3f,%.2f,%.2f,%.2f,%.4f,%.4f,"
                     "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n",
                     arm->name, seed, s, w.creatures.size(), popMean, w.totalBirths, births, w.totalReseeds, reseeds,
                     dPred, dStarve, dAge, eaten, w.food.size(), foodMean, w.predators.size(), predMean,
                     w.totalPredatorBirths, pBirths, w.totalPredatorReseeds, w.totalPredatorDeathsStarvation,
                     eatPerFish, popMean > 0 ? dPred / (popMean * simSecs) : 0.0, births / simSecs,
                     1000.0 * reseeds / n, predMean > 0 ? dPred / (predMean * simSecs) : 0.0, clearance,
                     sweep > 0 ? clearance / sweep : 0.0, spd / np, resv / np, zm, zsd, glen / np,
                     1e3 * secsWall / n, fishSteps > 0 ? 1e6 * secsWall / fishSteps : 0.0,
                     we.eyeAny / fe, we.eyeDirect / fe, we.eyeConns / fe, we.eyeAbsW / fe,
                     we.sensorConns > 0 ? we.eyeShareNum / we.sensorConns : 0.0, we.eyeVertDirect / fe,
                     we.foodMatched / fe, we.predMove / fe, we.labMove / fe,
                     wr.eyeAny / fr, wr.eyeDirect / fr, wr.eyeConns / fr, wr.eyeAbsW / fr,
                     wr.sensorConns > 0 ? wr.eyeShareNum / wr.sensorConns : 0.0, wr.eyeVertDirect / fr,
                     wr.foodMatched / fr, wr.predMove / fr, wr.labMove / fr);
        std::fflush(csv);
        std::printf("arm %c seed %u step %ld pop %zu births %lld reseeds %lld pred %zu eat/fish/s %.3f pred/fish/s %.4f"
                    " clr_norm %.2f %.3f ms/step\n",
                    arm->name, seed, s, w.creatures.size(), w.totalBirths, w.totalReseeds, w.predators.size(),
                    eatPerFish, popMean > 0 ? dPred / (popMean * simSecs) : 0.0, sweep > 0 ? clearance / sweep : 0.0,
                    1e3 * secsWall / n);
        std::fflush(stdout);
        lastBirths = w.totalBirths; lastReseeds = w.totalReseeds; lastPred = w.totalDeathsPredation;
        lastStarve = w.totalDeathsStarvation; lastAge = w.totalDeathsAge; lastEaten = w.totalFoodEaten;
        lastPredBirths = w.totalPredatorBirths;
        sumPop = sumFood = sumPreds = fishSteps = 0;
        tInt = clk::now();
    }
    std::fclose(csv);
    std::printf("run done in %.1f s\n", std::chrono::duration<double>(clk::now() - tRun).count());
    if (behav) {
        const auto tb = clk::now();
        behaviouralTest(w, armName, seed, out + "_behav.csv");
        std::printf("behavioural test in %.1f s\n", std::chrono::duration<double>(clk::now() - tb).count());
    }
    return 0;
}
