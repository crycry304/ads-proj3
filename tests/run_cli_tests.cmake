cmake_minimum_required(VERSION 3.16)

foreach(required_var RECOVER_EXE SAMPLES_DIR WORK_DIR CASE)
    if(NOT DEFINED ${required_var})
        message(FATAL_ERROR "Missing required variable ${required_var}")
    endif()
endforeach()

file(MAKE_DIRECTORY "${WORK_DIR}")
set(sample1 "${SAMPLES_DIR}/sample1.in")
set(sample2 "${SAMPLES_DIR}/sample2.in")
set(sample3 "${SAMPLES_DIR}/sample3.in")

function(expect_solver_and_validate name input)
    set(answer_file "${WORK_DIR}/${name}.out")
    execute_process(
        COMMAND "${RECOVER_EXE}"
        INPUT_FILE "${input}"
        OUTPUT_FILE "${answer_file}"
        ERROR_VARIABLE solve_error
        RESULT_VARIABLE solve_status
    )
    if(NOT solve_status EQUAL 0)
        message(FATAL_ERROR
            "${name}: solve failed with ${solve_status}: ${solve_error}")
    endif()

    execute_process(
        COMMAND "${RECOVER_EXE}" --validate "${input}" "${answer_file}"
        OUTPUT_VARIABLE validation_output
        ERROR_VARIABLE validation_error
        RESULT_VARIABLE validation_status
    )
    if(NOT validation_status EQUAL 0)
        message(FATAL_ERROR
            "${name}: validator rejected solver output with "
            "${validation_status}: ${validation_error}")
    endif()
    if(NOT validation_output MATCHES "valid connector layout")
        message(FATAL_ERROR
            "${name}: unexpected validator output: ${validation_output}")
    endif()
endfunction()

if(CASE STREQUAL "sample1-solve")
    expect_solver_and_validate(sample1 "${sample1}")
elseif(CASE STREQUAL "sample3-solve")
    expect_solver_and_validate(sample3 "${sample3}")
elseif(CASE STREQUAL "sample2-solve")
    execute_process(
        COMMAND "${RECOVER_EXE}"
        INPUT_FILE "${sample2}"
        OUTPUT_VARIABLE answer
        ERROR_VARIABLE solve_error
        RESULT_VARIABLE solve_status
    )
    string(REPLACE "\r\n" "\n" normalized_answer "${answer}")
    if(NOT solve_status EQUAL 0)
        message(FATAL_ERROR
            "sample2-solve: solve failed with ${solve_status}: ${solve_error}")
    endif()
    if(NOT normalized_answer STREQUAL "No Solution\n")
        message(FATAL_ERROR
            "sample2-solve: expected exact No Solution marker, got '${answer}'")
    endif()
elseif(CASE STREQUAL "model-info")
    execute_process(
        COMMAND "${RECOVER_EXE}" --model-info "${sample3}"
        OUTPUT_VARIABLE info
        ERROR_VARIABLE info_error
        RESULT_VARIABLE info_status
    )
    if(NOT info_status EQUAL 0)
        message(FATAL_ERROR
            "model-info failed with ${info_status}: ${info_error}")
    endif()
    foreach(expected
            "rows 3"
            "cols 5"
            "connectors 11"
            "candidate_edges 15"
            "conflict_pairs 1"
            "structurally_possible 1")
        if(NOT info MATCHES "${expected}")
            message(FATAL_ERROR
                "model-info is missing '${expected}': ${info}")
        endif()
    endforeach()
elseif(CASE STREQUAL "validate-valid")
    execute_process(
        COMMAND "${RECOVER_EXE}" --validate
                "${sample1}" "${SAMPLES_DIR}/sample1.out"
        OUTPUT_VARIABLE validation_output
        ERROR_VARIABLE validation_error
        RESULT_VARIABLE validation_status
    )
    if(NOT validation_status EQUAL 0)
        message(FATAL_ERROR
            "validate-valid returned ${validation_status}: ${validation_error}")
    endif()
    if(NOT validation_output MATCHES "valid connector layout")
        message(FATAL_ERROR
            "validate-valid produced unexpected output: ${validation_output}")
    endif()
