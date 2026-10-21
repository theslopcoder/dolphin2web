# dolphin2web status (work in progress)

Goal: run Dolphin in the browser (target: low-end Chromebook, 4 GB RAM, no WebGPU) using
WebAssembly, with a PowerPC -> WebAssembly recompiler instead of pure interpretation.

## Done (written, partly compiled)
- Emscripten build target (`emcmake cmake ...`, `EMSCRIPTEN` branch in root CMakeLists.txt).
- Web memory arena, sparse JIT bitsets (no 4 GiB fastmem), HTTP/GC-adapter stubs.
- PowerPC -> WebAssembly JIT tier on top of the Cached Interpreter
  (`CachedInterpreter/CachedInterpreterWasm.cpp`, `WasmEmitter.h`): hot blocks are compiled at
  runtime into wasm modules sharing Dolphin's memory/table; unsupported ops call back to C++.
- WebGL2 context (OffscreenCanvas in the emulation worker), WebAudio ring-buffer backend,
  keyboard/Gamepad controller backend.

## Not done yet
- `DolphinWeb` frontend (main loop, host callbacks) and final link flags.
- `dolphin2web.html` page (canvas, AudioWorklet, file loading, input glue).
- DirectIOFile on Emscripten, WebGL2/GLES 3.0 shader-path fixes in the OGL backend.
- Testing: nothing has been run in a browser; the JIT is untested. Performance on the target
  Chromebook is unknown.
- Requires cross-origin isolation (COOP/COEP) for SharedArrayBuffer/threads.
