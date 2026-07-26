cmake_minimum_required(VERSION 3.20)

project(RealScript VERSION 0.1.0 LANGUAGES C CXX)

option(REALSCRIPT_BUILD_TESTS "Build RealScript conformance tests" ON)
option(REALSCRIPT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

add_library(realscript_frontend STATIC
    src/text/Text.cpp
    src/diagnostics/Diagnostic.cpp
    src/aot_cpp/AotRuntime.cpp
    src/aot_cpp/AotCpp.cpp
    src/debug/DebugInfo.cpp
    src/debug/Debugger.cpp
    src/debug/DapServer.cpp
    src/hot_reload/HotReload.cpp
    src/tooling/Json.cpp
    src/tooling/LanguageService.cpp
    src/tooling/LspServer.cpp
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
add_library(RealScript::Runtime ALIAS realscript_frontend)
add_library(RealScript::AotSupport ALIAS realscript_frontend)

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

add_executable(rsdebug tools/rsdebug/main.cpp)
target_link_libraries(rsdebug PRIVATE RealScript::Frontend)
target_compile_features(rsdebug PRIVATE cxx_std_17)

add_executable(rslsp tools/rslsp/main.cpp)
target_link_libraries(rslsp PRIVATE RealScript::Frontend)
target_compile_features(rslsp PRIVATE cxx_std_17)

add_executable(rsaot tools/rsaot/main.cpp)
target_link_libraries(rsaot PRIVATE RealScript::Frontend)
target_compile_features(rsaot PRIVATE cxx_std_17)

include(cmake/RealScriptAot.cmake)

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

    add_executable(realscript_phase3b_tests tests/phase3b_tests.cpp)
    target_link_libraries(realscript_phase3b_tests PRIVATE RealScript::Frontend)
    target_compile_features(realscript_phase3b_tests PRIVATE cxx_std_17)
    add_test(NAME realscript.phase3b COMMAND realscript_phase3b_tests)

    add_executable(realscript_phase3c_tests tests/phase3c_tests.cpp)
    target_link_libraries(realscript_phase3c_tests PRIVATE RealScript::Frontend)
    target_compile_features(realscript_phase3c_tests PRIVATE cxx_std_17)
    add_test(NAME realscript.phase3c COMMAND realscript_phase3c_tests)

    add_executable(realscript_phase3d_tests tests/phase3d_tests.cpp)
    target_link_libraries(realscript_phase3d_tests PRIVATE RealScript::Frontend)
    target_compile_features(realscript_phase3d_tests PRIVATE cxx_std_17)
    add_test(NAME realscript.phase3d COMMAND realscript_phase3d_tests)

    add_executable(realscript_phase3e_tests tests/phase3e_tests.cpp)
    target_link_libraries(realscript_phase3e_tests PRIVATE RealScript::Frontend)
    target_compile_features(realscript_phase3e_tests PRIVATE cxx_std_17)
    add_test(NAME realscript.phase3e COMMAND realscript_phase3e_tests)

    add_executable(realscript_phase4_tests tests/phase4_tests.cpp)
    target_link_libraries(realscript_phase4_tests PRIVATE RealScript::Frontend)
    target_compile_features(realscript_phase4_tests PRIVATE cxx_std_17)
    add_test(NAME realscript.phase4 COMMAND realscript_phase4_tests)


    add_executable(realscript_phase5_tests tests/phase5_tests.cpp)
    target_link_libraries(realscript_phase5_tests PRIVATE RealScript::Frontend)
    target_compile_features(realscript_phase5_tests PRIVATE cxx_std_17)
    target_compile_definitions(
        realscript_phase5_tests
        PRIVATE REALSCRIPT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME realscript.phase5.generator COMMAND realscript_phase5_tests)

    add_executable(realscript_phase5_abi_c_test tests/phase5_abi_c_test.c)
    target_include_directories(
        realscript_phase5_abi_c_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    set_target_properties(
        realscript_phase5_abi_c_test
        PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO
    )
    if(MSVC)
        target_compile_options(realscript_phase5_abi_c_test PRIVATE /W4)
        if(REALSCRIPT_WARNINGS_AS_ERRORS)
            target_compile_options(realscript_phase5_abi_c_test PRIVATE /WX)
        endif()
    else()
        target_compile_options(
            realscript_phase5_abi_c_test PRIVATE -Wall -Wextra -Wpedantic
        )
        if(REALSCRIPT_WARNINGS_AS_ERRORS)
            target_compile_options(realscript_phase5_abi_c_test PRIVATE -Werror)
        endif()
    endif()
    add_test(NAME realscript.phase5.abi-c COMMAND realscript_phase5_abi_c_test)

    set(REALSCRIPT_PHASE5_GENERATED_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/generated/phase5")
    realscript_add_aot_library(
        realscript_phase5_generated
        PROGRAM_NAME Phase5Fixture
        OUTPUT_DIRECTORY "${REALSCRIPT_PHASE5_GENERATED_DIR}"
        SOURCES
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/phase5_model.rs"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/phase5_app.rs"
    )

    add_executable(realscript_phase5_aot_tests tests/phase5_aot_tests.cpp)
    target_link_libraries(
        realscript_phase5_aot_tests
        PRIVATE realscript_phase5_generated RealScript::Frontend
    )
    target_compile_features(realscript_phase5_aot_tests PRIVATE cxx_std_17)
    target_compile_definitions(
        realscript_phase5_aot_tests
        PRIVATE REALSCRIPT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME realscript.phase5.aot COMMAND realscript_phase5_aot_tests)
endif()
