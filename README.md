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

### Deploy (Vercel, static)

```bash
wasm/build_site.sh               # -> dist/: index.html, content-hashed assets, vercel.json
vercel deploy dist               # preview
vercel deploy dist --prod        # production
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

_Measured numbers only. Filled in from `wasm/bench.sh` and `?bench`; see below._

## Documentation

- [`docs/evo_boids_flags.md`](docs/evo_boids_flags.md): every CLI flag, what it does and when to use it.

Rule of thumb from this project: the simulation is chaotic, so compare distributions across several seeds, not single runs.