elseif(CASE STREQUAL "validate-no-solution")
    execute_process(
        COMMAND "${RECOVER_EXE}" --validate
                "${sample2}" "${SAMPLES_DIR}/sample2.out"
        OUTPUT_VARIABLE validation_output
        ERROR_VARIABLE validation_error
        RESULT_VARIABLE validation_status
    )
    if(NOT validation_status EQUAL 3)
        message(FATAL_ERROR
            "validate-no-solution expected status 3, got ${validation_status}: "
            "${validation_error}")
    endif()
    if(NOT validation_output MATCHES "output claims no solution")
        message(FATAL_ERROR
            "validate-no-solution produced unexpected output: "
            "${validation_output}")
    endif()
elseif(CASE STREQUAL "validate-invalid")
    set(invalid_answer "${WORK_DIR}/invalid.out")
    file(WRITE "${invalid_answer}" "1 3 0 1 0 1\n")
    execute_process(
        COMMAND "${RECOVER_EXE}" --validate "${sample1}" "${invalid_answer}"
        OUTPUT_VARIABLE validation_output
        ERROR_VARIABLE validation_error
        RESULT_VARIABLE validation_status
    )
    if(NOT validation_status EQUAL 1)
        message(FATAL_ERROR
            "validate-invalid expected status 1, got ${validation_status}: "
            "${validation_error}")
    endif()
    if(NOT validation_output MATCHES "missing connector")
        message(FATAL_ERROR
            "validate-invalid produced unexpected output: ${validation_output}")
    endif()
elseif(CASE STREQUAL "invalid-arguments")
    execute_process(
        COMMAND "${RECOVER_EXE}" --model-info
        OUTPUT_VARIABLE command_output
        ERROR_VARIABLE command_error
        RESULT_VARIABLE command_status
    )
    if(NOT command_status EQUAL 2)
        message(FATAL_ERROR
            "invalid-arguments expected status 2, got ${command_status}")
    endif()
    if(NOT command_error MATCHES "Usage:")
        message(FATAL_ERROR
            "invalid-arguments must print usage to stderr: ${command_error}")
    endif()
elseif(CASE STREQUAL "missing-file")
    execute_process(
        COMMAND "${RECOVER_EXE}" --model-info
                "${WORK_DIR}/does-not-exist.in"
        OUTPUT_VARIABLE command_output
        ERROR_VARIABLE command_error
        RESULT_VARIABLE command_status
    )
    if(NOT command_status EQUAL 2)
        message(FATAL_ERROR
            "missing-file expected status 2, got ${command_status}")
    endif()
    if(NOT command_error MATCHES "Cannot open input file")
        message(FATAL_ERROR
            "missing-file produced unexpected stderr: ${command_error}")
    endif()
elseif(CASE STREQUAL "malformed-input")
    set(malformed_input "${WORK_DIR}/malformed.in")
    file(WRITE "${malformed_input}" "2 2\n1\n")
    execute_process(
        COMMAND "${RECOVER_EXE}"
        INPUT_FILE "${malformed_input}"
        OUTPUT_VARIABLE command_output
        ERROR_VARIABLE command_error
        RESULT_VARIABLE command_status
    )
    if(NOT command_status EQUAL 2)
        message(FATAL_ERROR
            "malformed-input expected status 2, got ${command_status}")
    endif()
    if(NOT command_error MATCHES "grid is shorter than n\\*m")
        message(FATAL_ERROR
            "malformed-input produced unexpected stderr: ${command_error}")
    endif()
else()
    message(FATAL_ERROR "Unknown CLI test case: ${CASE}")
endif()

message(STATUS "CLI test passed: ${CASE}")
