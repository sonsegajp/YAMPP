# Shim DLL over aurora's GX layer.
#
# Built through CMake rather than a hand-written cl.exe link line: aurora_core
# pulls SDL3, Dawn and Windows COM transitively (310 unresolved externals when
# linked by hand), and that closure changes whenever aurora does. Linking
# aurora::gx lets CMake resolve it.
cmake_minimum_required(VERSION 3.20)
project(aurora_shim C CXX)

set(AURORA_ENABLE_GX ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_DVD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD OFF CACHE BOOL "" FORCE)

# The hybrid runtime is intentionally Vulkan-only. The prebuilt Dawn package
# may contain other platform backends, but Aurora's compiled selection list is
# filtered below and the shim validates the actual adapter after initialization.
set(DAWN_ENABLE_VULKAN ON CACHE BOOL "Aurora shim Vulkan backend" FORCE)

option(AUSHIM_ENABLE_DIAGNOSTICS
       "Export synthetic GX state/triangle bring-up helpers" OFF)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../refs/aurora aurora_build)

# Aurora's prebuilt Dawn provider re-enables every Windows backend in INTERNAL
# cache entries. Remove those compile-time feature macros from Aurora itself so
# its fallback list contains Vulkan only. Dawn may contain other backend code,
# but this application cannot select it.
get_target_property(_aushim_aurora_definitions aurora_core COMPILE_DEFINITIONS)
if(_aushim_aurora_definitions)
    list(FILTER _aushim_aurora_definitions EXCLUDE REGEX
         "^DAWN_ENABLE_BACKEND_(D3D11|D3D12|OPENGL|DESKTOP_GL|OPENGLES|NULL)$")
    set_property(TARGET aurora_core PROPERTY COMPILE_DEFINITIONS
                 "${_aushim_aurora_definitions}")
endif()

add_library(aurora_shim SHARED shim.c readback.cpp aurora_shim.h)
target_include_directories(aurora_shim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../refs/aurora/lib)
target_compile_definitions(aurora_shim PRIVATE AUSHIM_BUILD_DLL=1)
# aurora headers are C++20 (inline variables, class-template CTAD in math.hpp)
target_compile_features(aurora_shim PRIVATE cxx_std_20)

# Aurora's current device and PAD translation units trigger an MSVC 14.44
# c2.dll internal error during LTCG.  Keep the normal Release /O2 build here;
# the hot-path improvements live in Aurora itself and do not depend on IPO.
if(AUSHIM_ENABLE_DIAGNOSTICS)
    target_compile_definitions(aurora_shim PRIVATE AUSHIM_ENABLE_DIAGNOSTICS=1)
endif()
# aurora::vi supplies configured_fb_size(), which gx and core both reference --
# the single symbol left after CMake resolved the other 309.
target_link_libraries(aurora_shim PRIVATE
    aurora::gx aurora::core aurora::vi aurora::pad)
set_target_properties(aurora_shim PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/out"
    ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/out")
