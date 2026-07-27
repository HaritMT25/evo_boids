cmake_minimum_required(VERSION 3.16)
project(evo_boids VERSION 0.22.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")

# Reproducible floating point. Without this the compiler is free to fuse a*b+c
# into a single-rounded FMA wherever it likes, which makes results depend on
# -march, compiler version and target: two builds of IDENTICAL source then
# disagree in the last bits and diverge (chaotic system). WebAssembly has no
# scalar FMA instruction at all, so a native build WITH contraction can never
# reproduce a wasm build.
#
# Keeping contraction off is what lets a fixed --seed mean the same run
# everywhere, which is the premise of the deterministic serial build, of
# resuming from a checkpoint, and of verifying a result by replaying it. It is
# also what makes the flattened feedForward provably identical to the original
# (see NeuralNet::flat in genome.hpp and the equivalence test in core_tests).
# Do not add -ffast-math: it implies contraction and much worse.
add_compile_options(-ffp-contract=off)

# Serial build switch. OpenMP parallelises the sensor/movement phase with
# per-thread RNGs, which makes runs NON-reproducible even at a fixed --seed (see
# the Stage-A schooling finding). Configure with -DEVO_NO_OPENMP=ON to build a
# fully single-threaded, DETERMINISTIC binary (slower, but a fixed --seed
# reproduces exactly) — the recommended target for definitive cause-and-effect
# runs. Default OFF keeps the fast parallel build.
option(EVO_NO_OPENMP "Build single-threaded (deterministic at a fixed --seed)" OFF)

if(EVO_NO_OPENMP)
    message(STATUS "EVO_NO_OPENMP=ON -> serial, deterministic build (no OpenMP)")
else()
    find_package(OpenMP)
endif()

# ------------------------------------------------------------------
# evo_core — the simulation. Header-only and SFML-FREE.
# Anything that links this can be built and tested without SFML.
# ------------------------------------------------------------------
add_library(evo_core INTERFACE)
target_include_directories(evo_core INTERFACE ${CMAKE_SOURCE_DIR}/src)
if(OpenMP_CXX_FOUND)
    target_link_libraries(evo_core INTERFACE OpenMP::OpenMP_CXX)
    target_compile_definitions(evo_core INTERFACE USE_OPENMP)
endif()

# ------------------------------------------------------------------
# evo_boids — the SFML frontend (window, rendering, input).
# SFML is required ONLY for this target.
# ------------------------------------------------------------------
find_package(SFML 2.6 COMPONENTS graphics window system REQUIRED)
add_executable(evo_boids
    src/app/main.cpp
    src/render/sfml_renderer.cpp
)
target_link_libraries(evo_boids PRIVATE evo_core sfml-graphics sfml-window sfml-system)
target_compile_definitions(evo_boids PRIVATE EVO_VERSION="${PROJECT_VERSION}")

file(COPY ${CMAKE_SOURCE_DIR}/img DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

# ------------------------------------------------------------------
# core_tests — unit tests for the core. No SFML.
# Run with: ctest  (or execute the binary directly)
# ------------------------------------------------------------------
enable_testing()
add_executable(core_tests tests/core_tests.cpp)
target_link_libraries(core_tests PRIVATE evo_core)
add_test(NAME core_tests COMMAND core_tests)

# predator_smoke — a longer no-SFML integration run with predators ON, asserting
# the run-level invariants (pop floor, finite positions, predation occurs,
# predator_align in range, deaths == predation+starvation+age, fixed predator
# count). Core-only, so it joins the same `ctest` suite as core_tests.
add_executable(predator_smoke tests/predator_smoke.cpp)
target_link_libraries(predator_smoke PRIVATE evo_core)
add_test(NAME predator_smoke COMMAND predator_smoke)

# ------------------------------------------------------------------
# core_bench — single-binary throughput benchmark for World::update.
# Core-only (no SFML). Build Release and run e.g. `./core_bench 2000`.
# Links evo_core, so the OpenMP speedup is included when OpenMP is found.
# ------------------------------------------------------------------
add_executable(core_bench tests/bench.cpp)
target_link_libraries(core_bench PRIVATE evo_core)
