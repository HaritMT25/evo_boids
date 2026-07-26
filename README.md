# evo_boids

An **evolutionary** (not learning) boids / artificial-life simulation in C++20. Each
creature carries a genome that wires a small free-topology neural net; the weights are
fixed at birth by crossover + mutation, so every adaptation is cross-generational. The
question the project exists to ask is what selection pressure — food scarcity, movement
cost, ocean currents, predators — is *sufficient* to produce a given collective
behaviour, schooling above all.

Not a game: there is no fitness function and no training loop. Creatures that fail to
eat, starve.

## Layout

```
src/core/      the simulation. header-only and SFML-free — evo_core
src/render/    SFML frontend (the only place SFML appears), behind an IRenderer boundary
src/app/       headless + windowed entry point, ~50 CLI flags
tests/         core_tests, predator_smoke, core_bench — no SFML, run under ctest
wasm/          C ABI shim, Emscripten build script, native-vs-wasm parity harness
web/           canvas renderer + demo page for the wasm build
docs/          full CLI reference and the project context / research log
tools/         gen_planes (regenerates the frozen projection-plane constants)
```

`evo_core` is an INTERFACE library: anything that links it builds and tests without SFML,
which is what keeps the sim portable — and is what made the WebAssembly build a thin
shim rather than a port.

## Build

```bash
cmake -S . -B build && cmake --build build -j
cd build && ctest                      # core_tests + predator_smoke
./evo_boids --headless 50000 --name evolved_50k --log-every 2000
```

Add `-DEVO_NO_OPENMP=ON` for the **single-threaded deterministic build**, where a fixed
`--seed` reproduces a run exactly. The OpenMP build parallelises the per-creature phase
with per-thread RNGs and is therefore *not* run-to-run reproducible — use the serial
build for any definitive cause-and-effect run.

Floating point is compiled with `-ffp-contract=off`. This is deliberate and load-bearing:
without it the compiler fuses multiply-adds into FMAs at will, so results depend on
`-march`, compiler version and target — and WebAssembly has no scalar FMA instruction at
all. Never add `-ffast-math`.

## WebAssembly

```bash
source ~/emsdk/emsdk_env.sh
wasm/build_wasm.sh                     # -> web/evo_boids.{js,wasm}
python3 -m http.server 8123            # serve the REPO ROOT (web/ reads ../img/)
                                       # open http://localhost:8123/web/
wasm/parity.sh                         # native serial == wasm, bit for bit
```

The wasm build (single-threaded, `-ffp-contract=off`, vendored portable numerics as of
v0.18.0) writes **byte-identical checkpoints to the native serial build** for the same
seed, config and step count — `wasm/parity.sh` proves it across a seed/config matrix.
That makes the browser module a universal deterministic reference: the same artifact
runs identically in every browser and under Node, and a browser run can be audited by
replaying its seed natively (or vice versa). Checkpoints are JSON on both sides.

## Documentation

- [`docs/evo_boids_flags.md`](docs/evo_boids_flags.md) — every CLI flag, what it does and
  when to reach for it.
- [`docs/evo_boids_context.md`](docs/evo_boids_context.md) — project context and research
  log: empirical results, the reasoning behind design decisions, per-stage session logs,
  and the things already measured and rejected so they aren't retried.

The operating rule throughout, learned the hard way: **replicates, not single runs.**
