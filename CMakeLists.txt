cmake_minimum_required(VERSION 3.20)

project(RealScript VERSION 0.1.0 LANGUAGES CXX)

option(REALSCRIPT_BUILD_TESTS "Build RealScript conformance tests" ON)
option(REALSCRIPT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

add_library(realscript_frontend STATIC
    src/text/Text.cpp
    src/diagnostics/Diagnostic.cpp
    src/syntax/SyntaxFacts.cpp
    src/syntax/Lexer.cpp
    src/syntax/SyntaxNodes.cpp
    src/syntax/Parser.cpp
    src/semantic/Symbols.cpp
    src/semantic/FlowAnalysis.cpp
    src/semantic/SemanticBinding.cpp
    src/semantic/SemanticExpressions.cpp
    src/mir/MirFacts.cpp
    src/mir/MirLowerer.cpp
    src/mir/MirExpressions.cpp
    src/mir/MirVerifier.cpp
    src/mir/MirPrinter.cpp
    src/compiler/Compilation.cpp
    src/bytecode/BytecodeFacts.cpp
    src/bytecode/BytecodeLowerer.cpp
    src/bytecode/BytecodeVerifier.cpp
    src/bytecode/BytecodeDisassembler.cpp
    src/bytecode/BytecodeCodec.cpp
    src/runtime/Interpreter.cpp
    src/runtime/RuntimeServices.cpp
    src/runtime/ManagedHeap.cpp
)

add_library(RealScript::Frontend ALIAS realscript_frontend)

target_include_directories(realscript_frontend
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(realscript_frontend PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(realscript_frontend PRIVATE /W4 /permissive-)
    if(REALSCRIPT_WARNINGS_AS_ERRORS)
        target_compile_options(realscript_frontend PRIVATE /WX)
    endif()
else()
    target_compile_options(realscript_frontend PRIVATE -Wall -Wextra -Wpedantic)
    if(REALSCRIPT_WARNINGS_AS_ERRORS)
        target_compile_options(realscript_frontend PRIVATE -Werror)
    endif()
endif()

add_executable(rsc tools/rsc/main.cpp)
target_link_libraries(rsc PRIVATE RealScript::Frontend)
target_compile_features(rsc PRIVATE cxx_std_17)

if(REALSCRIPT_BUILD_TESTS)
    enable_testing()

    add_executable(realscript_tests tests/tests.cpp)
    target_link_libraries(realscript_tests PRIVATE RealScript::Frontend)
    target_compile_features(realscript_tests PRIVATE cxx_std_17)
    target_compile_definitions(
        realscript_tests
        PRIVATE REALSCRIPT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME realscript.frontend COMMAND realscript_tests)

    add_executable(realscript_phase1c_tests tests/phase1c_tests.cpp)
    target_link_libraries(realscript_phase1c_tests PRIVATE RealScript::Frontend)
    target_compile_features(realscript_phase1c_tests PRIVATE cxx_std_17)
    add_test(NAME realscript.phase1c COMMAND realscript_phase1c_tests)

    add_executable(realscript_phase2a_tests tests/phase2a_tests.cpp)
    target_link_libraries(realscript_phase2a_tests PRIVATE RealScript::Frontend)
    target_compile_features(realscript_phase2a_tests PRIVATE cxx_std_17)
    target_compile_definitions(
        realscript_phase2a_tests
        PRIVATE REALSCRIPT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME realscript.phase2a COMMAND realscript_phase2a_tests)

    add_executable(realscript_phase2b_tests tests/phase2b_tests.cpp)
    target_link_libraries(realscript_phase2b_tests PRIVATE RealScript::Frontend)
    target_compile_features(realscript_phase2b_tests PRIVATE cxx_std_17)
    add_test(NAME realscript.phase2b COMMAND realscript_phase2b_tests)

    add_executable(realscript_phase2c_tests tests/phase2c_tests.cpp)
    target_link_libraries(realscript_phase2c_tests PRIVATE RealScript::Frontend)
    target_compile_features(realscript_phase2c_tests PRIVATE cxx_std_17)
    add_test(NAME realscript.phase2c COMMAND realscript_phase2c_tests)

    add_executable(realscript_phase3a_tests tests/phase3a_tests.cpp)
    target_link_libraries(realscript_phase3a_tests PRIVATE RealScript::Frontend)
    target_compile_features(realscript_phase3a_tests PRIVATE cxx_std_17)
    add_test(NAME realscript.phase3a COMMAND realscript_phase3a_tests)
endif()
