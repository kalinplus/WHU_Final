cmake_minimum_required(VERSION 3.20)
project(toyc-compiler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)

if(MSVC)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
endif()

FetchContent_MakeAvailable(googletest)

function(toyc_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(toyc-core
    src/driver/options.cpp
    src/driver/diagnostics.cpp
    src/frontend/token.cpp
    src/frontend/lexer.cpp
    src/frontend/parser.cpp
    src/frontend/ast.cpp
    src/frontend/ast_printer.cpp
    src/frontend/ast_visitor.cpp
    src/frontend/ast_access.cpp
    src/sema/sema.cpp
    src/sema/const_eval.cpp
    src/sema/symbol_table.cpp
    src/ir/ir.cpp
    src/ir/ir_printer.cpp
    src/ir/ir_builder.cpp
    src/ir/irgen.cpp
)

target_include_directories(toyc-core PUBLIC include)
toyc_enable_warnings(toyc-core)

add_executable(toyc-compiler
    src/driver/main.cpp
)
target_link_libraries(toyc-compiler PRIVATE toyc-core)
toyc_enable_warnings(toyc-compiler)

enable_testing()

add_test(
    NAME parser_regression
    COMMAND ${CMAKE_COMMAND}
        -DCOMPILER=$<TARGET_FILE:toyc-compiler>
        -P ${CMAKE_SOURCE_DIR}/test/regression/parser/run_tests.cmake
)

add_executable(toyc-ir-tests
    test/ir/ir_tests.cpp
)
target_link_libraries(toyc-ir-tests PRIVATE toyc-core GTest::gtest_main)
toyc_enable_warnings(toyc-ir-tests)

include(GoogleTest)
gtest_discover_tests(toyc-ir-tests)

add_test(
    NAME irgen_regression
    COMMAND ${CMAKE_COMMAND}
        -DCOMPILER=$<TARGET_FILE:toyc-compiler>
        -P ${CMAKE_SOURCE_DIR}/test/regression/irgen/run_irgen.cmake
)

add_test(
    NAME sema_regression
    COMMAND ${CMAKE_COMMAND}
        -DCOMPILER=$<TARGET_FILE:toyc-compiler>
        -P ${CMAKE_SOURCE_DIR}/test/regression/sema/run_tests.cmake
)
