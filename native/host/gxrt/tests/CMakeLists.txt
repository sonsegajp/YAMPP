cmake_minimum_required(VERSION 3.20)
project(gx_translate_tests C)

enable_testing()

add_executable(gx_translate_test
    gx_translate_test.c
    ../gx_translate.c
)
target_include_directories(gx_translate_test PRIVATE ..)
target_compile_features(gx_translate_test PRIVATE c_std_11)
if(MSVC)
    target_compile_options(gx_translate_test PRIVATE /W4 /WX)
else()
    target_compile_options(gx_translate_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_test(NAME gx_translate COMMAND gx_translate_test)

add_executable(rollback_test rollback_test.c ../rollback.c)
target_include_directories(rollback_test PRIVATE ..)
target_compile_features(rollback_test PRIVATE c_std_11)
if(MSVC)
    target_compile_options(rollback_test PRIVATE /W4 /WX)
else()
    target_compile_options(rollback_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME rollback COMMAND rollback_test)

# Include the production HLE implementation in the harness so parked scheduler
# fixtures can be constructed without launching generated guest code.
add_executable(scheduler_checkpoint_test scheduler_checkpoint_test.c)
target_include_directories(scheduler_checkpoint_test PRIVATE
    stubs ../../../runtime ../../../vendor/include)
target_compile_features(scheduler_checkpoint_test PRIVATE c_std_11)
add_test(NAME scheduler_checkpoint COMMAND scheduler_checkpoint_test)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Threads REQUIRED)
    add_executable(platform_compat_test platform_compat_test.c)
    foreach(test_target IN ITEMS platform_compat_test gx_translate_test scheduler_checkpoint_test)
        target_compile_definitions(${test_target} PRIVATE _GNU_SOURCE)
        target_link_libraries(${test_target} PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    endforeach()
    target_compile_features(platform_compat_test PRIVATE c_std_11)
    add_test(NAME platform_compat COMMAND platform_compat_test)
endif()
