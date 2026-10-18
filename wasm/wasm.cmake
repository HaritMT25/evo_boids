# wasm.cmake — the browser module, included from CMakeLists.txt under
# Emscripten. Two flavours, picked by EVO_POOL_THREADS:
#
#   OFF  web/evo_boids.{js,wasm}     single-threaded: the deterministic build,
#                                    native serial == wasm bit for bit
#                                    (wasm/parity.sh)
#   ON   web/evo_boids_mt.{js,wasm}  pthreads + thread pool: faster at high
#                                    creature counts, NOT reproducible; needs a
#                                    cross-origin-isolated page
#                                    (COOP same-origin + COEP require-corp)
#
# Flags that are part of the determinism contract (do not relax):
#   -ffp-contract=off   set for every target in CMakeLists.txt
#   no -ffast-math      ever
#
# Usage (after `source ~/emsdk/emsdk_env.sh`):
#   cmake --preset wasm-st && cmake --build --preset wasm-st
#   cmake --preset wasm-mt && cmake --build --preset wasm-mt
# Each build copies its .js/.wasm into web/.

set(EVO_WASM_EXPORTS
    "-sEXPORTED_FUNCTIONS=_malloc,_free"
    "-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8,HEAP8,HEAPU8,HEAP32,HEAPF32,HEAPF64,wasmMemory"
    # Module options the page passes in (web/loader.js). Emscripten ignores any
    # option not listed here. mainScriptUrlOrBlob tells the pthread workers
    # which script to load: by default they load the script that called the
    # factory, which is wrong when that is the Turbo worker (worker.js) or when
    # the deploy build has renamed the file.
    "-sINCOMING_MODULE_JS_API=locateFile,mainScriptUrlOrBlob,print,printErr,onAbort")

if(EVO_POOL_THREADS)
    set(EVO_WASM_NAME evo_boids_mt)
    # Upper bound on pool threads, shared by the C++ pool (thread_pool.hpp) and
    # the pre-spawned worker pool below; the two must agree, because a thread
    # started from the browser's main thread can only come from that pool.
    set(EVO_POOL_MAX_THREADS 8)
    target_compile_definitions(evo_core INTERFACE EVO_POOL_MAX_THREADS=${EVO_POOL_MAX_THREADS})
    set(EVO_WASM_THREAD_FLAGS
        # One worker per pool thread beyond the caller. Same formula as
        # Pool::maxThreads(): hardwareConcurrency capped at the max.
        "-sPTHREAD_POOL_SIZE=Math.min((globalThis.navigator||{}).hardwareConcurrency||4,${EVO_POOL_MAX_THREADS})-1"
        # Asking for a thread beyond the pool is an error, not a silent hang.
        "-sPTHREAD_POOL_SIZE_STRICT=2"
        # Growing shared memory forces JS views to be re-created after every
        # growth; start large enough that a typical session never grows.
        "-sINITIAL_MEMORY=134217728"
        "-sMAXIMUM_MEMORY=1073741824")
else()
    set(EVO_WASM_NAME evo_boids)
    set(EVO_WASM_THREAD_FLAGS "")
endif()

add_executable(evo_wasm wasm/evo_wasm.cpp)
set_target_properties(evo_wasm PROPERTIES OUTPUT_NAME ${EVO_WASM_NAME} SUFFIX ".js")
target_link_libraries(evo_wasm PRIVATE evo_core)
target_compile_definitions(evo_wasm PRIVATE EVO_VERSION="${PROJECT_VERSION}")
target_link_options(evo_wasm PRIVATE
    --no-entry
    -sMODULARIZE=1
    -sEXPORT_NAME=createEvoBoids
    -sENVIRONMENT=web,worker,node
    -sALLOW_MEMORY_GROWTH=1
    ${EVO_WASM_EXPORTS}
    ${EVO_WASM_THREAD_FLAGS})

add_custom_command(TARGET evo_wasm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE_DIR:evo_wasm>/${EVO_WASM_NAME}.js
            $<TARGET_FILE_DIR:evo_wasm>/${EVO_WASM_NAME}.wasm
            ${CMAKE_SOURCE_DIR}/web/
    COMMENT "copying ${EVO_WASM_NAME}.{js,wasm} into web/")
