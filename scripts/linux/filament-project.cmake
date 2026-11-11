# The upstream interop patch adds an intentionally header-only virtual interface.
# Clang diagnoses its weak vtable; it is valid C++, so retain all other -Werror
# checks and demote this one warning on the only translation unit using it.
if(CMAKE_CURRENT_SOURCE_DIR MATCHES "/filament/backend$")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan/VulkanDriver.cpp"
        APPEND PROPERTY COMPILE_OPTIONS -Wno-error=weak-vtables)
endif()
# The same patch's new public Engine/View pointer declarations omit Clang's
# optional nullability annotations. Defer until the engine target exists.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/details/Engine.cpp")
    cmake_language(DEFER CALL target_compile_options filament PRIVATE
        -Wno-error=nullability-completeness)
endif()
