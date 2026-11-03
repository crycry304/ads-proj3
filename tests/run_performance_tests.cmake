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
            "${SOURCE_DIR}/tools/generate_cases.py"
            --output-dir "${cases_dir}"
            --seed 20261103
    RESULT_VARIABLE generate_status
    OUTPUT_VARIABLE generate_output
    ERROR_VARIABLE generate_error
)
if(NOT generate_status EQUAL 0)
    message(FATAL_ERROR
        "Benchmark generation failed with ${generate_status}: "
        "${generate_output}${generate_error}")
endif()

execute_process(
    COMMAND "${PYTHON_EXECUTABLE}"
            "${SOURCE_DIR}/tools/run_benchmarks.py"
            --benchmark-exe "${BENCHMARK_EXE}"
            --cases-dir "${cases_dir}"
            --output-dir "${results_dir}"
            --label ctest
            --repeat 5
            --timeout 30
    RESULT_VARIABLE benchmark_status
    OUTPUT_VARIABLE benchmark_output
    ERROR_VARIABLE benchmark_error
)
if(NOT benchmark_status EQUAL 0)
    message(FATAL_ERROR
        "Performance corpus failed with ${benchmark_status}: "
        "${benchmark_output}${benchmark_error}")
endif()

message(STATUS "${benchmark_output}")
