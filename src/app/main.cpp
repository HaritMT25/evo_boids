#include <iostream>
#include <iomanip>
#include <chrono>
#include <string>
#include <cstdlib>
#include <memory>
#include <algorithm>
#include <fstream>
#include <filesystem>

#include "core/world.hpp"
#include "core/stats.hpp"
#include "core/cluster_map_io.hpp"
#include "core/wiring_log.hpp"
#include "core/checkpoint_io.hpp"
#include "render/sfml_renderer.hpp"  // the only translation unit pulling SFML

// Set by CMake (project version). Falls back to "dev" for ad-hoc builds.
#ifndef EVO_VERSION
#define EVO_VERSION "dev"
#endif

namespace {

void printUsage(const char* prog) {
    std::cout <<
        "Usage: " << prog << " [options]\n\n"
        "Options:\n"
        "  --headless N      Run N simulation steps with no window, THEN open the\n"
        "                    window so you can watch the evolved population.\n"
        "  --no-render       Exit after the headless run instead of opening a window.\n"
        "                    Use on a display-less box, or when piping output to a file.\n"
        "  --csv PATH        Write per-interval metrics to PATH as CSV (headless only).\n"
        "  --name NAME       Name the run: logs to runs/NAME.csv (dir auto-created).\n"
        "                    Ignored if --csv is also given.\n"
        "  --log-every N     Sample/print interval in steps (default 1000).\n"
        "  --flush-every N   Flush the CSV to disk every N logged rows (default 5).\n"
        "  --seed S          Seed the RNG for repeatable runs (see note below).\n"
        "  --version         Print version + build time, then exit.\n"
        "  --help, -h        Show this message.\n\n"
        "Simulation parameters (defaults reproduce the original behaviour):\n"
        "  --toroidal            wrap-around senses: neighbour/food/predator perception,\n"
        "                        pheromones and the long probe cross the world seam, and\n"
        "                        the boundary sensors read no-wall (default off; positions\n"
        "                        have always wrapped — this makes the SENSES wrap too)\n"
        "  --population N        starting creature count (default 300)\n"
        "  --min-pop N           reseed floor (default 50)\n"
        "  --max-pop N           hard birth cap, 0 = unlimited (default 0)\n"
        "  --target-food N       baseline food count (default 600)\n"
        "  --food-spawn F        mean food spawned/step (default 3)\n"
        "  --food-amp F          food abundance swing (default 2)\n"
        "  --food-energy F       energy per food eaten (default 5)\n"
        "  --pickup-radius F     food pickup distance (default 10)\n"
        "  --metabolism F        energy burned/sec (default 1.5)\n"
        "  --gene-cost F         extra energy/sec per gene, parsimony pressure (default 0)\n"
        "  --move-cost F         kinetic cost: extra energy/sec = F * speed^2, 0=off (default 0)\n"
        "  --max-age F           death age (default 120)\n"
        "  --repro-energy F      energy needed to breed (default 20)\n"
        "  --newborn-energy F    child starting energy (default 25)\n"
        "  --perception F        sensing radius / grid cell (default 80)\n"
        "  --interact-radius F   social range (default 20)\n"
        "  --max-neighbors N     attend to only the N nearest neighbours in range,\n"
        "                        0=all (default 0 = metric interaction)\n"
        "  --topological         shorthand for --max-neighbors 7 (Ballerini et al. 2008:\n"
        "                        starlings track ~6-7 nearest; fish studies suggest fewer)\n"
        "  --repro-cooldown F    (default 3)   --interact-cooldown F (default 2)\n"
        "  --min-speed F         (default 30)  --max-speed F (default 150)\n"
        "  --triangular-motion   couple speed+turn: fast=>can't turn, slow=>turns freely (default off)\n"
        "  --max-turn-rate F     [triangular] max heading change in rad/s at zero forward drive (default 4.0)\n"
        "  --genome-min N        (default 16)  --genome-max N (default 80)\n"
        "  --genome-init-min N   (default 24)  --genome-init-max N (default 48)\n"
        "  --mutation F          mutation-rate multiplier (default 1.0)\n"
        "  --current F           ocean-current peak speed, 0=off (default 0)\n"
        "  --current-mode M      'eddies' (moving vortices) or 'gyre' (double-gyre) (default eddies)\n"
        "  --current-eddies N    [eddies] number of vortices / 'eyes' (default 5)\n"
        "  --current-core F      [eddies] eddy radius (default 150)\n"
        "  --current-drift F     how fast the field evolves, 0=static (default 1.0)\n"
        "  --gyre-eps F          [gyre] dividing-line oscillation amount (default 0.25)\n"
        "  --food-patches N      cluster food into N patches, 0=uniform (default 0)\n"
        "  --food-patch-radius F spread of food around a patch (default 120)\n"
        "  --food-drift          advect food with the current (forms filaments)\n"
        "Predator (Stage A: scripted, immortal, fixed-count; off by default):\n"
        "  --predators N             number of predators, 0=none (default 0)\n"
        "  --predator-perception F   predator prey-detection radius (default 150; >--perception => ambush asymmetry)\n"
        "  --predator-speed F        predator cruise speed (default 110)\n"
        "  --predator-capture-radius F  contact distance that catches a prey (default 14)\n"
        "  --predator-cooldown F     digestion cooldown in seconds after a kill (default 3)\n"
        "Predator energy economy (Stage B: predators starve/reproduce; opt-in):\n"
        "  --predator-economy        turn the economy ON (default off => Stage A immortal)\n"
        "  --predator-metabolism F   predator energy burn/sec (default 2.0)\n"
        "  --predator-move-cost F    predator kinetic cost coeff; <0 reuses --move-cost (default reuse)\n"
        "  --predator-repro-energy F energy needed to bud a child (default 60)\n"
        "  --predator-newborn-energy F  child / initial predator energy (default 30)\n"
        "  --predator-meal F         energy per kill; <0 absorbs the victim's reserves (default absorb)\n"
        "  --predator-max-age F      predator death age; 0=immortal-until-starved (default 0)\n\n"
        "Analysis / inspection:\n"
        "  --cluster-map             at end of run, write <name>.deaths.csv + <name>.cluster.svg\n"
        "                            (a 2-D projection of each dead creature's network)\n"
        "  --cluster-epoch N         deaths per colour band in the map (default 1000)\n"
        "  --cluster-max N           max points kept in RAM; uniformly decimated above this (default 20000)\n"
        "  --cluster-pca             use PCA axes (per-run, variance-aligned) instead of the fixed random map\n"
        "  --cluster-color M         'epoch' (time, default) or 'lineage' (colour points by clade)\n"
        "  --wiring-log              write <name>.wiring.csv: per-sensor wiring fraction over time (headless)\n"
        "  --profile                 print a build/parallel/serial phase timing breakdown at end (headless)\n"
        "  --fast-tanh               approximate the neural activations (~1.2-1.3x faster, CHANGES\n"
        "                            results; exploratory sweeps only — see config.hpp)\n"
        "  --save-checkpoint FILE    write the full world state to FILE (JSON) at end of run\n"
        "  --load-checkpoint FILE    resume from FILE instead of starting fresh. In a serial\n"
        "                            build the continuation is bit-for-bit identical. Edit the\n"
        "                            'config' block in the file to change settings mid-run.\n"
        "  --checkpoint-every N      also write <name>.<frame>.json every N steps\n"
        "  (In the live window: LEFT-CLICK a fish to watch its neural net fire in a top-right panel.)\n\n"
        "Log to CSV and then watch the result (omit --no-render):\n"
        "  " << prog << " --headless 50000 --name evolved_50k --log-every 2000\n\n"
        "Log to CSV on a headless box and capture the console too:\n"
        "  " << prog << " --headless 50000 --name evolved_50k --no-render | tee runs/evolved_50k.log\n\n"
        "Note: in an OpenMP build, --seed makes the single-threaded parts (spawning,\n"
        "interactions, deaths, init) repeatable; the parallel sensor/movement phase\n"
        "uses per-thread RNGs. For a fully deterministic run, build the serial target.\n";
}

}  // namespace

