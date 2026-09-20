cmake_minimum_required(VERSION 3.25)
project(YAMPP LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin")
include("${CMAKE_SOURCE_DIR}/local.cmake" OPTIONAL)
set(AURORA_ENABLE_GX ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_DVD ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD ON CACHE BOOL "" FORCE)
add_subdirectory(upstream/aurora EXCLUDE_FROM_ALL)
add_executable(melee_renderer_check src/renderer_check.c src/readback.cpp)
target_include_directories(melee_renderer_check PRIVATE upstream/aurora/lib)
target_compile_definitions(melee_renderer_check PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(melee_renderer_check PRIVATE
    aurora::core aurora::gx aurora::vi aurora::mtx aurora::pad aurora::os dawn::webgpu_dawn)
if(WIN32)
    add_custom_command(TARGET melee_renderer_check POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:melee_renderer_check> $<TARGET_FILE_DIR:melee_renderer_check>
        COMMAND_EXPAND_LISTS)
endif()

if(WIN32)
    # Build the unchanged DrawASCII/DrawRectangle source slice with Clang.
    # The complete game-source compilation is measured separately by audit_native.py.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    find_program(MELEE_CLANG_EXECUTABLE NAMES clang.exe clang
        HINTS "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/x64/bin" REQUIRED)
    if(MELEE_CLANG_EXECUTABLE)
        set(hsd_control "${CMAKE_BINARY_DIR}/hsd_control.obj")
        add_custom_command(OUTPUT "${hsd_control}"
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/compile_control.py"
                --clang "${MELEE_CLANG_EXECUTABLE}" --output "${hsd_control}"
            DEPENDS scripts/compile_control.py src/platform/native_prelude.h
                upstream/melee/src/sysdolphin/baselib/hsd_3915.c
                scripts/audit_native.py
            VERBATIM)
        set_source_files_properties("${hsd_control}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
        target_sources(melee_renderer_check PRIVATE "${hsd_control}")
        target_compile_definitions(melee_renderer_check PRIVATE MELEE_HSD_CONTROL=1)
        target_link_options(melee_renderer_check PRIVATE /OPT:REF)
    endif()
endif()

# Stable and development launchers expose the same menu/settings capabilities.
# Separate DLL names still let a development build link while the game is open.
if(WIN32)
    set(MELEE_SETTINGS_SRC native/host/aurora_shim/settings.cpp)
else()
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
    set(MELEE_SETTINGS_SRC native/host/aurora_shim/settings_linux.cpp)
endif()
set(MELEE_SHIM_SOURCES
    native/host/aurora_shim/shim.c
    native/host/aurora_shim/readback.cpp
    native/host/aurora_shim/mod_menu.cpp
    ${MELEE_SETTINGS_SRC}
    native/host/aurora_shim/test_input.cpp
    native/host/aurora_shim/netplay_menu.cpp
    native/host/aurora_shim/music_bar.cpp
    native/host/aurora_shim/netplay_table.cpp
    native/host/aurora_shim/controllers.cpp
    native/host/aurora_shim/profile.cpp)
# The artwork preview is isolated until explicitly promoted to a regular launcher.
foreach(shim_target melee_aurora melee_aurora_mod melee_aurora_art melee_aurora_polish melee_aurora_online_next melee_aurora_online_v4 melee_aurora_static melee_aurora_yampp melee_aurora_yampp_qa melee_aurora_yampp_final melee_aurora_wide melee_aurora_controllers)
    add_library(${shim_target} SHARED ${MELEE_SHIM_SOURCES})
    target_include_directories(${shim_target} PRIVATE upstream/aurora/lib)
    target_compile_definitions(${shim_target} PRIVATE AUSHIM_BUILD_DLL=1 AUSHIM_SETTINGS=1 _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(${shim_target} PRIVATE aurora::gx aurora::core aurora::vi aurora::pad imgui absl::flat_hash_map)
    if(WIN32)
        target_link_libraries(${shim_target} PRIVATE xmllite shlwapi)
    else()
        target_link_libraries(${shim_target} PRIVATE OpenSSL::Crypto)
    endif()
endforeach()
set_target_properties(melee_aurora PROPERTIES OUTPUT_NAME melee_aurora_v2)
if(WIN32)
    add_custom_command(TARGET melee_aurora POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:melee_aurora> $<TARGET_FILE_DIR:melee_aurora>
        COMMAND_EXPAND_LISTS)
endif()
if(MSVC)
    target_link_options(melee_aurora_mod PRIVATE "/MAP:${CMAKE_BINARY_DIR}/melee_aurora_mod.map")
endif()
