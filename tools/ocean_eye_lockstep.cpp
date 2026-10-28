// ocean_eye_lockstep.cpp — native host for tools/ocean_eye_block.cpp: loads
// one or more builds as shared objects and times several variants of the same
// world in LOCKSTEP (each runs --chunk steps in turn, the order rotating every
// slice), so all of them see the same CPU clock. Each rep restarts every
// variant from its snapshot; round -1 is a discarded warm-up.
//
//   ocean_eye_lockstep --blocks plain.so,eye.so --variants 1:0,1:1,1:0
//       --pop 3000 --reps 7 --warm 5 --steps 100 --chunk 1
//
// A variant is BLOCK:EYE. Variants on the same block are clones of one
// snapshot (blk_clone; every variant is a clone, none runs the base world),
// so 1:0,1:1,1:0 is eye off / on / off on the same world: the A/B and an A/A
// control. Variants on different blocks each build (and evolve) their own
// world with the same seed.
// Options: --seed S --pop N --w W --h H --preset P --evolve E --evolve-eye 0|1
//          --reps R --warm K --steps M --chunk C --label TEXT
#include <dlfcn.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct Block {
    void* so = nullptr;
    int (*has_eye)() = nullptr;
    int (*has_prof)() = nullptr;
    int (*nw)(unsigned, int, int, int, int, int, int) = nullptr;
    int (*clone)(int) = nullptr;
    void (*reset)(int, int) = nullptr;
    double (*run)(int, int) = nullptr;
    double (*fish)(int) = nullptr;
    void (*clear)(int) = nullptr;
    double (*prof)(int, int) = nullptr;
};

template <class F>
static bool sym(void* so, const char* name, F& f) {
    f = reinterpret_cast<F>(dlsym(so, name));
    if (!f) std::fprintf(stderr, "missing %s\n", name);
    return f != nullptr;
}

static double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n == 0 ? 0.0 : (n % 2) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

static std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    size_t a = 0;
    while (a <= s.size()) {
        size_t b = s.find(',', a);
        if (b == std::string::npos) b = s.size();
        out.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    return out;
}

