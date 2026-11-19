cmake_minimum_required(VERSION 3.19)
project(live_mako LANGUAGES CXX)
find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)
find_program(GLSLANG NAMES glslangValidator glslang HINTS "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/glslang/bin" REQUIRED)
set(copy_shader "${CMAKE_CURRENT_BINARY_DIR}/mako_copy.h")
add_custom_command(OUTPUT "${copy_shader}"
    COMMAND "${GLSLANG}" -V --vn makoCopy -o "${copy_shader}" "${CMAKE_CURRENT_SOURCE_DIR}/copy.comp"
    DEPENDS copy.comp VERBATIM)

add_library(live_mako SHARED plugin.cpp exchange.cpp "${copy_shader}")
target_compile_features(live_mako PRIVATE cxx_std_20)
target_include_directories(live_mako PRIVATE "${MAKO_SDK}/runtime/x64/include" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(live_mako PRIVATE "${MAKO_SDK}/runtime/x64/lib/libmako-fg-runtime.a"
    Vulkan::Vulkan Threads::Threads ${CMAKE_DL_LIBS})
target_compile_options(live_mako PRIVATE -fvisibility=hidden)
target_link_options(live_mako PRIVATE "-Wl,--exclude-libs,ALL" "-Wl,-z,defs")
