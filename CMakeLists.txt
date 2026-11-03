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

enable_testing()
add_test(NAME recover_tests COMMAND recover_tests)
