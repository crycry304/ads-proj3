cmake_minimum_required(VERSION 3.16)

foreach(required_var PYTHON_EXECUTABLE BENCHMARK_EXE SOURCE_DIR WORK_DIR)
    if(NOT DEFINED ${required_var})
        message(FATAL_ERROR "Missing required variable ${required_var}")
    endif()
endforeach()

set(cases_dir "${WORK_DIR}/cases")
set(results_dir "${WORK_DIR}/results")
file(MAKE_DIRECTORY "${cases_dir}")
file(MAKE_DIRECTORY "${results_dir}")

execute_process(
    COMMAND "${PYTHON_EXECUTABLE}"
            "${SOURCE_DIR}/tools/generate_stress_cases.py"
            --output-dir "${cases_dir}"
            --seed 20261104
            --density 0.55
    RESULT_VARIABLE generate_status
    OUTPUT_VARIABLE generate_output
    ERROR_VARIABLE generate_error
)
if(NOT generate_status EQUAL 0)
    message(FATAL_ERROR
        "Stress-case generation failed with ${generate_status}: "
        "${generate_output}${generate_error}")
endif()

execute_process(
    COMMAND "${PYTHON_EXECUTABLE}"
            "${SOURCE_DIR}/tools/run_stress_tests.py"
            --benchmark-exe "${BENCHMARK_EXE}"
            --cases-dir "${cases_dir}"
            --output-dir "${results_dir}"
            --timeout 30
    RESULT_VARIABLE stress_status
    OUTPUT_VARIABLE stress_output
    ERROR_VARIABLE stress_error
)
if(NOT stress_status EQUAL 0)
    message(FATAL_ERROR
        "Stress harness failed with ${stress_status}: "
        "${stress_output}${stress_error}")
endif()

message(STATUS "${stress_output}")
