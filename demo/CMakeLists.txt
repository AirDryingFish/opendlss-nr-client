# The DLSS 5 NR demo: Filament (the patched checkout, installed to third_party/filament-install) + SDL2 + the NR
# host code of src/. Two Vulkan worlds, each its own target with its own headers: `nr` (volk + tools/Vulkan-Headers:
# the device, the NR pass) and `demo_filament_vulkan` (Filament's bundled headers: the shared context, the interop).
# Configured and built by scripts/build_demo.ps1.
cmake_minimum_required(VERSION 3.19)
project(dlss5-demo LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(TOOLS ${ROOT}/tools)
set(FILAMENT ${ROOT}/third_party/filament-install)
set(FILAMENT_SRC ${ROOT}/third_party/filament)
if (NOT EXISTS ${FILAMENT}/include/filament/Engine.h)
    message(FATAL_ERROR "third_party/filament-install is missing: run scripts/fetch_filament.ps1 and scripts/build_filament.ps1")
endif()
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" ARCH)
string(REPLACE "amd64" "x86_64" ARCH "${ARCH}")
set(FILAMENT_LIB ${FILAMENT}/lib/${ARCH})
if (UNIX AND NOT APPLE)
    # Filament installs Linux libraries directly in lib/ on some releases.
    if (EXISTS "${FILAMENT}/lib/libfilament.a")
        set(FILAMENT_LIB ${FILAMENT}/lib)
    endif()
endif()
if (MSVC)
    # Filament's static libraries are built with /MT
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/EHsc /W3 /wd4244 /wd4267 /utf-8 /Zi)
    add_link_options(/DEBUG)   # a PDB next to the executable: the crash report symbolizes the demo's own frames
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

# ---- the NR side: the device, the pass, the kernels' host code (volk, the repository's Vulkan headers)
add_library(nr STATIC
    ${ROOT}/src/vk_context.cpp
    ${ROOT}/src/kernels.cpp
    ${ROOT}/src/nr_graph.cpp
    ${ROOT}/src/nr_model.cpp
    ${ROOT}/src/reference.cpp
    ${TOOLS}/volk/volk.c
    vulkan_device.cpp
    nr_pass.cpp)
