# evo_boids — CLI reference (v0.17.0)

Headless runner for the evolutionary boids simulation. Defaults reproduce the
original behaviour, so any flag is an opt-in change.

> **v0.17.0 — checkpoint / resume** (`--save-checkpoint`, `--load-checkpoint`,
> `--checkpoint-every`). A checkpoint is the complete world: config, frame counter and
> cumulative counters, **the rng engine state**, every creature (genome, position,
> velocity, energy, timers, and the recurrent neuron outputs), food, predators, the
> pheromone layer, and the rng-seeded ocean vortices and food-patch centres.
>
> **A resume continues the run bit-for-bit** in the serial build. Verified two ways:
> `core_tests` runs 220 steps → checkpoint → 140 more, then reloads and runs 140, and
> compares a deep fingerprint of every creature, predator, food item and pheromone cell;
> and end to end through the CLI, where 600 steps in one go produces a byte-identical
> final checkpoint to 300 + resume + 300. In an OpenMP build a resume is valid but not
> bit-exact (per-thread rngs), and the binary says so on load.
>
> **Format is JSON**, chosen because the planned wasm build needs the browser to read
> these and `JSON.parse` is built in. Floats are written `%.9g`, which round-trips every
> float32 exactly, so it is as lossless as a binary dump while staying diffable and
> hand-editable. **Editing the `config` block and resuming is the supported way to
> intervene mid-run** — change the predator economy, metabolic rate or perception at
> step 300 and watch whether the regime shifts. ~1.2 KB per creature.

> **v0.16.0 — a perf cycle that actually shipped, plus reproducible floating point.**
> Unlike the reverted v0.14.0 attempt, this one is measured: **1.17–1.47× faster at
> every one of 30 (population × perception) grid points, mean 1.29×**, and
> **bit-for-bit identical output** — verified by fingerprinting whole 500- and
> 2000-step runs against the pre-change build across 7 seeds, and by a `core_tests`
> oracle that re-implements the old `feedForward` and asserts exact equality over
> 400 nets × 6 steps.
>
> *What changed:* the net's connections are stored once, flattened into two index
> spaces (sensor/neuron sources, action/neuron sinks) and grouped by sink with a
> **stable** counting sort — so each accumulator still sums in genome order and
> rounds identically — which removes two unpredictable branches per connection.
> Two things mattered as much as the branch removal, and both were found by
> measurement, not reasoning: the flat entry must stay **4 bytes** (an 8-byte
> version was *net slower* despite a faster `feedForward`), and the old
> genome-ordered array must be **deleted, not kept alongside** (keeping both was
> ~0.97× — the duplicate competed for cache with the neighbour scan). `asGene()`
> decodes an entry for the wiring log and inspector.
>
> *Also:* `-ffp-contract=off` is now a build flag. Without it the compiler fuses
> multiply-adds into FMAs wherever it likes, so results depend on `-march`,
> compiler version and target — and **wasm has no scalar FMA at all**, meaning a
> native build could never reproduce a browser build. This is a **one-time results
> shift** (old binaries used FMA), and it is what makes a fixed `--seed` mean the
> same run everywhere: the premise of deterministic replay, checkpoint resume, and
> verifying a result by re-running it. Never add `-ffast-math`.
>
> New flags: `--max-neighbors N` / `--topological` (biology), `--fast-tanh` (speed).
> `CMakeLists.txt` is 0.16.0.

> **v0.15.0 — triangular action space** (`--triangular-motion`, off by default). The
> net commands a forward DRIVE and a TURN, and the feasible turn is throttled by drive
> (`turnEff = turn * (1 - drive)`): a fast strike can't also turn; a slow reorient turns
> freely. This forced trade-off is what the hunting paper (Malik/Singh/Harpaz/Engert/
> Rajan 2025) found produces clean fast-straight vs slow-turning bouts — which the old
> decoupled steer/speed axes could not. `--max-turn-rate F` sets the turn ceiling (rad/s)
> at zero drive (default 4.0). Default off ⇒ byte-for-byte identical to before (verified).
> `CMakeLists.txt` is 0.15.0.
>
> *Earlier this session a v0.14.0 perf cycle investigated three optimizations and shipped
> none (all reverted) — see the changelog in the context doc; the build skipped from 0.13.0
> to 0.15.0 for that reason. The `--profile` profiler from 0.13.0 remains.*

> **v0.12.0 extends the death-cluster map and adds lineage + wiring analysis** (all
> opt-in, dynamics-neutral). `--cluster-pca` gives per-run variance-aligned axes (vs
> the fixed random map); `--cluster-color lineage` colours points by clade; the map
> now draws **epoch-centroid drift arrows** and a **top-lineage summary** (share +
> epoch span = how long a clade dominated); the deaths CSV gains a `lineage` column;
> and `--wiring-log` writes `<name>.wiring.csv` — the fraction of the population
> wiring each sensor as a source, per log interval (watch selection recruit sensors).
> Creatures now carry a founder `lineageId` (maternal line). `CMakeLists.txt` is 0.12.0.

> **v0.11.0 adds two inspection/analysis features** (both opt-in, zero effect on
> dynamics or existing CSVs). (1) **Live neural inspector** — LEFT-CLICK a fish in
> the window to watch its neural net fire in a translucent top-right panel (sensor →
> hidden → action nodes, lit by live activation; connections tinted by
> weight×signal). (2) **Death-cluster map** (`--cluster-map`) — every death logs a
> 2-D projection of that creature's network; at end of run it writes
> `<name>.deaths.csv` + `<name>.cluster.svg`, points coloured by epoch (a new colour
> band every `--cluster-epoch` deaths, default 1000) so you can read how the
> population's networks drift over the run. `CMakeLists.txt` is at 0.11.0.

