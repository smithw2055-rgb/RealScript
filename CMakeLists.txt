cmake_minimum_required(VERSION 3.20)

project(RealScript VERSION 0.2.0 LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(REALSCRIPT_BUILD_TESTS "Build RealScript conformance tests" ON)
option(REALSCRIPT_BUILD_BENCHMARKS "Build RealScript performance benchmarks" OFF)
option(REALSCRIPT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(REALSCRIPT_ENABLE_TOOLCHAIN_JIT_TESTS "Run external C++ toolchain JIT integration tests" ON)
option(REALSCRIPT_INSTALL_SDK "Generate installable RealScript SDK targets" ON)

# Component static libraries are the supported consumer surface. Downstream
# CMake files keep linking RealScript::GameSdk / ::Compiler / ::Runtime /
# ::Full (and the other exported names) without depending on how sources are
# partitioned internally.
#
# Remaining shared-archive constraints, documented for a later split:
# - Runtime still PUBLIC-links Compiler because bytecode decode/verify and
#   semantic helpers (Symbols.cpp) are called from the interpreter/heap, and
#   moving those TUs would be a semantic/bytecode refactor.
# - Interpreter calls DebugController, so Debugger.cpp lives in Runtime rather
#   than Debug. Splitting it out would create a Runtime <-> Debug link cycle.
# - AotCpp.cpp (the generator) stays in Compiler because it consumes MIR;
#   AotRuntime.cpp + Jit.cpp live in AotSupport. The JIT host links a single
#   support .a (ToolchainOptions::supportLibrary), so RealScript::Frontend
#   remains a combined convenience archive built from the same object files.

function(realscript_enable_cxx_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        if(REALSCRIPT_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(REALSCRIPT_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

function(realscript_configure_component target)
    set_target_properties(${target} PROPERTIES
        POSITION_INDEPENDENT_CODE ON
    )
    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    target_compile_features(${target} PUBLIC cxx_std_17)
endfunction()

function(realscript_add_component_library target export_name)
    add_library(${target}_objects OBJECT ${ARGN})
    realscript_configure_component(${target}_objects)
    realscript_enable_cxx_warnings(${target}_objects)

    add_library(${target} STATIC $<TARGET_OBJECTS:${target}_objects>)
    realscript_configure_component(${target})
    set_target_properties(${target} PROPERTIES
        EXPORT_NAME ${export_name}
        LINKER_LANGUAGE CXX
    )
    add_library(RealScript::${export_name} ALIAS ${target})
endfunction()

function(realscript_add_umbrella target export_name)
    add_library(${target} INTERFACE)
    target_link_libraries(${target} INTERFACE ${ARGN})
    set_target_properties(${target} PROPERTIES EXPORT_NAME ${export_name})
    add_library(RealScript::${export_name} ALIAS ${target})
endfunction()

set(REALSCRIPT_COMPILER_SOURCES
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
    src/optimization/Optimizer.cpp
    src/debug/DebugInfo.cpp
    src/aot_cpp/AotCpp.cpp
)
set(REALSCRIPT_RUNTIME_SOURCES
    src/runtime/Interpreter.cpp
    src/runtime/RuntimeServices.cpp
    src/runtime/ExecutionServices.cpp
    src/runtime/ManagedHeap.cpp
    src/debug/Debugger.cpp
)
set(REALSCRIPT_AOT_SUPPORT_SOURCES
    src/aot_cpp/AotRuntime.cpp
    src/jit/Jit.cpp
)
set(REALSCRIPT_GAME_RUNTIME_SOURCES
    src/game/GameApi.cpp
    src/game/ScriptObjectRuntime.cpp
    src/game/ScriptInvokeRuntime.cpp
    src/game/ScriptValueRuntime.cpp
    src/game/SceneLifecycleRuntime.cpp
    src/game/SceneEventRuntime.cpp
    src/game/GameProductization.cpp
    src/game/GameplayPrimitives.cpp
    src/game/GameplayMetadata.cpp
    src/game/GameplayScripting.cpp
    src/game/GameplayStateCodec.cpp
)
set(REALSCRIPT_GAME_COMPILER_SOURCES
    src/game/GameScriptCompiler.cpp
)
set(REALSCRIPT_DEBUG_SOURCES
    src/debug/DapServer.cpp
    src/hot_reload/HotReload.cpp
)
set(REALSCRIPT_TOOLING_SOURCES
    src/tooling/Json.cpp
    src/tooling/LanguageService.cpp
    src/tooling/LspServer.cpp
)

realscript_add_component_library(
    realscript_compiler Compiler ${REALSCRIPT_COMPILER_SOURCES})
realscript_add_component_library(
    realscript_runtime Runtime ${REALSCRIPT_RUNTIME_SOURCES})
realscript_add_component_library(
    realscript_aot_support AotSupport ${REALSCRIPT_AOT_SUPPORT_SOURCES})
realscript_add_component_library(
    realscript_game_runtime GameRuntime ${REALSCRIPT_GAME_RUNTIME_SOURCES})
realscript_add_component_library(
    realscript_game_compiler GameCompiler ${REALSCRIPT_GAME_COMPILER_SOURCES})
realscript_add_component_library(
    realscript_debug Debug ${REALSCRIPT_DEBUG_SOURCES})
realscript_add_component_library(
    realscript_tooling Tooling ${REALSCRIPT_TOOLING_SOURCES})

target_link_libraries(realscript_runtime PUBLIC realscript_compiler)
target_link_libraries(realscript_tooling PUBLIC realscript_compiler)
target_link_libraries(realscript_aot_support PUBLIC realscript_runtime)
target_link_libraries(realscript_game_runtime PUBLIC realscript_runtime)
target_link_libraries(
    realscript_game_compiler PUBLIC realscript_compiler realscript_game_runtime)
target_link_libraries(
    realscript_debug PUBLIC realscript_runtime realscript_tooling)
if(CMAKE_DL_LIBS)
    target_link_libraries(realscript_aot_support PUBLIC ${CMAKE_DL_LIBS})
endif()

realscript_add_umbrella(
    realscript_game_sdk GameSdk
    realscript_game_runtime
    realscript_game_compiler)
realscript_add_umbrella(
    realscript_full Full
    realscript_game_sdk
    realscript_aot_support
    realscript_debug
    realscript_tooling)

# Combined convenience archive for RealScript::Frontend compatibility and for
# the toolchain JIT, which still accepts a single support-library path.
add_library(realscript_frontend STATIC
    $<TARGET_OBJECTS:realscript_compiler_objects>
    $<TARGET_OBJECTS:realscript_runtime_objects>
    $<TARGET_OBJECTS:realscript_aot_support_objects>
    $<TARGET_OBJECTS:realscript_game_runtime_objects>
    $<TARGET_OBJECTS:realscript_game_compiler_objects>
    $<TARGET_OBJECTS:realscript_debug_objects>
    $<TARGET_OBJECTS:realscript_tooling_objects>
)
realscript_configure_component(realscript_frontend)
set_target_properties(realscript_frontend PROPERTIES
    EXPORT_NAME Frontend
    LINKER_LANGUAGE CXX
)
add_library(RealScript::Frontend ALIAS realscript_frontend)
if(CMAKE_DL_LIBS)
    target_link_libraries(realscript_frontend PUBLIC ${CMAKE_DL_LIBS})
endif()

if(MSVC)
    set_source_files_properties(
        src/aot_cpp/AotCpp.cpp
        PROPERTIES COMPILE_OPTIONS "/wd4267"
    )
    # MSVC diagnoses repeated if-initializer names across mutually exclusive
    # branches as C4456. These translation units intentionally use that pattern.
    set_source_files_properties(
        src/optimization/Optimizer.cpp
        src/runtime/ExecutionServices.cpp
        src/game/GameProductization.cpp
        src/game/GameplayStateCodec.cpp
        PROPERTIES COMPILE_OPTIONS "/wd4456"
    )
endif()

add_executable(rsc tools/rsc/main.cpp)
target_link_libraries(rsc PRIVATE RealScript::Compiler RealScript::Runtime)
target_compile_features(rsc PRIVATE cxx_std_17)

add_executable(rsdebug tools/rsdebug/main.cpp)
target_link_libraries(rsdebug PRIVATE RealScript::Debug)
target_compile_features(rsdebug PRIVATE cxx_std_17)

add_executable(rslsp tools/rslsp/main.cpp)
target_link_libraries(rslsp PRIVATE RealScript::Tooling)
target_compile_features(rslsp PRIVATE cxx_std_17)

add_executable(rsaot tools/rsaot/main.cpp)
target_link_libraries(rsaot PRIVATE RealScript::Compiler RealScript::AotSupport)
target_compile_features(rsaot PRIVATE cxx_std_17)

add_executable(rsbench tools/rsbench/main.cpp)
target_link_libraries(rsbench PRIVATE RealScript::Compiler RealScript::Runtime)
target_compile_features(rsbench PRIVATE cxx_std_17)

foreach(realscript_tool IN ITEMS rsc rsdebug rslsp rsaot rsbench)
    realscript_enable_cxx_warnings(${realscript_tool})
endforeach()

include(cmake/RealScriptAot.cmake)

if(REALSCRIPT_BUILD_BENCHMARKS)
    set(REALSCRIPT_BENCH_INTEGER_GENERATED_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/generated/bench_integer")
    realscript_add_aot_library(
        realscript_bench_integer_generated
        PROGRAM_NAME IntegerLoop
        OUTPUT_DIRECTORY "${REALSCRIPT_BENCH_INTEGER_GENERATED_DIR}"
        OPT_LEVEL 2
        SOURCES
            "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/micro/integer_loop.rs"
    )
    add_executable(
        rsbench_aot benchmarks/aot_integer_driver.cpp)
    target_link_libraries(
        rsbench_aot
        PRIVATE realscript_bench_integer_generated RealScript::Full)
    target_compile_features(rsbench_aot PRIVATE cxx_std_17)
    realscript_enable_cxx_warnings(rsbench_aot)

    add_executable(
        rsbench_jit benchmarks/jit_integer_driver.cpp)
    target_link_libraries(rsbench_jit PRIVATE RealScript::Full)
    target_compile_features(rsbench_jit PRIVATE cxx_std_17)
    target_compile_definitions(
        rsbench_jit
        PRIVATE
            REALSCRIPT_BENCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
            REALSCRIPT_BENCH_JIT_COMPILER="${CMAKE_CXX_COMPILER}"
            REALSCRIPT_BENCH_JIT_INCLUDE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/include"
            REALSCRIPT_BENCH_JIT_SUPPORT_LIBRARY="$<TARGET_FILE:realscript_frontend>"
            REALSCRIPT_BENCH_JIT_CACHE_BASE="${CMAKE_CURRENT_BINARY_DIR}/jit-benchmark-cache"
    )
    realscript_enable_cxx_warnings(rsbench_jit)

    add_executable(
        rsbench_product benchmarks/product_macro_driver.cpp)
    target_link_libraries(rsbench_product PRIVATE RealScript::Full)
    target_compile_features(rsbench_product PRIVATE cxx_std_17)
    realscript_enable_cxx_warnings(rsbench_product)
endif()

if(REALSCRIPT_BUILD_TESTS)
    enable_testing()

    function(realscript_add_cpp_test target test_name source)
        add_executable(${target} ${source})
        target_link_libraries(${target} PRIVATE RealScript::Full)
        target_compile_features(${target} PRIVATE cxx_std_17)
        realscript_enable_cxx_warnings(${target})
        add_test(NAME ${test_name} COMMAND ${target})
    endfunction()

    realscript_add_cpp_test(realscript_tests realscript.frontend tests/tests.cpp)
    target_compile_definitions(
        realscript_tests PRIVATE REALSCRIPT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

    realscript_add_cpp_test(
        realscript_phase1c_tests realscript.phase1c tests/phase1c_tests.cpp)
    realscript_add_cpp_test(
        realscript_phase2a_tests realscript.phase2a tests/phase2a_tests.cpp)
    target_compile_definitions(
        realscript_phase2a_tests PRIVATE REALSCRIPT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    realscript_add_cpp_test(
        realscript_phase2b_tests realscript.phase2b tests/phase2b_tests.cpp)
    realscript_add_cpp_test(
        realscript_phase2c_tests realscript.phase2c tests/phase2c_tests.cpp)
    realscript_add_cpp_test(
        realscript_phase3a_tests realscript.phase3a tests/phase3a_tests.cpp)
    realscript_add_cpp_test(
        realscript_phase3b_tests realscript.phase3b tests/phase3b_tests.cpp)
    realscript_add_cpp_test(
        realscript_phase3c_tests realscript.phase3c tests/phase3c_tests.cpp)
    realscript_add_cpp_test(
        realscript_phase3d_tests realscript.phase3d tests/phase3d_tests.cpp)
    realscript_add_cpp_test(
        realscript_phase3e_tests realscript.phase3e tests/phase3e_tests.cpp)
    realscript_add_cpp_test(
        realscript_phase4_tests realscript.phase4 tests/phase4_tests.cpp)
    realscript_add_cpp_test(
        realscript_phase5_tests realscript.phase5.generator tests/phase5_tests.cpp)
    target_compile_definitions(
        realscript_phase5_tests PRIVATE REALSCRIPT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

    add_executable(realscript_phase5_abi_c_test tests/phase5_abi_c_test.c)
    target_include_directories(
        realscript_phase5_abi_c_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    set_target_properties(
        realscript_phase5_abi_c_test
        PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(realscript_phase5_abi_c_test PRIVATE /W4)
        if(REALSCRIPT_WARNINGS_AS_ERRORS)
            target_compile_options(realscript_phase5_abi_c_test PRIVATE /WX)
        endif()
    else()
        target_compile_options(
            realscript_phase5_abi_c_test PRIVATE -Wall -Wextra -Wpedantic)
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
        OPT_LEVEL 0
        SOURCES
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/phase5_model.rs"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/phase5_app.rs"
    )

    add_executable(realscript_phase5_aot_tests tests/phase5_aot_tests.cpp)
    target_link_libraries(
        realscript_phase5_aot_tests
        PRIVATE realscript_phase5_generated RealScript::Full)
    target_compile_features(realscript_phase5_aot_tests PRIVATE cxx_std_17)
    target_compile_definitions(
        realscript_phase5_aot_tests
        PRIVATE REALSCRIPT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME realscript.phase5.aot COMMAND realscript_phase5_aot_tests)

    if(REALSCRIPT_ENABLE_TOOLCHAIN_JIT_TESTS)
        realscript_add_cpp_test(
            realscript_phase6_jit_tests
            realscript.phase6.jit
            tests/phase6_jit_tests.cpp)
        target_compile_definitions(
            realscript_phase6_jit_tests
            PRIVATE
                REALSCRIPT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
                REALSCRIPT_JIT_COMPILER="${CMAKE_CXX_COMPILER}"
                REALSCRIPT_JIT_INCLUDE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/include"
                REALSCRIPT_JIT_SUPPORT_LIBRARY="$<TARGET_FILE:realscript_frontend>"
                REALSCRIPT_JIT_CACHE_DIR="${CMAKE_CURRENT_BINARY_DIR}/jit-cache"
        )
    endif()

    realscript_add_cpp_test(
        realscript_phase6_tests realscript.phase6.core tests/phase6_tests.cpp)

    add_executable(realscript_phase6_aot_tests tests/phase6_aot_tests.cpp)
    target_link_libraries(
        realscript_phase6_aot_tests
        PRIVATE realscript_phase5_generated RealScript::Full)
    target_compile_features(realscript_phase6_aot_tests PRIVATE cxx_std_17)
    target_compile_definitions(
        realscript_phase6_aot_tests
        PRIVATE REALSCRIPT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME realscript.phase6.aot COMMAND realscript_phase6_aot_tests)

    realscript_add_cpp_test(
        realscript_phase7_tests
        realscript.phase7.game-sdk
        tests/phase7_tests.cpp)
    realscript_add_cpp_test(
        realscript_productization_tests
        realscript.productization
        tests/productization_tests.cpp)
    realscript_add_cpp_test(
        realscript_phase8_gameplay_primitives_tests
        realscript.phase8.gameplay-primitives
        tests/phase8_gameplay_primitives_tests.cpp)
    realscript_add_cpp_test(
        realscript_phase9_gameplay_scripting_tests
        realscript.phase9.gameplay-scripting
        tests/phase9_gameplay_scripting_tests.cpp)
    realscript_add_cpp_test(
        realscript_phase10_gameplay_state_tests
        realscript.phase10.gameplay-state
        tests/phase10_gameplay_state_tests.cpp)
    realscript_add_cpp_test(
        realscript_phase11_18_native_language_tests
        realscript.phase11-18.native-language
        tests/phase11_18_native_language_tests.cpp)
    realscript_add_cpp_test(
        realscript_phase18_native_control_flow_tests
        realscript.phase18.native-control-flow
        tests/phase18_native_control_flow_tests.cpp)
    realscript_add_cpp_test(
        realscript_phase19_runtime_polymorphism_tests
        realscript.phase19.runtime-polymorphism
        tests/phase19_runtime_polymorphism_tests.cpp)
    realscript_add_cpp_test(
        realscript_phase20_delegate_tests
        realscript.phase20.delegates
        tests/phase20_delegate_tests.cpp)
    realscript_add_cpp_test(
        realscript_phase21_generics_collections_tests
        realscript.phase21.generics-collections
        tests/phase21_generics_collections_tests.cpp)
    target_compile_definitions(
        realscript_phase21_generics_collections_tests
        PRIVATE REALSCRIPT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    realscript_add_cpp_test(
        realscript_phase22_coroutine_tests
        realscript.phase22.coroutines
        tests/phase22_coroutine_tests.cpp)
    realscript_add_cpp_test(
        realscript_phase23_value_semantics_tests
        realscript.phase23.value-semantics
        tests/phase23_value_semantics_tests.cpp)
    realscript_add_cpp_test(
        realscript_phase24_language_completeness_tests
        realscript.phase24.language-completeness
        tests/phase24_language_completeness_tests.cpp)
    set(REALSCRIPT_PHASE20_GENERATED_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/generated/phase20")
    realscript_add_aot_library(
        realscript_phase20_generated
        PROGRAM_NAME Phase20Aot
        OUTPUT_DIRECTORY "${REALSCRIPT_PHASE20_GENERATED_DIR}"
        OPT_LEVEL 2
        SOURCES
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/phase20_aot.rs"
    )
    add_executable(
        realscript_phase20_aot_tests tests/phase20_aot_tests.cpp)
    target_link_libraries(
        realscript_phase20_aot_tests
        PRIVATE realscript_phase20_generated RealScript::Full)
    target_compile_features(
        realscript_phase20_aot_tests PRIVATE cxx_std_17)
    target_compile_definitions(
        realscript_phase20_aot_tests
        PRIVATE REALSCRIPT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(
        NAME realscript.phase20.aot
        COMMAND realscript_phase20_aot_tests)
    set(REALSCRIPT_PHASE21_GENERATED_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/generated/phase21")
    realscript_add_aot_library(
        realscript_phase21_generated
        PROGRAM_NAME Phase21Aot
        OUTPUT_DIRECTORY "${REALSCRIPT_PHASE21_GENERATED_DIR}"
        OPT_LEVEL 2
        SOURCES
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/phase21_aot.rs"
    )
    add_executable(
        realscript_phase21_aot_tests tests/phase21_aot_tests.cpp)
    target_link_libraries(
        realscript_phase21_aot_tests
        PRIVATE realscript_phase21_generated RealScript::Full)
    target_compile_features(
        realscript_phase21_aot_tests PRIVATE cxx_std_17)
    target_compile_definitions(
        realscript_phase21_aot_tests
        PRIVATE REALSCRIPT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(
        NAME realscript.phase21.aot
        COMMAND realscript_phase21_aot_tests)
    set(REALSCRIPT_PHASE22_GENERATED_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/generated/phase22")
    realscript_add_aot_library(
        realscript_phase22_generated
        PROGRAM_NAME Phase22Aot
        OUTPUT_DIRECTORY "${REALSCRIPT_PHASE22_GENERATED_DIR}"
        OPT_LEVEL 2
        SOURCES
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/phase22_aot.rs"
    )
    add_executable(
        realscript_phase22_aot_tests tests/phase22_aot_tests.cpp)
    target_link_libraries(
        realscript_phase22_aot_tests
        PRIVATE realscript_phase22_generated RealScript::Full)
    target_compile_features(
        realscript_phase22_aot_tests PRIVATE cxx_std_17)
    target_compile_definitions(
        realscript_phase22_aot_tests
        PRIVATE REALSCRIPT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(
        NAME realscript.phase22.aot
        COMMAND realscript_phase22_aot_tests)
    set(REALSCRIPT_PHASE23_GENERATED_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/generated/phase23")
    realscript_add_aot_library(
        realscript_phase23_generated
        PROGRAM_NAME Phase23Aot
        OUTPUT_DIRECTORY "${REALSCRIPT_PHASE23_GENERATED_DIR}"
        OPT_LEVEL 2
        SOURCES
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/phase23_aot.rs"
    )
    add_executable(
        realscript_phase23_aot_tests tests/phase23_aot_tests.cpp)
    target_link_libraries(
        realscript_phase23_aot_tests
        PRIVATE realscript_phase23_generated RealScript::Full)
    target_compile_features(
        realscript_phase23_aot_tests PRIVATE cxx_std_17)
    target_compile_definitions(
        realscript_phase23_aot_tests
        PRIVATE REALSCRIPT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(
        NAME realscript.phase23.aot
        COMMAND realscript_phase23_aot_tests)
    set(REALSCRIPT_PHASE24_GENERATED_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/generated/phase24")
    realscript_add_aot_library(
        realscript_phase24_generated
        PROGRAM_NAME Phase24Aot
        OUTPUT_DIRECTORY "${REALSCRIPT_PHASE24_GENERATED_DIR}"
        OPT_LEVEL 2
        SOURCES
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/phase24_aot.rs"
    )
    add_executable(
        realscript_phase24_aot_tests tests/phase24_aot_tests.cpp)
    target_link_libraries(
        realscript_phase24_aot_tests
        PRIVATE realscript_phase24_generated RealScript::Full)
    target_compile_features(
        realscript_phase24_aot_tests PRIVATE cxx_std_17)
    target_compile_definitions(
        realscript_phase24_aot_tests
        PRIVATE REALSCRIPT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(
        NAME realscript.phase24.aot
        COMMAND realscript_phase24_aot_tests)
    set(REALSCRIPT_PHASE19_GENERATED_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/generated/phase19")
    realscript_add_aot_library(
        realscript_phase19_generated
        PROGRAM_NAME Phase19Aot
        OUTPUT_DIRECTORY "${REALSCRIPT_PHASE19_GENERATED_DIR}"
        OPT_LEVEL 2
        SOURCES
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/phase19_aot.rs"
    )
    add_executable(
        realscript_phase19_aot_tests tests/phase19_aot_tests.cpp)
    target_link_libraries(
        realscript_phase19_aot_tests
        PRIVATE realscript_phase19_generated RealScript::Full)
    target_compile_features(
        realscript_phase19_aot_tests PRIVATE cxx_std_17)
    target_compile_definitions(
        realscript_phase19_aot_tests
        PRIVATE REALSCRIPT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(
        NAME realscript.phase19.aot
        COMMAND realscript_phase19_aot_tests)
    if(REALSCRIPT_ENABLE_TOOLCHAIN_JIT_TESTS)
        target_compile_definitions(
            realscript_phase19_runtime_polymorphism_tests
            PRIVATE
                REALSCRIPT_PHASE19_JIT_COMPILER="${CMAKE_CXX_COMPILER}"
                REALSCRIPT_PHASE19_JIT_INCLUDE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/include"
                REALSCRIPT_PHASE19_JIT_SUPPORT_LIBRARY="$<TARGET_FILE:realscript_frontend>"
                REALSCRIPT_PHASE19_JIT_CACHE_DIR="${CMAKE_CURRENT_BINARY_DIR}/phase19-jit-cache"
        )
    endif()

    add_test(
        NAME realscript.phase6.cli-smoke
        COMMAND rsc
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/phase5_model.rs"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/phase5_app.rs"
            --run Phase5.App::main
            --opt-level 2
            --deterministic
            --digest
            --profile
    )
    add_test(
        NAME realscript.phase6.benchmark-smoke
        COMMAND rsbench
            --entry Phase5.App::main
            --iterations 3
            --warmup 1
            --opt-level 2
            --json
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/phase5_model.rs"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/phase5_app.rs"
    )
    add_test(
        NAME realscript.phase6.cli-multimodule-package
        COMMAND ${CMAKE_COMMAND}
            -DREALSCRIPT_RSC=$<TARGET_FILE:rsc>
            -DREALSCRIPT_SOURCE_A=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/phase5_model.rs
            -DREALSCRIPT_SOURCE_B=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/phase5_app.rs
            -DREALSCRIPT_OUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-multimodule/$<CONFIG>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/multimodule_cli_test.cmake
    )

    foreach(realscript_aot_test IN ITEMS
            realscript_phase5_aot_tests
            realscript_phase6_aot_tests
            realscript_phase19_aot_tests
            realscript_phase20_aot_tests
            realscript_phase21_aot_tests
            realscript_phase22_aot_tests
            realscript_phase23_aot_tests
            realscript_phase24_aot_tests)
        realscript_enable_cxx_warnings(${realscript_aot_test})
    endforeach()

    add_test(
        NAME realscript.productization.install-consumer
        COMMAND ${CMAKE_COMMAND}
            -DREALSCRIPT_BUILD_DIR=${CMAKE_BINARY_DIR}
            -DREALSCRIPT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DREALSCRIPT_INSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/install-consumer/$<CONFIG>/sdk
            -DREALSCRIPT_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}
            -DREALSCRIPT_CONSUMER_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}/install-consumer/$<CONFIG>/build
            -DREALSCRIPT_CONFIG=$<CONFIG>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/install_consumer_test.cmake
    )
endif()

if(REALSCRIPT_INSTALL_SDK)
    install(
        TARGETS
            realscript_frontend
            realscript_runtime
            realscript_compiler
            realscript_game_runtime
            realscript_game_compiler
            realscript_game_sdk
            realscript_aot_support
            realscript_debug
            realscript_tooling
            realscript_full
            rsaot
        EXPORT RealScriptTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(
        TARGETS rsc rsdebug rslsp rsbench
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(FILES LICENSE
        DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/RealScript)
    install(FILES cmake/RealScriptAot.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RealScript)

    configure_package_config_file(
        cmake/RealScriptConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/RealScriptConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RealScript
    )
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/RealScriptConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMinorVersion
    )
    install(
        EXPORT RealScriptTargets
        FILE RealScriptTargets.cmake
        NAMESPACE RealScript::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RealScript
    )
    install(
        FILES
            "${CMAKE_CURRENT_BINARY_DIR}/RealScriptConfig.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/RealScriptConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RealScript
    )
endif()