int main(int argc, char** argv) {
    // Not const: resuming from a checkpoint adopts that world's dimensions.
    int W = 1920;
    int H = 1080;

    int         headlessSteps = 0;
    int         logEvery      = 1000;
    int         flushEvery    = 5;     // flush CSV to disk every N logged rows
    bool        noRender      = false;
    bool        haveSeed      = false;
    unsigned    seed          = 0;
    std::string csvPath;
    std::string runName;
    std::string saveCheckpoint;    // --save-checkpoint FILE (written at end of run)
    std::string loadCheckpoint;    // --load-checkpoint FILE (resume)
    int         checkpointEvery = 0;   // --checkpoint-every N (0 = off)
    std::string checkpointBase;        // prefix for the periodic files

    int       initialPopulation = 300;  // starting creature count
    SimConfig cfg;                       // all other tunable parameters

    // Parse args. next() consumes the following token as this flag's value.
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << name << " requires a value\n";
                std::exit(1);
            }
            return argv[++i];
        };

        if      (arg == "--headless")  headlessSteps = std::atoi(next("--headless"));
        else if (arg == "--log-every") logEvery      = std::max(1, std::atoi(next("--log-every")));
        else if (arg == "--flush-every") flushEvery   = std::max(1, std::atoi(next("--flush-every")));
        else if (arg == "--csv")       csvPath       = next("--csv");
        else if (arg == "--name")      runName       = next("--name");
        else if (arg == "--seed")    { seed = static_cast<unsigned>(std::strtoul(next("--seed"), nullptr, 10)); haveSeed = true; }
        else if (arg == "--no-render") noRender      = true;
        // --- simulation parameters (override SimConfig defaults) ---
        else if (arg == "--population")       initialPopulation     = std::atoi(next("--population"));
        else if (arg == "--min-pop")          cfg.minPopulation     = std::atoi(next("--min-pop"));
        else if (arg == "--max-pop")          cfg.maxPopulation     = std::atoi(next("--max-pop"));
        else if (arg == "--target-food")      cfg.targetFood        = std::atoi(next("--target-food"));
        else if (arg == "--food-spawn")       cfg.foodSpawnBase     = std::atof(next("--food-spawn"));
        else if (arg == "--food-amp")         cfg.foodSpawnAmp      = std::atof(next("--food-amp"));
        else if (arg == "--food-energy")      cfg.foodEnergy        = std::atof(next("--food-energy"));
        else if (arg == "--pickup-radius")    cfg.pickupRadius      = std::atof(next("--pickup-radius"));
        else if (arg == "--metabolism")       cfg.metabolicRate     = std::atof(next("--metabolism"));
        else if (arg == "--gene-cost")         cfg.geneCost          = std::atof(next("--gene-cost"));
        else if (arg == "--move-cost")         cfg.moveCost          = std::atof(next("--move-cost"));
        else if (arg == "--max-age")          cfg.maxAge            = std::atof(next("--max-age"));
        else if (arg == "--repro-energy")     cfg.reproEnergy       = std::atof(next("--repro-energy"));
        else if (arg == "--newborn-energy")   cfg.newbornEnergy     = std::atof(next("--newborn-energy"));
        else if (arg == "--perception")       cfg.perceptionRadius  = std::atof(next("--perception"));
        else if (arg == "--interact-radius")  cfg.interactRadius    = std::atof(next("--interact-radius"));
        else if (arg == "--save-checkpoint")  saveCheckpoint        = next("--save-checkpoint");
        else if (arg == "--load-checkpoint")  loadCheckpoint        = next("--load-checkpoint");
        else if (arg == "--checkpoint-every") checkpointEvery       = std::atoi(next("--checkpoint-every"));
        else if (arg == "--max-neighbors")    cfg.maxNeighbors      = std::atoi(next("--max-neighbors"));
        else if (arg == "--topological")      cfg.maxNeighbors      = 7;
        else if (arg == "--fast-tanh")        cfg.fastTanh          = true;
        else if (arg == "--repro-cooldown")   cfg.reproCooldown     = std::atof(next("--repro-cooldown"));
        else if (arg == "--interact-cooldown")cfg.interactCooldown  = std::atof(next("--interact-cooldown"));
        else if (arg == "--min-speed")        cfg.minSpeed          = std::atof(next("--min-speed"));
        else if (arg == "--max-speed")        cfg.maxSpeed          = std::atof(next("--max-speed"));
        else if (arg == "--toroidal")         cfg.toroidal          = true;
        else if (arg == "--triangular-motion") cfg.triangularMotion = true;
        else if (arg == "--max-turn-rate")    cfg.maxTurnRate       = std::atof(next("--max-turn-rate"));
        else if (arg == "--genome-min")       cfg.genomeMinLength   = std::atoi(next("--genome-min"));
        else if (arg == "--genome-max")       cfg.genomeMaxLength   = std::atoi(next("--genome-max"));
        else if (arg == "--genome-init-min")  cfg.genomeInitMin     = std::atoi(next("--genome-init-min"));
        else if (arg == "--genome-init-max")  cfg.genomeInitMax     = std::atoi(next("--genome-init-max"));
        else if (arg == "--mutation")         cfg.mutationMultiplier= std::atof(next("--mutation"));
        // --- currents / spatial food (environment) ---
        else if (arg == "--current")           cfg.currentStrength   = std::atof(next("--current"));
        else if (arg == "--current-mode")    { std::string m = next("--current-mode"); cfg.currentMode = (m == "gyre") ? 1 : 0; }
        else if (arg == "--current-eddies")    cfg.currentEddies     = std::atoi(next("--current-eddies"));
        else if (arg == "--current-core")      cfg.currentCore       = std::atof(next("--current-core"));
        else if (arg == "--current-drift")     cfg.currentDrift      = std::atof(next("--current-drift"));
        else if (arg == "--gyre-eps")          cfg.currentGyreEps    = std::atof(next("--gyre-eps"));
        else if (arg == "--food-patches")      cfg.foodPatches       = std::atoi(next("--food-patches"));
        else if (arg == "--food-patch-radius") cfg.foodPatchRadius   = std::atof(next("--food-patch-radius"));
        else if (arg == "--food-drift")        cfg.foodDrift         = true;
        // --- predator (Stage A) ---
        else if (arg == "--predators")              cfg.predatorCount       = std::atoi(next("--predators"));
        else if (arg == "--predator-perception")    cfg.predatorPerception  = std::atof(next("--predator-perception"));
        else if (arg == "--predator-speed")         cfg.predatorSpeed       = std::atof(next("--predator-speed"));
        else if (arg == "--predator-capture-radius")cfg.predatorCaptureRadius = std::atof(next("--predator-capture-radius"));
        else if (arg == "--predator-cooldown")      cfg.predatorCooldown    = std::atof(next("--predator-cooldown"));
        // --- predator energy economy (Stage B) ---
        else if (arg == "--predator-economy")       cfg.predatorEconomy     = true;
        else if (arg == "--predator-metabolism")    cfg.predatorMetabolism  = std::atof(next("--predator-metabolism"));
        else if (arg == "--predator-move-cost")     cfg.predatorMoveCost    = std::atof(next("--predator-move-cost"));
        else if (arg == "--predator-repro-energy")  cfg.predatorReproEnergy = std::atof(next("--predator-repro-energy"));
        else if (arg == "--predator-newborn-energy")cfg.predatorNewbornEnergy = std::atof(next("--predator-newborn-energy"));
        else if (arg == "--predator-meal")          cfg.predatorMeal        = std::atof(next("--predator-meal"));
        else if (arg == "--predator-max-age")       cfg.predatorMaxAge      = std::atof(next("--predator-max-age"));
        else if (arg == "--cluster-map")             cfg.clusterMap          = true;
        else if (arg == "--cluster-max")             cfg.clusterMax          = std::atoi(next("--cluster-max"));
        else if (arg == "--cluster-epoch")           cfg.clusterEpoch        = std::max(1, std::atoi(next("--cluster-epoch")));
        else if (arg == "--cluster-pca")             cfg.clusterPca          = true;
        else if (arg == "--cluster-color")         { std::string m = next("--cluster-color"); cfg.clusterColor = (m == "lineage") ? 1 : 0; }
        else if (arg == "--wiring-log")              cfg.wiringLog           = true;
        else if (arg == "--profile")                 cfg.profile             = true;
        else if (arg == "--version") {
            std::cout << "evo_boids " << EVO_VERSION
                      << " (built " << __DATE__ << " " << __TIME__ << ")\n";
            return 0;
        }
        else if (arg == "--help" || arg == "-h") { printUsage(argv[0]); return 0; }
        else {
            std::cerr << "Unknown option: " << arg << "\n\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    if (haveSeed) rng.seed(seed);

    // A named run logs to runs/<name>.csv unless an explicit --csv path is given.
    if (csvPath.empty() && !runName.empty()) {
        csvPath = "runs/" + runName + ".csv";
    }

    // Base path for the death-cluster map outputs (<stem>.deaths.csv / .cluster.svg).
    std::string clusterStem;
    if (!runName.empty())      clusterStem = "runs/" + runName;
    else if (!csvPath.empty()) clusterStem = std::filesystem::path(csvPath).replace_extension("").string();
    else                       clusterStem = "runs/cluster";

    if (!csvPath.empty() && headlessSteps <= 0) {
        std::cerr << "Note: CSV logging only happens during the --headless run; "
                     "nothing will be written without --headless.\n";
    }

    // Light validation: keep ranges sane and warn on anything clamped.
    auto fixOrder = [](auto& lo, auto& hi, const char* what) {
        if (lo > hi) { std::cerr << "Note: " << what << " min > max; swapping.\n"; std::swap(lo, hi); }
    };
    fixOrder(cfg.minSpeed, cfg.maxSpeed, "speed");
    fixOrder(cfg.genomeMinLength, cfg.genomeMaxLength, "genome length");
    fixOrder(cfg.genomeInitMin, cfg.genomeInitMax, "genome init");
    if (cfg.perceptionRadius < 1.0f) { std::cerr << "Note: perception clamped to 1.\n"; cfg.perceptionRadius = 1.0f; }
    if (cfg.maxNeighbors < 0)        { std::cerr << "Note: max-neighbors < 0 treated as 0 (unlimited).\n"; cfg.maxNeighbors = 0; }
    if (checkpointEvery > 0) {
        checkpointBase = !saveCheckpoint.empty() ? saveCheckpoint
                       : (!runName.empty() ? ("runs/" + runName) : std::string("runs/checkpoint"));
        // strip a trailing .json so the periodic files read name.<frame>.json
        if (checkpointBase.size() > 5 && checkpointBase.compare(checkpointBase.size() - 5, 5, ".json") == 0)
            checkpointBase.resize(checkpointBase.size() - 5);
        std::filesystem::create_directories(std::filesystem::path(checkpointBase).parent_path());
    }
    if (!saveCheckpoint.empty()) {
        const auto parent = std::filesystem::path(saveCheckpoint).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
    }
    if (cfg.maxNeighbors > MAX_TOPOLOGICAL_NEIGHBORS) {
        std::cerr << "Note: max-neighbors clamped to " << MAX_TOPOLOGICAL_NEIGHBORS
                  << " (use 0 for unlimited/metric interaction).\n";
        cfg.maxNeighbors = MAX_TOPOLOGICAL_NEIGHBORS;
    }
    if (initialPopulation < 0)       initialPopulation = 0;

    // Init world with the chosen configuration.
    World world;
    world.cfg = cfg;

    if (!loadCheckpoint.empty()) {
        // Resuming replaces the whole world, including its config, so the flags
        // on this command line are ignored EXCEPT the ones that don't live in a
        // checkpoint (--headless, --name, logging). To change a simulation
        // setting on resume, edit the "config" block inside the checkpoint —
        // that is the supported way to intervene mid-run.
        std::string err;
        if (!checkpoint::load(loadCheckpoint, world, err)) {
            std::cerr << "!! cannot resume from '" << loadCheckpoint << "': " << err << "\n";
            return 1;
        }
        std::cout << "Resumed from " << loadCheckpoint
                  << " at frame " << world.frame
                  << " (" << world.creatures.size() << " creatures, "
                  << world.predators.size() << " predators)\n";
#ifdef USE_OPENMP
        std::cerr << "Note: this is an OpenMP build, so the resumed run will NOT reproduce\n"
                  << "      the original bit-for-bit. Build -DEVO_NO_OPENMP=ON for that.\n";
#endif
        W = world.width;
        H = world.height;
    } else {
        world.init(W, H, initialPopulation);
    }

    // --- Headless phase: pure CPU, fixed dt, no rendering ---
    if (headlessSteps > 0) {
        std::cout << "evo_boids " << EVO_VERSION
                  << " (built " << __DATE__ << " " << __TIME__ << ")\n";
        if (!runName.empty()) std::cout << "Run: " << runName << "\n";
        std::cout << "Headless mode: simulating " << headlessSteps
                  << " steps (logging every " << logEvery << ")";
        if (haveSeed) std::cout << ", seed=" << seed;
        std::cout << "...\n";
        std::cout << "Config: population=" << initialPopulation << "\n"
                  << world.cfg.dump() << "\n";

        std::ofstream csv;
        if (!csvPath.empty()) {
            const std::filesystem::path p(csvPath);
            if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
            csv.open(csvPath);
            if (!csv) {
                std::cerr << "Could not open CSV file for writing: " << csvPath << "\n";
                return 1;
            }
            csv << statsCsvHeader() << "\n";
            csv.flush();  // header on disk immediately, so `tail -f` works from t=0

            // Sidecar: record exactly what produced this CSV, so a run is
            // self-documenting (open <stem>.config.txt next to the .csv).
            std::filesystem::path meta = p;
            meta.replace_extension(".config.txt");
            if (std::ofstream m{meta}) {
                m << "evo_boids " << EVO_VERSION
                  << " (built " << __DATE__ << " " << __TIME__ << ")\n";
                if (!runName.empty()) m << "run: " << runName << "\n";
                m << "seed: " << (haveSeed ? std::to_string(seed) : std::string("random")) << "\n"
                  << "steps: " << headlessSteps << "   log_every: " << logEvery << "\n"
                  << "population: " << initialPopulation << "\n"
                  << world.cfg.dump() << "\n";
            }
        }

        const float fixedDt = 0.016f;
        const auto  startTime = std::chrono::steady_clock::now();

        // Optional per-sensor wiring log (headless, opt-in). Separate file so the
        // 31-col metrics CSV format is untouched.
        std::ofstream wiringCsv;
        if (world.cfg.wiringLog) {
            const std::string wpath = clusterStem + ".wiring.csv";
            const std::filesystem::path wp(wpath);
            if (wp.has_parent_path()) std::filesystem::create_directories(wp.parent_path());
            wiringCsv.open(wpath);
            if (wiringCsv) { wiringCsv << wiring::header() << "\n"; wiringCsv.flush(); }
        }

        auto      lastSampleTime  = startTime;
        int       lastSampleStep  = 0;
        long long lastTotalBirths = 0;
        long long lastTotalDeaths = 0;
        long long lastTotalKills  = 0;
        long long lastTotalStarv  = 0;
        long long lastTotalAge    = 0;
        long long lastPredBirths  = 0;
        long long lastPredStarv   = 0;
        long long lastPredAge     = 0;
        int       rowsWritten     = 0;

        // Take one measurement: rich console line + optional CSV row. Per-interval
        // birth/death counts and steps/sec are diffed against the previous sample.
        auto sample = [&](int stepsDone) {
            const auto   now     = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double>(now - startTime).count();
            const double dStep   = static_cast<double>(stepsDone - lastSampleStep);
            const double dSecs   = std::chrono::duration<double>(now - lastSampleTime).count();
            const double sps     = (dSecs > 1e-9) ? dStep / dSecs : 0.0;

            const WorldStats st = computeStats(world);
            const int birthsInt = static_cast<int>(st.totalBirths - lastTotalBirths);
            const int deathsInt = static_cast<int>(st.totalDeaths - lastTotalDeaths);
            // Per-interval death decomposition (cumulative totals, diffed like
            // births/deaths). By construction these partition deathsInt:
            //   deathsInt == killsInt + starvInt + ageInt.
            const int killsInt  = static_cast<int>(st.totalKills            - lastTotalKills);
            const int starvInt  = static_cast<int>(st.totalDeathsStarvation - lastTotalStarv);
            const int ageInt    = static_cast<int>(st.totalDeathsAge        - lastTotalAge);
            // Predator economy per-interval diffs (Stage B; all 0 in Stage A).
            const int pBirthsInt = static_cast<int>(st.totalPredatorBirths           - lastPredBirths);
            const int pStarvInt  = static_cast<int>(st.totalPredatorDeathsStarvation - lastPredStarv);
            const int pAgeInt    = static_cast<int>(st.totalPredatorDeathsAge         - lastPredAge);

            std::cout << "  step " << std::setw(7) << stepsDone << '/' << headlessSteps
                      << "  pop "  << std::setw(5) << st.population
                      << "  food " << std::setw(5) << st.food
                      << "  +"     << std::setw(4) << birthsInt
                      << " -"      << std::setw(4) << deathsInt
                      << std::fixed
                      << "  age "  << std::setprecision(1) << std::setw(5) << st.avgAge
                      << "  gLen " << std::setprecision(1) << std::setw(5) << st.avgGenomeLen
                      << "  div "  << std::setprecision(2) << st.geneticDiversity
                      << "  sp "   << std::setw(3) << st.species
                      << "  algn " << std::setprecision(2) << std::setw(5) << st.avgNeighborAlign;
            // Predator segment: only printed when predators are active, so the
            // default (no-predator) console output is byte-for-byte unchanged.
            if (st.predatorPop > 0) {
                std::cout << "  pred " << std::setw(3) << st.predatorPop
                          << "  kills " << std::setw(4) << killsInt
                          << "  pAlign " << std::setprecision(2) << std::setw(5) << st.avgPredatorAlign;
                // Stage B economy readout: mean predator energy + this interval's
                // predator births / deaths (the oscillator at a glance).
                if (world.cfg.predatorEconomy) {
                    std::cout << "  pE " << std::setprecision(1) << std::setw(6) << st.predatorAvgEnergy
                              << "  p+" << std::setw(3) << pBirthsInt
                              << " p-" << std::setw(3) << (pStarvInt + pAgeInt);
                }
            }
            std::cout << "  ("     << std::setprecision(1) << elapsed << "s, "
                      << static_cast<long>(sps) << "/s)\n";

            if (csv) {
                csv << statsCsvRow(st, elapsed, sps, birthsInt, deathsInt,
                                   killsInt, starvInt, ageInt,
                                   pBirthsInt, pStarvInt, pAgeInt) << "\n";
                ++rowsWritten;
                // Periodic flush: keeps the on-disk file at most `flushEvery`
                // rows behind, so it is readable mid-run and a Ctrl-C loses at
                // most that many rows instead of the whole buffer.
                if (rowsWritten % flushEvery == 0) csv.flush();
            }

            if (wiringCsv) {
                float wf[NUM_SENSES]; wiring::fractions(world, wf);
                wiringCsv << stepsDone;
                for (int s = 0; s < NUM_SENSES; ++s) wiringCsv << ',' << wf[s];
                wiringCsv << "\n";
                if (rowsWritten % flushEvery == 0) wiringCsv.flush();
            }

            lastSampleTime  = now;
            lastSampleStep  = stepsDone;
            lastTotalBirths = st.totalBirths;
            lastTotalDeaths = st.totalDeaths;
            lastTotalKills  = st.totalKills;
            lastTotalStarv  = st.totalDeathsStarvation;
            lastTotalAge    = st.totalDeathsAge;
            lastPredBirths  = st.totalPredatorBirths;
            lastPredStarv   = st.totalPredatorDeathsStarvation;
            lastPredAge     = st.totalPredatorDeathsAge;
        };

        // Periodic checkpoints are written to <name>.<frame>.json so a long run
        // leaves a trail you can rewind to, rather than a single end state.
        auto writeCheckpoint = [&](const std::string& path) {
            std::string err;
            if (!checkpoint::save(world, path, EVO_VERSION, err)) {
                std::cerr << "!! checkpoint failed: " << err << "\n";
                return false;
            }
            std::cout << "  checkpoint -> " << path << " (frame " << world.frame << ")\n";
            return true;
        };

        for (int i = 0; i < headlessSteps; ++i) {
            world.update(fixedDt);
            const int stepsDone = i + 1;
            if (stepsDone % logEvery == 0) sample(stepsDone);
            if (checkpointEvery > 0 && stepsDone % checkpointEvery == 0) {
                writeCheckpoint(checkpointBase + "." + std::to_string(world.frame) + ".json");
            }
        }
        // Always capture the final state, even if it isn't on an interval boundary.
        if (headlessSteps % logEvery != 0) sample(headlessSteps);

        if (!saveCheckpoint.empty() && !writeCheckpoint(saveCheckpoint)) return 1;

        const auto   endTime = std::chrono::steady_clock::now();
        const double total   = std::chrono::duration<double>(endTime - startTime).count();
        std::cout << "Headless done. " << headlessSteps << " steps in "
                  << std::fixed << std::setprecision(1) << total << "s ("
                  << static_cast<long>(headlessSteps / std::max(1e-9, total)) << " steps/sec)\n";
        if (csv) {
            csv.close();
            std::cout << "Wrote " << rowsWritten << " rows to " << csvPath << "\n";
        }
        if (wiringCsv) {
            wiringCsv.close();
            std::cout << "Wrote sensor-wiring log to " << clusterStem << ".wiring.csv\n";
        }

        if (noRender) {
            if (world.cfg.profile) world.dumpProfile();
            if (world.cfg.clusterMap) {
                auto [cpath, spath] = clustermap::write(world, clusterStem);
                if (!spath.empty())
                    std::cout << "Wrote death-cluster map: " << spath
                              << " (+ " << cpath << ", " << world.deathLog.size()
                              << " of " << world.totalDeathsLogged_ << " deaths)\n";
            }
            return 0;
        }
        std::cout << "Opening window...\n";
    }

    // The live neural inspector captures per-step activations; only needed now that
    // a window is opening (headless paid nothing for it).
    world.cfg.captureActivations = true;

    // --- Visual phase: drive the simulation through the renderer interface ---
    std::unique_ptr<IRenderer> renderer =
        std::make_unique<SfmlRenderer>(W, H, "evo_boids — evolved flocking");

    bool paused = false;
    int simSpeed = 1;  // how many sim steps per rendered frame

    // FPS counter (SFML-free, wall-clock based)
    int fps = 0;
    int frameCount = 0;
    auto fpsClock = std::chrono::steady_clock::now();

    while (renderer->isOpen()) {
        InputEvents ev = renderer->poll();
        if (ev.quit) break;
        if (ev.togglePause) paused = !paused;
        if (ev.speedUp)   simSpeed = std::min(simSpeed + 1, 50);
        if (ev.speedDown) simSpeed = std::max(simSpeed - 1, 1);

        float dt = renderer->frameDelta();
        dt = std::min(dt, 0.033f);

        // Simulate multiple steps per rendered frame.
        // Fast-forward: simulate N real ticks, render once.
        if (!paused) {
            for (int s = 0; s < simSpeed; ++s) {
                world.update(dt);
            }
        }

        // FPS counter
        frameCount++;
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<float>(now - fpsClock).count() >= 1.0f) {
            fps = frameCount;
            frameCount = 0;
            fpsClock = now;
        }

        // Assemble HUD data from the simulation state
        float avgGenome = 0;
        for (const auto& c : world.creatures) avgGenome += c.genome.size();
        if (!world.creatures.empty()) avgGenome /= world.creatures.size();

        HudInfo hud;
        hud.population = world.creatures.size();
        hud.food       = world.food.size();
        hud.predators  = world.predators.size();
        hud.frame      = world.frame;
        hud.births     = world.births;
        hud.deaths     = world.deaths;
        hud.avgGenome  = avgGenome;
        hud.simSpeed   = simSpeed;
        hud.fps        = fps;
        hud.paused     = paused;

        renderer->render(world, hud);
    }

    return 0;
}