> **v0.9.0 adds a predator (Stage A).** It is **off by default** (`--predators 0`):
> with no predators the world is byte-for-byte the old behaviour *except* that the
> three new prey threat-sensors bump `NUM_SENSES` 38→41, which reseeds the genome
> baseline (see the NUM_SENSES note below).
>
> **v0.9.1 adds schooling instrumentation** — two CSV columns, `neighbor_align` and
> `neighbor_count` (see CSV columns). No new flags, no behaviour change; it only
> *measures*. It exists because `predator_align` assumes prey flee individually and
> is blind to evasion-by-schooling.
>
> **v0.9.2 is perf-only** — the schooling-metric neighbour lookup is now O(n) (temp
> spatial grid) instead of O(n²). No result or CSV-format change.
>
> **v0.10.0 adds the predator energy economy (Stage B).** It is **off by default**
> (`--predator-economy` not given): predators stay Stage-A immortal/fixed-count and
> the energy machinery is inert, so all §10 runs and tests are unchanged. With the
> switch ON, predators gain energy per capture, burn metabolism + a kinetic
> move-cost, reproduce above a threshold and **starve** below 0 — `predator_pop`
> becomes time-varying (the coupled oscillator). **No new sensors**, so `NUM_SENSES`
> stays 41 and 0.9.x predator runs remain comparable. **4 CSV columns appended**
> (27→31): `predator_births_interval, predator_deaths_starv, predator_deaths_age,
> predator_avg_energy`. A new build option **`-DEVO_NO_OPENMP=ON`** produces a
> deterministic serial binary for reproducible single runs. `CMakeLists.txt` is at
> 0.10.0, so `--version` matches the code.

Check what you're running at any time:

```
./evo_boids --version          # e.g. "evo_boids 0.9.2 (built ...)"
./evo_boids --help             # same reference as below
```

---

## Workflow

```bash
# 1) Rebuild ONLY when you have new code from Claude (a fresh tarball in Downloads).
#    NOTE: rebuild_evo.sh wipes ~/evo_boids — never run it while a sim is live.
#    After it runs, re-enter the dir: `cd ~/evo_boids/build` (the old shell cwd is a
#    now-deleted inode → otherwise "./evo_boids: No such file or directory").
~/rebuild_evo.sh

# 2) Run (no rebuild needed between runs — just change flags).
cd ~/evo_boids/build
./evo_boids --headless 1000000 --name evolved_1M --log-every 2000 --gene-cost 0.02 --no-render

# 3) Pull a run's data into Windows (safe mid-run; CSV stays current to ~5 rows).
cp ~/evo_boids/build/runs/evolved_1M.csv ~/evo_boids/build/runs/evolved_1M.config.txt /mnt/c/Users/harit/Downloads/
```

`--name X` writes `runs/X.csv` plus `runs/X.config.txt` (a sidecar recording the
version, seed, step count, and every parameter — so a CSV is never an orphan).

