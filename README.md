# evo_boids

An artificial-life simulation in C++20 where fish with small neural networks evolve by inheritance and mutation. It runs natively and in the browser through WebAssembly.

**Live demo:** _link goes here after the first deploy_

![evo_boids running in the browser](docs/demo.gif) <!-- placeholder: GIF to be recorded -->

## What it simulates

Every creature has a **genome**: a list of 16–80 genes, where each gene is one weighted connection in a small neural network. A connection runs from one of 41 sensors (nearby creatures, food, pheromone, own energy, age, an internal oscillator, …) or one of 12 hidden neurons to a hidden neuron or one of 12 actions (steer, speed up, emit a signal, reproduce, share food, fight, …). The genome fixes the network at birth, and the network never learns during the creature's life.

**Nothing scores the creatures. There is no fitness function and no training loop.** Each creature burns energy every second, and it dies when its energy runs out, when it reaches its maximum age, or when a predator catches it (predators are optional). When two creatures meet, their networks' outputs are summed. The pair then shares food, reproduces, fights, or ignores each other, depending on which option scores highest. Reproduction also needs enough energy in both. The child gets a crossover of the two genomes plus random mutation. So selection is only this: genomes whose behaviour finds food and partners leave copies, and the rest don't.

The question the project asks is which environmental pressures (food scarcity, a cost on movement, ocean currents, predators) are enough to produce a collective behaviour such as schooling.

Optional mechanisms, all off by default: predators that eat, breed and starve; food in patches; ocean currents; heritable body size and elongation; a genetic-distance limit on who can pair with whom.

## Architecture

```
src/core/      the simulation: header-only, no rendering, no OS calls (evo_core)
src/render/    SFML window frontend (native only)
src/app/       native entry point: headless runs, CLI flags, CSV logging
wasm/          C API for JavaScript, Emscripten build, benchmarks, parity test
web/           browser frontend: canvas renderer, module loader, Turbo worker
tests/         core_tests, predator_smoke, core_bench (no SFML needed)
```

- **Spatial index.** Creatures and food go into uniform grids (`spatial_grid.hpp`), with cell size equal to the perception radius. The grids are rebuilt every step without reallocating, and a neighbour query only scans the cells overlapping its radius.
- **One step.**
  - The per-creature phase (sense, run the network, move) only writes to the creature being updated, so it runs in parallel.
  - Eating, interactions, births and deaths change shared state, so they run on one thread.
- **Parallelism.**
  - Native: OpenMP.
  - Browser: the same loop runs on a small thread pool (`thread_pool.hpp`) over Emscripten pthreads.
  - Both threaded paths give each thread its own random generator, so a threaded run cannot be replayed from its seed.
- **Determinism.**
  - The single-threaded build reproduces a run exactly from its seed.
  - Floating point is compiled with `-ffp-contract=off` and never `-ffast-math`, and the core uses its own copies of the math functions it needs.
  - With that, the native single-threaded build and the single-threaded WebAssembly module write byte-identical checkpoints for the same seed and step count. `wasm/parity.sh` checks this across 19 seed/config cases.
- **Browser module.**
  - JavaScript calls a small C API (`wasm/evo_wasm.cpp`): `evo_init`, `evo_step(n)`, `evo_reset`, `evo_set_param`, `evo_sync`, plus getters.
  - The getters return pointers into the wasm heap, and the renderer reads them as typed-array views every frame. No copies, no JSON.
  - Checkpoints are JSON and load in both the native build and the browser.
- **Two browser builds.**
  - `evo_boids_mt` uses pthreads. It needs `SharedArrayBuffer`, which browsers allow only on cross-origin-isolated pages (COOP `same-origin` + COEP `require-corp`).
  - `evo_boids` is single-threaded.
  - `web/loader.js` checks `self.crossOriginIsolated` and picks one. Inside an iframe on a page without those headers, it always gets the single-threaded build.
- **Drawing.** With WebGL2, the background, food, fish and predators are drawn with one instanced draw call per kind (`web/gl_renderer.js`). The result is copied onto a 2D canvas, which draws the HUD and the neural inspector on top. Without WebGL2, everything is drawn with canvas 2D, at one fill per body segment.
- **Frame budget.** When a frame takes too long, the page first lowers the simulation speed. Then it removes creatures uniformly at random and caps births, so the agent count drops instead of the frame rate. The status line reports when this happens, because a run changed this way can no longer be replayed from its seed. It can be switched off ("hold 60 fps").

## Build and run

### Native

Requires CMake ≥ 3.16, a C++20 compiler, and optionally OpenMP and SFML 2.6. Without SFML the window app is skipped, and the tests and benchmark still build.

```bash
cmake -S . -B build && cmake --build build -j              # OpenMP: fast
cmake -S . -B build_serial -DEVO_NO_OPENMP=ON && cmake --build build_serial -j   # deterministic
(cd build_serial && ctest)                                 # core_tests + predator_smoke
./build/evo_boids --headless 50000 --name run1 --log-every 2000 --no-render
./build/evo_boids                                          # windowed (SFML)
```

`docs/evo_boids_flags.md` lists every CLI flag.

### Browser (WebAssembly)

