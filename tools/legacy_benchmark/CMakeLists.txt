cmake_minimum_required(VERSION 3.16)

project(recover_legacy_benchmark LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(RECOVER_SOURCE_DIR "" CACHE PATH "Historical checkout to benchmark")
option(RECOVER_LEGACY_BASELINE "Hide propagation fields absent from the baseline" OFF)
option(RECOVER_FIXED_ORDER "Disable MRV and use smallest unsatisfied vertex" OFF)
option(RECOVER_DISABLE_CONFLICT_PROPAGATION
       "Disable immediate rejection of crossing edges" OFF)

if(NOT RECOVER_SOURCE_DIR)
    message(FATAL_ERROR "RECOVER_SOURCE_DIR is required")
endif()

add_executable(recover_benchmark
    ${CMAKE_CURRENT_LIST_DIR}/../../tests/benchmark_runner.cpp
    ${RECOVER_SOURCE_DIR}/src/io.cpp
    ${RECOVER_SOURCE_DIR}/src/model.cpp
    ${RECOVER_SOURCE_DIR}/src/solver.cpp
    ${RECOVER_SOURCE_DIR}/src/validator.cpp
)

target_include_directories(recover_benchmark PRIVATE ${RECOVER_SOURCE_DIR}/src)

if(RECOVER_FIXED_ORDER)
    target_compile_definitions(recover_benchmark PRIVATE RECOVER_FIXED_ORDER)
endif()
if(RECOVER_DISABLE_CONFLICT_PROPAGATION)
    target_compile_definitions(recover_benchmark PRIVATE RECOVER_DISABLE_CONFLICT_PROPAGATION)
endif()

if(MSVC)
    target_compile_options(recover_benchmark PRIVATE /W4 /permissive- /utf-8)
    if(RECOVER_LEGACY_BASELINE)
        target_compile_definitions(recover_benchmark PRIVATE RECOVER_LEGACY_BASELINE)
    endif()
else()
    target_compile_options(recover_benchmark PRIVATE -Wall -Wextra -Wpedantic)
    if(RECOVER_LEGACY_BASELINE)
        target_compile_definitions(recover_benchmark PRIVATE RECOVER_LEGACY_BASELINE)
    endif()
endif()
