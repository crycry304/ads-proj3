cmake_minimum_required(VERSION 3.16)

project(ads_proj3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(recover_core
    src/io.cpp
    src/model.cpp
    src/solver.cpp
    src/validator.cpp
)
target_include_directories(recover_core PUBLIC src)
target_compile_options(recover_core PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /utf-8>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

add_executable(recover src/main.cpp)
target_link_libraries(recover PRIVATE recover_core)
target_compile_options(recover PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /utf-8>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

add_executable(recover_tests tests/test_runner.cpp)
target_link_libraries(recover_tests PRIVATE recover_core)
target_compile_options(recover_tests PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /utf-8>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

add_executable(recover_benchmark tests/benchmark_runner.cpp)
target_link_libraries(recover_benchmark PRIVATE recover_core)
target_compile_options(recover_benchmark PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /utf-8>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

option(
    ADS_PROJ3_ENABLE_PERFORMANCE_TESTS
    "Register the full 24-case benchmark corpus as a CTest"
    OFF
)
option(
    ADS_PROJ3_ENABLE_STRESS_TESTS
    "Register the fixed adversarial 50x50 stress probe as a CTest"
    OFF
)

include(CTest)
if(BUILD_TESTING)
    set(recover_test_groups
        parser
        model
        validator
        samples
        exhaustive-small
        random-differential
        planted
        propagation
        geometry
    )
    foreach(group IN LISTS recover_test_groups)
        add_test(
            NAME "recover.${group}"
            COMMAND recover_tests "${group}"
        )
        set_tests_properties(
            "recover.${group}"
            PROPERTIES LABELS "correctness;${group}"
        )
    endforeach()

    set(recover_cli_cases
        sample1-solve
        sample2-solve
        sample3-solve
        model-info
        validate-valid
        validate-invalid
        validate-no-solution
        invalid-arguments
        missing-file
        malformed-input
    )
    foreach(cli_case IN LISTS recover_cli_cases)
        add_test(
            NAME "cli.${cli_case}"
            COMMAND "${CMAKE_COMMAND}"
                "-DRECOVER_EXE=$<TARGET_FILE:recover>"
                "-DSAMPLES_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/samples"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-tests/${cli_case}"
                "-DCASE=${cli_case}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_cli_tests.cmake"
        )
        set_tests_properties(
            "cli.${cli_case}"
            PROPERTIES LABELS "correctness;cli"
        )
    endforeach()

    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME quality.comments
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_comments.py"
                --root "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        set_tests_properties(
            quality.comments
            PROPERTIES LABELS "correctness;quality"
        )
    else()
        message(STATUS
            "Python 3 was not found; comment-quality CTest is not registered")
    endif()

    if(ADS_PROJ3_ENABLE_PERFORMANCE_TESTS)
        if(Python3_Interpreter_FOUND)
            add_test(
                NAME performance.corpus
                COMMAND "${CMAKE_COMMAND}"
                    "-DPYTHON_EXECUTABLE=${Python3_EXECUTABLE}"
                    "-DBENCHMARK_EXE=$<TARGET_FILE:recover_benchmark>"
                    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                    "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/performance-tests"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_performance_tests.cmake"
            )
            set_tests_properties(
                performance.corpus
                PROPERTIES
                    LABELS "performance"
                    TIMEOUT 300
            )
        else()
            message(WARNING
                "Performance tests requested, but Python 3 was not found")
        endif()
    endif()

    if(ADS_PROJ3_ENABLE_STRESS_TESTS)
        if(Python3_Interpreter_FOUND)
            add_test(
                NAME stress.adversarial-dense
                COMMAND "${CMAKE_COMMAND}"
                    "-DPYTHON_EXECUTABLE=${Python3_EXECUTABLE}"
                    "-DBENCHMARK_EXE=$<TARGET_FILE:recover_benchmark>"
                    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                    "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/stress-tests"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_stress_tests.cmake"
            )
            set_tests_properties(
                stress.adversarial-dense
                PROPERTIES
                    LABELS "stress"
                    TIMEOUT 45
            )
        else()
            message(WARNING
                "Stress tests requested, but Python 3 was not found")
        endif()
    endif()
endif()
