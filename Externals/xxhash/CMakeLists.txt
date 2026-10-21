project(xxhash C)

add_library(xxhash STATIC xxHash/xxhash.c)
target_include_directories(xxhash
PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/xxHash
)
add_library(xxhash::xxhash ALIAS xxhash)

if(EMSCRIPTEN)
  # xxhash includes Emscripten's NEON->SIMD128 shim (<arm_neon.h>) from inside an extern "C"
  # block, which breaks the C++ templates in it. Including it up front avoids that.
  target_compile_options(xxhash INTERFACE "$<$<COMPILE_LANGUAGE:CXX>:-includearm_neon.h>")
endif()
