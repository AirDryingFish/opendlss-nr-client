cmake_minimum_required(VERSION 3.19)
project(live_rife LANGUAGES C CXX)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(NCNN_OPENMP OFF CACHE BOOL "")
set(NCNN_RUNTIME_CPU OFF CACHE BOOL "")
set(NCNN_AVX OFF CACHE BOOL "")
set(NCNN_AVX2 OFF CACHE BOOL "")
set(NCNN_AVX512 OFF CACHE BOOL "")
# The upstream project defines the exact ncnn layers and shader generation it
# requires. Only its inference core is linked into the live client plugin.
add_subdirectory("${RIFE_SOURCE}/src" rife EXCLUDE_FROM_ALL)
add_library(live_rife SHARED plugin.cpp "${RIFE_SOURCE}/src/rife.cpp" "${RIFE_SOURCE}/src/warp.cpp")
add_dependencies(live_rife generate-spirv)
target_include_directories(live_rife PRIVATE "${RIFE_SOURCE}/src" "${CMAKE_CURRENT_BINARY_DIR}/rife")
target_link_libraries(live_rife PRIVATE ncnn)
target_compile_features(live_rife PRIVATE cxx_std_17)
target_compile_options(live_rife PRIVATE -fvisibility=hidden)
target_link_options(live_rife PRIVATE "-Wl,--exclude-libs,ALL" "-Wl,-z,defs")
find_package(OpenMP QUIET)
if(TARGET OpenMP::OpenMP_CXX)
    target_link_libraries(live_rife PRIVATE OpenMP::OpenMP_CXX)
endif()
# ncnn advertises -fno-exceptions to its consumers. The adapter translates
# allocation and startup errors into the C ABI error buffer.
set_source_files_properties(plugin.cpp PROPERTIES COMPILE_OPTIONS "-fexceptions")
