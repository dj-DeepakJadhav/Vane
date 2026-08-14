cmake_minimum_required(VERSION 3.20)
project(vane LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Sources are listed explicitly. A glob previously pulled two translation
# units that each define main() into the shared library, which cannot link.
add_library(vane SHARED
    src/vane_dispatch.cpp
    src/vane_telemetry.cpp
    src/kernels/kernel_scalar.cpp
    src/kernels/kernel_neon.cpp
    src/kernels/kernel_sve.cpp)

target_include_directories(vane PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(vane PROPERTIES PREFIX "" OUTPUT_NAME "vane"
                                        C_VISIBILITY_PRESET hidden
                                        CXX_VISIBILITY_PRESET hidden)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64" OR ANDROID_ABI STREQUAL "arm64-v8a")
    message(STATUS "[vane] AArch64 target: building NEON and SVE kernel paths")

    # -ffast-math is deliberately NOT used. It permits reassociation, which
    # would invalidate the scalar-vs-vector equivalence guarantee that
    # tools/vane_verify asserts.
    target_compile_options(vane PRIVATE -O3 -fno-fast-math)

    # Per-translation-unit architecture baselines. Runtime dispatch guarantees
    # an unsupported path is never entered, so raising a single file's baseline
    # does not raise the library's minimum CPU requirement.
    set_source_files_properties(src/kernels/kernel_neon.cpp PROPERTIES
        COMPILE_FLAGS "-march=armv8.2-a+simd+dotprod")
    set_source_files_properties(src/kernels/kernel_sve.cpp PROPERTIES
        COMPILE_FLAGS "-march=armv8.2-a+sve")
else()
    message(STATUS "[vane] Non-AArch64 target: scalar reference only. "
                   "Vector paths require Arm hardware.")
    target_compile_options(vane PRIVATE -O3)
endif()

foreach(tool vane_probe vane_verify vane_bench)
    add_executable(${tool} tools/${tool}.cpp)
    target_link_libraries(${tool} PRIVATE vane)
endforeach()

enable_testing()
add_test(NAME kernel_equivalence COMMAND vane_verify)

# ---------------------------------------------------------------- demo/atlas
# Semantic search over INT4 word vectors. POSIX sockets only (Linux, Android,
# macOS) — the demo is optional and skipped on other targets rather than
# failing the whole build.
if(UNIX)
    add_executable(atlas_server demo/atlas_server.cpp)
    target_link_libraries(atlas_server PRIVATE vane)
    if(ANDROID)
        target_link_libraries(atlas_server PRIVATE log)
    endif()
endif()