Requires [Emscripten](https://emscripten.org) (`source ~/emsdk/emsdk_env.sh`) and CMake ≥ 3.21 for the presets.

```bash
cmake --preset wasm-st && cmake --build --preset wasm-st   # -> web/evo_boids.{js,wasm}
cmake --preset wasm-mt && cmake --build --preset wasm-mt   # -> web/evo_boids_mt.{js,wasm}

python3 wasm/serve.py            # serves the repo root WITH COOP/COEP
                                 # open http://localhost:8123/web/  (threaded)
python3 -m http.server 8123      # plain server: same page, single-threaded
```

Page options:

| Option | Effect |
|---|---|
| `?threads=0` | forces the single-threaded module |
| `?nthreads=N` | sets the thread count |
| `?bench` | runs the frame-rate benchmark below |
| `?gl=0` | forces canvas 2D drawing instead of WebGL2 |

Browser requirements:
- **WebAssembly.**
- **WebGL2**, optional: without it the fish are drawn with canvas 2D, which is slower at high counts.
- **Cross-origin isolation** for the threaded module: Chrome 92+, Firefox 79+ and Safari 15.2+ support it. Anywhere else, the page runs the single-threaded module.

Tested in Chrome 155 on Windows 11. Firefox and Safari have not been tested.

### Deploy (Vercel, static)

```bash
wasm/build_site.sh                                  # -> dist/: index.html, hashed assets, vercel.json
vercel project add evo-boids                        # once: create the project
vercel link --cwd dist --project evo-boids --yes    # once: link dist/ to it (kept across rebuilds)
vercel deploy --cwd dist                            # preview deployment
vercel deploy --cwd dist --prod                     # production
```

`dist/vercel.json` sets:
- COOP/COEP on every path (the Turbo worker and the pthread workers need them too)
- `application/wasm` for `.wasm` files
- a one-year immutable cache for the hashed assets, and `no-cache` for `index.html`

Every asset is same-origin, so cross-origin isolation holds. There are no serverless functions.

### Tests

```bash
(cd build_serial && ctest)       # unit + integration, serial build
(cd build && ctest)              # same suite, OpenMP build
wasm/parity.sh                   # native serial == wasm, byte for byte
```

## Benchmarks

All numbers were measured on one machine on 2026-10-18:

- Intel Core i7-1165G7 (4 cores / 8 threads), 16 GB RAM, Windows 11.
- Measured **on battery, with the "Balanced" power plan**. On AC power the CPU may run faster.
- Native builds: g++ 13.3, `-O3`, in WSL2 (Ubuntu 24.04).
- WebAssembly: Emscripten 6.0.4. Step times were measured under Node 22.16 (V8), and frame rates in Chrome 155.

### Agents at a stable 60 fps in the browser

Measured with `?bench`. The population is pinned at N, and the page takes one simulation step per frame on a 1920×1080 canvas. "Stable" means a mean of at least 58 fps and a 95th-percentile frame time of 20 ms or less, over 300 frames.

| module | drawing | stable 60 fps up to | next N measured |
|---|---|---:|---|
| threaded, 8 threads | WebGL2 | 10,000 | 12,000: 43.6 fps |
| single-threaded | WebGL2 | 4,000 | 6,000: 53.9 fps |
| threaded, 8 threads | canvas 2D | 1,500 | 2,000: 54.1 fps |
| single-threaded | canvas 2D | 1,000 | 1,500: 55.4 fps |

With canvas 2D, drawing was the bottleneck: at 2,000 agents a frame spent 11.6–13.2 ms drawing, against 0.3–0.7 ms with WebGL2. That is why WebGL2 is the default.

The page starts the single-threaded module at 300 creatures and the threaded one at 600. Under the default settings, the population then settles between roughly 250 and 800.

### Step time at a fixed creature count

Milliseconds per simulation step, lower is better. Each cell is the median of 3 runs, and each run is the median of 5 blocks of 200 steps. Produced by `wasm/bench.sh`: one source file (`wasm/bench_fixed.cpp`) built for every target. The population is pinned, on a 1920×1080 world with default settings.

| creatures | native, 1 thread | native OpenMP, 2 | OpenMP, 4 | OpenMP, 8 | wasm, 1 thread | wasm threaded, 2 | threaded, 4 | threaded, 8 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 500 | 0.67 | 0.47 | 0.38 | 0.38 | 0.83 | 0.65 | 0.59 | 0.49 |
| 1,000 | 1.35 | 0.96 | 0.75 | 0.73 | 1.64 | 1.20 | 0.97 | 0.92 |
| 2,000 | 3.05 | 2.06 | 1.54 | 1.28 | 3.55 | 2.44 | 1.66 | 1.55 |
| 4,000 | 7.55 | 5.10 | 4.08 | 4.09 | 10.22 | 6.29 | 4.73 | 4.80 |
| 8,000 | 22.13 | 14.02 | 9.57 | 7.58 | 25.49 | 16.39 | 11.53 | 9.16 |

- Single-threaded wasm took 1.15–1.35× as long per step as the native single-threaded build.
- The threaded wasm build took 1.08–1.55× as long as native OpenMP at the same thread count, about 1.2× in most cells.
- From 2,000 creatures up, the best thread count cut the step time by 1.8–2.9× natively and 2.2–2.8× in wasm.
- Going from 4 to 8 threads gained 0–17% below 8,000 creatures and about 20% at 8,000. The machine has 4 physical cores.

To reproduce: `wasm/bench.sh` for the table above, and `?bench` (add `&threads=0` or `&gl=0`) in a browser for the frame rates.

## Documentation

- [`docs/evo_boids_flags.md`](docs/evo_boids_flags.md): every CLI flag, what it does and when to use it.

Rule of thumb from this project: the simulation is chaotic, so compare distributions across several seeds, not single runs.
