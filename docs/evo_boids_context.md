# evo_boids — project context & handoff

Paste this into a new chat to restore context. It captures the project state, what
changed and why, empirical results, and the design decisions/reasoning so we don't
re-litigate them. Companion file: `evo_boids_flags.md` (full CLI reference).

> **Reading note (updated this session — the "Stage B session"):** the predator
> **energy economy (Stage B) is now BUILT and verified** (v0.10.0). Sections 2, 5, 6,
> 7, 9, 11 are updated to reflect that; the new **§12** is this session's move-by-move
> log (what was built, the decisions, and the tuning characterization). The previous
> sessions remain authoritative: currents are closed (§3), Stage A predator schooling
> is done-but-stochastic (§10). **Headline of this session:** Stage B mechanics are
> correct and conservative, and the three economy regimes are characterized
> (predators-explode / coexist-at-floor / predators-extinct), but **the sustained
> oscillator is a real-run + replicates tuning task** (sandbox runs are short and
> OpenMP-nondeterministic) — exactly as §11 anticipated. A deterministic serial build
> (`-DEVO_NO_OPENMP=ON`) now exists for that tuning. The operating rule is unchanged
> and reinforced: **replicates, not single runs** (§10.4/§11.1/§12).

> **Reading note (newest session — the "perf cycle #2" session, v0.16.0):** this session
> touched **no science defaults** — every behavioural change is opt-in and off, so all
> existing findings stand. Three things to know before doing anything else:
> **(1)** `feedForward` was rewritten to a branch-free, sink-grouped flat connection list;
> it is **bit-for-bit identical** (proven by an oracle test + whole-run fingerprints) and
> **1.29× faster** on average, 1.17–1.47× across a 30-point grid. This reverses the
> v0.14.0 conclusion that "perf work here has poor ROI" — that verdict was right for the
> levers tried then (spatial layout, threading thresholds) and wrong in general: the
> neural inner loop was never examined, and `std::tanh` alone was >half of it.
> **(2) `-ffp-contract=off` is now a build flag, which shifts results once versus every
> binary built before it.** Old builds fused multiply-adds into FMAs; results therefore
> depended on `-march`/compiler/target, and **wasm has no scalar FMA at all**. Anything
> comparing new CSVs against the pre-0.16.0 corpus must account for this. It is the
> precondition for deterministic replay / checkpoints / verifying a run by re-running it.
> **(3)** Two new opt-in flags: `--topological` (biology — §4 "Metric vs topological") and `--fast-tanh`
> (speed, changes the numbers — §3 "Performance facts"). A new **§3 "Performance facts"** subsection records the
> hardware facts *and two measured dead ends* (shrinking `Creature`; dense-matrix brains
> for GPU) so they aren't retried. The operating rule is unchanged: **replicates, not
> single runs** — and now also **never trust a microbenchmark without an end-to-end
> confirmation** (§3 "Performance facts": one said 1.43× while the full sim said 0.97×).

---

## 1. What the project is

`evo_boids` — an **evolutionary** (NOT learning) boids / artificial-life simulation
in **C++20**.

- **Neuroevolution**: each creature's neural-net weights are fixed at birth (parent
  crossover + mutation). Nets never change during life. All adaptation is
  cross-generational. (In-life learning like Hebbian/NEAT is a *future* option, not
  built.)
- **Architecture**: header-only, SFML-free core (`evo_core`) under `src/core/`, plus
  a thin SFML frontend (`src/render/sfml_renderer.{hpp,cpp}`) that is the only place
  naming `sf::*`. App entry: `src/app/main.cpp`. Tests: `tests/core_tests.cpp` (**2048**
  structural checks as of 0.9.1), `tests/predator_smoke.cpp` (no-SFML predator
  integration harness, also in `ctest`), and `tests/bench.cpp`. CMake build.
- **Core source files**: `world.hpp` (sim loop, creatures, food, signal grid, spatial
  grids, `SimConfig cfg` member, `CurrentField current_` member), `stats.hpp`
  (snapshot + CSV), `config.hpp` (`SimConfig`, all defaults reproduce original
  behavior), `genome.hpp` (Gene/Genome/NeuralNet), `creature.hpp`,
  `sensors_actions.hpp` (Sensor/Action enums), `current.hpp` (ocean current field),
  `vec2.hpp`, `random.hpp`, `spatial_grid.hpp`. **Added v0.6–0.8:**
  `connectivity.hpp` (shared genome→connectivity embedding) and `genome_color.hpp`
  (locality-preserving colour). **Added v0.9 (predator):** `predator.hpp` (the
  scripted `Predator` struct); the hunt + `predators` vector + deaths decomposition
  live in `world.hpp`; the predator/schooling metrics live in `stats.hpp`.

### Environment / workflow
- Runs in **WSL** (`~/evo_boids`). Build: `rm -rf build && cmake -S . -B build &&
  cmake --build build -j` → binary at `~/evo_boids/build/evo_boids`. Use
  `~/rebuild_evo.sh` (it extracts the latest tarball from Downloads, wipes
  `~/evo_boids`, and rebuilds).
  - **Gotcha logged this session:** `rebuild_evo.sh` deletes & recreates
    `~/evo_boids` while your shell may be sitting in `~/evo_boids/build`. After a
    rebuild the shell points at a now-deleted inode, so `./evo_boids` →
    "No such file or directory" even though the prompt shows the right path. Fix:
    `cd ~/evo_boids/build` again (re-resolves the path). Not a build failure.
- Headless runs write to `runs/<name>.csv` + `runs/<name>.config.txt` (config
  sidecar), created under the launch directory.
- **Sandbox limitation (for the assistant)**: the dev sandbox has no cmake, no SFML,
  no emcc. The assistant verifies the core by compiling directly with
  `g++ -std=c++20 -O2 -fopenmp -DUSE_OPENMP -I src ...` and runs the real `main.cpp`
  against a stub renderer. **SFML renderer code cannot be link-verified** in the
  sandbox — only type-checked against a hand-rolled `sf::` stub. (Delivery is a full
  drop-in **tarball**, not a patch — patches fight `rebuild_evo.sh`, which rebuilds
  from a fresh extract.)

---

## 2. Current version: v0.22.0

> **Banner matches the code.** `CMakeLists.txt` is at **0.22.0** — the pairing graph:
> `World::logBirth` records which two creatures produced each successor record, the
> new always-on `repro_components` CSV column (32nd, appended — every existing
> column index stable) counts connected components of that graph over a trailing
> window, and `--repro-log` archives the events to `<stem>.births.csv`. All of it
> observational — fingerprint-verified byte-identical to 0.21.0 at defaults; see the
> 0.22.0 history entry. Measured on the sweep rerun: at NO tested threshold does the
> gate split the pairing graph — components FALL as the gate tightens (12.5 control
> → 3.3 at 16), so gating concentrates pairing into fewer clusters or collapses the
> run; the speciation signature (components rising above control) was not observed.
> 0.21.0 added `--repro-max-distance`, the opt-in match-distance gate (default off,
> fingerprint-verified byte-identical; see the 0.21.0 history entry). Two findings
> from its sweep are
> load-bearing for everything downstream: the usable threshold window is narrow
> and ends in a **bistable collapse at ~16** (below ~14 nothing breeds at all),
> and the **`species` metric turns out to be saturated** — it tracks population
> almost one-to-one and so cannot measure speciation at all, which is why the
> reproduction graph is a prerequisite rather than a nicety. 0.20.0 was frontend parity:
> native window gains [T] turbo + [S] checkpoint and the fixed-dt loop; unified
> `img/rocks.jpg` background — see the 0.20.0 history entry). 0.19.0 adds `--toroidal`
> (opt-in wrap-around perception — see the 0.19.0 history entry; default off,
> fingerprint-verified byte-identical). 0.17.0 added
> checkpoint/resume (bit-exact in the serial build, JSON format). 0.18.0 is the
> **portable-determinism cycle + the WebAssembly build**: the native serial build and
> the Emscripten/wasm build now produce **byte-identical checkpoints for the same
> seed/config/steps** (proven across a 9-case seed/config matrix by `wasm/parity.sh`),
> which makes the wasm module a universal deterministic artifact — and it came with a
> **one-time numerical-lineage break** (documented below, same framing as
> `-ffp-contract=off` in 0.16.0). Earlier history: 0.16.0 is the perf cycle that
> shipped branch-free `feedForward` (bit-identical, mean 1.29×), `-ffp-contract=off`
> for reproducible floating point, and the opt-in `--topological` / `--fast-tanh`
> flags.
> Historical note on this banner: `CMakeLists.txt` sat at 0.12.0 while the doc said
> 0.15.0. (0.12.0 extends the death-cluster map with PCA axes
> (`--cluster-pca`), lineage colouring (`--cluster-color lineage`), epoch drift arrows
> and a lineage summary, adds a `lineage` CSV column and a `--wiring-log` per-sensor
> recruitment log; all opt-in, dynamics-neutral. 0.11.0 added the live neural overlay
> + the death-cluster map; 0.10.0 the predator energy economy. No metrics-CSV change;
> NUM_SENSES still 41.)

### Newest code this session (v0.18.0; verified by per-primitive bit probes + a 9-case native-vs-wasm byte-equality matrix)
- **The wasm build reproduces the native serial build bit for bit.** `wasm/parity.sh`
  builds `wasm/parity_driver.cpp` with both toolchains and byte-compares final
  checkpoints: 5 seeds × 2000 steps default config, 2 seeds × rich config (eddies,
  food patches + drift, 4 predators + economy, move cost, topological 7, gene cost),
  triangular motion (exercises `atan2f`), and 10000 steps — **all PASS**. The
  checkpoint pins every creature (incl. recurrent neuron state), predators, food, the
  pheromone grid, the rng engine state, and the config, so byte equality of
  checkpoints == bit-identical simulations.
