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

Tested on Windows 11 in:
- Chrome 155
- Playwright's Firefox 155 build
- Playwright's WebKit 26.6 build, which is Safari's engine but not Safari itself

Checks run in each:
- the threaded module and WebGL2 drawing on the isolated page
- `?threads=0` and `?gl=0`
- the single-threaded fallback without isolation headers
- running inside a cross-origin iframe
- Turbo on and off
- checkpoint download

Safari on macOS/iOS has not been tested.

### Phones and touch screens

The page adapts to small and touch screens. A desktop browser with a mouse, in a landscape window, gets the layout above unchanged.

- **World shape.** In portrait, or on any touch screen, a start or restart creates a world with the area of 1920×1080 but the shape of the screen, so it fills the screen instead of a 16:9 strip. Food density and crowding stay the same. Rotating the device keeps the running world, and a "fit to screen" button restarts it with a fitted one. A checkpoint keeps its own size, and `?bench` always uses 1920×1080.
- **Inspector.** On screens 700 px wide or less, and on touch screens, the selected fish's network is drawn in a panel docked to the screen edge away from the fish, with 11–12 px text, instead of on the canvas. The on-canvas HUD is left out there; the status line carries the essentials.
- **Touch.** A tap selects the nearest fish within about 24 CSS px. Tapping a parameter's name (marked ⓘ) shows its explanation. Inputs use 16 px text, so iOS does not zoom the page on focus, and buttons and rows are at least 44 px tall.
- **Load.** Touch devices start with 200 creatures (single-threaded) or 400 (threaded), on at most 4 threads; `?nthreads=N` overrides the thread count. The threaded module still pre-spawns min(cores, 8) − 1 workers. The frame-budget governor works as everywhere else.

So far this is measured in emulation only: Playwright's iPhone 13 profile in WebKit 26.6 and its Pixel 7 profile in Chrome 155, portrait and landscape. The world covers 95% of the screen in all four cases (before: 31%, 26%, 75% and 69%). No real phone has been tested, so frame rate, battery use, memory, and whether the threaded module starts on iOS are unmeasured.

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

CI (`.github/workflows/ci.yml`) runs on every push:
- the test suite in the serial, OpenMP and thread-pool builds
- a rebuild of both browser modules, which must match the committed `web/*.wasm`
- `wasm/parity.sh`

## Benchmarks

All numbers were measured on one machine on 2026-10-18:

- Intel Core i7-1165G7 (4 cores / 8 threads), 16 GB RAM, Windows 11, "Balanced" power plan.
- Power source as noted per table.
- Native builds: g++ 13.3, `-O3`, in WSL2 (Ubuntu 24.04).
- WebAssembly: Emscripten 6.0.4. Step times were measured under Node 22.16 (V8), and frame rates in Chrome 155.

### Agents at a stable 60 fps in the browser

Measured with `?bench`, on battery, on a 60 Hz display. The population is pinned at N, and the page takes one simulation step per frame on a 1920×1080 canvas. "Stable" means a mean of at least 58 fps and a 95th-percentile frame time of 20 ms or less, over 300 frames.

| module | drawing | stable 60 fps up to | next N measured |
|---|---|---:|---|
| threaded, 8 threads | WebGL2 | 10,000 | 12,000: 43.6 fps |
| single-threaded | WebGL2 | 4,000 | 6,000: 53.9 fps |
| threaded, 8 threads | canvas 2D | 1,500 | 2,000: 54.1 fps |
| single-threaded | canvas 2D | 1,000 | 1,500: 55.4 fps |

With canvas 2D, drawing was the bottleneck: at 2,000 agents a frame spent 11.6–13.2 ms drawing, against 0.3–0.7 ms with WebGL2. That is why WebGL2 is the default.

A repeat on AC power is not in the table, because the display refreshed at about 56.6 Hz during it, so it could not test 60 fps. Against that rate, the page held steady up to:
- threaded + WebGL2: 12,000 (37.6 fps at 16,000)
- single-threaded + WebGL2: 6,000 (38 fps at 8,000)
- both canvas 2D rows: 2,000 (39–41 fps at 3,000)

`?bench` now detects a display slower than 58 Hz and reports against its rate instead.

The page starts the single-threaded module at 300 creatures and the threaded one at 600. Under the default settings, the population then settles between roughly 250 and 800.

### Step time at a fixed creature count

Milliseconds per simulation step, lower is better, measured on AC power. Each cell is the median of 3 runs, and each run is the median of 5 blocks of 200 steps. Produced by `wasm/bench.sh`: one source file (`wasm/bench_fixed.cpp`) built for every target. The population is pinned, on a 1920×1080 world with default settings.

| creatures | native, 1 thread | native OpenMP, 2 | OpenMP, 4 | OpenMP, 8 | wasm, 1 thread | wasm threaded, 2 | threaded, 4 | threaded, 8 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 500 | 0.65 | 0.37 | 0.26 | 0.22 | 0.73 | 0.49 | 0.55 | 0.52 |
| 1,000 | 1.37 | 0.92 | 0.70 | 0.62 | 1.59 | 1.08 | 0.88 | 0.77 |
| 2,000 | 2.97 | 2.03 | 1.38 | 1.30 | 3.48 | 2.47 | 1.91 | 1.64 |
| 4,000 | 7.43 | 4.77 | 3.59 | 3.05 | 8.82 | 5.85 | 4.42 | 3.59 |
| 8,000 | 21.17 | 13.39 | 10.21 | 7.82 | 25.12 | 16.41 | 11.57 | 9.27 |

- Single-threaded wasm took 1.13–1.19× as long per step as the native single-threaded build.
- From 1,000 creatures up, the threaded wasm build took 1.13–1.38× as long as native OpenMP at the same thread count, about 1.2× in most cells. At 500 it took 1.3–2.4× as long: steps that short are dominated by the cost of waking the threads, and wasm pays more for that.
- From 2,000 creatures up, the best thread count cut the step time by 2.3–2.7× natively and 2.1–2.7× in wasm.
- Going from 4 to 8 threads (hyper-threads on 4 physical cores) gained 5–23%.
- A battery-powered run earlier the same day gave single-threaded times from 1% faster to 16% slower than these. Its threaded times ranged from 13% faster to 73% slower: multi-threaded timings on this laptop vary much more from run to run.

To reproduce: `wasm/bench.sh` for the table above, and `?bench` (add `&threads=0` or `&gl=0`) in a browser for the frame rates.

## Documentation

- [`docs/evo_boids_flags.md`](docs/evo_boids_flags.md): every CLI flag, what it does and when to use it.

Rule of thumb from this project: the simulation is chaotic, so compare distributions across several seeds, not single runs.

## Credits

- [biosim4](https://github.com/davidrmiller/biosim4) by David R. Miller: the genome-to-network encoding, and several sensors and actions are adapted from it (MIT).
- [learning_boids](https://github.com/Dairop/learning_boids) by Dairop: ideas for the food abundance cycle and the social interactions. No code or assets are taken from it.
- [musl](https://musl.libc.org) / FreeBSD fdlibm: the portable float math, used so native and WebAssembly builds give identical results (MIT / Sun notice).

## License

© 2026 Harit Tarwani. Licensed under [CC BY-NC 4.0](https://creativecommons.org/licenses/by-nc/4.0/) (see `LICENSE`).

You may copy, share and adapt this work for **non-commercial** purposes, as long as you **credit the author**, link to the license, and say whether you changed anything. Commercial use needs permission.

Third-party parts keep their own licenses; see [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