target_include_directories(nr PUBLIC ${TOOLS}/Vulkan-Headers/include ${TOOLS}/volk ${ROOT}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(nr PUBLIC VK_NO_PROTOTYPES VK_ENABLE_BETA_EXTENSIONS)

# ---- the Filament-side Vulkan glue (Filament's bundled headers, through bluevk)
add_library(demo_filament_vulkan STATIC filament_vulkan.cpp)
target_include_directories(demo_filament_vulkan PRIVATE ${FILAMENT}/include ${CMAKE_CURRENT_SOURCE_DIR})

# ---- the application
set(DEMO_SOURCES main.cpp scene.cpp crash_report.cpp client_controls.cpp model_file_browser.cpp)
if (APPLE)
    list(APPEND DEMO_SOURCES native_window_cocoa.mm)
endif()
add_executable(dlss5-demo ${DEMO_SOURCES})
# (the NR headers first: main.cpp reaches Vulkan through volk, never through Filament's copy of the headers)
target_include_directories(dlss5-demo PRIVATE
    ${TOOLS}/Vulkan-Headers/include ${TOOLS}/volk ${ROOT}/src ${CMAKE_CURRENT_SOURCE_DIR}
    ${FILAMENT}/include ${FILAMENT}/include/SDL2 ${FILAMENT}/include/imgui ${FILAMENT_SRC}/third_party/stb ${FILAMENT_SRC}/third_party/cgltf)
target_compile_definitions(dlss5-demo PRIVATE VK_NO_PROTOTYPES VK_ENABLE_BETA_EXTENSIONS)
set(FILAMENT_LIBS
    filagui imgui gltfio_core uberarchive uberzlib ktxreader basis_transcoder dracodec meshoptimizer image
    filament-iblprefilter filament backend bluevk smol-v filabridge filaflat geometry mikktspace ibl
    utils abseil zstd stb getopt perfetto sdl2)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Filament uses glibc getopt_long rather than installing its Windows shim.
    list(REMOVE_ITEM FILAMENT_LIBS getopt)
endif()
foreach(lib ${FILAMENT_LIBS})
    list(APPEND FILAMENT_LIB_FILES ${FILAMENT_LIB}/${CMAKE_STATIC_LIBRARY_PREFIX}${lib}${CMAKE_STATIC_LIBRARY_SUFFIX})
endforeach()
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Static Filament archives have mutual dependencies; GNU/LLVM linkers need
    # a rescan group, unlike MSVC's archive lookup semantics.
    target_link_libraries(dlss5-demo PRIVATE nr demo_filament_vulkan
        "-Wl,--start-group" ${FILAMENT_LIB_FILES} "-Wl,--end-group")
    # resgen assembly contains material data and needs no executable stack.
    target_link_options(dlss5-demo PRIVATE "-Wl,-z,noexecstack")
else()
    target_link_libraries(dlss5-demo PRIVATE nr demo_filament_vulkan ${FILAMENT_LIB_FILES})
endif()
if (WIN32)
    target_link_libraries(dlss5-demo PRIVATE Shlwapi user32 gdi32 imm32 version winmm setupapi ole32 oleaut32 advapi32 shell32)
    set_target_properties(dlss5-demo PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")
elseif (APPLE)
    target_link_libraries(dlss5-demo PRIVATE "-framework Cocoa" "-framework QuartzCore" "-framework Metal" "-framework IOKit"
        "-framework CoreVideo" "-framework Carbon" "-framework AudioToolbox" "-framework ForceFeedback" iconv)
else()
    target_compile_definitions(dlss5-demo PRIVATE FILAMENT_SUPPORTS_X11)
    target_link_libraries(dlss5-demo PRIVATE pthread dl xcb X11)
endif()
set_target_properties(dlss5-demo PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${ROOT}/build/demo BUILD_RPATH "$ORIGIN/../lib")

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(X11 REQUIRED COMPONENTS Xcomposite)
    find_package(SDL2 2.0.19 CONFIG REQUIRED)
    set(imgui_dir ${FILAMENT_SRC}/third_party/imgui)
    add_executable(dlss5-live live_main.cpp live_x11.cpp live_nr.cpp live_processing.cpp live_controls.cpp
        live_framegen.cpp live_rife.cpp live_mako.cpp
        ${imgui_dir}/backends/imgui_impl_sdl2.cpp ${imgui_dir}/backends/imgui_impl_sdlrenderer2.cpp)
    target_sources(dlss5-demo PRIVATE live_rife.cpp live_mako.cpp)
    add_executable(live-nr-stability-test EXCLUDE_FROM_ALL ../tests/live_nr_stability.cpp live_nr.cpp)
    target_include_directories(live-nr-stability-test PRIVATE ${FILAMENT_SRC}/third_party/stb)
    target_link_libraries(live-nr-stability-test PRIVATE nr)
    set_target_properties(live-nr-stability-test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${ROOT}/build/demo BUILD_RPATH "$ORIGIN/../lib")
    target_compile_definitions(dlss5-live PRIVATE FILAMENT_SUPPORTS_X11)
    target_include_directories(dlss5-live PRIVATE ${imgui_dir})
    target_link_libraries(dlss5-live PRIVATE nr SDL2::SDL2 ${FILAMENT_LIB}/libimgui.a
        X11::Xcomposite X11::X11 ${CMAKE_DL_LIBS} pthread)
    if (TARGET X11::Xext AND X11_XShm_FOUND)
        target_compile_definitions(dlss5-live PRIVATE LIVE_X11_SHM)
        target_link_libraries(dlss5-live PRIVATE X11::Xext)
    endif()
    find_package(CUDAToolkit QUIET)
    add_executable(live-multipliers-test EXCLUDE_FROM_ALL ../tests/live_multipliers.cpp
        live_framegen.cpp live_rife.cpp live_mako.cpp)
    target_include_directories(live-multipliers-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(live-multipliers-test PRIVATE ${CMAKE_DL_LIBS})
    set_target_properties(live-multipliers-test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${ROOT}/build/demo BUILD_RPATH "$ORIGIN/../lib")
    if (CUDAToolkit_FOUND AND CUDAToolkit_NVCC_EXECUTABLE)
        target_compile_definitions(dlss5-demo PRIVATE LIVE_NVIDIA_FLOW)
        add_executable(live-framegen-caption-test EXCLUDE_FROM_ALL ../tests/live_framegen_captions.cpp live_framegen.cpp)
        target_include_directories(live-framegen-caption-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(live-framegen-caption-test PRIVATE ${CMAKE_DL_LIBS})
        set_target_properties(live-framegen-caption-test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${ROOT}/build/demo BUILD_RPATH "$ORIGIN/../lib")
        set(midpoint_ptx ${ROOT}/build/data/shaders/live_midpoint.ptx)
        add_custom_command(OUTPUT ${midpoint_ptx}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${ROOT}/build/data/shaders
            COMMAND ${CUDAToolkit_NVCC_EXECUTABLE} --ptx -arch=compute_75 -O3
                ${CMAKE_CURRENT_SOURCE_DIR}/shaders/live_midpoint.cu -o ${midpoint_ptx}
            DEPENDS shaders/live_midpoint.cu live_flow_quality.h VERBATIM)
        add_custom_target(live-midpoint-ptx DEPENDS ${midpoint_ptx})
        foreach(target dlss5-live live-framegen-caption-test live-multipliers-test)
            target_compile_definitions(${target} PRIVATE LIVE_NVIDIA_FLOW)
            target_include_directories(${target} PRIVATE ${ROOT}/third_party/nvidia-optical-flow ${CUDAToolkit_INCLUDE_DIRS})
            add_dependencies(${target} live-midpoint-ptx)
        endforeach()
        file(MAKE_DIRECTORY ${ROOT}/build/licenses/nvidia-optical-flow)
        configure_file(${ROOT}/third_party/nvidia-optical-flow/LICENSE
            ${ROOT}/build/licenses/nvidia-optical-flow/LICENSE COPYONLY)
    endif()
    configure_file(${ROOT}/scripts/client_start.py.in ${ROOT}/build/demo/client_start.py @ONLY)
    configure_file(${ROOT}/scripts/client_launch.py ${ROOT}/build/demo/client_launch.py COPYONLY)
    configure_file(${ROOT}/scripts/linux/kernel_resources.py ${ROOT}/build/demo/linux/kernel_resources.py COPYONLY)
    set_target_properties(dlss5-live PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${ROOT}/build/demo BUILD_RPATH "$ORIGIN/../lib")
    add_dependencies(dlss5-demo dlss5-live)
endif()