- **What had to change (measured first, fixed second).** A probe hashing every
  determinism-relevant primitive on both toolchains (gcc/libstdc++/glibc vs
  emcc/libc++/musl) found exactly these divergent: `std::uniform_int_distribution`
  (different values AND draw counts → stream desync on the first bounded draw),
  `std::normal_distribution`, and libm's `tanhf`/`sinf`/`cosf`/`atan2f`/`expm1f`.
  Already bit-identical and left untouched: the mt19937 output stream,
  `uniform_real_distribution`, `expf`, `logf`, `sqrtf` (IEEE-exact), C-locale float
  formatting. The fixes, each scoped to exactly what diverged:
  - `RandomGen::uint/intRange` → Lemire multiply-shift with rejection;
    `unit/range` → explicit `(engine()>>8) * 2^-24` (one draw, exact).
  - Sim-path float transcendentals → **vendored musl implementations**
    (`portable_math.hpp`, `evomath::` — MIT, taken from the musl copy bundled with
    emsdk). Verified: the vendored code compiled natively reproduces the wasm libm's
    output hashes **exactly**, over 4.2M-sample full-bit-pattern sweeps per function.
    `std::exp`/`std::log` stay on the system libm (probe-identical).
  - **`PortableMT19937`**: the engine's *outputs* are portable but its *serialized
    text* is not — libstdc++ and libc++ expose different internal twist
    representations, so checkpoints' `rngState` differed even when the runs were
    bit-identical (the parity matrix caught this: every counter matched, only
    `rngState` bytes differed). The vendored block-twist engine serializes in
    libstdc++'s exact format — **old checkpoints still load** — and is pinned against
    `std::mt19937` (outputs, text at 6 stream positions incl. block boundaries,
    cross-restore) in `core_tests`.
  - **Frozen projection planes** (`plane_data.hpp`, generated by
    `tools/gen_planes.cpp` from libstdc++'s `normal_distribution` values): genome
    colours and the species SimHash are now constants — **identical across native and
    wasm AND byte-identical to ≤0.17.0 runs** (no observable break for colours or the
    species metric). The 32-dim cluster-sketch planes regenerate at runtime via a
    portable Box–Muller (`portableGaussian`) instead — **cluster-map axes changed
    once at 0.18.0**; maps are comparable only within the same side of that boundary.
- **The lineage break, stated precisely:** default-config trajectories shifted once vs
  0.17.0 (new RNG draw mapping + musl-lineage transcendentals). Colours and species
  metrics did NOT shift (frozen planes). Old checkpoints load, but continuing one under
  0.18.0 diverges from what 0.17.0 would have produced — same rule as every numerics
  change since `-ffp-contract=off`. Distribution-level findings stand (invariant: the
  science lives in distributions across seeds, and even the far cruder `--fast-tanh`
  moved nothing detectable).
- **Perf did not regress — it improved:** serial 4000-step run at seed 42:
  0.17.0 = 4.78 s, 0.18.0 = **4.27 s (~1.12×)** (musl's `expm1f`-based `tanhf` +
  branch-free distributions beat glibc + libstdc++). The wasm build runs at **~79% of
  native serial speed** (1325 vs 1678 steps/s on the identical bit-for-bit run) —
  ~20× headroom over 60 fps at default population.
- **The browser build itself:** `wasm/evo_wasm.cpp` (C ABI: `evo_init(seed, w, h,
  pop, configJson)` accepting the checkpoint config schema, `evo_step(n)` at pinned
  dt 0.016, `evo_sync()` + packed getters for segments/colours/ids/food/predators/
  current-lattice/stats, `evo_checkpoint`/`evo_load_checkpoint` on JSON strings),
  built single-threaded by `wasm/build_wasm.sh` (`-ffp-contract=off`, no OpenMP, no
  SharedArrayBuffer → no COOP/COEP headers needed) into `web/evo_boids.{js,wasm}`
  (~240 KB wasm); `web/index.html` + `web/renderer.js` are a dependency-free canvas
  front-end mirroring the SFML renderer (verified in Chrome: creatures/tails/colours,
  food, predators, patches, vortex overlay, HUD, selection ring, restart form; zero
  console errors; checkpoint save→run→load→run re-verified bit-exact under Node).
- `core_tests` **2810/0** in both build modes; `predator_smoke` unchanged. The
  feedForward oracle now uses `evomath::tanhf` (it pins the flattening, and must
  share the activation or it would test the tanh instead of the wiring). Touched:
  `random.hpp`, `portable_math.hpp` (new), `plane_data.hpp` (new, generated),
  `genome_color.hpp`, `stats.hpp`, `cluster_projection.hpp`, `world.hpp`,
  `creature.hpp`, `current.hpp`, `genome.hpp`, `tools/gen_planes.cpp` (new),
  `wasm/*` (new), `web/*` (new), `tests/core_tests.cpp`, `CMakeLists.txt`.

### Previous session (v0.17.0 — checkpoint / resume; see the 0.17.0 history entry)

### Earlier session (v0.16.0; verified by oracle test + whole-run fingerprints + 30-point A/B)
- **Branch-free `feedForward`** on a flattened, sink-grouped connection list
  (`NeuralNet::flat`, 4 bytes/entry). **Bit-for-bit identical** to the old
  implementation, **mean 1.29× faster** (1.17–1.47× across the grid).
- **`-ffp-contract=off`** in `CMakeLists.txt` — reproducible floating point across
  toolchains and targets; **one-time results shift** vs every earlier binary.
- **`--max-neighbors N` / `--topological`** — topological interaction (opt-in, off).
  Model correction, throughput-neutral. §4 "Metric vs topological".
- **`--fast-tanh`** — Padé (7,6) activations, 1.2–1.3×, changes the numbers (opt-in,
  off). Distribution evidence in §3 "Performance facts".
- `core_tests` **2752/0** in both build modes. Full detail in the 0.16.0 changelog entry
  below; hardware facts and dead ends in §3 "Performance facts".

### Previous session (v0.12.0; all headless-verified by compile + run + numeric checks)
- **Lineage**: `Creature::lineageId` (founder of maternal line); assigned at spawn/
  reseed (own id) and reproduction (parent1's). In `DeathRecord` + deaths CSV; powers
  `--cluster-color lineage` and the SVG lineage summary. Verified: founders = own id,
  no new lineages appear without reseed.
- **PCA map** (`--cluster-pca`): 32-dim random sketch stored per death;
  `cluster_map_io.hpp` runs a compact Jacobi eigensolver on the sketch covariance for
  variance-aligned axes. Verified: recovers a planted dominant axis (PC1 spread ≫ PC2).
- **Drift arrows + lineage summary** in the SVG; **`--wiring-log`** →
  `<name>.wiring.csv` (`wiring_log.hpp`). Verified: 42-col output, fractions ∈ [0,1],
  planted single-sensor creature → that sensor's fraction = 1.
- `core_tests` **2332/0**. Stage-A/B paths, the 31-col metrics CSV, and the v0.11
  overlay are unchanged.



### What's in it (cumulative)
- **CSV logging — now 32 cols** (`repro_components` appended in 0.22.0; predator
  economy columns added in 0.10.0). Through 0.9.1 it was 27 cols ending in
  **neighbor_count**; 0.10.0 appended predator_births_interval,
  predator_deaths_starv, predator_deaths_age, predator_avg_energy; 0.22.0 appended
  repro_components.
- **Predator energy economy (Stage B)** behind `--predator-economy` (off by default):
  predators gain energy per capture (absorb victim reserves, or a fixed
  `--predator-meal`), burn `--predator-metabolism` + a kinetic move-cost, bud a child
  above `--predator-repro-energy` (paying `--predator-newborn-energy`, so it conserves
  energy), and starve below 0 (and optionally die past `--predator-max-age`). No
  predator reseed floor — extinction is observable. See §9/§11/§12.
- **Deterministic serial build** via `-DEVO_NO_OPENMP=ON` (CMake option): a
  single-threaded binary where a fixed `--seed` reproduces byte-for-byte (verified) —
  the recommended target for definitive cause-and-effect runs, per §11.1.
- **CSV logging through 0.8.0** was 20 cols ending in **flow_align**; 0.9.0 appended
  predator_pop, kills_interval, deaths_starvation, deaths_age, predator_align; 0.9.1
  appended neighbor_align, neighbor_count.
- **Runtime config** via `SimConfig` + ~50 CLI flags. All defaults reproduce original
  behavior.
- **Ocean currents** (off by default), two field structures (`eddies`/`gyre`) +
  spatial food + `--food-drift`.
- **Two current sensors**: `CURRENT_FWD`, `CURRENT_LR` (flow relative to heading).
- **`--move-cost F`**: kinetic energy cost `F·speed²` per second. Default 0
  (reproduces prior free-thrust behaviour). **Foundational** — see §3/§4; it is *why*
  the predator economy can have a kinetic cost too.
- **Locality-preserving genome colouring**: creature colour is a 3-D random projection
  of the (source,sink) connectivity vector.
- **`flow_align` metric**: mean cosine between each creature's heading and the local
  ocean-flow direction. See §7.

### New code this session (all verified by compile + run + numerical checks)
1. **Predator energy economy (Stage B)** — `predator.hpp` gains `energy`/`age`/`alive`
   (inert in Stage A); `config.hpp` gains 7 economy params (`predatorEconomy`,
   `predatorMetabolism`, `predatorMoveCost`, `predatorReproEnergy`,
   `predatorNewbornEnergy`, `predatorMeal`, `predatorMaxAge`); `world.hpp`
   `updatePredators` gains the burn/capture-meal/bud/starve/age logic (gated on
   `econ`, so the Stage-A branch is the original code path) + 3 cumulative counters
   (predator births / starvation / age); `stats.hpp` gains the 4 columns +
   `predator_avg_energy`; `main.cpp` gains the 7 flags, help, console economy segment,
   and the per-interval predator diffs. **Verified:** core_tests **2065/0** (was 2048;
   +17 economy checks incl. energy-conservation on capture/birth, starvation-to-
   extinction, Stage-A-inert energy); predator_smoke now runs **both** a Stage A and a
   Stage B harness and asserts the economy is live (births AND deaths occur) with the
   prey deaths-partition and predator-counter-monotonicity intact.
2. **Serial build option** (`-DEVO_NO_OPENMP=ON`) — verified the core compiles without
   OpenMP and two identical-seed runs are **byte-for-byte identical** (the determinism
   §11.1 asked for). The default OpenMP build is unchanged.

`core_tests`: **2065 checks, 0 failures** after all of the above (OpenMP *and* serial).

### Version history (why each bump)
- 0.3.0 — `--version` + metrics rework (diversity/species).
- 0.4.0 — `SimConfig` (runtime params).
- 0.5.0 — `--gene-cost` + CSV flushing for `tail -f`.
- 0.6.0 — currents (eddies) + 2 sensors → **NUM_SENSES 36→38** (changes genome→net
  mapping). 0.6.0+ runs are a fresh baseline; genomes/diversity NOT comparable to
  pre-0.6 CSVs. Added `--food-patches`.
- 0.7.0 — `--current-mode gyre`, `--gyre-eps`, `--food-drift`, GUI flow overlay.
- **0.8.0** — `--move-cost` (kinetic cost), `flow_align` (20th CSV
  col), locality-preserving genome colouring (`connectivity.hpp` + `genome_color.hpp`).
  Colouring does **not** change sim RNG/dynamics; `move-cost` and `flow_align` are
  the substantive simulation changes. NUM_SENSES unchanged at 38 (so 0.8.0 currents
  runs *are* comparable to 0.7.0 ones — the predator will be the next sensor-count
  change).
- **0.9.0 (this session)** — **Predator Stage A.** New `predator.hpp` (scripted,
  immortal, fixed-count `Predator`); separate `predators` vector + scripted hunt
  (`updatePredators`) in `world.hpp`; 3 prey threat sensors `PREDATOR_FWD/LR/PROX`
  → **NUM_SENSES 38→41** (fresh genome baseline; holds even at `--predators 0`); 5
  flags (`--predators`, `--predator-perception/-speed/-capture-radius/-cooldown`);
  deaths decomposed into predation/starvation/age; **CSV 20→25 cols** (predator_pop,
  kills_interval, deaths_starvation, deaths_age, predator_align); predators render as
  red discs + HUD `Pred: N`. No-predator path byte-for-byte unchanged. The two §5/§9
  "open decisions" were resolved as **tunable flags** (see §9): perception 150 (ambush
  asymmetry), instant-kill capture.
- **0.9.1 (this session)** — **schooling instrumentation only** (no behaviour change):
  **CSV 25→27 cols** — `neighbor_align` (local polarization) + `neighbor_count`
  (local crowding), computed in the snapshot pass. Added because `predator_align`
  measures *individual radial flight* and is blind to schooling. See §7 and §10.
- **0.9.2 (predator session)** — **perf only, no result/format change.** The schooling
  neighbour lookup in `stats.hpp` was O(n²) (brute-force pairwise); replaced with a
  **throwaway `SpatialGrid` built from the live creatures** → ~O(n). Verified
  **identical** to the brute-force version (differential on a 440-creature evolved
  state: align/count match to 7 decimals; `core_tests` 2048/0 unchanged). Reviewer's
  alternative — reusing World's `creatureGrid_` — was **rejected as a correctness
  bug** (that grid is stale by the time `computeStats` runs; §10.5). Negligible at
  the populations run so far (it was never the cause of the slowdown — §10.5), but it
  removes the O(n²) cliff for large-N / Stage-B runs.
- **0.22.0 (the pairing graph — `repro_components` CSV column + `--repro-log`;
  observational only, fingerprint-verified byte-identical to 0.21.0 at defaults).**
  The independent grouping measure the plan's §3.3 demanded: `World::logBirth`
  records (c1.id, c2.id) on the success path of `doInteraction` case 2 (serial
  phase, same thread-safety argument as `logDeath`; no rng, two integer writes).
  Two structures, because the metric and the archive want opposite things: an
  always-maintained UNDECIMATED ring of the last `--repro-window` (default 2000)
  events feeds the new 32nd CSV column `repro_components` — connected components
  with ≥ `--repro-min-cluster` (default 3) members, union-find in new
  `repro_graph.hpp` (decimating this graph would fragment components and inflate
  the very count being measured) — and `--repro-log` keeps a bounded, uniformly
  decimated archive (`birth_log.hpp`, gate → ordinal → stride → halve-at-2x,
  exactly the `logDeath` shape) written to `<stem>.births.csv` on headless runs.
  - Checkpoints: `reproWindow`/`reproMinCluster` serialized (old files load with
    the defaults 2000/3); the archive's decimation counters (`totalBirthsLogged`,
    `reproStride`) serialized like the death log's; the window and archive
    CONTENTS deliberately not (per-run analysis state, same rule as the death
    log) — a resumed run restarts the window empty, so `repro_components` is
    comparable to an uninterrupted run only after ≤ `reproWindow` post-resume
    events. Stated in `checkpoint_io.hpp`'s not-saved list and the flags doc.
  - Verified: byte identity vs 5f5f628 with everything at defaults — 20/20
    step-for-step fingerprint cases (4 seeds x 5 config modes) PASS, checkpoint
    differing by exactly the four new fields (2 config + 2 counters); ctest green
    in both builds; native==wasm parity 13/13. New tests: constructed graphs
    with known component counts (bridge-collapses-two-into-one, min-member
    floor, duplicate/self edges), ring eviction driven directly through
    `logBirth` (a bridge edge must age OUT of the window), the observational
    claim as a test rather than a code-reading (window 0/64/2000, archive on/off
    — all deepFingerprint-identical), same seed twice ⇒ identical archive
    records, checkpoint round trip incl. a legacy file with both keys removed
    and a load into an already-run World (stale window must clear), and the
    header append pinned (species stays column 19; repro_components is the last
    field).
  - **MEASURED (sweep rerun: 6 arms x 6 seeds x 20k steps, serial,
    `--no-render`; arms off/24/22/20/18/16): the speciation signature was NOT
    observed at any threshold.** `repro_components` tail means: control
    12.5±4.4, 24: 12.6±1.9, 22: 10.3±4.0, 20: 9.1±1.7, 18: 7.2±1.7, 16:
    3.3±1.2. Noise floor stated: the control's own seed spread is 7.7–20.7, so
    off/24/22 are indistinguishable, 20/18 sit at the edge of noise, and only
    16 is unambiguous — and the direction is DOWN everywhere: components never
    rise above control. Isolation would read as components RISING (groups that
    stop exchanging edges); what gating actually does is thin the graph — fewer
    eligible pairs ⇒ fewer clusters reaching 3 members inside the window — and
    at 16 the §7.1 bistability reproduces on fresh seeds (3/6 at or near the
    floor with 0–15 births/interval; 3/6 ABOVE control at 530–617): the
    survivors are a SINGLE compatible breeding core (components 2.7–5.0 at
    populations 530–617, vs the control's ~12 at ~480), i.e. the gate
    concentrates pairing into one cluster rather than separating several. The
    control's ~12 components are sampling fragmentation of a sparse 2000-event
    graph, not latent population structure — read the column RELATIVE to a
    control, not as an absolute.
  - `species`/population ≈ 0.98–0.99 in EVERY arm including the arm-16
    survivors (601 buckets / 617 creatures): the 0.21.0 saturation finding
    reproduced with the new column running alongside — `species` counts
    individuals, `repro_components` is the one that can move independently.
  - Confound stated up front (plan §7.2): `GENETIC_SIM_NEAREST` /
    `GENETIC_SIM_FWD` remain wired, so a future separation result must rule out
    an acquired similarity-seeking policy before crediting the gate. Left wired
    deliberately — changing the sensor set re-bases `geneCell` and the whole
    historical corpus (see §5).
  - Also in this commit: `logBirth` guards against a mid-run `reproWindow`
    shrink (unreachable today, pinned anyway); `deserialize` clears pairing
    state so an in-place reload cannot keep stale edges; the stale "31-col"
    comment and the flags-doc column list updated to 32.
- **0.21.0 (`--repro-max-distance` — reproductive isolation; opt-in, off =
  byte-identical, fingerprint-verified across 4 seeds x 5 config modes).**
  Parents whose genomes are farther apart than the threshold in the folded
  (source,sink) connectivity space cannot produce a child. New
  `src/core/isolation.hpp`: `genomeDistanceSq()` folds a genome into
  (cell, summed weight) pairs via the existing `geneCell()` embedding, then
  merge-joins. Permutation-invariant and indel-robust — the two properties
  `genomeSimilarity()` lacks, and the reason it was NOT used (it compares gene
  *i* to gene *i* and only sourceType/sinkType, so one indel shifts every
  downstream gene; measured: a single duplicate-insert drops it to **0.654**
  while the new distance moves only **2.0**). The gate is the last conjunct of
  the existing precondition `if` in `doInteraction` case 2, so an incompatible
  pair burns its interaction cooldown exactly as an under-energised pair already
  does — no new semantic. Off-path costs one float compare and draws no rng.
  - **Determinism detail worth recording.** The fold sorts on the PAIR
    (cell, weight), not on cell alone. Sorting on cell alone leaves genes
    sharing a cell in genome order, their weights are then summed in that order,
    and float addition is not associative — so a permuted genome would fold to a
    different vector and distance-to-self would stop being 0. Ordering by the
    pair makes equal elements bit-identical in both fields, so the sorted array
    is a function of the input multiset alone: same answer from any input order
    and from any sort algorithm. Same argument the topological tie-break uses.
    `d(a,b) == d(b,a)` holds bitwise (IEEE subtraction is exactly antisymmetric).
  - **Measured distance scale** (this is what makes the flag usable): unrelated
    random genomes **19.5** apart; in-run pairs within `--interact-radius`
    median **18-20**, p05 **13-17** (3 seeds x 20k steps, sampled every 5k);
    a mutated clone **~0**. Local and global pair distances track each other
    closely — the population is near-panmictic in genome space.
  - **THE SWEEP (8 thresholds x 8 seeds x 20k steps, serial, `--no-render`).**
    Control noise floor: population 479.7 +- 16.1, births 2481 +- 80,
    species 475 +- 15 (**~3.2% cv**) — differences smaller than that are not
    results. Threshold 24: inert, within noise of control. 22: births -3%
    (noise edge). 20: births **-10%**, diversity 13.7 -> 12.6 (outside noise).
    18: births **-23%**, population -6%. **16: BIMODAL** — 4/8 seeds collapse to
    the `--min-pop` floor (tail population 50-126), 4/8 establish a compatible
    breeding cluster and end at or ABOVE control (456-617). 14 and 12: total
    sterilisation, **every** seed pinned at the floor of 50 with 0-2 births
    across 20k steps. So the live window is narrow and sharply bounded:
    **inert above ~22, graded 20-18, bistable at 16, dead at <=14.**
  - **§7.1's collapse mechanism is confirmed and is a positive feedback.** In the
    dead arms `genetic_diversity` goes back UP (13.7-13.9, i.e. control level)
    while the population sits at the floor: the reseed floor is injecting
    genetically random founders, which are incompatible with everyone and with
    each other, so they eat, never breed, and keep the population pinned. The
    gate does not merely reduce births, it converts the reseed floor from a
    safety net into a diversity pump that cannot reproduce.
  - **SURPRISE, and it matters more than the gate: the `species` column is
    saturated and cannot measure speciation.** Across every arm INCLUDING the
    control, species count tracks population almost one-to-one (control 475
    species / 480 creatures; one run 659 / 693). At 16 SimHash bits the metric is
    effectively a near-injective hash of the genome — it is counting
    individuals, not species, and always has been. It therefore cannot detect
    reproductive isolation even in principle, and no threshold sweep against it
    means anything. This is exactly the circularity §3.3 of the plan warned
    about, arriving by a different route, and it is why the reproduction graph
    (0.22.0) is a prerequisite for calling any of this speciation rather than an
    optional extra. Pre-existing property, not caused by this change.
  - Verified: core_tests **2847/0** in both builds (20 new checks: exact
    permutation invariance, bitwise symmetry, self-zero, agreement with a dense
    STATS_DIM reference, the genomeSimilarity contrast, permissive-gate-is-a-noop
    bit equality, tight-gate-blocks-all, gate-is-selective-not-just-restrictive,
    determinism under the rich config, checkpoint round trip incl. a pre-0.21.0
    file with the key removed); byte identity vs 3a10fc6 with the flag off
    (20/20 step-for-step fingerprint cases, checkpoint differing by exactly the
    one new config line); native==wasm parity **13/13** including two new cases
    that exercise the gate at thresholds which actually bite (18 and 14).
  - Not done, deliberately: the §7.1 mitigations (soft/probabilistic gate,
    exempting reseeded founders). The sweep now says exactly where they would be
    needed — at and below 16 — so they can be built against measured collapse
    rather than an anticipated one.
- **0.20.0 (frontend parity: native window == web page).** App/frontend only —
  zero core changes. The native windowed phase gains the web's controls: **[T]**
  toggles turbo (flat-out fixed-dt stepping in ~200 ms adaptive chunks, window
  stays responsive at ~5 fps, HUD shows measured steps/s — the CLI's --headless
  remains the fastest path) and **[S]** saves `<base>.<frame>.json` mid-run using
  the --checkpoint-every base rules, closing the old gap where the windowed phase
  could not checkpoint. The windowed loop also switches from wall-clock dt
  (clamped 0.033) to the **fixed dt 0.016** the headless phase and the web have
  always used — a deliberate one-time change to the (never-scientific) visual
  phase that makes windowed sessions per-step reproducible and all three
  frontends behaviourally identical. Background art unified: `img/rocks.jpg`
  (user's ink texture) is now the single asset — native loads it (SFML reads
  JPG), web reads `../img/rocks.jpg`, `img/rocks.png` and the web copy removed;
  the dev server must now serve the REPO ROOT (`python3 -m http.server 8123`,
  open `/web/`).
- **0.19.0 (`--toroidal` — wrap-around perception; opt-in, off = byte-identical,
  fingerprint-verified vs 0.18.0 across 2 seeds x 1500 rich-config steps).**
  Motivated by a user report of fish "stuck" at edges at 50x. Empirical diagnosis
  first (edge_probe, 2 seeds x 6000 + 24000 steps): positions already wrap and
  nothing is immobile — median edge visit is 19 steps (shorter than one rendered
  frame at 50x, which is the illusion), band occupancy is 1.05-1.19x uniform with
  total churn — BUT a real thin tail of wall-hugger phenotypes exists (2-7
  creatures/run pinned >3000 steps until maxAge; two classes: wall-runners cruising
  the border at ~max speed, and seam-oscillators re-crossing the wrap line every ~5
  steps), and the mechanism is a measured **sensory cliff**: an edge-band creature
  perceives 3.0-3.3 neighbours where minimum-image distance gives 5.8-6.0 (~90%
  more), because SpatialGrid queries clamp at the seam, boundary sensors report a
  wall, the long probe goes blind off-plane, and the pheromone gradient flattens.
  The flag wraps ALL of it: `SpatialGrid::queryRadiusWrapped` (up to 9 clipped
  image queries, no duplicates for r < min(W,H)/2, (0,0)-image first so interior
  result order matches the metric path), min-image dx/dy in the metric +
  topological gathers, nearest-neighbour attributes, food sensing/eating,
  interaction pairing, predator hunt/chase/capture and prey threat sensors, wrapped
  long probe, wrapped SignalGrid emit/read (index-wrapped over the true cell period,
  padded +1 column untouched so checkpoint geometry is unchanged), wrapped newborn
  midpoint and spawn placements (same rng draws either way), and BOUNDARY_* = 1.0
  saturated no-wall. LOC_X/LOC_Y stay absolute; currents stay a bounded basin
  (documented seam discontinuity). Checkpoints carry `"toroidal"` (old files load,
  default false). Verified: wrapped-query differential vs brute-force min-image,
  seam-visibility through the real sensor path, pheromone seam wrap, toroidal
  resume bit-exactness over the full rich config, native==wasm parity for 2
  toroidal cases (11-case matrix all PASS), ~1% throughput cost. core_tests
  **2827/0** both builds. Expected science effect per the probe: wall-hugger and
  seam-oscillator phenotypes disappear; aggregate band occupancy (~3.4%) barely
  moves.
- **0.18.0 (portable determinism + the WebAssembly build.)** Native serial and
  Emscripten builds now write byte-identical checkpoints for the same
  seed/config/steps (9-case matrix in `wasm/parity.sh`). Portable `RandomGen`
  distributions (Lemire / explicit 24-bit unit), vendored musl float transcendentals
  (`portable_math.hpp`), `PortableMT19937` with libstdc++-compatible serialization
  (old checkpoints load), frozen colour/SimHash planes (`plane_data.hpp` — colours
  and species metric unchanged vs ≤0.17.0), portable gaussian for the cluster-sketch
  planes (cluster-map axes shifted once). **One-time default-trajectory shift**
  (rng + transcendental lineage), ~1.12× serial speedup. New: `wasm/` (C ABI shim,
  build script, parity harness), `web/` (canvas renderer + demo page). Full detail in
  the 0.18.0 session section above.
- **0.17.0 (checkpoint / resume).** `--save-checkpoint FILE`, `--load-checkpoint
  FILE`, `--checkpoint-every N`. A resume continues the run **bit-for-bit** in the
  serial build (deep-fingerprint-verified in `core_tests` and end-to-end through the
  CLI: 600 steps in one go == 300 + resume + 300, byte-identical final file). Format
  is JSON (`json_min.hpp` writer/reader, `checkpoint_io.hpp` save/load; `%.9g` floats
  round-trip float32 exactly; ~1.2 KB/creature) chosen so the wasm build's browser
  host can read it. Saved state includes the easy-to-miss pieces that were the real
  work: the RNG engine state, recurrent neuron outputs, and the rng-seeded vortices
  and food-patch centres. Editing the `config` block and resuming is the supported
  mid-run intervention. Not saved (derived/diagnostic): spatial grids, current-field
  centres, nets, colour, profiler, death-cluster log.
- **0.16.0 (perf cycle #2 — this one SHIPPED, plus reproducible floating point).**
  Unlike the reverted v0.14.0 cycle below, this one was a measured win: **1.17–1.47×
  faster at every one of 30 (population × perception) grid points, mean 1.29×, with
  bit-for-bit identical output.** The lever was the one v0.14.0 never touched — the
  **neural inner loop**, not spatial layout or threading. §3 "Performance facts" has the numbers and
  §4 "Metric vs topological" the reasoning; the short version:
  - **Flattened, sink-grouped connection list (the whole win).** `NeuralNet` used to
    store `std::vector<Gene> connections` and branch twice per connection inside
    `feedForward` (sensor-vs-neuron source, action-vs-neuron sink). Both branches are
    data-dependent and unpredictable, and interleaving hundreds of creatures per step
    thrashes the predictor. Now the wiring is stored **once**, flattened into two index
    spaces (`src`: sensors then neuron outputs; `sink`: actions then neurons) and grouped
    by sink with a **stable counting sort** — so every accumulator still sums its
    contributions in genome order and rounds *identically*. The loop body is
    `accum[c.sink()] += vals[c.src()] * c.w();` with no branches.
  - **Bit-exactness is proven, not assumed** (this was the acceptance criterion): a
    `core_tests` **oracle** re-implements the pre-change `buildNeuralNet`/`feedForward`
    verbatim (deliberately duplicated — an oracle that shares code with the thing it
    checks proves nothing) and asserts **exact float equality** over 400 nets × 6 steps
    (multi-step, so recurrent neuron state is exercised); plus whole-run state
    fingerprints (pos/vel/energy/age, FNV-1a) matching the pre-change build across 7
    seeds at 500 and 2000 steps.
  - **Two counter-intuitive requirements, both found by measurement, either of which
    silently turns the win into a loss:** (a) the flat entry must stay **4 bytes**
    (packed `src`/`sink` in one `uint16` + the `int16` weight). An 8-byte version made
    `feedForward` itself faster and the **whole sim slower** — this loop is
    bandwidth-sensitive. (b) The old genome-ordered array must be **deleted, not kept
    alongside**: keeping both measured **0.97× (a regression)** because the duplicate
    competed for cache with the neighbour scan. Only two consumers read it — the wiring
    log and the inspector overlay — and both now decode via `asGene()`, which inverts the
    flattening exactly. **The first version of this change was a 0.97× regression that
    looked like a win in a microbenchmark.**
  - **`-ffp-contract=off` is now a build flag** (`add_compile_options`). Without it the
    compiler fuses `a*b+c` into single-rounded FMAs wherever it likes, so identical
    source gives different results under different `-march`/compiler/target — and
    **wasm has no scalar FMA instruction**, so a native build with contraction could
    never reproduce a browser build. This is a **one-time results shift** (verified:
    old code with FMA ≠ old code without), and it is the precondition for deterministic
    replay, checkpoint resume, and verifying a run by re-running it. **Never add
    `-ffast-math`.** Tests that compare whole runs are now `#ifndef USE_OPENMP` — the
    OpenMP build has never been run-to-run reproducible (per-thread RNGs).
  - **`--max-neighbors N` / `--topological` (opt-in, default off = byte-for-byte
    unchanged).** Attend to only the N *nearest* neighbours in range instead of averaging
    over all of them. This is a **model correction, not a speed knob** — measured
    −7 % to +8 %, i.e. neutral. Justification and the surprise are in §4 "Metric vs topological".
  - **`--fast-tanh` (opt-in, default off).** Padé (7,6) rational for the activations,
    worst error 1.1e-4, worth **1.2–1.3×** end-to-end. **Changes the numbers**, so it is
    a separate numerical lineage; the distribution evidence is in §3 "Performance facts".
  - `core_tests` **2752/0** in both build modes (OpenMP and `EVO_NO_OPENMP=ON`);
    `predator_smoke` unchanged. Touched: `genome.hpp`, `world.hpp`, `config.hpp`,
    `main.cpp`, `wiring_log.hpp`, `sfml_renderer.cpp`, `tests/core_tests.cpp`,
    `CMakeLists.txt`.
- **0.15.0 (previous session) — triangular action space** (`--triangular-motion`, opt-in,
  off by default ⇒ byte-for-byte identical to 0.13.0; verified against golden CSVs for
  both a prey and a predator config). First behavioural change drawn from the Harpaz/
  Engert papers (see the design-ideas note). The original model is decoupled — STEER_X/Y
  nudge a heading vector (renormalised to unit) while SPEED_ADJUST moves a separate scalar
  speed — so there is no forced trade-off between going fast and turning, which is why the
  earlier quadratic burst/persist attempt produced no bimodal mode structure. The new
  model (in `world.hpp`, gated by `cfg.triangularMotion`) reads a forward **drive**
  `= clamp(0.5*(MOVE_FORWARD+1),0,1)` and a **turn** `= STEER_X ∈ [-1,1]`, couples them as
  `turnEff = turn*(1-drive)*resp`, advances heading by `turnEff * maxTurnRate * dt`
  (`--max-turn-rate`, default 4.0 rad/s), and sets `speed = minSpeed + drive*(maxSpeed-minSpeed)`.
  So a full-drive strike cannot turn and a zero-drive reorient turns freely — the forced
  trade-off the hunting paper (Malik/Singh/Harpaz/Engert/Rajan 2025, arXiv:2510.03699)
  identifies as the source of clean fast-straight vs slow-turning bouts. Smoke-verified:
  predator config holds the prey floor with active hunting; prey-only grows and forages.
  Whether the strike/reorient bimodality actually *emerges* is an open science question
  needing long evolved runs + bout analysis (speed/turn distributions) — not yet tested.
  STEER_Y, MOVE_RANDOM, SPEED_ADJUST are unused in this mode. Build skipped 0.14.0 (that
  number is the reverted perf cycle below). `core_tests` 2332/0 both build modes.
  - **Verified against the paper (arXiv:2510.03699 §2.1 / Fig 1d):** the coupling shape
    is faithful — a triangle in (forward, turn) with apex at max drive / base at zero
    drive is exactly `|turn| ≤ (1-drive)`, the linear form used. Caveats logged from the
    source: (a) their clean strike/reorient bimodality (Fig 2b) emerges under the FULL
    constraint set (coupling + linear hinge speed/turn costs [item 2] + binocular sensing
    + vergence cost) trained with PPO — the triangle alone is necessary, not sufficient,
    and ours is evolutionary, so bimodality is NOT guaranteed until item 2+ are added;
    (b) their model is bout-based (Δt=125 ms discrete decisions), ours integrates heading
    continuously per step; (c) their forward speed can be ~0 during a reorient, ours floors
    at minSpeed — so pair `--triangular-motion --min-speed 0` for their exact triangle;
    (d) no public code repo was found, so this is verified against the described geometry.
  - **Not yet done from the same paper (planned follow-ups, in priority order):**
    (2) replace the quadratic-from-zero `moveCost*speed^2` with a **linear hinge** penalty
    on forward speed and on turning (cost only above a threshold, free below) — keeps slow
    foraging cheap, makes bursts deliberate; (3) **sensory noise**, binocular/forward field
    less noisy than monocular/peripheral, distance only in the reliable region; (4) an
    energetic **vergence/lock-on cost** (precise orienting is rewarded by capture but
    converging costs energy, so the agent can't over-orient — replaces the misread
    "alignment penalty"). And from the 2021 collective-behaviour paper: an **occupancy**
    visual input (fraction of visual field filled) instead of per-neighbour vectors —
    biological realism, not perf; still needs the neighbour lookup, and is lossy by design.

  Three optimizations were tried against the v0.13.0 profiler on real hardware (8
  logical / 4 physical cores, memory-bandwidth-bound past 4 threads); all three were
  rejected by measurement and reverted, so the on-disk build is byte-for-byte 0.13.0.
  The cycle is recorded here as engineering history (don't re-try these):
  - **REVERTED — small-population parallel guard** (`--parallel-min-pop N`).
    `#pragma omp parallel if(numCreatures >= cfg.parallelMinPop)` — run the per-creature
    phase serially below N creatures. The hypothesis (OpenMP fork/join dominates at the
    ~20-prey floor) came from comparing a 1-thread *no-profile* run against an 8-thread
    *with-profile* run — i.e. the "~2× slower on 8 threads" was the profiler's per-creature
    `chrono` overhead, NOT fork/join. A clean apples-to-apples A/B on the tuned config
    (guard on=6229 steps/s vs off=6442 steps/s, same seed) showed the guard is a ~3%
    *regression*, within noise — no win. Also, both runs spend their first ~4000 steps
    at high pop (parallel in both cases), so the guard only touches the pop-20 tail where
    per-step work is microseconds. Reverted (config field, world pragma, `--parallel-min-pop`
    flag + help, flags table all removed). It was correct and harmless (serial build
    byte-identical), just pointless. `core_tests` 2332/0 before and after.
  - **REJECTED — CSR + SoA grid.** Rewrote `spatial_grid.hpp` as a flat counting-sort
    cell list with SoA positions. Verified byte-for-byte identical sim output (stable
    sort preserves neighbour return order, which matters because sensors sum floats).
    But microbench showed it ~5-10% SLOWER on query: splitting positions into px_/py_
    arrays touches 2 cache streams per candidate where the old AoS `Entry{pos,index}`
    touched 1, and the old per-bucket vectors were already contiguous (the ~9 bucket
    indirections it eliminated were never the cost). Fully reverted.
  - **REJECTED — cell-size tuning.** Swept grid cell {80,40,20,16} against the sim's
    query-radius mix; current cell = perceptionRadius = 80 is already fastest for every
    radius. No change.
  - **Overall finding (the real result):** at this scale (hundreds-of-prey transient,
    tens of predators, ~6000 steps/s; a 1.5M-step run is ~4 min) the simulator is not
    the bottleneck and none of the layout/threshold levers help. Perf work here has poor
    ROI; the durable artifact is the v0.13.0 profiler (kept) for spotting regressions.
  - **Lessons logged:** (1) serial-build CSVs differ run-to-run ONLY in timing columns
    (2=elapsed_s, 3=steps_per_sec); sim columns ARE deterministic — compare with
    `cut -d, -f1,4-` before claiming a regression. (2) The query cost is candidate
    distance-tests (O(N·k)), which neither layout nor cell-size reduces; only per-cell
    aggregate sensors would (deferred — needs behavioural validation, not byte-identical),
    and only matters in the schooling / large-N regime. (3) `core_bench` is multi-seed but
    has zero predators, so it under-represents real configs; always `--profile` a tuned
    config too. (4) Never trust a perf comparison where one side runs with `--profile` and
    the other doesn't — the profiler's `chrono` calls are not free.
- **0.13.0 (this session)** — **gated phase profiler (`--profile`), measurement only,
  dynamics-neutral.** `World` accumulates per-phase wall time (build / parallel /
  serial) plus a thread-summed CPU split inside the parallel loop (sensor-gather vs
  feedForward vs apply); `dumpProfile()` prints it; `tests/bench.cpp` rewritten to run
  multiple seeds and report mean ± spread. **Finding (single-core sandbox, 5 seeds,
  spreads <0.5pp so structural not stochastic):** build <1%, parallel ~96%
  (within: sensor-gather ~59%, feedForward ~35%, apply ~6%), serial ~3% — and
  cross-referenced with the standalone grid microbench, the gather slice is ~⅖
  neighbor-queries (~24% of step) and ~⅗ sensor-arithmetic (~34% of step). On
  multicore the ~96% parallel divides by cores, so serial's share rises (~20% at 8
  cores). **No optimizations applied** — this is the instrument to decide them. Timers
  gated off by default; `core_tests` **2332/0** unchanged.
- **0.12.0 (this session)** — **lineage + PCA + wiring analysis, on top of the
  death-cluster map; all opt-in, dynamics-neutral.** Creatures carry a founder
  `lineageId` (own id for spawned/reseeded founders, inherited from parent1 at birth;
  integer copy, no rng touch → byte-for-byte unchanged). The death-cluster projection
  now stores a fixed `CLUSTER_SKETCH_DIM=32` random **sketch** per death so the map
  can run **PCA** at end-of-run (`--cluster-pca`, compact Jacobi eigensolver over the
  32×32 sketch covariance) instead of the fixed random axes. The SVG adds **epoch-
  centroid drift arrows**, a **top-lineage summary** (share + epoch span), and a
  `--cluster-color lineage` mode (clade-hued points); deaths CSV gains a `lineage`
  column. New `--wiring-log` → `<name>.wiring.csv` (per-sensor wiring fraction over
  time; `wiring_log.hpp`, names guarded by `static_assert`). `core_tests` **2332/0**
  (+ sketch determinism, PCA planted-axis recovery, lineage inheritance, wiring
  fractions). **All headless-verifiable and verified; no renderer change this round.**
- **0.11.0 (this session)** — **two inspection/analysis features, both opt-in and
  dynamics-neutral.** (1) **Live neural inspector**: click a fish in the window → a
  translucent top-right panel draws its net firing (sensor→hidden→action nodes lit by
  activation, connections tinted by weight×signal). Needs stable creature `id`s (new
  field, assigned by World at every creation site — single-threaded, no rng touch, so
  dynamics stay byte-for-byte) and per-step captured `lastSensors`/`lastActions`
  (gated by `cfg.captureActivations`, auto-on only when a window opens, so headless
  pays nothing). All in the SFML renderer (`drawNeuralOverlay` + click pick) — the one
  place that names `sf::`. (2) **Death-cluster map** (`--cluster-map`): every death
  logs a 2-D random projection of its network (`cluster_projection.hpp`, sibling of
  genomeColor); RAM bounded to `--cluster-max` by uniform decimation; at end of run
  `cluster_map_io.hpp` writes `<name>.deaths.csv` + a self-contained
  `<name>.cluster.svg` scatter, points coloured by epoch (`--cluster-epoch`, default
  1000) on a blue→red ramp. No new sensors / no metrics-CSV change (the map is separate
  files). `core_tests` **2152/0** (+ id-uniqueness, projection determinism, capture
  gating, decimation bound). **Renderer verified compile/type-check only (no
  SFML/display in the sandbox) — the overlay's visuals need a real run to confirm.**
- **0.10.0 (this session — "Stage B session")** — **Predator energy economy (Stage B),
  off by default.** `predator.hpp` gains `energy`/`age`/`alive` (inert in Stage A);
  `config.hpp` gains 7 economy params; `world.hpp` `updatePredators` gains
  burn/capture-meal/bud/starve/age logic **gated on `--predator-economy`** (Stage-A
  branch = original code path) + 3 cumulative predator counters; **CSV 27→31 cols**
  (predator_births_interval, predator_deaths_starv, predator_deaths_age,
  predator_avg_energy); `main.cpp` gains 7 flags + a console economy segment. **No new
  sensors → NUM_SENSES stays 41**, so 0.9.x predator runs remain comparable. Also adds
  a **`-DEVO_NO_OPENMP=ON`** CMake option → deterministic serial binary (verified
  byte-for-byte across identical-seed runs). With the switch off, the Stage-A path is
  byte-for-byte unchanged (verified: `predator_smoke` Stage-A numbers identical to
  baseline). `core_tests` **2065/0** (+17 economy checks). See §9/§11/§12.

---

## 3. Empirical results (don't re-derive these)

### Tier-2 undulation go/no-go — the undulator harness (2026-07-27, `tools/undulator.cpp`)
*(Standalone, untracked by CMake and never linked into the sim:
`g++ -O2 -std=c++20 -ffp-contract=off tools/undulator.cpp -o undulator`, then
`./undulator --tune --converge --grid --cost`. Rebuilt from the repo file and rerun
2026-07-27; every figure below is from that run. The question, asked BEFORE paying for a
tier-2 format break: is travelling-wave locomotion **reachable, climbable, affordable**?)*

- **What the harness is.** A chain of N point masses joined by distance constraints;
  semi-implicit Euler + Gauss–Seidel constraint relaxation (PBD-style), velocities rebuilt
  from the projected positions. Muscles are **internal** angular springs at each interior
  joint, driving it toward a travelling-wave target curvature `phi*(j,t) = A·sin(wt − k·j)`;
  the three-particle force triple **sums to zero by construction**, so muscles apply no net
  force to the body — any forward motion must come from the fluid. Drag is anisotropic per
  segment at **`CN/CT = 20`**, and *that anisotropy is the entire physical basis of thrust*.
  **Reference gait** (all figures unless stated): A 0.45 rad, w 12 rad/s, k 0.35 rad/joint,
  20 cells, 16 substeps, 4 relax, kSpring 10000.
- **Falsification first — all four PASS.** A harness that reports thrust for the wrong
  reason is worse than no harness, so belief is gated on these, not on the grid:
  **T1 zero drag** → com moved (8.95e-10, 6.26e-11), i.e. muscles apply no net force;
  **T2 isotropic drag** → **+0.0000 BL/s** (anisotropy is *necessary*);
  **T3 anisotropic drag** → **+0.2902 BL/s** with gait tracking 88% (0.396 of 0.450 rad
  commanded) (*sufficient*); **T4 reversed wave** → **−0.2930 BL/s** (thrust tracks the
  wave, not a bias).
- **Why the gait diagnostic exists — the trap.** The harness's *first* version reported
  "no thrust anywhere on the grid", which reads exactly like an honest negative result and
  would have killed tier-2 on a bug. It was a **sign error in the muscle force triple** that
  folded every joint to ±pi. The commanded-vs-achieved amplitude readout and the four tests
  above are what turn a plausible NO-GO into a checkable one — keep them in any variant.
- **Q1 — reachable? YES, and converged** (steps=1200, substeps 2 → 128):
  `0.2708, 0.2786, 0.2856, 0.2902, 0.2927, 0.2941, 0.2948` BL/s — **monotone and smooth**,
  settling at **~0.29 BL/s**; the reference 16 substeps is within 1.6% of 128. The zero-drag
  |L| residual falls to **0.083 × I·w·A** at 128 substeps. So the thrust is physical, not an
  integrator artifact.
- **Muscle authority is the binding constraint** (tune sweep at CN 0.60). Joint natural
  frequency w_n = sqrt(kSpring/I) must sit **above** the drive w or the body barely bends:
  kSpring 900 → w_n 5.3 rad/s, 9% tracking, +0.003 BL/s; 3000 → 31%, +0.016;
  **10000 → w_n 17.7 rad/s, 88%, +0.291 — the sweet spot**; 30000 → 188% (overshoot),
  +0.243; 100000 → 249%, **−0.058** (backwards); 300000 → 150%, +0.476 but erratic across
  drag columns (over-amplitude, not a usable operating point). This is also *why* Q3 fails:
  stiff muscles are what force the substepping.
- **Q2 — climbable? YES at k ≥ 0.5 rad/joint.** There the A × w plane holds a **broad smooth
  plateau of 0.2–0.38 BL/s** across most cells (best +0.375 at k 0.80, A 0.45, w 24) — a
  gradient a search can walk, not an isolated spike. At k 0.10–0.20 the surface is **ragged
  with sign flips**: reachable in principle, not climbable there.
- **Q3 — affordable? NO at 20-cell fidelity.** Reference gait: **0.291 BL/s at 37.65 µs per
  creature per step** (this machine, today; an earlier session measured ~46 µs for the same
  config — the µs figures move ~±20% between sessions, the physics reproduces exactly).
  Against the **~1.5 µs whole-creature** baseline below, that is **~25–30× the entire
  current per-creature budget**, not the ~5% the plan estimated (which assumed ~100 flops
  and no substepping). Cost table, the rows that matter: 20 cells / 8 substeps / kSpring
  4000 / w 8 → 0.092 BL/s at **21.3 µs** (swims slowly, still ~14×); **8 cells / 2 substeps
  / kSpring 600 / w 4 (k scaled 0.875) → 0.081 BL/s at 0.96 µs** — swims, at ~64% of the
  whole-creature budget, i.e. **~2× total per-creature cost**. **Every other cheap row does
  not swim** (<0.05 BL/s cutoff), so the 8-cell row is the single cheap survivor, an entry
  point rather than a representative sample.
- **GRADED VERDICT.** Q1 reachable **YES** (~0.29 BL/s, converged, falsification-tested).
  Q2 climbable **YES** at k ≥ 0.5 (broad smooth plateau, not a spike). Q3 affordable **NO at
  20-cell fidelity** (~25–30× budget; muscle stiffness w_n > drive w forces the 16 substeps)
  — **but** an 8-cell soft-gait configuration swims at ~2× total per-creature cost with
  ~3.6× slower thrust, so **tier-2 is not dead**. The escalation decision (plan §3.5 — a
  *capability* decision) therefore **moves to Phase 5 planning as a body-size / fidelity
  trade**, with the 8-cell configuration as the entry point.
- **Open — not run: the §4.4 gait-maintenance test.** Initialise the live set with
  hand-written undulation controllers *inside the real sim* and see whether selection
  **maintains** the gait. Maintenance is strictly easier than discovery, so its failure
  would be conclusive. Worth running before any tier-2 build.

*Line numbers/figures measured 2026-07-27 on the v0.22.0 tree.*

### (NEW) Performance facts — measured on real hardware, don't re-derive these
*(8-core WSL, gcc 13, `-O3 -march=native -ffp-contract=off`, serial build unless stated.
Method: 156-run sweep = a 5×3 population × perception grid plus 24 randomised SimConfigs,
2 seeds each, 3 repetitions, medians. Both sides of every A/B run `--profile`, per the
v0.14.0 lesson.)*

- **The cost model is boring and that is the finding:** step time ≈ f(population,
  local density). Predators (even 7–8 with Stage B economy), food patches, currents and
  triangular motion are **cost-invisible** next to population × density. Per-creature
  cost fits **≈1.5 µs fixed + ~14–21 ns per neighbour in range**.
- **`std::tanh` was over half of `feedForward`** — each creature makes
  `MAX_INTERNAL_NEURONS + NUM_ACTIONS` = 24 calls per step *regardless of genome size*
  (a ~36-gene genome is only ~36 multiply-adds). This is why the neural loop was worth
  attacking and the v0.13.0 profiler's "gather ≫ feedForward" read was scale-dependent:
  at demo scale (200–800 creatures) feedForward is **35–53 %** of parallel CPU; the
  gather only dominates at high density (**79 %** at 4000 creatures / perception 160).
- **Throughput after 0.16.0** (serial): 200 creatures ≈ 3 500 steps/s, 500 ≈ 1 380,
  1000 ≈ 660, 2000 ≈ 315, 4000 @ perception 160 ≈ 73. So a **browser/WASM demo at
  200–800 creatures has ~10× headroom over 60 fps** even at half native speed.
- **OpenMP scaling is worse than assumed:** ~2–3.5× on 8 threads above ~1000 creatures
  and **~nothing below ~500** (sometimes a slight loss, with large seed-to-seed
  variance) — the serial phase's wall share grows to 10–20 % and the gather is
  bandwidth-bound. Reinforces §11.1: use the serial build for small-N work; you lose
  little and gain determinism.
- **DEAD END — shrinking `Creature`.** `sizeof(Creature)` is 416 bytes, over half of it
  (`lastSensors[41]` + `lastActions[12]` = 212 B) unused in headless runs. Rebuilt at
  216 B: **exactly 1.00× (mean and median over 30 runs)**, no gain at any population.
  Reason: `SpatialGrid` already stores positions inline in its buckets, so the hot
  neighbour loop is sequential over compact 12-byte entries and barely touches
  `Creature`. Don't retry.
- **DEAD END — dense-matrix brains (the GPU idea).** Embedding the free-topology net in
  fixed dense matrices to enable batched matmuls would be 53×12 + 53×12 ≈ **1 272
  multiply-accumulates versus the ~36 the sparse genome actually uses — ~35× more
  arithmetic** to avoid branches that the 0.16.0 flattening removes for free. A GPU
  would have to win 35× just to break even. Sparse-and-branch-free is the right answer
  on both CPU and GPU.
- **`--fast-tanh` does not move the science** (the reason it is allowed at all):
  12 seeds × 4000 steps per arm, run **twice** — a plain config and a predator config
  (5 predators + move-cost 7e-5) — comparing baseline / `--topological 7` /
  `--fast-tanh` / both. Population, schooling (`neighbor_align`), genome length,
  genetic diversity and species count are **statistically indistinguishable across all
  four arms** (e.g. predator config population 421.4±18.7 baseline vs 423.1±15.4
  fast-tanh, n=12). **Caveat on the evidence:** the schooling metric sat near zero in
  *both* configs, so this has little power to detect a schooling-specific shift —
  population/diversity carried the signal. Individual runs of course diverge; the claim
  is only about distributions.
- **Harness** (kept, in WSL `~/bench_sweep/`, outside the repo): `bench_sweep.cpp`
  (grid + random sweep, CSV out), `bench_opt.cpp` (per-arm throughput + outcome
  distributions), `fingerprint.cpp` (whole-run FNV-1a state hash — the tool that proves
  old-vs-new equivalence), `ff_micro.cpp` (feedForward variant microbenchmark).
  **Warning from this cycle: `ff_micro` said the change was 1.43× when the full sim said
  0.97×.** Microbenchmarks run with hot caches and no competing traffic; always confirm
  end-to-end before believing one.

### Pre-session baselines (unchanged, for reference)
- **Baseline 10M-step run** (no gene cost, pre-0.6 metrics): textbook predator-prey
  boom-bust limit cycle, never damps. Pop 134–979 (mean 520), ~6000-step period.
  Genome length ratcheted to the cap (35→79) — a **mutation-operator artifact**, NOT
  selection. Evolved phenotype fast/twitchy (avg_speed→~147, responsiveness→0.98).
- **Gene-cost 0.02 run** (1M, v0.5.0): genome length went the other way (35→23);
  ~23 genes suffices, so the no-cost climb to 80 was mostly neutral bloat. Boom
  softened ~25–30% but via **lower carrying capacity**, not diversity; population
  converged *more*. For intermediate length use gene-cost 0.005–0.01.

### Currents A/B — eddies vs gyre (current 80, food-patches 6, food-drift, gene-cost 0.01, seed 42, 1M, NO move-cost)
- **Same boom-bust in both.** FFT dominant population period ~6,300 steps in both
  (57–63% of power), identical to the no-current baseline. Currents do **not** change
  the cycle frequency. (An early autocorrelation read suggested ~44k for eddies —
  that was an **artifact**; FFT corrected it.)
- **Neither collapses.** Troughs 115 (eddies) / 129 (gyre), both well above the
  reseed floor of 50.
- **Eddies = gentler:** better-fed (tail avg_energy ~48 vs ~36, gap widens), longer-
  lived, lower turnover; higher-amplitude swings (CV ~0.47); converges more
  (species/pop ~0.35).
- **Gyre = harsher but preserves diversity:** lower energy, ~45% more deaths/interval,
  younger deaths; tighter cycle (CV ~0.40); **species/pop ~0.54** — the predicted
  geographic-isolation/speciation payoff, and the cleaner substrate for future NEAT.
- gene-cost 0.01 does **not** tame genome bloat under currents (eddies →52 still
  rising; gyre →46 plateaued). Keep treating `avg_genome_len` as a dead signal here;
  trust `species` over `genetic_diversity` (latter is length-coupled).

### The currents sweep + the move-cost result (the main finding of this session)

The arc, in order — **each was a single-variable step:**

1. **min-speed 30 vs 0** (gyre, current 80, NO move-cost): **NULL.** `avg_speed`
   pinned at ~147 in *both*; `flow_align ≈ 0` in both. The drift arm never used its
   freedom to slow (slowest logged ~142). → **Ruled out** "the min-speed floor forbade
   riding." → **Supported** "the fish are overpowered; flow too weak to matter." (Run-
   to-run noise at fixed seed+config already exceeds every between-arm difference:
   genome length swings ≥7 genes, species/pop ≥2.7×. So treat genome/species deltas
   as noise unless replicated.)
2. **current 80 → 150** (NO move-cost): still `flow_align ≈ 0`, `avg_speed` maxed.
   The current is *felt* (standing food +42%, deeper troughs) but as **foraging
   stress, not adaptation**. So even at flow = swim-max, free-thrust fish ignore it
   and power through.
3. **current 300 = 2× swim-max** (NO move-cost, 2M): **`flow_align` finally goes
   positive** — steady ~**+0.06** after warmup, positive in ~72% of rows, never
   decays. `avg_speed` still maxed → **active surfing** (full thrust angled
   downstream), not passive drift; avg_energy recovers as alignment rises → the surf
   is functional. **Weak but real, stable.** (The truncated 820k run's apparent
   **+0.27** was a transient blip — corrected by the full 2M run. Mechanism: at 2×
   swim-max, food advects past faster than anything can swim, so going *with* the flow
   is the only way to close on it.)
   - **Mistake owned:** after step 2 the assistant said currents were "conclusively a
     dead movement-pressure at any strength." That **over-extrapolated from two
     points** and was **wrong** — c300 showed signal, and (below) move-cost revived
     80/150. Correct framing: *currents are marginal when locomotion is free.*
4. **`move-cost` root-cause fix.** The real reason `avg_speed` pinned at max in
   *every* prior run (no-current, 80, 150, 300): the energy burn had **no speed term**
   — thrust was free, so max speed was a free dominant strategy. Adding `F·speed²`
   creates an **interior optimal cruise speed**.
   - **Calibration** (gyre, current 0, seed 42, 1M): `move-cost 3e-5` → cruise ~84,
     pop mean 353 (min 117); `7e-5` → cruise ~82, pop mean 312 (min 72). **Both
     populations healthy** (neither near the floor); the two coefficients land at the
     *same* ~half-max speed → a **robust interior optimum**. **Picked `7e-5`** (bites
     harder while staying viable → maximises the fight-vs-ride energy gap). `flow_align`
     = 0.000 at current 0 in both (clean confirmation the metric isn't self-drifting).
   - **Mistake owned:** the sandbox's short-horizon sweep predicted `7e-5` would
     *floor* the population. The real 1M runs did **not** floor — patchy/drifting food
     pays the tax fine. Running it for real beat trusting the sandbox.
5. **THE KEY RESULT — pricing thrust unmasks the current** (gyre, food-patches 6,
   food-drift, gene-cost 0.01, **move-cost 7e-5**, min-speed 0, seed 42, 1M):

   | run | flow_align (tail) | %rows >0 | avg_speed |
   |---|---|---|---|
   | current 0 (calm) | +0.000 | 0% | 82.1 |
   | current 80, **no** move-cost | −0.060 | 12% | 146.5 |
   | current 80, **move-cost 7e-5** | **−0.092** | 8% | 119.0 |
   | current 150, **no** move-cost | −0.062 | 18% | 147.5 |
   | current 150, **move-cost 7e-5** | **+0.068** | 80% | 132.5 |
   | current 300, no move-cost (ref) | +0.063 | 72% | 147.5 |

   - **A sign flip.** With thrust priced, **current 80 → they FIGHT** (heading biased
     upstream, `flow_align −0.092`) and **current 150 → they RIDE** (`+0.068`, the
     same alignment that previously needed current **300**). Pricing thrust did the
     work that doubling the current used to.
   - **Mechanism (kinematic threshold):** with `--food-drift`, food rides the current.
     At current 80 ≈ cruise (~82), they can *win* a fight upstream and intercept the
     food being carried toward them → face into the flow. At current 150 > cruise,
     fighting is hopeless and ruinously expensive → flip to riding and feed off what
     co-drifts. **"I can fight this and it pays" (80) vs "I can't, so I ride" (150).**
     This is the design doc's "fight-vs-ride is kinematic" made real — invisible until
     thrust had a price.
   - `avg_speed` in a current **rises above** the calm cruise of 82 (119 / 133): they
     spend extra energy to cope with flow.
   - **Corrected headline:** currents are a **real, structured selection pressure on
     movement — but only when locomotion is costly**, and the fight-vs-ride choice
     even depends on flow strength relative to cruise speed.
   - **Caveats (kept honest):** single-seed, single-run; magnitudes modest
     (|flow_align| < 0.1), variance meaningful (sd ~0.07–0.08), trajectories wander
     (c80 dips to −0.15 mid-run; c150 swings +0.18 → −0.03 → +0.17). The *directions*
     (80 negative, 150 positive) are clear and mechanistic; exact values are not
     pinned. Ecology of costed-current runs is leaner (pop ~225–229 vs 312 calm); c150
     min pop 58 — viable but close enough that I would **not** raise the coefficient
     at that strength. Genome/species deltas inside the noise floor — ignore.

**Currents: CLOSED.** Conclusion above. The sweep also served as a **capability
check**: given a genuine fitness gradient, the neuroevolution + `CURRENT_FWD/LR`
sensors **do** find the adaptive direction and the response is functional — exactly
the reassurance wanted before the predator.

**Currents loose ends — deliberately dropped** (would only refine a pressure now
understood; chasing them = the over-exploration trap):
- a midpoint `--current 115` run to pin the 80→150 fight/ride crossover — **dropped**
  by Harit's call.
- the `--food-drift`-OFF "fight static patches" regime — parked.
- multi-seed error bars on the directions — parked (would give publication-grade
  confidence on a sub-result; not needed to proceed).

---

## 4. Design decisions & conclusions (the "why")

> The pre-session entries below (boom-bust, gene-bloat stances, need-driven
> complexity, coevolution, NEAT speciation, currents eddies-vs-gyre, the
> streamfunction-feedback evaluation) are **unchanged and still authoritative** —
> kept verbatim so they aren't re-litigated. New this-session entries follow.

### Boom-bust is intrinsic
Predator-prey oscillation is correct ecology, not a bug. Flags only **damp** it.
Genuine flattening needs **density-dependent (logistic) food regrowth** = a code
change (offered, not built). Gene cost is the **wrong tool** for the boom. For
damping, use `--repro-energy 38`, `--food-amp 0`, or the logistic-food change.

### Gene bloat — three stances
Pinning at the cap is a **mutation-operator artifact**; it makes
`avg_genome_len`/`genome_len_std` dead signals. Neutral genes aid evolvability, so
parsimony pressure trades evolvability for cleanliness. Options: (1) leave it & don't
read length as fitness; (2) rebalance insertion/deletion operators (most honest fix);
(3) gene cost — what we use; answers "what length survives when genes are taxed."

### The real goal: need-driven complexity
A **static task has a fixed optimal complexity** (~23 genes here), so it never grows.
Need-driven complexity requires (1) a **demand gradient** — coevolution (Red Queen,
self-escalating) or an *escalating* environment (a fixed-frequency cycle has constant
difficulty → still plateaus) — and (2) **NEAT-style complexification** (start minimal,
add nodes/connections by mutation, innovation numbers for crossover alignment,
speciation via fitness-sharing to protect new structure). Caveat repeated: **most
ALife plateaus; open-ended growth is closer to unsolved than default. Expect a
ceiling.**

### Coevolution (quick def)
Two+ things evolving against each other so each reshapes the other's selection —
antagonistic (predator/prey, parasite/host) or mutualistic. A fixed environment lets
organisms climb to an optimum and stop; coevolution keeps the target moving.

### NEAT speciation — mechanism & biology
A compatibility distance (gene mismatch via innovation IDs + weight diffs) groups
genomes into species; fitness is divided by species size (fitness sharing) and
offspring allotted per species → a young species of novel mutants gets a few slots.
A grace period, not immunity. Biological analogs: geographic isolation (strongest),
negative frequency-dependent selection, gene duplication. Engineered conveniences: the
global fitness-bookkeeper and innovation registry.

### Currents design (eddies vs gyre)
Harit's "moving eyes" = a vortex superposition = localized streamfunctions,
divergence-free by construction. Both modes incompressible (velocity = curl of a
scalar streamfunction), so passive matter stretches into filaments, never compresses
to points. **Eddies**: K drifting vortices → many small wandering pockets. **Gyre**:
canonical time-periodic double-gyre, two counter-rotating basins + an oscillating
dividing line → cleaner two-region geographic isolation. Baked-in rules: (a) currents
only matter with spatial food; (b) fight-vs-ride is kinematic (peak flow ≈ swim-max);
(c) `--food-drift` advects food into filaments without collapse.

### Evaluation of external feedback (streamfunction vs "random eyes") — still stands
Its **physics principle is right and is what we built** (divergence-free deterministic
field). Its central factual claim — that our eddies are random noise with **sinks that
compress food to points** — is **false for our implementation** (blob-dispersion test
disproves it; our eddies are incompressible vortices = localized streamfunctions, the
feedback's own "best of both worlds"). Its one good actionable idea was food
advection → filaments (built as `--food-drift`). The double-gyre is a legitimate
*alternative*, not "definitively" better. Its compute-cost points were overstated at
small K and self-contradictory.

### (NEW) Why `move-cost` is foundational, not a currents patch
Free thrust made max-speed a dominant strategy in every run; that single fact masked
the currents and would also wreck the predator (a free-thrust predator runs flat-out
forever — no burst-and-rest). `F·speed²` (kinetic-energy form, the principled choice)
installs an interior cruise speed and makes "fighting a current" genuinely expensive.
Strictly better than lowering `--max-speed` (which would just re-pin at the new cap):
a *cost* makes them **choose** a lower speed endogenously and choose to ride when
riding pays. **Keep `--move-cost 0.00007` permanently** in the baseline (see §5).

### (NEW) Body-size / weight trait — discussed, deferred to the predator
A size trait tied to strength (resist flow) + slower speed + higher metabolism +
larger receptive field is a **pure tradeoff → single ESS, NOT branching.** Adaptive
dynamics: a tradeoff has one peak; selection climbs to it and stops (just like speed/
responsiveness/wiring all converged). **Branching needs frequency-dependent
selection** — typically resource competition along a niche axis (big and small eat
*different depletable* food → whichever morph is common depletes its own food, rare
morph does better → both persist). In our well-mixed, single-resource, high-mobility
world (perception 80, fast swimmers), the likely outcome is **convergence to
small-and-fast** (size is mostly cost; its flow-resistance benefit only pays in strong
zones a mobile creature avoids). The perception coupling actively *works against*
branching (bigger receptive field → more mixing → blurrier niches). **Where size has
teeth: WITH a predator** (big/strong/fast-but-hungry vs small/agile/cheap, size
refuges) — the textbook driver of size-based branching. **Decision: introduce size
alongside/just after the predator (Stage B+), as its own variable — not in the static
task, not in the first predator build.** Note: the `species` metric is a SimHash over
*neural connectivity* and is **blind to a body-size trait** — branching would show as
**bimodality in the trait itself** (log size dispersion/modality, not `avg_size`).

### (NEW) Predator feedback evaluation (the 5-point external list)
Accurate about the substrate this time (verified: `INTERACT_FIGHT`,
`GENETIC_SIM_NEAREST`, `AVG_NEIGHBOR_DX` all exist; the fight is winner `+15` / loser
`−15`, death only if reserves ≤ 0). But one fact reframes the whole list:
- **The existing `INTERACT_FIGHT` is a symmetric *adjacency contest*, not a pursuit.**
  It only fires within `interactRadius` (20), resolves on the **sum** of both parties'
  `INTERACT_FIGHT` output, winner by age+luck. No chase, no capture-on-contact, no
  speed/maneuver. **Building predators on it yields cannibalism / contest competition,
  NOT the pursuit predator the plan needs.** The chase/escape arms race does not arise
  from this mechanic.
- **Point 5 (metabolism/energy economy for hunters): MOST IMPORTANT, correct.** This
  is the energy-economy closure that bounds the two coupled oscillators. Go further
  than "raise metabolism for fighters" → give the predator the *full* economy so the
  population self-regulates. Build first.
- **Point 1 (lethal, absorptive fight): right direction.** Loser dies, winner absorbs
  reserves → carnivory becomes calorie-dense. **Drop the "meat bonus from genome
  length"** — genome length is the noisy, drift-dominated, mostly-neutral signal;
  tie meal value to the victim's *reserves* (or a real size trait). This is also the
  lever most likely to trigger the extinction spiral → tune with point 5.
- **Point 2 (split kin vs prey sensors): sound but architecture-dependent.**
  `AVG_KIN_DX`/`AVG_PREY_DX` by genetic similarity fits the **one-population cannibal**
  model. For a **separate predator entity** (our choice), use `PREDATOR_FWD/LR` on prey
  and `PREY_*` on the hunter instead. Either way bumps NUM_SENSES → fresh baseline.
- **Point 3 (camouflage via prey-colour-vs-background shrinking predator perception):
  cleverest idea — but it COLLIDES with our colouring.** Colour is now a **deterministic
  projection of the connectivity genome** (the locality-preserving scheme), so
  selecting for camouflage selects on the **same vector that drives behaviour** —
  pleiotropy. To make camouflage evolvable you must **decouple displayed colour from
  the wiring** (a small independent "pigment" gene set) — which **breaks the
  "colour = lineage signature" property** we just built. **You must decide what colour
  is FOR: a lineage viz marker, or an evolvable camouflage trait — you can't read both
  off the same channel.** (The `PREY_VISIBILITY` sensor is optional; camouflage evolves
  from differential predation whether or not prey sense their own visibility.)
- **Point 4 (ambush-in-shear vs pursuit-in-calm role split): speculation, contradicted
  by our data.** A spatial role split needs creatures to *stay* in distinct flow
  regions = low perception + low mobility + voluntary station-keeping in strong flow —
  the exact behaviour we watched them *not* do until current hit 2× swim-max. Don't
  design around it; test for it.
- **Harit's background-gradient-basins idea = the strongest thing in the exchange.**
  A spatially-varying background gives **position-dependent selection** (matched in
  one region = exposed in another) → a reason to **localise** → the niche partitioning
  that *both* camouflage-speciation and point 4 need, and which doesn't depend on
  voluntary station-keeping. This is the real escape from the isolation-vs-gradient
  tension (below). **Note: these background basins do NOT exist yet** — only the flow-
  field *overlay* (streamlines) exists. They're a future camouflage-phase addition.

### (NEW) The pocket/lineage observation & the isolation-vs-gradient tension
Harit observed: at **current 0** (move-cost on) the population formed **distinct
pockets, each a different-coloured lineage**; at **current 150** it collapsed to **~2
related lineages, mixedly riding**. Explanation:
- **Current 0 = isolation → many lineages (allopatry).** No flow to move creatures;
  `--food-patches 6` pins food; creatures settle, breed locally, *stay* → each patch
  drifts to its own lineage. Spatially **segregated** lineages read as distinct
  colours.
- **Current 150 = a mixing pump AND a shared selector → few lineages.** Flow > cruise
  advects creatures across the field faster than they resist → **destroys spatial
  isolation** (forces gene flow) *and* imposes a **shared strong pressure** (ride to
  feed) → the best riders propagate everywhere. Isolation-by-distance collapses into
  one well-mixed population; directional selection narrows it.
- **The deep tension:** speciation needs isolation (current 0 gives it but has no
  demand gradient); a strong gradient everyone feels (current 150) destroys isolation.
  A **uniform** current can't give both. **Spatially-varying background** (camouflage
  phase) is the escape — position-dependent pressure restores a reason to stay local.
- **Honest caveats:** (a) colour is a **3-D shadow of the species SimHash** (same
  vector) → NOT independent evidence; (b) the numbers don't show fewer lineages —
  species/pop is *higher* at c150 (0.162) than c0 (0.093); what changed is plausibly
  **spatial arrangement** (segregated → mixed), which the eye reads as "fewer
  pockets"; (c) **spatial structure is UNMEASURED** — the CSV logs only aggregates.
  Confirming it needs a **per-creature snapshot** (positions + connectivity at final
  step) or a **spatial-autocorrelation metric (Moran's I on genome)**. This is the
  start of the speciation phase, not a mid-currents side-quest.

### (NEW) Renderer bug — flow overlay/world not fully visible (NOT YET FIXED)
Symptom: the live GUI flow-field streamlines don't appear even with a current on and
without `--no-render`. **Root cause (diagnosed, not faintness, not the SFML-stub
issue):** the window is created at `sf::VideoMode(width, height)` with **no
`setView`**, while the world is **2000×2000**. SFML's default view maps window pixels
1:1 to world coords → only the **top-left corner** of the world is visible. The
overlay samples the *whole* world on a 64px grid (`drawCurrentField`), so most
segments draw off-screen and clip. Food/creatures look fine because enough land in the
visible corner. **Fix (small, but UNVERIFIED in sandbox — no SFML to link-test):** set
an `sf::View(sf::FloatRect(0,0,world.width,world.height))` via `window_.setView(...)`
so the full world maps into the window. This would simultaneously show the full flow
field, the entire population, and fix any "fish disappear off the edge" effect.
**Action before fixing:** confirm the actual window size and whether fish vanish at
edges, then apply + link-verify in Harit's build. **Caveat this throws on the pocket
observation:** if only a corner was visible, the "4–5 pockets" may be a sub-sample —
counts/arrangement unreliable until the view shows everything. Purely a viz fix; zero
effect on sim/CSVs.

### (NEW) Metric vs topological interaction — and why the cap is *not* a perf feature

`--max-neighbors N` / `--topological` (default off) exists because the metric rule is
**biologically wrong**, not because it is slow. Under the metric rule a creature averages
over every neighbour inside `--perception`, so a fish in a dense school integrates over
~150 others. Real animals don't: starlings track a roughly **fixed ~6–7 nearest
neighbours regardless of flock density** (Ballerini et al., PNAS 2008 — the canonical
topological-interaction result), and fish studies find fewer still (Katz et al. and
Herbert-Read et al., PNAS 2011, both find golden shiner / mosquitofish motion is largely
explained by the 1–2 nearest). `--topological` = 7 is the Ballerini number and the
conservative end of the fish range; use 1–4 to test the stronger fish-specific claim.

Two design decisions worth not re-litigating:
- **Only the interaction percepts are capped** (`AVG_NEIGHBOR_DX/DY`,
  `AVG_NEIGHBOR_HEADING_X/Y`, `POPULATION_FWD/LR`, and the count used to average them).
  `NEIGHBOR_COUNT` and `POPULATION_DENSITY` still report the **true** number in range,
  because crowding is a percept in its own right and evolution shouldn't lose access to
  it just because attention is bounded.
- **Selection is by `(distanceSq, index)`, a total order**, so both the chosen set and
  the order it is summed in are uniquely determined — equidistant neighbours can't
  reorder, and nothing depends on the library's sort internals. That is what keeps a
  capped run reproducible on another toolchain or under wasm.

**The surprise, and the reason this is filed under design rather than performance:** the
cap is **throughput-neutral (−7 % to +8 %)**, not the large win predicted from the
density scaling. Visiting a candidate to *decide* it isn't in the nearest 7 costs nearly
what processing it costs — the saved work (a vel load, a sqrt, two dot products) is small
next to the position load you must do anyway. The first implementation, which
materialised the candidate list and `partial_sort`ed it, was **actively slower
(0.85–0.99×)**: writing ~150 candidates per creature per step is more memory traffic than
the work it skips. The shipped version keeps a small sorted stack array and rejects a
too-far neighbour on **one comparison**, which is what got it back to neutral. Take the
flag for the biology; the speed is a wash.

---

## 5. Agreed forward plan, frozen baseline & open questions

### Sequence (single-variable discipline)
1. **Currents + spatial food** — **DONE & CLOSED** (v0.6.0–0.8.0). Result in §3.
2. **A predator** —
   - **Stage A (scripted/immortal/fixed-count): DONE & EXPLORED** (v0.9.0–0.9.1).
     Built, tuned across many runs, schooling confirmed. Full log in **§10**.
   - **Stage B (predator energy economy): BUILT & VERIFIED** (v0.10.0; §12). Mechanics
     correct and conservative; the **oscillator tuning** (find the band where neither
     population troughs to zero) is the **active real-run + replicates task** — short
     sandbox runs are non-representative (§11.1/§12). Three economy regimes are
     characterized in §12.
   - **Stage C (neural predator): ACTIVE NEXT** (after the Stage-B oscillator is tuned).
     Spec in §9 + §11.
3. **NEAT-style complexification** — last; only once there's escalating demand.
- **Camouflage + spatial background** and **body-size** fold in around Stage B/C (the
  speciation phase), after a stable energy-coupled predator.
- **Phase 4 verdict is in** (§3 tier-2 undulation entry): undulation is **reachable** and
  **climbable**, 20-cell fidelity is **unaffordable** (~25–30× budget), an 8-cell soft gait
  swims at **~2× budget** — so tier-2 escalation is now a **Phase 5 body-size / fidelity
  trade**, not a yes/no. The gait-maintenance test (§4.4) is still open.

### Open questions — RESOLVED this session (were "not yet decided")
1. **Detection asymmetry → made a tunable flag.** `--predator-perception` default
   **150** vs prey perception 80 (ambush asymmetry on by default); set `=80` for a
   symmetric-sight experiment. Not hardcoded — a dial to sweep.
2. **Capture model → instant-kill on contact** (`--predator-capture-radius`, default
   14), the Stage-A simplicity lean. Kill rate is capped by `--predator-cooldown`
   (default 3s digestion) — the first anti-spiral knob.

### FROZEN BASELINE (this is the environment the predator enters)
```
--current 0 --current-mode gyre --food-patches 6 --food-drift --gene-cost 0.01 --move-cost 0.00007 --min-speed 0 --seed 42 --log-every 2000
```
Rationale per flag:
- `--move-cost 0.00007` — **keep permanently.** Interior cruise speed; the predator
  needs it. Calibrated, population-healthy.
- `--gene-cost 0.01` — keep for **continuity** (in every run this sequence); it does
  NOT meaningfully tame bloat — don't read length as fitness.
- `--min-speed 0` — keep. With costly thrust, drifting (speed 0, zero kinetic cost) is
  a real option; forbidding it re-confounds.
- `--current 0` — **start the predator in CALM water** (Harit's call). Debug/balance
  the most failure-prone piece in the simplest environment first; add current as the
  *next* single variable.
- `--current-mode gyre` + `--food-patches 6` + `--food-drift` — kept in the string
  even though **`--food-drift` is a no-op at current 0** (no flow to advect). Keeping
  them means turning the current on for phase 2 is a *one-flag* change, not several.
- **Phase plan:** predator stable in calm → then **`--current 150`** (the ride
  regime, the interesting one). **`--current 115` (midpoint) DROPPED.**
- **Food-patches caveat:** patchy food concentrates prey → may be *more* prone to the
  extinction spiral. Lean keep (spatial structure is the point); if the first predator
  runs show instant prey collapse, **flatten to `--food-patches 0` (uniform)** as the
  first diagnostic knob.

### Stage-B oscillator — recommended starting arena (the active tuning task)
Once turning on `--predator-economy`, the goal is the band where **neither population
troughs to zero**. §12 characterizes three regimes; the practical starting point that
avoids the worst degeneracies:
- **Isolation arena, max-pop relaxed** so prey can oscillate rather than pin at a cap:
  `--food-patches 0 --target-food 2000 --food-spawn 12 --max-pop 600 --min-pop 20`.
- **A fixed meal, not absorb-reserves**, to start: `--predator-meal 14` (absorb-reserves
  — the default — is calorie-dense and explodes when prey are fat; switch to it later
  once the chase/economy are balanced). `--predator-metabolism 2 --predator-repro-energy 60`.
- **First knobs (one at a time):** `--predator-meal`, `--predator-metabolism`,
  `--predator-repro-energy`. Then **ease the chase** (`--predators 4`,
  `--predator-cooldown 5`) — the chase is usually the binding constraint (too many fast
  predators crash prey before the economy throttles them; §12).
- **Use the serial build** (`-DEVO_NO_OPENMP=ON`) for deterministic single runs, or
  run 5–10 replicates; ≥1.5M steps for the dynamics to settle.

### Open questions for the next chat (predator design — NOT yet decided)
*(Resolved — see the "Open questions — RESOLVED" block above. Kept this heading so
old cross-references still land.)*

### Parked (do NOT build)
- A browser/WebApp version. Notes for later: CPU/WASM via Emscripten is feasible (the
  SFML-free core is well-positioned); GPU = a separate WebGPU rewrite; "logs to a
  server" needs a backend → for now just define the endpoint contract.
- Midpoint-current confirmation; food-drift-off "fight" regime; multi-seed currents
  error bars (all currents-thread refinements, not needed to proceed).

---

## 6. Still-hardcoded (not yet flags)
Min reproduction age (5.0s), reproduction energy cost (±12), and a few others remain
hardcoded in `world.hpp`. **Now flags as of 0.8.0:** kinetic move cost (`--move-cost`).
**Now flags as of 0.10.0:** the predator **Stage-B energy economy** — `--predator-economy`
plus `--predator-metabolism / -move-cost / -repro-energy / -newborn-energy / -meal /
-max-age` (§9/§12). Also new: the deterministic serial build (`-DEVO_NO_OPENMP=ON`).
Still **code changes, not flags:** predator **Stage C** (a neural/evolving predator
population — §9), logistic/density-dependent food regrowth, mutation-operator
rebalancing, camouflage / spatially-varying background, a body-size trait, a
per-creature spatial-structure snapshot / Moran's I, the renderer `setView` fix (§4),
and in-life (Hebbian/NEAT) learning.

## 7. Key metric definitions
- **genetic_diversity**: RMS distance of a genome from the population mean in
  (source,sink) connectivity space (~14 scale; exact via a variance identity; ≈
  brute-force all-pairs). Coupled to genome length (shorter genomes → smaller value).
- **species**: count of distinct SimHash buckets (16 random hyperplanes, origin-
  centered). Collapses toward 1 on convergence, ≈ population when diverse. Cleaner
  convergence signal than diversity. **Blind to non-connectivity traits** (e.g. a
  future body-size gene).
- **(NEW) flow_align**: mean cosine between each creature's heading and the local
  ocean-flow direction. **+1** = swimming with the flow (riding/surfing); **−1** =
  against (fighting upstream); **0** = crosswise *or* no current active. Read **with
  `avg_speed`**: low speed = drifting regardless of heading; high speed + positive =
  surfing downstream; high speed + negative = fighting upstream. Computed in the
  snapshot pass (not the hot loop). Verified sign convention (±1 / 0).
- **(NEW) genome colour**: a 3-D random projection of the same connectivity vector the
  species SimHash uses → RGB. Similar genomes → similar colours; lineages drift
  smoothly. **It is a low-dimensional *shadow* of the species metric, NOT independent
  evidence of genetic structure.** 3-D is lossy (similar colour ≠ similar genome).
- **(v0.9.0) predator_align**: mean cosine between a prey's heading and the
  **away-from-nearest-predator** direction, gated to prey with a predator within their
  own perception (80). **+1** flee, **−1** toward, **0** crosswise / no threat. Read
  WITH `avg_speed`. **Session verdict (important): this measures the WRONG thing for
  this sim.** It assumes prey flee *individually and radially*; the prey's actual
  anti-predator strategy is **schooling/aggregation**, which is invisible to it. It
  sits near a **culling-artifact floor (~0.2)** — prey heading at a predator die, so
  the surviving sample is mildly enriched for facing away regardless of any evolved
  reflex — even when strong schooling is happening. Don't read evasion off it.
- **(v0.9.1) neighbor_align** *(the schooling instrument — use this instead)*: for
  each prey, cosine between its heading and the **mean heading of prey within its
  perception (80)**, averaged over prey with ≥1 neighbour. **+1** = locally aligned
  (a polarized school); **0** = disordered. **Local on purpose** — several schools
  pointing different ways each read high (a *global* polarization cancels them to ~0).
  Empirically clean: **0.97 in a schooled run vs 0.06 in a non-schooled run** (§10).
  Caveat: it measures **polarization**, so a group *milling* up-and-down a common axis
  (bidirectional) reads ~0 even though it looks coherent — that's a real failure mode
  we hit (`school_pred`).
- **(v0.9.1) neighbor_count**: mean number of prey within perception (local crowding /
  clumping). A school is **aligned AND clumped** → read with `neighbor_align`:
  high+high = a tight school (saw ~27); high align + low count = loose comigration;
  low align = no polarized school regardless of count.
- **(v0.10.0) predator_births_interval**: predators budded during the interval (diff of
  a cumulative total). **0 in Stage A** (economy off). With the economy on, it is the
  predator birth rate — read against the two death columns to see which way the
  oscillator is moving.
- **(v0.10.0) predator_deaths_starv / predator_deaths_age**: predator deaths during the
  interval by cause (energy < 0 / past `--predator-max-age`). **Both 0 in Stage A.**
  `births − (starv+age) > 0` ⇒ predators rising; `< 0` ⇒ falling. Starvation is the
  coupling channel (prey crash → predators starve); age is usually 0 unless
  `--predator-max-age` is set.
- **(v0.10.0) predator_avg_energy**: instantaneous mean energy over the **live**
  predators (0 if none). **Reported as 0 in Stage A** — the energy field is an inert
  seeded constant there, so it is gated to economy-on to stay unambiguous. **This is
  the early-warning gauge when tuning:** a predator population can hold its count while
  its mean energy slides toward zero and then crash. Keep it **above 0**; read it the
  way you read prey `avg_energy`. Break-even intuition: a predator eating once per
  cooldown breaks even near `meal ≈ cooldown·(metabolism + move_cost·speed²)` (≈8.5 at
  defaults), ~1.4× for a breeding surplus (§12).

## 8. Collaboration notes
Verify claims with evidence (compile + run + numerical checks), not assertion. Own
mistakes plainly. Flag verified vs not. Single-variable experiments. Harit
over-architects/optimizes vs shipping by his own admission — bias toward building and
measuring the next concrete thing rather than expanding scope.

**Mistakes owned this session** (kept as a record of the method working): the ~44k
autocorrelation period (artifact; FFT corrected to ~6.3k); the c300 "+0.27" oversold
from a *truncated* run (full 2M → steady ~+0.06); "currents conclusively dead at any
strength" (over-extrapolation from two points — c300 then move-cost disproved it);
the sandbox prediction that move-cost 7e-5 would floor the population (real 1M runs
didn't). Pattern: short-horizon/sandbox/few-point reads were corrected by full real
runs — trust the latter.

**Mistakes owned in the predator session (same method, kept as record):**
- **Misdiagnosed `-O0` from a blank `CMAKE_BUILD_TYPE` cache.** Told Harit the empty
  cache meant an unoptimized binary. It's **cosmetic**: the CMakeLists
  `if(NOT CMAKE_BUILD_TYPE) set(CMAKE_BUILD_TYPE Release)` applies `-O3` via a *normal*
  variable without writing the cache. Proved by replicating his exact configure
  (cache blank, but `CXX_FLAGS = -O3`). The real slowdown was config (≈4000 food =
  ~6× more grid scanning) + thermal throttling, **not** the schooling code (which is
  sampling-time: log-every 2000 vs log-every 1 was only 1.5× in the sandbox).
- **Over-generalized "no polarized schooling exists."** Concluded it from `school_pred`
  (`neighbor_align` 0.06) — but that was one *non-schooling* stochastic run.
  `evade_iso_again` (identical command) hit `neighbor_align` 0.97. Schooling is real;
  the metric is right; the outcome is stochastic. Corrected by running it again.
- Misread the HUD `Pred: 6` as `Pred: 0` (predators ran the whole time per the CSV).

---

## 9. PREDATOR PLAN — Stage A & B DONE; Stage C is the path

> **Status:** Stage A is **built and explored** (v0.9.0–0.9.1) — see §10. Stage B (the
> energy economy) is **built and verified** (v0.10.0) — see the session log in **§12**;
> its mechanics are correct/conservative and the **oscillator tuning** is the active
> real-run task (§11/§12). **Stage C (neural predator) is now the next step** — spec
> below + §11. The staged plan below is unchanged and still correct; A and B are now
> annotated DONE.
>
> Frame (still the governing idea): **a predator is not an entity, it's a second
> energy loop coupled to the prey-food loop.** Stage A deliberately had *no* such loop
> (immortal predators) to isolate the chase; **Stage B turned the loop on** (v0.10.0),
> and that coupling is the whole point. Stage C makes the predator itself evolve.

### The build (chosen architecture)
- **A separate `Predator` population with its own energy economy.** Predators burn
  metabolism **+ move-cost** (this is *why* move-cost had to exist), gain energy by
  eating prey, reproduce above a threshold, **starve** below 0. → the **population**
  self-regulates against prey density (prey crash → predators starve → decline → prey
  rebound → predators recover). This negative feedback is what bounds everything.
  *Not* fixed-count immortal predators (can't self-regulate; knife-edge tuning).
- **Separate species, NOT emergent cannibalism on `INTERACT_FIGHT`.** Lets us script
  the predator first, tune the chase against debuggable prey, add one clean variable,
  and measure the arms race as two diverging lineages. (The one-population
  carnivory-emerges experiment is cool but harder, less controllable, and fights the
  single-variable method. `INTERACT_FIGHT` is an adjacency contest, not a pursuit —
  see §4.)
- **Reuses existing machinery:** `creatureGrid_` for nearest-prey queries; the
  food-pickup mechanic for capture (different radius); `foodReserves` for energy; the
  lethal-absorptive transfer from feedback point 1 (absorb victim reserves; **no
  genome-length meat bonus**).

### Staged introduction (do NOT collapse these — each isolates one unknown)
- **Stage A — scripted, immortal, fixed count (~5–10). ✅ DONE (v0.9.0–0.9.1).**
  Hard-coded hunt: nearest prey in detection radius → steer toward it at predator
  cruise → instant capture on contact (capture radius) → digestion **cooldown** →
  wander if none in range. Prey got `PREDATOR_FWD/LR/PROX` sensors. Tuned across many
  runs (§10): the spiral-vs-irrelevance window is real, and **schooling emerged** as
  the evolved anti-predator response. Lessons in §10; the cooldown is the kill-rate
  cap (first anti-spiral knob) exactly as predicted.
- **Stage B — turn on the predator energy economy. ✅ DONE (v0.10.0; §12).** Predators
  breed when fed, starve when not → the real coupled (Lotka-Volterra-ish) oscillator.
  Built behind `--predator-economy` (off by default ⇒ Stage A byte-for-byte). Meal =
  absorb victim reserves (default) or fixed `--predator-meal`; burn = metabolism +
  kinetic move-cost; bud above `--predator-repro-energy` (energy-conserving); starve
  below 0; no predator reseed floor (extinction observable). **Mechanics verified
  conservative** (energy conserved on capture/birth, deaths partition, no NaN; §12).
  **Still to do (real-run task):** tune metabolism / meal / repro so **neither
  population's trough hits zero** — §12 shows the three regimes and why the chase is
  usually the binding constraint. *(The **body-size** trait still folds in here or just
  after — §4 — as its own variable: big/strong/fast-but-hungry vs small/agile/cheap,
  size refuges.)*
- **Stage C — make the predator neural.** Replace the scripted policy with the same
  genome/NeuralNet/sensor machinery the prey use, as its own evolving population with
  separate selection. **Now it's a genuine Red Queen arms race** — the real demand
  gradient the whole plan targets. Only earns its keep once A & B are stable.

### Sensors (mandatory — evasion can't evolve if prey can't perceive threat)
- **Prey:** `PREDATOR_FWD` / `PREDATOR_LR` (bearing to nearest predator, relative to
  heading) + a proximity/urgency channel (`1/dist` or falloff).
- **Predator (Stage C):** the mirror — `PREY_FWD` / `PREY_LR` + prey-proximity.
- **Bumps `NUM_SENSES` again** → predator runs are a **fresh genome baseline**, not
  comparable to currents CSVs. Expected (same as the 36→38 change). Update the doc's
  "0.8.0 currents runs are comparable" note once this lands.

### Metric — wire on Stage A day one (the `flow_align` lesson)
- **`predator_align`** = mean cosine between each prey's heading and the
  **away-from-nearest-predator** direction, **gated** to prey with a predator actually
  in range. Positive = fleeing; ~0 = oblivious. Without it you cannot distinguish
  "evasion evolved" from "the slow ones just died." Build the readout *with* the
  feature.
- Also log: **predator population**, **kills/interval**, and **decompose
  `deaths_interval` into predation / starvation / age** ("prey deaths up" is
  meaningless until you know which kind).

### Emergent payoffs (expected to *appear*, NOT to be hand-coded)
- **Burst-and-rest pursuit** — quadratic move-cost makes sustained max-speed chasing
  ruinous → efficient predators should burst when close, coast otherwise. Most likely
  cool spontaneous behaviour; falls out of the energy economy.
- **Flocking-as-defense** — prey already have neighbour sensors; selfish-herd /
  dilution / predator-confusion can emerge as schooling.
- **Size niches** — once the size trait is in (Stage B+).

### Failure modes (watch from row one)
- **Extinction spiral** (predator too effective → eats all prey → both die): watch
  prey **pop_min hitting the reseed floor early**. First diagnostic knob: uniform food
  (`--food-patches 0`).
- **Perfect evasion / starvation** (prey too evasive or predator too weak → predators
  never eat → die out, prey revert): watch **predator pop → 0**.
- Stage A's whole job is finding the speed/radius/cooldown window between these two.

### Two open decisions before coding — RESOLVED (see §5)
Detection asymmetry → `--predator-perception` (default 150, tunable). Capture model →
instant-kill (`--predator-capture-radius`, capped by `--predator-cooldown`).

### Suggested first action for the next chat
**Stage A is done — do NOT rebuild it.** Start **Stage B** (predator energy economy)
per **§11**. Before any quantitative claim, adopt the new methodology rule
(replicates / serial build) from §10–§11 — runs are non-deterministic and a single
run is not representative.

---

## 10. STAGE A SESSION LOG — every run, decision, and finding

This is the move-by-move record of the predator session so it doesn't have to be
re-derived. Read §9 first (the plan), then this (what actually happened).

### 10.1 What was built (and the decisions behind it)
- **Intent inference:** the session opened with a tarball + context + flags doc and
  **no instruction**. Read §9's "active next step" + "suggested first action" as the
  brief and **built Stage A** rather than asking — matching Harit's ship-over-process
  bias. (Correct call; confirmed by him engaging with results immediately.)
- **Both "open decisions" → tunable flags, not hardcoded forks** (dials to sweep):
  detection asymmetry = `--predator-perception` (default 150 > prey 80); capture =
  instant-kill on contact (`--predator-capture-radius` 14), rate-capped by
  `--predator-cooldown` (3s). Defaults: `--predators 0` ⇒ baseline unchanged, tests
  green. Chosen predator defaults: speed 110 (prey cruise ~82, sprint 150), so a
  predator out-cruises a forager but a sprinting prey can pull away — real tension.
- **Verification discipline (sandbox has no cmake/SFML):** core compiled with g++
  `-fopenmp`; `core_tests` kept green (2030 → **2048** after predator + schooling
  tests, 0 failures); a no-SFML **`predator_smoke`** integration harness asserts the
  run-level invariants (pop floor, finite positions, predation occurs, predator_align
  ∈ [−1,1], **deaths == predation+starvation+age** exact partition, predator count
  fixed); the SFML renderer was **type-checked against a hand-rolled `sf::` stub**
  (not runtime-verified — flagged). Delivered as full drop-in tarballs.
- **Two bugs caught during the build** (from a prior compaction, fixed before ship):
  a missing `WorldStats` closing brace; the `drawHud` function header consumed by an
  edit. Both fixed; clean rebuild from the shipped tarball verified.

### 10.2 Experiment log (chronological)
All runs share this **BASE** unless noted; only the **delta** is listed per run:
`--current 0 --current-mode gyre --gene-cost 0.01 --move-cost 0.00007 --min-speed 0 --seed 42 --log-every 2000 --no-render`
The "isolation arena" runs add: `--food-patches 0 --target-food 2000 --food-spawn 12 --max-pop 350`.

| # | run / CSV | delta from BASE | steps | outcome (key numbers) | insight |
|---|-----------|-----------------|-------|------------------------|---------|
| 1 | **pred6** `pred6.csv` | `--predators 6 --food-patches 6 --food-drift` (target-food default 600) | 10M | **Extinction spiral.** Pop hit floor (50) by step 10k, ≤50 for 87% of run; births ~9 vs deaths ~62 (held up by reseeds); energy-rich (137) but mate-starved (low density); species/pop ~1.0 (random pool, no lineage); deaths 69% predation. | Predators too strong **with food patches** → dilution/spiral. The "sitting still" Harit saw was **station-keeping/circling** (avg_speed ~96, not 0). `predator_align` is pure noise at the floor (tiny N). |
| 2 | **pred4** `pred4.csv` | `--predators 4 --food-patches 0` (target-food 600) | 1M | Healthy (pop ~180, off-floor 94%). Death mix flipped to **76% starvation / 18% predation**. avg_speed jumped to ~140 (foraging scramble). species/pop ~0.66. predator_align +0.29. | Fewer predators + uniform food **escaped the spiral**, but flipped the dominant pressure to **food**. "Sitting still" gone (speed 140). |
| 3 | **pred4_food** `pred4_food.csv` | `--predators 4 --food-patches 0 --target-food 1200` | 2M | Still **~70% starvation / ~22% predation** — food barely moved the mix. species/pop **0.19** (hard convergence). predator_align +0.33. | **KEY:** predation is a *fixed absolute rate* (predator count × cooldown ≈ 32 kills/interval); starvation/births *scale with pop*. More food → bigger pop → predation's **share stays ~20%**. You cannot buy predation-share with food — only more predators or a smaller pop. |
| 4 | **evade_iso** `evade_iso.csv` (v0.9.0) | isolation arena, `--predators 6` | 2M | Starvation crushed (~5/interval, energy ~400). Death mix **41% predation / 55% age / 4% starvation** — the clean arena. But predator_align **dropped to ~0.21** and species/pop stayed **0.82** (no sweep). Visually: schools, **uniform blue**. | Removing starvation removed the *strong directional* selection; **age (neutral) now dominates** → weak selection, no convergence. predator_align ~0.21 ≈ **culling-artifact floor**. |
| 5 | **schools_ctrl** `schools_ctrl.csv` (v0.9.0) | isolation arena, `--predators 0` | 2M | Visually: **no schools** — spread out, high colour diversity. | First control. Schools vanish without predators ⇒ **predator-driven**. Motivated building `neighbor_align` (v0.9.1) since predator_align couldn't see it. |
| 6 | **school_pred** `school_pred.csv` (v0.9.1) | isolation arena, `--predators 6` | 2M | `neighbor_align` **0.06** (NO polarization), neighbor_count 3.75, species/pop 0.80, 42% predation. | This run **did NOT school** (loose mill). Led to the (wrong) conclusion "no polarized schooling." |
| 7 | **school_ctrl_final** `school_ctrl_final.csv` (v0.9.1) | isolation arena, `--predators 0` | 2M | `neighbor_align` 0.06, neighbor_count 3.27, species/pop **0.94**. | Control: near-neutral selection ⇒ max diversity (0.94). The #6-vs-#7 comparison looked **null** — but it was **N=1 each** and #6 happened to be a non-schooling roll. |
| 8 | **evade_iso_again** `evade_iso_again.csv` (v0.9.1) | **identical command to #6** | 2M | `neighbor_align` **0.85→0.97** (strong polarization), neighbor_count → **27** (tight school), species/pop 0.80 (**multicoloured** — diverse genomes), 42% predation. **Phase transition at ~step 1.2M** (flat ~0.05 → 0.97). | **THE BIG ONE.** Same command as #6, opposite result. Proves: (a) polarized schooling **is real and evolves** under predation; (b) `neighbor_align` **is** the right metric (0.97 vs 0.06); (c) it's a **multicoloured** school ⇒ behavioural coordination *decoupled* from genetic convergence; (d) runs are **non-deterministic** ⇒ single runs aren't representative. |

### 10.3 The metric saga (a real lesson)
- `flow_align` taught "wire the metric with the feature." We did — `predator_align` on
  Stage A day one. **But it measures the wrong axis.** It assumes *individual radial
  flight*; the prey evolved **schooling** (a dilution/confusion strategy), which is
  invisible to a per-prey away-vector cosine. predator_align hovers at its
  **culling-artifact floor (~0.2)** even mid-schooling (prey heading at a predator die →
  survivors mildly enriched for facing away, regardless of evolution).
- Built **`neighbor_align`** (local polarization) to measure schooling directly. It
  works: 0.97 (schooled) vs 0.06 (not). **But it too has a blind spot:** it measures
  *polarization*, so a school that **mills** (moves up-and-down a common axis,
  bidirectional) reads ~0 while looking coherent. So "moving vertically" ≠ aligned.
- **Net lesson:** two heading-cosine metrics in a row each missed the real behaviour
  because the behaviour is **spatial** (clump) and sometimes **bidirectional** (mill).
  `neighbor_count` (clumping) and `species`/colour (is selection happening at all?)
  are the robust companions. Read the cluster of metrics together, never one alone.

### 10.4 The headline finding — predator-induced schooling, but stochastic
- **Real:** predation evolves a genuine, tight, polarized, *multicoloured* school
  (`neighbor_align` 0.97, `neighbor_count` 27) as a **phase transition** — flat for
  ~1M steps, then a schooling lineage sweeps around step ~1.2M and locks in.
- **Stochastic:** the *identical* command (same `--seed 42`) produced 0.06 in one run
  and 0.97 in another, because of **OpenMP non-determinism** — `--seed` only pins the
  single-threaded parts (spawning, deaths, interactions, predators); the parallel
  sensor/movement phase uses **per-thread RNGs**, so trajectories diverge and can land
  in different attractors (loose mill vs tight school). Documented behaviour in
  `flags.md`; it just bit *qualitatively* for the first time.
- **Consequences:** (1) a single run is **not** representative; (2) schooling needs
  **≥1.5M steps** to even appear, so short runs systematically miss it; (3) the
  #6-vs-#7 "null" was an artifact of N=1. ⇒ **the operating rule going forward is
  replicates (or the serial build).** See §11.1.

### 10.5 The performance "regression" (a red herring — fully resolved)
Harit reported ~4k→500 steps/sec and asked if the schooling code caused it. It did
not. The chain:
- **It's the config, mostly.** steps/sec falls with food×population. From his own
  CSVs (all pre-schooling): pred6 (600 food, pop 50) **12,608/s**; pred4 (600, 111)
  6,170; pred4_food (1200, 93) 4,804; **evade_iso (2000 food, pop 350) 2,767** — and
  evade_iso is the *same* config as the school runs, on the *old* binary. `--target-food
  2000` ⇒ ~4000 food items (cap is 2×) ⇒ ~6× more food-grid scanning per creature per
  step. His "4k" was pred4_food; the school config is just the heavy end.
- **Not the schooling metric.** `computeStats` runs once per `--log-every` (2000)
  steps; its O(N²) schooling pass at N≤350 is ~0.1M ops (~250 ops/step amortized vs
  ~1M/step of sim). Sandbox A/B: log-every 2000 = 1140/s vs log-every **1** (compute
  every step, 2000× more often) = 765/s — only **1.5×** even pathologically. The hot
  loop (`world.update()`) was **byte-identical** v0.9.0→v0.9.1 (untouched).
- **Not `-O0`** (I wrongly claimed this). His `grep CMAKE_BUILD_TYPE` showed blank,
  and I said blank = unoptimized. **Blank is cosmetic:** the CMakeLists
  `if(NOT CMAKE_BUILD_TYPE) set(CMAKE_BUILD_TYPE Release)` applies `-O3` via a normal
  variable without writing the cache. Replicated his exact `cmake .. >/dev/null`:
  cache reads blank but `CXX_FLAGS = -O3`. His binary was optimized all along.
- **It's thermal throttling.** His `top`: 800% CPU (OpenMP fine), 16h uptime, 189 min
  CPU on the PID from back-to-back 2M runs. All-core sustained load on a WSL2 laptop
  clocks the cores down — "100% busy," fewer cycles/sec. Same binary at ~1370/s with
  big swings vs evade_iso's steady 2767 = heat. Fix: Windows "Best performance" + AC,
  let it cool, cold short run to confirm it bounces back.
- **`rebuild_evo.sh` improvement (logged):** make Release explicit and stop hiding
  configure output, so build type is never ambiguous again — add
  `cmake .. -DCMAKE_BUILD_TYPE=Release` (the script already does `rm -rf build`), and
  echo the build type + `CXX_FLAGS` after configure. (His script is otherwise good —
  robust nested/flat extraction.) **All CSVs are scientifically valid regardless of
  optimization** — `-O0` vs `-O3` changes speed, not results.
- **Follow-up (v0.9.2): the O(n²) schooling scan → O(n) temp grid** (a reviewer
  flagged it). It was *never* the slowdown (sampling-time, once per log-interval), but
  it was a latent cliff for large N. Implemented as a fresh `SpatialGrid` built from
  the live creatures inside `computeStats` — **not** by reusing World's
  `creatureGrid_`, which is **stale** after `update()` (start-of-update positions +
  indices into the pre-erase/pre-reseed vector → out-of-range / wrong creature).
  Verified **identical** to the brute-force version (differential match to 7 decimals;
  tests 2048/0).

### 10.6 What Stage A established (the takeaways)
1. The spiral-vs-irrelevance window is real; the knobs behave as designed (cooldown
   caps kill rate; perception asymmetry shapes feasibility).
2. The **isolation arena** (superabundant food + pop cap ⇒ starvation ≈ 0) is the
   right setup to study *behaviour*, because it makes **predation the dominant
   selective force** instead of food. The "ecological" config (food patches, scarce
   food) is for studying *population dynamics* instead.
3. **Predation drives schooling** — a genuine, polarized, multicoloured, evolved
   anti-predator behaviour — confirmed by a no-predator control and a positive
   `neighbor_align` (0.97). It arrives as a **stochastic phase transition** (~1.2M
   steps, not every run).
4. Metrics: `neighbor_align` + `neighbor_count` + `species` together read schooling;
   `predator_align` does not (wrong axis). Don't trust any single metric.
5. **Runs are non-deterministic** under OpenMP ⇒ **replicates, not single runs.**

---

## 11. DIRECTION FORWARD — Stage B BUILT (v0.10.0); tuning + Stage C ahead

> **Status update (this session):** the Stage-B spec in §11.2 below is now **built and
> verified** (v0.10.0; move-by-move in **§12**). The meal model was implemented as
> *both* options (absorb-reserves default + fixed `--predator-meal`), predators are
> immortal-until-starved by default (`--predator-max-age` opt-in), the serial target
> (§11.1) exists and is verified deterministic, and `predator_smoke` was extended with
> a live-economy Stage-B harness. **What remains from §11.4 is items 3–4:** *tune* the
> oscillator on real long runs (§12 characterizes the regimes and the starting point),
> then **Stage C**. §11.2/§11.3 are kept as the original spec for reference.

### 11.1 Operating rule first (the methodology upgrade from §10.4)
**Stop drawing conclusions from single runs.** Either:
- **Replicates:** run the same command 5–10× (vary nothing) and report the
  *distribution* — e.g. "schooling emerged in k/N runs, onset ~X–Y steps" — not one
  trajectory; **or**
- **Serial build:** build the no-OpenMP target for *deterministic* single runs at a
  fixed seed (slower, but reproducible) — good for definitive cause-and-effect.
Use runs **≥1.5M steps** for anything involving emergent collective behaviour.
This rule matters *more* in Stage B, where the coupled oscillator adds its own
sensitivity on top of the RNG non-determinism.

### 11.2 Stage B — turn on the predator energy economy
The one new thing on top of an already-balanced chase (do NOT also change the chase):
- **Mechanics:** predators gain energy by capturing prey (**absorb the victim's
  `foodReserves`; NO genome-length meat bonus**), burn **metabolism + move-cost**
  (this is *why* move-cost exists), **reproduce** above an energy threshold, **starve**
  below 0. `predator_pop` becomes time-varying (already a CSV column since 0.9.0).
- **Result sought:** the coupled (Lotka–Volterra-ish) oscillator — prey crash →
  predators starve → predator decline → prey rebound → predators recover. **Tune so
  neither population's trough hits zero.**
- **Likely new flags** (mirror the prey economy; all opt-in, predators-off preserves
  Stage A): `--predator-metabolism`, `--predator-move-cost` (or reuse `--move-cost`),
  `--predator-repro-energy`, `--predator-newborn-energy`, `--predator-max-age` (or keep
  immortal-until-starved), and a meal model (absorb-reserves vs fixed `--predator-meal`).
  Keep Stage-A flags (`--predator-perception/-speed/-capture-radius/-cooldown`).
- **Reuse:** `creatureGrid_` (nearest-prey), the capture mechanic, `foodReserves`,
  the prey reproduction/mutation path (predators are a second `Predator`-genome-less
  population in Stage B; genome/neural is Stage C).
- **Verification (same discipline):** `core_tests` stays green at `--predators 0`;
  extend `predator_smoke` with an energy-conservation / non-extinction invariant;
  decompose predator deaths (starvation/age) if predators become mortal.
- **Watch from row one:** predator `pop → 0` (too weak / prey too evasive) or prey
  floor (too strong); the trough-to-zero failure on *either* line. First knobs: meal
  value, predator metabolism, repro threshold.

### 11.3 Baselines to run Stage B against
- **Behaviour-study (isolation) arena** — predation is the main selective force:
  `--food-patches 0 --target-food 2000 --food-spawn 12 --max-pop 350` + BASE. (For
  Stage B you may need to *relax* `--max-pop` so the prey population can actually
  oscillate rather than pin at the cap — decide explicitly; the cap was a Stage-A
  device to kill starvation.)
- **Ecological arena** — population dynamics with scarcity: the FROZEN BASELINE in §5
  (food patches, default food). This is where the Lotka–Volterra coupling is most
  visible. Add `--current 150` only as a *later* single variable (phase 2), per §5.

### 11.4 Concrete first actions for the next chat
1. ✅ **Done (§12):** the serial target is built and confirmed deterministic
   (`-DEVO_NO_OPENMP=ON`, byte-for-byte across identical-seed runs).
2. ✅ **Done (§12):** meal model = absorb-reserves (default) **and** fixed
   `--predator-meal`; predators immortal-until-starved by default (`--predator-max-age`
   opt-in).
3. ✅ **Done (§12):** Stage B built (economy + 7 flags), tests green at `--predators 0`
   and with the economy live; tarball shipped. **→ NOW: tune** meal/metabolism/repro on
   real long runs (≥1.5M steps, replicates **or** the serial build) **until neither line
   troughs to zero.** Start from the §5 "Stage-B oscillator" arena (isolation, max-pop
   relaxed to 600, fixed meal 14, min-pop 20); first knobs meal/metabolism/repro, then
   **ease the chase** (`--predators 4`, `--predator-cooldown 5`) — §12 shows the chase
   is usually the binding constraint.
4. Only after the oscillator is stable: **Stage C** (neural predators → the real Red
   Queen arms race) and the **body-size** trait (size niches/refuges), per §9.

> Schooling result to carry as *done*: the screenshot, `neighbor_align` 0.97, and the
> phase-transition curve in `evade_iso_again.csv`. It's solid — just stochastic.

---

## 12. STAGE B SESSION LOG — the energy economy (build + tuning characterization)

> **One-line summary:** the predator energy economy is **built, conservative, and
> tested** (v0.10.0); the **oscillator tuning** is the open real-run task. Three
> economy regimes were characterized in the sandbox; the sustained-coexistence sweet
> spot lies between them and needs long runs + replicates (or the serial build).

### 12.1 What was built (all behind `--predator-economy`, off by default)
- **`predator.hpp`** — `Predator` gains `energy`, `age`, `alive` (all **inert in
  Stage A**; a doc comment marks the A/B split).
- **`config.hpp`** — 7 economy params: `predatorEconomy` (bool), `predatorMetabolism`
  (2.0), `predatorMoveCost` (<0 ⇒ reuse prey `--move-cost`), `predatorReproEnergy`
  (60), `predatorNewbornEnergy` (30), `predatorMeal` (<0 ⇒ absorb victim reserves),
  `predatorMaxAge` (0 ⇒ immortal-until-starved). `dump()` extended.
- **`world.hpp`** — 3 cumulative counters (`totalPredatorBirths`,
  `totalPredatorDeathsStarvation`, `totalPredatorDeathsAge`); predator energy seeded at
  spawn; **`updatePredators` rewritten** with the economy gated on `econ`: each step a
  predator burns `metabolism + moveCost·speed²`; on a capture it gains the meal (absorb
  victim `foodReserves` or fixed `--predator-meal`); above `--predator-repro-energy` it
  **buds** a child (parent pays `--predator-newborn-energy`, so budding **conserves
  energy**); below 0 energy it **starves**; past `--predator-max-age` (if set) it dies
  of age. Newborns appended / dead erased after the loop (mirrors the prey `remove_if`).
  **The Stage-A branch (`econ==false`) is the original code path verbatim.**
- **`stats.hpp`** — 4 columns (`predator_births_interval`, `predator_deaths_starv`,
  `predator_deaths_age`, `predator_avg_energy`); `predator_avg_energy` is **gated to
  economy-on** (reports 0 in Stage A, where the field is an inert constant).
- **`main.cpp`** — 7 flags + help; per-interval predator-birth/death diffs; a console
  economy segment `pE <energy> p+<births> p-<deaths>` (printed only when predators are
  alive and the economy is on).
- **`CMakeLists.txt`** — bumped to **0.10.0**; added `option(EVO_NO_OPENMP ...)` (OFF)
  that skips the OpenMP find/link ⇒ a **deterministic serial** build.

### 12.2 Verification (compile + run + numerical, per §8)
- **`core_tests`: 2065 / 0** (was 2048; **+17** economy checks), in **both** the OpenMP
  and serial builds. New `test_predator_economy()` covers: (1) economy-OFF ⇒ energy is
  an untouched constant and predators are immortal/fixed-count even with captures; (2)
  economy-ON, no prey ⇒ every predator starves, `predator_deaths_starv == initial
  count`, no births; (3) reproduction + **energy conservation** — a predator above
  threshold buds exactly one child and the parent's energy drops by exactly
  `newbornEnergy` (after one step's burn); (4) capture **meal conservation** — a fixed
  meal adds exactly that much to the hunter.
  - *Bug found + fixed during this:* test (4) first used a bare `Creature` (no built
    neural net) placed into `world.creatures`; `World::update` ran `feedForward` on it
    → **segfault** (ASan: null read in `genome.hpp:140`). Fixed by building the prey
    with `makeRandomCreature(...)` (real net) and widening the capture radius so the
    assertion isn't seed-fragile. (Same class of bug as the Stage-A `Creature`-without-
    net trap.)
- **`predator_smoke`** rewritten into a `runWorld(econ,…)` harness run for **both**
  stages. Stage A asserts the predator count is fixed (immortal); Stage B asserts
  `predator_pop ≥ 0`, energy finite (no NaN/inf), the birth/death counters are
  monotone, and **both births AND deaths occur** (the economy is live, not inert).
  Prey-side invariants (floor held, in-torus, deaths partition exactly, predator_align
  ∈ [−1,1]) hold in both. It does **NOT** assert non-extinction — predator extinction
  is legitimate (no reseed floor). **SMOKE OK** in both builds.
- **Stage A preserved:** with the economy off, `predator_smoke`'s Stage-A numbers are
  identical to the pre-session baseline (pop=81, kills=238, starv=325, age=56 at 8000
  steps in the earlier config) — the switch-off path is byte-for-byte.
- **Determinism:** two identical-seed runs of the **serial** build are **byte-for-byte
  identical** (the §11.1 guarantee). The real `main.cpp`+renderer builds and prints
  `evo_boids 0.10.0`; headless output is a **31-column** CSV ending in the 4 economy
  columns.

### 12.3 Tuning characterization — THREE regimes (the real handoff finding)
Short sandbox runs (≤100k steps) are **non-representative** (OpenMP-nondeterministic,
far below the ≥1.5M-step requirement), so these are *qualitative* regimes, not tuned
numbers. The mechanics are correct and conservative throughout (verified: prey deaths
partition every row, `predator_pop ≥ 0`, energy finite, energy conserved on
capture/birth).

1. **Default economy (absorb-reserves) ⇒ predators EXPLODE, prey pinned at the reseed
   floor.** In both arenas, predators ran from 6 up to ~**800–1000** and stayed there
   while prey sat on the `--min-pop` floor (50) with ~0 births — a **degenerate
   equilibrium**, not extinction. *Why:* the prey reseed floor is a **perpetual prey
   supply**; absorb-reserves meals (~30 energy per reseeded prey) ÷ the cheap per-
   predator burn (~0.046/step) supports ~650 predators. Energy-flux math matched the
   observed count. **The isolation arena's reseed floor + abundant food gives predators
   an infinite free lunch.**
2. **Balanced economy (fixed meal ~14) + Stage-A chase (6 predators) ⇒ predators crash
   prey, then STARVE to extinction; prey rebound to the cap.** A self-terminating
   spiral — half a Lotka–Volterra cycle. Once predators are gone there is no reseed to
   bring them back (by design), so the system settles to prey-at-cap, no predators.
3. **Balanced economy + eased chase (3 predators, cooldown 6) ⇒ predators go EXTINCT
   without mattering; prey at the cap.** The predator-irrelevance failure: predators
   die before crashing prey.

**The sustained-coexistence sweet spot is between regimes 2 and 3** and requires real
long runs + replicates (or the serial build) to locate — exactly the §11.4 task. The
`predator_smoke` config (fixed meal 14, default 600 food, uniform, 6 predators, no
max-pop) happens to land in the **least-degenerate** band: predators **17–30**, live
births 61 / starvation 37, prey at the floor — a usable demonstration that the economy
is bounded and live, though prey are still floor-pinned.

### 12.4 The break-even heuristic (use this to aim the first knobs)
A predator that eats once per cooldown breaks even at
**`meal ≈ cooldown · (metabolism + move_cost · speed²)`** (≈ **8.5** at the defaults:
cooldown 3, metab 2, move-cost 7e-5, speed 110), and needs roughly **1.4×** that for a
breeding surplus when fed. Verified at the extremes: meal 4–6 at metab 6–10 ⇒ income
~2/s ≪ burn 6.8–10.8/s ⇒ the initial predators starved before breeding (regime 3);
absorb-reserves (~30) ⇒ income ≫ burn ⇒ explosion (regime 1). **But the chase is
usually the binding constraint:** 6 fast predators over-eat prey to a crash *before*
the economy throttles them (the same `pred6` spiral as Stage A §10), so after setting a
near-break-even meal, **ease the chase** (fewer predators / longer cooldown) as the
next move.

### 12.5 Recommended starting point for the real tuning runs
- **Arena (isolation, relaxed cap so prey can oscillate):**
  `--food-patches 0 --target-food 2000 --food-spawn 12 --max-pop 600 --min-pop 20`.
- **Economy (start with a fixed meal — absorb-reserves explodes on fat prey; adopt it
  later):** `--predator-economy --predator-meal 14 --predator-metabolism 2
  --predator-repro-energy 60`, 6 predators.
- **Tune in this order, one variable at a time:** `--predator-meal` →
  `--predator-metabolism` → `--predator-repro-energy`, then ease the chase
  (`--predators 4`, `--predator-cooldown 5`).
- **Watch `predator_avg_energy` (keep > 0)** and `predator_births_interval` vs
  `predator_deaths_starv` — they read the oscillator directly.
- **Determinism / replicates:** `cmake -S . -B build-serial -DEVO_NO_OPENMP=ON` for
  byte-reproducible single runs, **or** 5–10 OpenMP replicates; **≥1.5M steps**.

### 12.6 Mistakes owned this session (same method, kept as record)
- **Segfault in the new capture test** from re-using the Stage-A "bare `Creature` has
  no neural net" trap (put an un-built creature into `world.creatures` and called
  `update`). Caught with ASan, fixed by `makeRandomCreature`. (Recorded so the trap
  doesn't get re-hit in Stage C, where predators *will* get nets.)
- **First Stage-B sanity run looked like a success** (predators breeding, prey alive)
  but was a **degenerate equilibrium** (prey pinned at the reseed floor feeding ~800
  predators). Recognized only after the energy-flux math; reinforces §10.4 — short runs
  mislead, and "both populations alive" is not the same as "healthy oscillator."
