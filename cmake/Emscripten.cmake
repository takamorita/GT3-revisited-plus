# Web builds (Emscripten -> WebAssembly + WebGL2). Included from CMakeLists.txt before the
# dependencies: the compile flags at once (every object, SDL's and libchdr's too, is built with
# -pthread: the engine's threads run as Web Workers over shared memory), the link options at the
# end of CMakeLists.txt, once the targets exist (gt3r_web_targets, deferred).
#
#   emcmake cmake -S . -B build-web -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-web --target gt3r gt3r_tests
#
# 3D runs on the OpenGL ES 3.0 renderer (render/gpu_gles.*), which is WebGL2 here.

# -fwasm-exceptions: the engine throws and catches (Disc::open, the tests' runner); without it
# Emscripten aborts at the first catch.
set(_web_flags "-pthread -msimd128 -ffp-contract=off -fwasm-exceptions")
string(APPEND CMAKE_C_FLAGS " ${_web_flags}")
string(APPEND CMAKE_CXX_FLAGS " ${_web_flags}")
set(GT3R_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(GT3R_GLES ON CACHE BOOL "" FORCE)
# SDL's Emscripten port: threads on (the engine's mutexes and workers), no GPU API (no
# WebGPU backend for SDL_GPU: the engine uses its OpenGL ES device instead).
set(SDL_PTHREADS ON CACHE BOOL "" FORCE)

function(gt3r_web_targets)

# SDL's audio output: cmake/web/sdl_webaudio.c (an AudioWorklet fed by SDL's own audio thread) in
# place of SDL's ScriptProcessorNode driver, whose mixing ran on the browser's main thread.
if(TARGET SDL3-static)
  get_target_property(_sdl_sources SDL3-static SOURCES)
  list(FILTER _sdl_sources EXCLUDE REGEX "/src/audio/emscripten/SDL_emscriptenaudio\\.c$")
  set_property(TARGET SDL3-static PROPERTY SOURCES ${_sdl_sources} ${CMAKE_SOURCE_DIR}/cmake/web/sdl_webaudio.c)
endif()

set(GT3R_WEB_COMMON_LINK
  -pthread
  -fwasm-exceptions
  # Workers made at start for the engine's threads. A thread started when none is free waits for a new Worker, which the
  # browser starts only once the main thread is free: a main thread that joins such a thread would hang. A race load has
  # about 8 at once (the audio thread, the music read-ahead, the set-up's helpers, ...).
  -sPTHREAD_POOL_SIZE=16
  -sALLOW_MEMORY_GROWTH=1
  -sMAXIMUM_MEMORY=4GB
  -sSTACK_SIZE=8MB
  -sFORCE_FILESYSTEM=1
  # mimalloc: per-thread heaps. The default dlmalloc has one lock, and the loads that build models on several
  # threads (the race's opponents) spent their time waiting for it.
  -sMALLOC=mimalloc)

if(TARGET gt3r)
  target_link_options(gt3r PRIVATE ${GT3R_WEB_COMMON_LINK}
    -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sGL_ENABLE_GET_PROC_ADDRESS=1
    -lidbfs.js -sEXPORTED_RUNTIME_METHODS=FS,ccall --pre-js=${CMAKE_SOURCE_DIR}/cmake/web/browser_pre.js)
  # The page loads gt3r.js after the player picks a disc (web/index.html); a change to
  # the pre-js relinks.
  set_property(TARGET gt3r APPEND PROPERTY LINK_DEPENDS ${CMAKE_SOURCE_DIR}/cmake/web/browser_pre.js)
  # Keep the function names in the wasm (a few MB): the page's profiler (JS Self-Profiling, the
  # debug overlay in web/debug.js) shows them instead of wasm-function[1234].
  option(GT3R_WEB_PROFILING "Keep wasm function names for the browser profiler" ON)
  if(GT3R_WEB_PROFILING)
    target_link_options(gt3r PRIVATE --profiling-funcs)
  endif()
endif()

if(TARGET gt3r_tests)
  # Run with node: GT3R_SOURCE_DIR_MOUNT=<source tree> node build-web/gt3r_tests.js [filter]
  # cmake/web/node_pre.js mounts the source tree (and GT3R_DISC's folder) at their own paths
  # with NODEFS and passes the GT3R_* variables on; main runs on a pthread
  # (PROXY_TO_PTHREAD), so the tests' own threads and waits behave as natively.
  target_link_options(gt3r_tests PRIVATE ${GT3R_WEB_COMMON_LINK} -sENVIRONMENT=node,worker
    -sEXIT_RUNTIME=1 -sPROXY_TO_PTHREAD=1 -lnodefs.js
    --pre-js=${CMAKE_SOURCE_DIR}/cmake/web/node_pre.js)
  set_property(TARGET gt3r_tests APPEND PROPERTY LINK_DEPENDS ${CMAKE_SOURCE_DIR}/cmake/web/node_pre.js)
endif()
endfunction()
cmake_language(DEFER CALL gt3r_web_targets)