int main(int argc, char** argv) {
    std::string blocksArg, variantsArg = "0:0", label;
    unsigned seed = 42;
    int pop = 300, W = 1920, H = 1080, preset = 0, evolve = 0, evolveEye = 1;
    int reps = 7, warm = 10, steps = 100, chunk = 1;
    for (int i = 1; i < argc; ++i) {
        auto is = [&](const char* k) { return std::strcmp(argv[i], k) == 0 && i + 1 < argc; };
        if (is("--blocks")) blocksArg = argv[++i];
        else if (is("--variants")) variantsArg = argv[++i];
        else if (is("--label")) label = argv[++i];
        else if (is("--seed")) seed = (unsigned)std::strtoul(argv[++i], nullptr, 10);
        else if (is("--pop")) pop = std::atoi(argv[++i]);
        else if (is("--w")) W = std::atoi(argv[++i]);
        else if (is("--h")) H = std::atoi(argv[++i]);
        else if (is("--preset")) preset = std::atoi(argv[++i]);
        else if (is("--evolve")) evolve = std::atoi(argv[++i]);
        else if (is("--evolve-eye")) evolveEye = std::atoi(argv[++i]);
        else if (is("--reps")) reps = std::atoi(argv[++i]);
        else if (is("--warm")) warm = std::atoi(argv[++i]);
        else if (is("--steps")) steps = std::atoi(argv[++i]);
        else if (is("--chunk")) chunk = std::atoi(argv[++i]);
        else { std::fprintf(stderr, "unknown or incomplete argument: %s\n", argv[i]); return 2; }
    }
    if (chunk < 1 || steps % chunk) { std::fprintf(stderr, "--steps must be a multiple of --chunk\n"); return 2; }

    std::vector<Block> blocks;
    for (const std::string& path : split(blocksArg)) {
        Block b;
        b.so = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!b.so) { std::fprintf(stderr, "dlopen %s: %s\n", path.c_str(), dlerror()); return 3; }
        bool ok = sym(b.so, "blk_has_eye", b.has_eye) & sym(b.so, "blk_has_prof", b.has_prof) &
                  sym(b.so, "blk_new", b.nw) & sym(b.so, "blk_clone", b.clone) &
                  sym(b.so, "blk_reset", b.reset) & sym(b.so, "blk_run", b.run) &
                  sym(b.so, "blk_fish_steps", b.fish) & sym(b.so, "blk_clear_counts", b.clear) &
                  sym(b.so, "blk_prof", b.prof);
        if (!ok) return 3;
        blocks.push_back(b);
    }
    struct Var { int block, eye, h; };
    std::vector<Var> vars;
    std::vector<int> base(blocks.size(), -1);
    for (const std::string& v : split(variantsArg)) {
        Var x{};
        if (std::sscanf(v.c_str(), "%d:%d", &x.block, &x.eye) != 2 || x.block < 0 ||
            x.block >= (int)blocks.size()) { std::fprintf(stderr, "bad variant %s\n", v.c_str()); return 2; }
        if (x.eye && !blocks[x.block].has_eye()) { std::fprintf(stderr, "block %d has no eye\n", x.block); return 2; }
        Block& b = blocks[x.block];
        if (base[x.block] < 0) {
            base[x.block] = b.nw(seed, pop, W, H, preset, evolve, evolveEye);
            if (base[x.block] < 0) return 4;
        }
        x.h = b.clone(base[x.block]);
        vars.push_back(x);
    }
    const int V = (int)vars.size();
    std::vector<std::vector<double>> ms(V), popm(V);
    std::vector<std::vector<double>> prof(V, std::vector<double>(9, 0.0));

    for (int r = -1; r < reps; ++r) {
        for (int j = 0; j < V; ++j) {
            const Var& x = vars[(r + 1 + j) % V];
            blocks[x.block].reset(x.h, x.eye);
            if (warm > 0) blocks[x.block].run(x.h, warm);
            blocks[x.block].clear(x.h);
        }
        std::vector<double> t(V, 0.0);
        for (int k = 0; k < steps / chunk; ++k)
            for (int j = 0; j < V; ++j) {
                const int vi = (k + j) % V;
                t[vi] += blocks[vars[vi].block].run(vars[vi].h, chunk);
            }
        if (r < 0) continue;
        std::printf("rep %d:", r);
        for (int vi = 0; vi < V; ++vi) {
            const Var& x = vars[vi];
            ms[vi].push_back(1e3 * t[vi] / steps);
            popm[vi].push_back(blocks[x.block].fish(x.h) / steps);
            for (int k = 0; k < 9; ++k) prof[vi][k] += blocks[x.block].prof(x.h, k);
            std::printf(" v%d %.4f ms", vi, ms[vi].back());
        }
        std::printf("\n");
        std::fflush(stdout);
    }

    std::printf("summary label=%s pop0=%d world=%dx%d preset=%d evolve=%d steps=%d chunk=%d reps=%d\n",
                label.c_str(), pop, W, H, preset, evolve, steps, chunk, reps);
    for (int vi = 0; vi < V; ++vi) {
        const auto& m = ms[vi];
        const double med = median(m), lo = *std::min_element(m.begin(), m.end()),
                     hi = *std::max_element(m.begin(), m.end()), pm = median(popm[vi]);
        std::printf("  v%d block=%d(%s) eye=%d median_ms=%.4f min=%.4f max=%.4f spread=%.1f%% pop=%.0f us_per_fish_step=%.3f\n",
                    vi, vars[vi].block, blocks[vars[vi].block].has_eye() ? "eye" : "plain", vars[vi].eye, med,
                    lo, hi, med > 0 ? 100.0 * (hi - lo) / med : 0.0, pm, pm > 0 ? 1e3 * med / pm : 0.0);
        if (blocks[vars[vi].block].has_prof()) {
            const auto& P = prof[vi];
            const double fs = P[8] > 0 ? P[8] : 1.0;
            std::printf("    prof: build %.1f%% fish-phase %.1f%% serial %.1f%% | fish phase: sensors %.1f%% ff %.1f%%"
                        " move+metab %.1f%% | ns/fish-step: sensors %.0f ff %.0f move %.0f total %.0f\n",
                        100 * P[0] / P[3], 100 * P[1] / P[3], 100 * P[2] / P[3], 100 * P[4] / P[1],
                        100 * P[5] / P[1], 100 * P[6] / P[1], P[4] / fs, P[5] / fs, P[6] / fs, P[3] / fs);
        }
    }
    for (int vi = 1; vi < V; ++vi) {
        std::vector<double> rat;
        for (int r = 0; r < reps; ++r) rat.push_back(ms[vi][r] / ms[0][r]);
        std::printf("  ratio v%d/v0: median %.4f [min %.4f max %.4f]\n", vi, median(rat),
                    *std::min_element(rat.begin(), rat.end()), *std::max_element(rat.begin(), rat.end()));
    }
    return 0;
}