**Tip on `--no-render`:** without it, a headless run **opens the SFML window** after
finishing (and does not exit). Use `--no-render` for unattended/long runs. To *watch*
the result, omit it — but note the renderer currently shows only the top-left corner
of the world (see the context file's renderer-bug note) and the flow overlay only
draws with a current active. Predators draw as **red discs** (dimmed while digesting)
and the HUD gains a `Pred: N` field when any are alive.

**Backslash gotcha:** a line-continuation `\` must be the **last** character before
the newline. Pasting a multi-line command can turn `\`+newline into `\`+space, which
bash reads as a literal space → the next token becomes `" --flag"` (leading space) →
`Unknown option:  --flag` (note the double space). Run commands on a single line, or
ensure no trailing spaces after `\`.

---

## Run / logging

| Flag | Default | Meaning |
|---|---|---|
| `--headless N` | 0 | Run N steps headless, then open the window. |
| `--no-render` | off | Exit after the headless run instead of opening a window. Use for long/unattended runs or piping output. |
| `--name X` | — | Log to `runs/X.csv` (+ `runs/X.config.txt`). |
| `--csv PATH` | — | Explicit CSV path (overrides `--name`). |
| `--log-every N` | 1000 | Sample/print interval in steps. |
| `--flush-every N` | 5 | Flush the CSV to disk every N logged rows. Header flushes immediately, so `tail -f` works from t=0; a Ctrl-C loses at most N rows. |
| `--seed S` | random | Seed the RNG for repeatable runs (see note). |
| `--version` | — | Print version + build time, then exit. |
| `--help`, `-h` | — | Show usage. |

## Population

| Flag | Default | Meaning |
|---|---|---|
| `--population N` | 300 | Starting creature count. |
| `--min-pop N` | 50 | Reseed floor: random creatures injected below this. |
| `--max-pop N` | 0 | Hard birth cap (0 = unlimited). Birth *ceiling*, not a culler. |

## Food

| Flag | Default | Meaning |
|---|---|---|
| `--target-food N` | 600 | Baseline food count (hard cap is 2×). |
| `--food-spawn F` | 3 | Mean food spawned per step. |
| `--food-amp F` | 2 | Sinusoidal abundance swing around the base (0 removes the external food rhythm). |
| `--food-energy F` | 5 | Energy gained per food eaten. |
| `--pickup-radius F` | 10 | Distance within which food is eaten. |

## Predator (Stage A)

**Off by default** (`--predators 0`). Stage A predators are **scripted, immortal,
and fixed-count**: no genome, no neural net, no energy economy yet. They spawn at
random positions, chase the nearest prey they can see, and capture on contact. The
*only* things under test in Stage A are the **chase kinematics** — predator speed vs
prey speed, the capture radius, and the digestion cooldown. (Stage B will give them
an energy economy / starvation+reproduction; Stage C makes them neural like prey.)

| Flag | Default | Meaning |
|---|---|---|
| `--predators N` | 0 | Number of predators (0 = none → the no-predator baseline). **Suggested start: 6.** |
| `--predator-perception F` | 150 | Predator's prey-detection radius. **Larger than prey perception (80) on purpose** — the sight gap *is* the ambush asymmetry. Set `=80` for a symmetric-sight experiment. |
| `--predator-speed F` | 110 | Predator cruise speed. Prey calm cruise is ≈82 under `--move-cost 0.00007`, so a predator out-cruises a *foraging* prey but a prey sprinting to its max (150) can pull away — at a quadratic kinetic cost. That tension is the point. |
| `--predator-capture-radius F` | 14 | Contact distance at which a prey is caught (instant kill; no meal energy yet — that's Stage B). |
| `--predator-cooldown F` | 3 | Digestion cooldown (seconds) after a capture: a predator can't catch again until it elapses. **Caps each predator's kill rate — your first anti-spiral knob.** |

**Reading a predator run:**
- The console line gains a `pred N  kills K  pAlign X` segment **only when predators
  are active** (no predators → the line is unchanged).
- `kills` there is the per-interval predation count (same as the `kills_interval`
  CSV column).
- See `predator_align` under CSV columns for the evasion instrument.

**Two failure modes to watch (this is the Stage-A tuning target):**
1. **Extinction spiral** — prey `population` rides the `--min-pop` floor (50) and the
   reseed keeps injecting fresh random genomes (which never get to evolve evasion).
   Ease it with *more food* / `--food-patches 0` (uniform), fewer/slower predators
   (`--predators`, `--predator-speed`), or a longer `--predator-cooldown`.
2. **Perfect evasion / predator irrelevance** — `kills_interval` stuck at 0 and
   `predator_align` flat: the predator is too slow or too blind to matter. Raise
   `--predator-speed`, `--predator-perception`, or `--predator-capture-radius`.

## Predator energy economy (Stage B)

**Off by default** (omit `--predator-economy`). When ON, the scripted hunter from
Stage A also runs an **energy economy**: it gains energy by capturing prey, burns
**metabolism + a kinetic move-cost** each second, **reproduces** (buds a child)
above an energy threshold, and **starves** below 0 energy (and optionally dies of
old age). `predator_pop` becomes time-varying — this is the whole point: the coupled
(Lotka–Volterra-ish) oscillator where prey crash → predators starve → predator
decline → prey rebound → predators recover. There is **no predator reseed floor** —
predator extinction is a legitimate, observable failure mode, not something masked.

| Flag | Default | Meaning |
|---|---|---|
| `--predator-economy` | off | Master switch. Off ⇒ Stage A (immortal/fixed-count, energy inert). On ⇒ the economy below. |
| `--predator-metabolism F` | 2.0 | Predator fixed energy burn/sec (prey burn 1.5). Higher ⇒ predators need to eat more often to survive. |
| `--predator-move-cost F` | reuse | Predator kinetic cost coefficient (`F·speed²`/sec). **<0 ⇒ reuse the prey `--move-cost`** (the default); ≥0 ⇒ a predator-specific coefficient. |
| `--predator-repro-energy F` | 60 | Energy a predator needs to bud a child. Higher ⇒ slower breeding. |
| `--predator-newborn-energy F` | 30 | A child's (and the initial predators') starting energy. The parent pays this on each birth, so budding **conserves energy**. |
| `--predator-meal F` | absorb | Energy gained per capture. **<0 ⇒ absorb the victim's `foodReserves`** (the default; carnivory is calorie-dense — biological, but spiral-prone and unbounded when prey are fat). ≥0 ⇒ a fixed meal value (gentler, robust across arenas). |
| `--predator-max-age F` | 0 | Predator death age. **0 ⇒ immortal-until-starved** (the default); >0 ⇒ predators also age out. |

**The tuning target (this is a real-run, replicate task — see the reproducibility
note and context §11):** find the band where **neither population's trough hits
zero**. A useful starting heuristic from the energy balance: a predator that eats
once per cooldown breaks even at `meal ≈ cooldown · (metabolism + move_cost·speed²)`
(≈ 8.5 at the defaults), and needs roughly **1.4×** that for a breeding surplus when
fed. Below break-even ⇒ predators starve out (irrelevance); far above ⇒ they explode
until prey are pinned at the reseed floor (a degenerate "spiral" equilibrium where
the reseed feeds the predators). **The chase (Stage-A `--predators`/`-speed`/
`-cooldown`) is usually the binding constraint** — too many fast predators over-eat
prey to a crash *before* the economy can throttle them, so easing the chase
(fewer predators, longer cooldown) is the first diagnostic, exactly as in Stage A.

**Reading a Stage-B run:**
- The console gains `pE <energy>  p+<births> p-<deaths>` after the existing
  `pred/kills/pAlign` segment (only when predators are alive and the economy is on).
- `predator_avg_energy` (CSV) is the quantity to keep **above zero** — a population
  can look fine while its mean energy slides toward a crash, so watch it, not just
  `predator_pop`.
- `predator_births_interval` vs `predator_deaths_starv` (+ `_age`) read the
  oscillator directly: births>deaths ⇒ predators rising, the reverse ⇒ falling.
- **Judge across replicates over long runs.** Short runs land in different attractors
  (predators explode / persist / go extinct) under OpenMP non-determinism — use
  `-DEVO_NO_OPENMP=ON` for deterministic single runs, or run 5–10 replicates.

The win condition is the band in between: evasion evolves (it shows up as **schooling**
— rising `neighbor_align`, *not* `predator_align`; see CSV columns) **and neither side
goes extinct**. Note schooling is a **stochastic phase transition** (~1.2M+ steps, not
every run) — judge it across **replicates**, not one run (see the reproducibility note).

## Environment — currents & spatial food

Off by default (`--current 0`, `--food-patches 0`). Currents only *matter* when food
is spatial, so use them together.

| Flag | Default | Meaning |
|---|---|---|
| `--current F` | 0 | Peak ocean-flow speed (world units/sec). 0 = no current. Compare to **cruise speed** (≈82 with `--move-cost 0.00007`), not max-speed: at `--current 80` they *fight*, at `--current 150` they *ride* (see context §3). |
| `--current-mode M` | eddies | `eddies` = moving vortices → many small pockets. `gyre` = double-gyre → two counter-rotating basins + oscillating divider (cleaner two-region isolation). |
| `--current-eddies N` | 5 | [eddies] Number of vortices. |
| `--current-core F` | 150 | [eddies] Eddy radius (peak-speed distance). |
| `--current-drift F` | 1.0 | How fast the field evolves (0 = static). For gyre, the separatrix oscillation rate. |
| `--gyre-eps F` | 0.25 | [gyre] How far the dividing line swings (0 = fixed vertical split). |
| `--food-patches N` | 0 | Cluster food into N spatial patches (0 = uniform). Patchy food is what makes a current consequential. |
| `--food-patch-radius F` | 120 | Spread of food around each patch centre. |
| `--food-drift` | off | Advect food *with* the current → drifting filaments (no-op if `--current 0`). |

Both current modes are divergence-free (velocity = curl of a streamfunction) — no
sinks, matter stretches into filaments. Two sensors (`CURRENT_FWD`, `CURRENT_LR`) let
evolution perceive the flow. Predators are advected by the current too, so phase-2
(current on) stays kinematically consistent between hunter and hunted. The GUI draws
a faint flow overlay when a current is active (currently only visible in the world's
top-left corner — see context renderer note).

## Genes

| Flag | Default | Meaning |
|---|---|---|
| `--genome-min N` | 16 | Hard lower bound on gene count. |
| `--genome-max N` | 80 | Hard upper bound on gene count. |
| `--genome-init-min N` | 24 | Initial gene-count range (low). |
| `--genome-init-max N` | 48 | Initial gene-count range (high). |
| `--mutation F` | 1.0 | Multiplier on the per-birth mutation rate. |
| **`--repro-max-distance F`** | **off (<0)** | **Match distance** (v0.21.0). A pair farther apart than `F` in the folded **(source,sink) connectivity space** emits no successor record; every other precondition is unchanged, and an out-of-threshold pair burns its interaction cooldown exactly as an under-energised pair already does. Uses `genomeDistanceSq()` (`src/core/isolation.hpp`) — **not** `genomeSimilarity()`, which is positionally aligned and collapses on a single indel. **Measured scale** (so the number means something): unrelated random genomes ≈ **19.5** apart; in-run pairs within `--interact-radius` sit at median **18–20** (p05 ≈ 13–17); a mutated clone ≈ **0**. So the live window is roughly **12–22** — above ~23 the gate is inert, below ~12 essentially nothing pairs and the live count falls to the `--min-pop` floor. Off ⇒ byte-identical to before (fingerprint-verified, 4 seeds × 5 modes). Costs nothing when off (one float compare, no RNG draw). Requires `--genome-max ≤ 1024`. |
| **`--body-model`** | **off** | **Tier-1 body model** (v0.23.0). Two heritable per-creature parameters — `bodySize` and `bodyElong`, both clamped to **[0.5, 2.0]** — that modulate the *existing* speed/turn/metabolism model. No new physics, no new sensor, no format break. **Factors** (`src/core/body_model.hpp`, plain float arithmetic, no transcendentals, no RNG): speed ceiling × `2/(1+bodySize)`, turn rate × `2/(1+bodyElong)`, metabolism × `bodySize`. So a **bigger** body is slower *and* dearer to run; a **longer** body turns worse. All three are exactly **1.0** at the default body, and `x·1.0f` is IEEE-exact, so off ⇒ byte-identical to before (fingerprint-verified, 4 seeds × 5 modes). Deliberately a **separate parameter vector, not derived from the neural genome** — attribution ("body or brain?") and future body-gating need the body to vary independently of the wiring. The turn factor only bites under `--triangular-motion`, the only mode that reads `--max-turn-rate`. `bodySize`/`bodyElong` also scale the drawn body-segment spacing (`4·size·elong`), which is **visual only** — no sensor reads `body[]`. **`--max-speed` becomes the ceiling of a *default* body, not a hard world cap**: at the small rail the ceiling is `1.333 × --max-speed` (200 at the stock 150), at the large rail `0.667 ×` (100) — measured, a seed-42 run peaks at 159.7 with the flag on vs exactly 150.0 with it off. **`OWN_SPEED` is deliberately left normalised by `--max-speed`, not by the creature's own ceiling**: a speed-capped big body reads a permanently lower `OWN_SPEED`, and a small body can read *above* 1.0. That asymmetry is information the brain can evolve to use, which is why the sensor is untouched (adding one would re-base `geneCell` and break the genome corpus). **Founders start at 1.0/1.0 and draw nothing**, including reseeded ones — diversity comes from mutation, and it keeps the `--min-pop` reseed floor from injecting extreme bodies into a collapsing population. **RNG stream note:** with the flag on, each successful insertion draws **exactly two** `rng.range` values, `bodySize` then `bodyElong`, with no rejection loop and no Gaussian, so the draw count is input-independent (this is what keeps native == wasm bit-exact). With the flag **off** the body path draws nothing at all, so every archived run stays reproducible. Checkpoints carry both config keys and two always-written creature columns; pre-0.23 checkpoints load with the gate off and every body at 1.0. |
| **`--body-mutation F`** | **0.1** | [`--body-model` only] Uniform mutation **half-width per parameter per birth**: `child = clamp(mean(parents) + U(−F, +F), 0.5, 2.0)`. Must be **finite and ≥ 0** — negative and non-finite values are refused, not reinterpreted, and all three entry points agree: the CLI, the browser's `evo_init`, and the checkpoint reader (so a hand-edited config block cannot smuggle one in either). `0` = inheritance with no drift: a uniform founder body is passed down bit-for-bit forever, while the two draws still happen — the draw *count* never depends on `F`. Larger `F` explores morphology faster and reaches the clamps sooner. Inert when `--body-model` is off. |

## Selection pressure

| Flag | Default | Meaning |
|---|---|---|
| `--metabolism F` | 1.5 | Fixed energy burned per second, per creature. |
| `--gene-cost F` | 0 | Extra energy/sec **per gene** (parsimony pressure). >0 selects against bloat. (Note: 0.01 does **not** meaningfully tame bloat under currents — don't read genome length as fitness.) |
| **`--move-cost F`** | **0** | **Kinetic cost: extra energy/sec = `F · speed²`.** 0 = off (speed is free, reproducing prior behaviour). >0 makes thrust expensive → an **interior optimal cruise speed** and makes fighting a current costly. **Calibrated value: `0.00007`** → cruise ≈82 (half of max), population healthy. Robust optimum (3e-5 and 7e-5 both land ~82). **This is foundational** (the predator needs it) and is in the frozen baseline. |
| `--max-age F` | 120 | Death age. |
| `--repro-energy F` | 20 | Energy each parent needs to reproduce. |
| `--newborn-energy F` | 25 | Starting energy of a child. |

## Sensing / movement / cooldowns

| Flag | Default | Meaning |
|---|---|---|
| `--toroidal` | off | **Wrap-around senses** (v0.19.0). Positions have always wrapped; this makes perception wrap too: neighbour/food/predator queries and offsets use minimum-image distances across the seam, pheromone deposits/reads wrap, the long probe looks through the seam, and the `BOUNDARY_*` sensors read saturated no-wall (a torus has no walls). Cures the seam's sensory cliff (an edge-band creature otherwise perceives ~half its true neighbours — measured 3.0 vs 5.8) and the wall-hugger / seam-oscillator phenotypes it breeds. Currents stay a bounded basin (small flow discontinuity when crossing the seam); `LOC_X`/`LOC_Y` stay absolute position. Off ⇒ byte-identical to before (fingerprint-verified). ~1 % throughput cost. |
| `--perception F` | 80 | Neighbour/food sensing radius (also the spatial-grid cell size). Also the radius at which a prey *sees* a predator (the `PREDATOR_*` sensors). |
| `--interact-radius F` | 20 | Social-interaction range (also the `INTERACT_FIGHT` adjacency range). |
| `--max-neighbors N` | 0 (all) | **Topological interaction**: attend to only the `N` *nearest* neighbours inside `--perception`, instead of averaging over every one of them. 0 = the historical metric rule, preserved byte-for-byte. Real animals are topological, not metric — starlings track ~6–7 nearest regardless of flock density (Ballerini et al., PNAS 2008), and fish studies find fewer still (Katz et al. / Herbert-Read et al., PNAS 2011). Under the metric rule a fish in a dense school averages over ~150 neighbours, which no real animal does. Only the *interaction* percepts are capped (`AVG_NEIGHBOR_*`, `POPULATION_FWD/LR`); `NEIGHBOR_COUNT` and `POPULATION_DENSITY` still report the true count in range, since crowding is a real percept. Max 64. Roughly throughput-neutral (−7 % to +8 %) — this is a **model** change, not a speed knob. |
| `--topological` | — | Shorthand for `--max-neighbors 7`, the Ballerini number. |
| `--min-speed F` | 30 | Minimum speed. **Set 0** when `--move-cost` is on, so drifting (speed 0, zero kinetic cost) is an available strategy. |
| `--max-speed F` | 150 | Maximum speed. |
| `--triangular-motion` | off | Couple the action space: the net commands a forward **drive** and a **turn**, and feasible turn is throttled by drive (`turnEff = turn·(1−drive)`). Fast strike ⇒ can't turn; slow reorient ⇒ full turn authority. Replaces the decoupled steer/speed model. Off ⇒ byte-identical to original. Motivated by the hunting paper's finding that this forced trade-off produces clean strike-vs-reorient bouts. |
| `--max-turn-rate F` | 4.0 | [triangular only] Max heading change (rad/s) at zero forward drive; drive scales it down to 0 at full speed. |
| `--repro-cooldown F` | 3 | Seconds between reproductions. |
| `--interact-cooldown F` | 2 | Seconds between social interactions. |

---

## Analysis / inspection (v0.11.0)

Both features are opt-in and do **not** affect the simulation, the metrics CSV, or
any existing run — they only observe.

**Live neural inspector (windowed runs).** LEFT-CLICK a fish to select it; a
translucent panel appears top-right showing its neural network firing live:
- three node columns — **sensors** (left, 41 inputs) → **hidden** neurons (middle,
  up to 12) → **actions** (right, 12 outputs); each node's fill is its current
  activation (blue = negative, grey = idle, red = positive; brighter = stronger);
- **connections** are tinted by weight (green = excitatory, red = inhibitory) with
  opacity scaled by weight × source activation, so live signal flow is visible;
- the selected fish gets a yellow ring in the world; click empty water to deselect,
  click another fish to switch. Action nodes are labelled (steerX, moveFwd, …).

Driven by per-step captured activations, enabled automatically only when a window is
open (headless runs pay nothing for it).

| Flag | Default | Meaning |
|---|---|---|
| `--cluster-map` | off | At end of run, write `<name>.deaths.csv` + `<name>.cluster.svg` (below). Off ⇒ nothing logged, no allocation. |
| `--cluster-epoch N` | 1000 | Deaths per colour band. Each band is one step along the cool→warm ramp, so colour encodes *when* (in death order) a network died. |
| `--cluster-max N` | 20000 | Max points kept in RAM. Past this the log is uniformly decimated (keep every other point, double the stride), so a multi-million-death run stays bounded and the SVG renderable while remaining an even sample across the run. |
| `--cluster-pca` | off | Map axes = **PCA** over the death sketches (per-run, variance-aligned, fills the frame) instead of the fixed random projection. The deaths CSV keeps the stable random `x,y`; PCA is a view choice for the SVG only. |
| `--cluster-color M` | `epoch` | `epoch` colours points by death order (the ramp); `lineage` colours by clade (hashed founder id) so selective sweeps show as large same-colour clusters. |
| `--wiring-log` | off | Headless: write `<name>.wiring.csv`, one row per log interval — the fraction of the live population wiring each of the 41 sensors as a source. Reveals which inputs selection recruits over the run. |
| `--repro-log` | off | Headless: write `<name>.births.csv`, one row per kept pairing event — `index,frame,parent1,parent2,lineage1,lineage2`. This is the **pairing graph**: an edge means those two creatures actually produced a successor record. Bounded and uniformly decimated exactly like the cluster map, so `index` is the true event ordinal and the stride is reported in the file header. |
| `--repro-log-max N` | 20000 | Max pairing records kept in RAM; uniformly decimated above this. |
| `--repro-window N` | 2000 | Trailing pairing events behind the always-on `repro_components` CSV column (0 disables the column). Undecimated on purpose — dropping edges fragments a connected graph and would inflate the very count being measured, which is why this is a separate ring buffer from the archive above. |
| `--repro-min-cluster N` | 3 | Ignore pairing-graph components smaller than N. A one-off pairing is not a group; without this the count is dominated by isolated pairs. |
| `--profile` | off | Headless: accumulate per-phase timings and print a build / parallel{sensor-gather, feedForward, apply} / serial split at end. Diagnostic only; gated so it never affects dynamics. Run it at your real N and core count before optimizing. |
| `--save-checkpoint FILE` | — | Write the complete world state to `FILE` (JSON) at the end of the headless run. ~1.2 KB per creature (a 233-creature world is 270 KB; gzips to ~40 %). |
| `--load-checkpoint FILE` | — | Resume from `FILE` instead of starting fresh. **In a serial build the continuation is bit-for-bit identical to the run that was interrupted** — verified end to end: 600 steps in one go produces the same bytes as 300 + resume + 300. Adopts the checkpoint's world size and config, so simulation flags on the resume command line are ignored (edit the file instead). |
| `--checkpoint-every N` | 0 (off) | Also write `<base>.<frame>.json` every N steps, so a long run leaves a trail you can rewind to. Base is `--save-checkpoint`'s path if given, else `runs/<name>`. |
| `--fast-tanh` | off | Replace `std::tanh` in the net with a Padé (7,6) rational, worst error 1.1e-4. Worth **1.2–1.3×** end-to-end, because every creature makes 24 `tanh` calls per step regardless of genome size — over half of `feedForward`'s cost. **Changes the numbers**: a chaotic system amplifies 1e-4 into different trajectories, so runs made with it are a separate numerical lineage and won't match the existing corpus. Measured over 12 seeds × 4000 steps, with and without predators, the outcome *distributions* (population, schooling, genome length, genetic diversity, species) are statistically indistinguishable — so it's sound for exploratory sweeps; re-confirm headline findings with it off. Recorded in the run sidecar. |

**Death-cluster map.** Every time a creature dies, its network is projected (a fixed
random sketch is stored; the SVG plots either the fixed 2-D random map or, with
`--cluster-pca`, a PCA of the sketches). At end of run:
- `<name>.cluster.svg` — every logged death as a point, coloured by epoch (ramp) or
  lineage; **epoch centroids joined by arrows** show the population's drift direction
  through projection space; a **top-lineage summary** (id, % of deaths, epoch span)
  reads selective sweeps directly. Self-contained; opens in any browser. Reading it: a
  tight single-colour clump = a converged population then; the centroid track moving =
  networks drifting under selection; one lineage colour dominating a region/epoch span
  = a sweep.
- `<name>.deaths.csv` — `index,x,y,epoch,lineage,age,genome_len,cause` per kept death
  (`cause` ∈ p/s/a). Re-plot or analyse freely (PCA over x,y, group by lineage, etc.).

**Pairing graph.** `--repro-log` → `<name>.births.csv`, plus the always-on
`repro_components` CSV column (32nd, appended in v0.22.0 — every earlier column keeps
its index). The column is the number of connected components with ≥ `--repro-min-cluster`
members in the graph of which records paired with which, over the last `--repro-window`
pairing events.

Read it **instead of** `species`, not alongside it, when the question is group
separation. `species` is a SimHash of the same folded connectivity vector that
`--repro-max-distance` thresholds, so it cannot evaluate that gate without
circularity — and it is **saturated** regardless: measured across every arm of the
v0.21.0 sweep including the ungated control, it tracks the live-set count almost
one-to-one (475 buckets for 480 creatures), i.e. at 16 hash bits it is counting
individuals. `repro_components` is mechanism-level: an edge exists because an insertion
happened, so it cannot be circular with a `Genome`-space gate. `0` means no edges in the
window — the honest report for a live set that has stopped pairing; read it with
`births_interval`.

**Sensor-wiring frequency.** `--wiring-log` → `<name>.wiring.csv`: `step` plus one
column per sensor (named), each the fraction of the population whose genome wires that
sensor as a source. Plotting a column over time shows recruitment — e.g. the
`PREDATOR_*`/neighbour sensors climbing once predation selects for evasion, or unused
sensors sitting near their random-chance baseline.

> Projections are **lossy shadows** of a ~1272-dim space. The fixed random map is
> reproducible/comparable across runs and coherent with the genome colour; PCA is
> per-run and variance-aligned (better for one run's legibility, not comparable across
> runs because the axes are data-dependent). Lineage = founder of the **maternal**
> line (parent1 at each birth); crossover means a clade's genomes still recombine.

## CSV columns (one row per `--log-every` interval) — **32 cols as of v0.22.0**

`step, elapsed_s, steps_per_sec, population, food, births_cum, deaths_cum,
births_interval, deaths_interval, avg_age, max_age, avg_energy, avg_speed,
avg_genome_len, genome_len_std, avg_responsiveness, avg_osc_period,
genetic_diversity, species, flow_align,
predator_pop, kills_interval, deaths_starvation, deaths_age, predator_align,
neighbor_align, neighbor_count,
predator_births_interval, predator_deaths_starv, predator_deaths_age, predator_avg_energy,
repro_components`

- `genetic_diversity`: RMS distance from the population mean in connectivity space
  (~14 scale, exact). Coupled to genome length. Not comparable to pre-rework CSVs.
- `species`: distinct SimHash buckets — collapses toward 1 on convergence, rises
  toward population size when diverse. Cleaner convergence signal. **Blind to
  non-connectivity traits** (e.g. a future body-size gene).
- `flow_align`: mean cosine of heading vs local flow. **+1** ride/surf, **−1** fight
  upstream, **0** crosswise *or* no current. **Read with `avg_speed`** (low speed =
  drifting; high+positive = surfing; high+negative = fighting).
- `repro_components` (32nd, appended in v0.22.0): connected components with ≥
  `--repro-min-cluster` members in the pairing graph over the last `--repro-window`
  events — see the *Pairing graph* section above. **Restarts empty on checkpoint
  resume** (the window is per-run state, like the death log), so the column needs up
  to `--repro-window` post-resume pairing events before it is comparable to an
  uninterrupted run's.

**New in v0.9.0 (5 columns, appended last → existing column-by-name parsing is
unaffected):**

- **`predator_pop`** — instantaneous predator count. Constant in Stage A (predators
  are immortal/fixed-count); it's a column now so Stage B (where it varies) needs no
  format change.
- **`kills_interval`** — predation deaths this interval (diffed from a cumulative
  total, like births/deaths). Identical to the console `kills` field.
- **`deaths_starvation`** / **`deaths_age`** — the other two death causes this
  interval. Together with predation they **partition** the deaths:
  `deaths_interval == kills_interval + deaths_starvation + deaths_age` (exact, every
  row). This is what lets you tell *how* a population is dying — predator pressure vs
  starvation (food too scarce) vs old age (a calm, food-rich run).
- **`predator_align`** — the evasion instrument, mirroring `flow_align`: mean cosine
  between a prey's heading and the **away-from-nearest-predator** direction, gated to
  prey that actually have a predator within their own perception (80). **+1** fleeing
  straight away, **−1** swimming right at it, **0** crosswise *or* no threat in range.
  **Read it WITH `avg_speed`:** fleeing only matters if they're also moving — without
  the speed context you can't distinguish "evasion evolved" from "the slow ones just
  got eaten." Expect ~0 early (a fresh genome baseline hasn't evolved evasion yet);
  a sustained climb is the signal you're tuning for.

**New in v0.9.1 — schooling instruments (2 columns, appended last):**

- **`neighbor_align`** — local polarization: for each prey, the cosine between its
  heading and the **mean heading of prey within its perception (80)**, averaged over
  prey that have ≥1 neighbour. **+1** = locally aligned (a school), **0** = neighbours'
  headings cancel (disordered foraging). It is **local on purpose** — several schools
  pointing different ways each read high, whereas a *global* polarization (whole-pop
  mean heading) would cancel them to ~0 and hide the schooling. **This is the schooling
  metric — use it, not `predator_align`.** Measured empirically: **0.97 in a schooled
  run vs 0.06 in a non-schooled one.** Two caveats: (a) it measures *polarization*, so
  a school that **mills** (up-and-down a common axis, bidirectional) reads ~0 even
  though it looks coherent; (b) schooling is **stochastic** — the *same command* gave
  0.06 and 0.97 on different runs (OpenMP non-determinism), and it only appears after
  ~1.2M steps, so **judge it across replicates over long runs**, never one run.
- **`predator_align`** is the *wrong* axis for this sim (kept for completeness): it
  assumes prey flee individually/radially, but they evade by **schooling**, so it sits
  near a culling-artifact floor (~0.2) even mid-schooling. Don't read evasion off it.
- **`neighbor_count`** — mean number of prey within perception (local crowding). A
  school is aligned **and** clumped, so read the two together: high align + high count
  = a tight school (saw ~27); high align + low count = a loose comigration; low align =
  no polarized school regardless of count.

**New in v0.10.0 — predator energy economy (4 columns, appended last):**

- **`predator_births_interval`** — predators budded this interval (diffed from a
  cumulative total). 0 in Stage A.
- **`predator_deaths_starv`** / **`predator_deaths_age`** — predator deaths this
  interval by cause (starvation / old age). Together with births they read the
  oscillator: births > (starv+age) ⇒ predators rising. Both 0 in Stage A.
- **`predator_avg_energy`** — instantaneous mean energy over the live predators
  (0 when no predators, and **0 in Stage A** where the field is inert). This is the
  quantity to keep **above zero** when tuning — a predator population can look stable
  while its mean energy slides toward a crash. Read it the way you read prey
  `avg_energy`.

**How to use them (the experiment that found the schools):** run the same config
**with and without predators** (`--predators N` vs `--predators 0`, everything else
identical). If `neighbor_align` (and `neighbor_count`) are markedly higher *with*
predators, the schooling is predator-induced — i.e. it's collective evasion — rather
than baseline flocking. **Run several replicates of each** (≥1.5M steps): a *single*
run is not representative — schooling is a stochastic phase transition, so one
predator run may read 0.06 (didn't tip) and another 0.97 (tipped). The contrast you
trust is *with-predator replicates vs without-predator replicates*, not one vs one.
(Full move-by-move experiment log, with every run's command + CSV name + result, is in
the context file, §10.)

> Note: creature **colour** is a locality-preserving 3-D projection of the
> connectivity genome (similar genomes → similar colours; lineages drift smoothly).
> It's a visualisation aid and a *shadow* of the `species` metric — not an independent
> measure, and it does not affect sim RNG/dynamics.

---

## Notes

- **This is evolution, not learning.** Neural-net weights are fixed at birth (parent
  crossover + mutation) and never change during life. Adaptation is cross-generational.
  (Stage-A predators don't even have a net — they're scripted.)
- **`--seed`, OpenMP & reproducibility (READ THIS before drawing conclusions):**
  with an OpenMP build, `--seed` pins only the **single-threaded** parts (spawning,
  interactions, deaths, predator hunt + economy, init); the **parallel** sensor/
  movement phase uses **per-thread RNGs**, so runs are **not reproducible** at a
  fixed seed. This bit *qualitatively* in Stage A: the **same command** gave
  `neighbor_align` 0.06 on one run and 0.97 on another (loose mill vs tight school).
  ⇒ **Operating rule: use replicates, not single runs** — run a command 5–10× and
  report the distribution; **or build the serial target** for deterministic single
  runs: `cmake -S . -B build-serial -DEVO_NO_OPENMP=ON && cmake --build build-serial`
  (slower, but a fixed `--seed` reproduces byte-for-byte — verified). Emergent
  collective behaviour also needs **≥1.5M steps** to appear. This matters **more in
  Stage B**, where the coupled oscillator adds its own sensitivity on top of the RNG
  non-determinism. (Older noise-floor note still holds: genome-length / species swing
  run-to-run even at fixed seed — don't over-read small deltas.)
- **Build type & speed:** the binary is built **`-O3`** (Release). `rebuild_evo.sh`
  configures with explicit `-DCMAKE_BUILD_TYPE=Release` and prints the build type +
  `CXX_FLAGS` after configuring, so optimization is never ambiguous. (Note: a *blank*
  `CMAKE_BUILD_TYPE` in `CMakeCache.txt` is cosmetic — the CMakeLists defaults to
  Release via a normal variable and still emits `-O3`.) If a run is unexpectedly slow,
  it's almost always **(a)** the config (≈4000 food at `--target-food 2000` ⇒ ~6× more
  food-grid scanning than 600-food) or **(b)** thermal throttling on a laptop after a
  long all-core session (CPU pegged at 800% but clocked down) — **not** the metric
  code (which runs once per `--log-every`, ~negligible).
- **`--name` is just a label** — `--headless N` sets the step count.
- **Not yet flags** (code changes): **predator Stage C** (neural predators — their
  own evolving genome/net population). **Now flags as of 0.10.0:** the predator
  Stage B energy economy (`--predator-economy` + the meal/metabolism/repro/age/
  move-cost knobs). Still code changes: logistic/density-dependent food regrowth,
  mutation-operator rebalancing, camouflage / spatially-varying background, a
  body-size trait, a per-creature spatial-structure snapshot / Moran's I, the
  renderer `setView` fix, and in-life (Hebbian/NEAT) learning.
- **NUM_SENSES history:** v0.6.0 changed it 36→38; v0.9.0 changes it **38→41** (three
  `PREDATOR_*` prey sensors). So **0.9.0 runs are a fresh baseline** — their
  genomes/diversity are *not* mutually comparable with 0.6–0.8 runs (and pre-0.6 even
  less so). This holds **even with `--predators 0`**: the sensors exist regardless;
  predators just never populate them. Re-establish a baseline on 0.9.0 before reading
  cross-version deltas.

## Examples

```bash
# === SCHOOLING / ISOLATION ARENA (the runs that produced the Stage-A result) ===
# Superabundant food + pop cap => starvation ~0 => predation is the main selection.
# Run BOTH and several replicates of each (schooling is stochastic; needs >=1.5M steps).
# WITH predators (schooling emerged here, as a phase transition ~1.2M steps):
./evo_boids --headless 2000000 --name school_pred --predators 6 --food-patches 0 --target-food 2000 --food-spawn 12 --max-pop 350 --current 0 --current-mode gyre --gene-cost 0.01 --move-cost 0.00007 --min-speed 0 --seed 42 --log-every 2000 --no-render
# WITHOUT predators (control — no schooling, high colour diversity):
./evo_boids --headless 2000000 --name school_ctrl --predators 0 --food-patches 0 --target-food 2000 --food-spawn 12 --max-pop 350 --current 0 --current-mode gyre --gene-cost 0.01 --move-cost 0.00007 --min-speed 0 --seed 42 --log-every 2000 --no-render
# Read: neighbor_align (0.97 schooled vs ~0.06 not) + neighbor_count (tight ~27 vs ~3.3)
# across replicates. NOT predator_align (wrong axis). Lighter/faster variant: drop
# --target-food to 800 and --max-pop to 200 (still no starvation, ~2x faster).

# === PREDATOR Stage A — the older "frozen baseline" start (food patches) ===
# NOTE: this patchy config SPIRALED at --predators 6 (prey crashed to the floor).
# It's kept for continuity with §3/§5; for behaviour studies use the isolation arena above.
./evo_boids --headless 1000000 --name pred6 --predators 6 \
  --current 0 --current-mode gyre --food-patches 6 --food-drift \
  --gene-cost 0.01 --move-cost 0.00007 --min-speed 0 --seed 42 --log-every 2000 --no-render

# Dial sweep examples (one knob at a time):
#   easier on prey:   --predator-cooldown 5     (fewer kills/predator)  or  --predators 4
#   harder on prey:   --predator-speed 130       or  --predator-capture-radius 20
#   symmetric sight:  --predator-perception 80   (removes the ambush asymmetry)

# === FROZEN BASELINE (no predator; calm-water start) ===
./evo_boids --headless 1000000 --name base --current 0 --current-mode gyre \
  --food-patches 6 --food-drift --gene-cost 0.01 --move-cost 0.00007 \
  --min-speed 0 --seed 42 --log-every 2000 --no-render
# (--food-drift is a no-op at current 0, kept so phase-2 is a one-flag change)

# === PREDATOR Stage B — the energy economy (the ACTIVE tuning frontier) ===
# Turn the economy on with --predator-economy. The defaults (absorb-reserves meal,
# metabolism 2, repro 60) are a STARTING POINT, not a tuned oscillator: in short
# sandbox runs the absorb-reserves default lets predators explode until prey pin at
# the reseed floor. A fixed meal is more controllable. TUNE on long runs +
# replicates (or the serial build) until NEITHER population troughs to zero.
#
# Isolation arena, fixed meal, max-pop relaxed so prey can oscillate (not pin at a cap):
./evo_boids --headless 2000000 --name predB_iso --predators 6 --predator-economy \
  --predator-meal 14 --predator-metabolism 2 --predator-repro-energy 60 \
  --food-patches 0 --target-food 2000 --food-spawn 12 --max-pop 600 --min-pop 20 \
  --current 0 --current-mode gyre --gene-cost 0.01 --move-cost 0.00007 --min-speed 0 \
  --seed 42 --log-every 2000 --no-render
# Watch predator_avg_energy (keep > 0) and predator_births vs predator_deaths_starv.
# First knobs if it spirals/collapses (one at a time): --predator-meal,
# --predator-metabolism, --predator-repro-energy; then EASE THE CHASE
# (--predators 4, --predator-cooldown 5) — the chase is usually the binding constraint.
#
# Deterministic single run for definitive cause-and-effect (build the serial target):
#   cmake -S . -B build-serial -DEVO_NO_OPENMP=ON && cmake --build build-serial -j
#   ./build-serial/evo_boids ... (a fixed --seed reproduces byte-for-byte)

# Phase 2 once the predator is stable: turn the current on (the "ride" regime)
./evo_boids --headless 1000000 --name pred6_c150 --predators 6 --current 150 --current-mode gyre \
  --food-patches 6 --food-drift --gene-cost 0.01 --move-cost 0.00007 \
  --min-speed 0 --seed 42 --log-every 2000 --no-render

# THE KEY currents result — pricing thrust unmasks the current (fight vs ride):
#   current 80  + move-cost 7e-5 -> flow_align ~ -0.09 (fights upstream)
#   current 150 + move-cost 7e-5 -> flow_align ~ +0.07 (rides) [needed current 300 without move-cost]
./evo_boids --headless 1000000 --name gyre_c80_mc  --current 80  --current-mode gyre \
  --food-patches 6 --food-drift --gene-cost 0.01 --move-cost 0.00007 --min-speed 0 --seed 42 --log-every 2000 --no-render
./evo_boids --headless 1000000 --name gyre_c150_mc --current 150 --current-mode gyre \
  --food-patches 6 --food-drift --gene-cost 0.01 --move-cost 0.00007 --min-speed 0 --seed 42 --log-every 2000 --no-render

# move-cost calibration (current 0): finds the interior cruise speed (~82) without flooring pop
./evo_boids --headless 1000000 --name gyre_c0_mc --current 0 --current-mode gyre \
  --food-patches 6 --food-drift --gene-cost 0.01 --move-cost 0.00007 --min-speed 0 --seed 42 --log-every 2000 --no-render
```
