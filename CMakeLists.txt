cmake_minimum_required(VERSION 3.20)

project(RealScript VERSION 0.98.0 LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(REALSCRIPT_BUILD_TESTS "Build RealScript conformance tests" ON)
option(REALSCRIPT_BUILD_BENCHMARKS "Build RealScript performance benchmarks" OFF)
option(REALSCRIPT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(REALSCRIPT_ENABLE_TOOLCHAIN_JIT_TESTS "Run external C++ toolchain JIT integration tests" ON)
option(REALSCRIPT_INSTALL_SDK "Generate installable RealScript SDK targets" ON)
option(REALSCRIPT_ENABLE_DEBUG_SERVICES "Build L1 Debug & DAP application layer" ON)
option(REALSCRIPT_ENABLE_TOOLING "Build L1 Formatter, Linter, Fuzzer & LSP application layer" ON)
option(REALSCRIPT_ENABLE_EXTENSIBILITY "Build L1 Reflection & Sandboxed ModHost application layer" ON)
option(REALSCRIPT_ENABLE_GAME_SDK "Build L1 Game Engine SDK & Scene Runtime application layer" ON)

function(realscript_enable_cxx_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /wd4324 /permissive-)
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

if(MSVC)
    set_source_files_properties(
        src/aot_cpp/AotRangeAnalysis.cpp
        src/aot_cpp/AotTypedAnalysis.cpp
        src/aot_cpp/AotSimdVectorizer.cpp
        src/aot_cpp/AotTypedEmitter.cpp
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
        src/reflection/Reflection.cpp
        src/plugin/ModHost.cpp
        PROPERTIES COMPILE_OPTIONS "/wd4456"
    )
endif()

# Physical layered OBJECT targets (each .cpp compiled once, zero circular deps).
function(realscript_add_layer_library target)
    add_library(${target} OBJECT ${ARGN})
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(${target}
        PRIVATE
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    )
    target_compile_features(${target} PRIVATE cxx_std_17)
    realscript_enable_cxx_warnings(${target})
endfunction()

# Physical STATIC slice archives (each .cpp belongs to at most one STATIC archive).
function(realscript_add_slice_library target export_name)
    add_library(${target} STATIC ${ARGN})
    set_target_properties(${target} PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        LINKER_LANGUAGE CXX
        EXPORT_NAME ${export_name}
    )
    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    target_compile_features(${target} PUBLIC cxx_std_17)
    add_library(RealScript::${export_name} ALIAS ${target})
endfunction()

# ============================================================================
# Tier 0: L0 Microkernel Physical STATIC Slices (always built)
# ============================================================================

# 1. Foundation (text, GUID, diagnostics, JSON primitives, pure semantic type metadata)
realscript_add_layer_library(realscript_layer_foundation
    src/text/Text.cpp
    src/text/AssetGuid.cpp
    src/diagnostics/Diagnostic.cpp
    src/tooling/Json.cpp
    src/semantic/TypeMetadata.cpp
)
realscript_add_slice_library(realscript_slice_foundation_core Foundation
    $<TARGET_OBJECTS:realscript_layer_foundation>
)
if(MSVC)
    target_sources(realscript_slice_foundation_core INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tools/debuggers/RealScript.natvis>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_DATADIR}/RealScript/debuggers/RealScript.natvis>
    )
endif()

# 2. Bytecode Format & Debug Metadata (shared contract between Compiler and Runtime)
realscript_add_layer_library(realscript_layer_bytecode_format
    src/bytecode/BytecodeFacts.cpp
    src/bytecode/BytecodeVerifier.cpp
    src/bytecode/BytecodeDisassembler.cpp
    src/bytecode/BytecodeCodec.cpp
    src/debug/DebugInfo.cpp
)
realscript_add_slice_library(realscript_slice_bytecode_core Bytecode
    $<TARGET_OBJECTS:realscript_layer_bytecode_format>
)
target_link_libraries(realscript_slice_bytecode_core PUBLIC realscript_slice_foundation_core)

# 3. Compiler Core (syntax, semantic symbols/binder, MIR, optimizer, bytecode lowerer, compiler)
realscript_add_layer_library(realscript_layer_compiler_core
    src/syntax/SyntaxFacts.cpp
    src/syntax/Lexer.cpp
    src/syntax/SyntaxNodes.cpp
    src/syntax/Parser.cpp
    src/semantic/Symbols.cpp
    src/semantic/FlowAnalysis.cpp
    src/semantic/SemanticBinding.cpp
    src/semantic/SemanticOverload.cpp
    src/semantic/SemanticOperators.cpp
    src/semantic/SemanticPatterns.cpp
    src/semantic/SemanticCallsAndObjects.cpp
    src/semantic/SemanticAccessAndAssign.cpp
    src/mir/MirFacts.cpp
    src/mir/MirLowerer.cpp
    src/mir/MirExpressions.cpp
    src/mir/MirVerifier.cpp
    src/mir/MirPrinter.cpp
    src/optimization/Optimizer.cpp
    src/bytecode/BytecodeLowerer.cpp
    src/compiler/BuiltinModules.cpp
    src/compiler/NativeGenerics.cpp
    src/compiler/TypeHierarchyAndContracts.cpp
    src/compiler/SequenceAndEventSynthesis.cpp
    src/compiler/MetadataAndValidation.cpp
    src/compiler/Compilation.cpp
    src/compiler/ModuleBuildCache.cpp
    src/compiler/ProjectConfig.cpp
)
realscript_add_slice_library(realscript_slice_compiler_core Compiler
    $<TARGET_OBJECTS:realscript_layer_compiler_core>
)
target_link_libraries(realscript_slice_compiler_core PUBLIC realscript_slice_bytecode_core)

# 4. Runtime Core (HostAbi, ABI governance, VM, GC, fibers, async)
realscript_add_layer_library(realscript_layer_runtime_core
    src/runtime/HostAbi.cpp
    src/runtime/AbiGovernance.cpp
    src/runtime/ValueOperations.cpp
    src/runtime/BindingRegistry.cpp
    src/runtime/ProgramImage.cpp
    src/runtime/RuntimeServices.cpp
    src/runtime/Interpreter.cpp
    src/runtime/InterpreterSlowOps.cpp
    src/runtime/InterpreterFlatLoop.cpp
    src/runtime/ExecutionServices.cpp
    src/runtime/ManagedHeap.cpp
    src/runtime/ParallelJob.cpp
    src/runtime/FiberScheduler.cpp
    src/runtime/AsyncRuntime.cpp
)
realscript_add_slice_library(realscript_slice_runtime_core Runtime
    $<TARGET_OBJECTS:realscript_layer_runtime_core>
)
target_link_libraries(realscript_slice_runtime_core PUBLIC realscript_slice_bytecode_core)
if(CMAKE_DL_LIBS)
    target_link_libraries(realscript_slice_runtime_core PUBLIC ${CMAKE_DL_LIBS})
endif()

# 5. Native Backends & Hot Reload (aot_cpp, jit, hot_reload)
realscript_add_layer_library(realscript_layer_native_backends
    src/aot_cpp/AotRuntime.cpp
    src/aot_cpp/AotRangeAnalysis.cpp
    src/aot_cpp/AotTypedAnalysis.cpp
    src/aot_cpp/AotSimdVectorizer.cpp
    src/aot_cpp/AotTypedEmitter.cpp
    src/aot_cpp/AotCpp.cpp
    src/jit/ToolchainJit.cpp
    src/jit/StencilJitRuntime.cpp
    src/jit/StencilJitX64.cpp
    src/jit/StencilJitArm64.cpp
    src/jit/StencilJit.cpp
    src/hot_reload/HotReload.cpp
)
realscript_add_slice_library(realscript_slice_native_backends NativeBackends
    $<TARGET_OBJECTS:realscript_layer_native_backends>
)
target_link_libraries(realscript_slice_native_backends PUBLIC
    realscript_slice_runtime_core
    realscript_slice_compiler_core
)

# 6. L0 INTERFACE Composition & Internal JIT Support Archive
add_library(realscript_aot_support INTERFACE)
target_link_libraries(realscript_aot_support INTERFACE realscript_slice_native_backends)
set_target_properties(realscript_aot_support PROPERTIES EXPORT_NAME AotSupport)
add_library(RealScript::AotSupport ALIAS realscript_aot_support)

add_library(realscript_kernel INTERFACE)
target_link_libraries(realscript_kernel INTERFACE
    realscript_slice_native_backends
    realscript_slice_compiler_core
    realscript_slice_runtime_core
)
set_target_properties(realscript_kernel PROPERTIES EXPORT_NAME Kernel)
add_library(RealScript::Kernel ALIAS realscript_kernel)

if(REALSCRIPT_BUILD_TESTS OR REALSCRIPT_BUILD_BENCHMARKS)
    add_library(realscript_jit_support_archive STATIC EXCLUDE_FROM_ALL
        $<TARGET_OBJECTS:realscript_layer_foundation>
        $<TARGET_OBJECTS:realscript_layer_bytecode_format>
        $<TARGET_OBJECTS:realscript_layer_runtime_core>
        $<TARGET_OBJECTS:realscript_layer_native_backends>
    )
    set_target_properties(realscript_jit_support_archive PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        LINKER_LANGUAGE CXX
    )
    target_include_directories(realscript_jit_support_archive
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    )
    target_compile_features(realscript_jit_support_archive PUBLIC cxx_std_17)
    if(CMAKE_DL_LIBS)
        target_link_libraries(realscript_jit_support_archive PUBLIC ${CMAKE_DL_LIBS})
    endif()
endif()

# ============================================================================
# Tier 1: L1 Application Physical STATIC Libraries (optional)
# ============================================================================

if(REALSCRIPT_ENABLE_DEBUG_SERVICES)
    realscript_add_layer_library(realscript_layer_debug
        src/debug/Debugger.cpp
        src/debug/DapServer.cpp
    )
    realscript_add_slice_library(realscript_app_debug Debug
        $<TARGET_OBJECTS:realscript_layer_debug>
    )
    target_link_libraries(realscript_app_debug PUBLIC
        realscript_slice_runtime_core
        realscript_slice_compiler_core
    )

    add_executable(rsdebug tools/rsdebug/main.cpp)
    target_link_libraries(rsdebug PRIVATE RealScript::Debug)
    target_compile_features(rsdebug PRIVATE cxx_std_17)
    realscript_enable_cxx_warnings(rsdebug)
endif()

if(REALSCRIPT_ENABLE_TOOLING)
    realscript_add_layer_library(realscript_layer_tooling
        src/tooling/Formatter.cpp
        src/tooling/GovernanceLinter.cpp
        src/tooling/GovernanceFuzzer.cpp
        src/tooling/LanguageService.cpp
        src/tooling/LspServer.cpp
    )
    realscript_add_slice_library(realscript_app_tooling Tooling
        $<TARGET_OBJECTS:realscript_layer_tooling>
    )
    target_link_libraries(realscript_app_tooling PUBLIC
        realscript_slice_compiler_core
        realscript_slice_runtime_core
        realscript_slice_native_backends
    )

    add_executable(rslsp tools/rslsp/main.cpp)
    target_link_libraries(rslsp PRIVATE RealScript::Tooling)
    target_compile_features(rslsp PRIVATE cxx_std_17)
    realscript_enable_cxx_warnings(rslsp)
endif()

if(REALSCRIPT_ENABLE_EXTENSIBILITY)
    realscript_add_layer_library(realscript_layer_extensibility
        src/reflection/Reflection.cpp
        src/plugin/ModHost.cpp
    )
    realscript_add_slice_library(realscript_app_extensibility Extensibility
        $<TARGET_OBJECTS:realscript_layer_extensibility>
    )
    target_link_libraries(realscript_app_extensibility PUBLIC
        realscript_slice_runtime_core
        realscript_slice_compiler_core
    )
endif()

if(REALSCRIPT_ENABLE_GAME_SDK)
    realscript_add_layer_library(realscript_layer_game
        src/game/RsBindParser.cpp
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
    realscript_add_slice_library(realscript_app_game_sdk GameSdk
        $<TARGET_OBJECTS:realscript_layer_game>
    )
    target_link_libraries(realscript_app_game_sdk PUBLIC
        realscript_slice_runtime_core
        realscript_slice_compiler_core
        realscript_slice_native_backends
    )
    if(REALSCRIPT_ENABLE_DEBUG_SERVICES)
        target_link_libraries(realscript_app_game_sdk PUBLIC realscript_app_debug)
        target_compile_definitions(realscript_layer_game PRIVATE REALSCRIPT_HAS_DEBUG_SERVICES=1)
        target_compile_definitions(realscript_app_game_sdk PUBLIC REALSCRIPT_HAS_DEBUG_SERVICES=1)
    else()
        target_compile_definitions(realscript_layer_game PRIVATE REALSCRIPT_HAS_DEBUG_SERVICES=0)
        target_compile_definitions(realscript_app_game_sdk PUBLIC REALSCRIPT_HAS_DEBUG_SERVICES=0)
    endif()

    add_library(realscript_game_runtime INTERFACE)
    target_link_libraries(realscript_game_runtime INTERFACE realscript_app_game_sdk)
    set_target_properties(realscript_game_runtime PROPERTIES EXPORT_NAME GameRuntime)
    add_library(RealScript::GameRuntime ALIAS realscript_game_runtime)

    add_library(realscript_game_compiler INTERFACE)
    target_link_libraries(realscript_game_compiler INTERFACE realscript_app_game_sdk)
    set_target_properties(realscript_game_compiler PROPERTIES EXPORT_NAME GameCompiler)
    add_library(RealScript::GameCompiler ALIAS realscript_game_compiler)

    add_executable(rsbind tools/rsbind/main.cpp)
    target_link_libraries(rsbind PRIVATE RealScript::GameSdk)
    target_compile_features(rsbind PRIVATE cxx_std_17)
    realscript_enable_cxx_warnings(rsbind)
endif()

# ============================================================================
# Always-Built L0 CLIs & Umbrella INTERFACE Targets
# ============================================================================

add_executable(rsc tools/rsc/main.cpp)
target_link_libraries(rsc PRIVATE RealScript::Compiler RealScript::Runtime)
target_compile_features(rsc PRIVATE cxx_std_17)
if(REALSCRIPT_ENABLE_TOOLING)
    target_link_libraries(rsc PRIVATE RealScript::Tooling)
    target_compile_definitions(rsc PRIVATE REALSCRIPT_HAS_TOOLING=1)
else()
    target_compile_definitions(rsc PRIVATE REALSCRIPT_HAS_TOOLING=0)
endif()
realscript_enable_cxx_warnings(rsc)

add_executable(rsaot tools/rsaot/main.cpp)
target_link_libraries(rsaot PRIVATE RealScript::Compiler RealScript::AotSupport)
target_compile_features(rsaot PRIVATE cxx_std_17)
realscript_enable_cxx_warnings(rsaot)

add_executable(rsbench tools/rsbench/main.cpp)
target_link_libraries(rsbench PRIVATE RealScript::Compiler RealScript::Runtime)
target_compile_features(rsbench PRIVATE cxx_std_17)
realscript_enable_cxx_warnings(rsbench)

add_library(realscript_frontend INTERFACE)
target_link_libraries(realscript_frontend INTERFACE realscript_kernel)
set_target_properties(realscript_frontend PROPERTIES EXPORT_NAME Frontend)
add_library(RealScript::Frontend ALIAS realscript_frontend)

add_library(realscript_full INTERFACE)
target_link_libraries(realscript_full INTERFACE realscript_kernel)
set_target_properties(realscript_full PROPERTIES EXPORT_NAME Full)
add_library(RealScript::Full ALIAS realscript_full)

foreach(_rs_opt IN ITEMS DEBUG_SERVICES TOOLING EXTENSIBILITY GAME_SDK)
    if(REALSCRIPT_ENABLE_${_rs_opt})
        set(REALSCRIPT_HAS_${_rs_opt}_CMAKE ON)
        set(_rs_has_def 1)
    else()
        set(REALSCRIPT_HAS_${_rs_opt}_CMAKE OFF)
        set(_rs_has_def 0)
    endif()
    target_compile_definitions(realscript_frontend INTERFACE
        REALSCRIPT_HAS_${_rs_opt}=${_rs_has_def})
    target_compile_definitions(realscript_full INTERFACE
        REALSCRIPT_HAS_${_rs_opt}=${_rs_has_def})
endforeach()

if(REALSCRIPT_ENABLE_DEBUG_SERVICES)
    target_link_libraries(realscript_frontend INTERFACE realscript_app_debug)
    target_link_libraries(realscript_full INTERFACE realscript_app_debug)
endif()
if(REALSCRIPT_ENABLE_TOOLING)
    target_link_libraries(realscript_frontend INTERFACE realscript_app_tooling)
    target_link_libraries(realscript_full INTERFACE realscript_app_tooling)
endif()
if(REALSCRIPT_ENABLE_EXTENSIBILITY)
    target_link_libraries(realscript_frontend INTERFACE realscript_app_extensibility)
    target_link_libraries(realscript_full INTERFACE realscript_app_extensibility)
endif()
if(REALSCRIPT_ENABLE_GAME_SDK)
    target_link_libraries(realscript_frontend INTERFACE realscript_app_game_sdk)
    target_link_libraries(realscript_full INTERFACE realscript_app_game_sdk)
endif()

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
            "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/micro/integer_loop.rscript"
    )
    add_executable(
        rsbench_aot benchmarks/aot_integer_driver.cpp)
    target_link_libraries(
        rsbench_aot
        PRIVATE realscript_bench_integer_generated RealScript::Kernel)
    target_compile_features(rsbench_aot PRIVATE cxx_std_17)
    realscript_enable_cxx_warnings(rsbench_aot)

    add_executable(
        rsbench_jit benchmarks/jit_integer_driver.cpp)
    target_link_libraries(rsbench_jit PRIVATE RealScript::Kernel)
    add_dependencies(rsbench_jit realscript_jit_support_archive)
    target_compile_features(rsbench_jit PRIVATE cxx_std_17)
    target_compile_definitions(
        rsbench_jit
        PRIVATE
            REALSCRIPT_BENCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
            REALSCRIPT_BENCH_JIT_COMPILER="${CMAKE_CXX_COMPILER}"
            REALSCRIPT_BENCH_JIT_INCLUDE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/include"
            REALSCRIPT_BENCH_JIT_SUPPORT_LIBRARY="$<TARGET_FILE:realscript_jit_support_archive>"
            REALSCRIPT_BENCH_JIT_CACHE_BASE="${CMAKE_CURRENT_BINARY_DIR}/jit-benchmark-cache"
    )
    realscript_enable_cxx_warnings(rsbench_jit)

    if(REALSCRIPT_ENABLE_GAME_SDK)
        add_executable(
            rsbench_product benchmarks/product_macro_driver.cpp)
        target_link_libraries(rsbench_product PRIVATE RealScript::Full)
        target_compile_features(rsbench_product PRIVATE cxx_std_17)
        realscript_enable_cxx_warnings(rsbench_product)
    endif()
endif()

if(REALSCRIPT_BUILD_TESTS)
    enable_testing()

    function(realscript_add_cpp_test target test_name slice_target source)
        add_executable(${target} ${source})
        target_link_libraries(${target} PRIVATE ${slice_target})
        target_compile_features(${target} PRIVATE cxx_std_17)
        realscript_enable_cxx_warnings(${target})
        add_test(NAME ${test_name} COMMAND ${target})
    endfunction()

    # Tier-0 L0 Kernel Suites (always built; zero L1 dependency)
    realscript_add_cpp_test(
        realscript_kernel_architecture_and_abi_tests
        realscript.kernel.architecture-and-abi
        RealScript::Kernel
        tests/kernel_architecture_and_abi_tests.cpp)
    target_compile_definitions(
        realscript_kernel_architecture_and_abi_tests
        PRIVATE REALSCRIPT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

    add_executable(realscript_kernel_host_abi_c_tests tests/kernel_host_abi_c_tests.c)
    target_link_libraries(realscript_kernel_host_abi_c_tests PRIVATE RealScript::Runtime)
    set_target_properties(
        realscript_kernel_host_abi_c_tests
        PROPERTIES
            C_STANDARD 11
            C_STANDARD_REQUIRED YES
            C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(realscript_kernel_host_abi_c_tests PRIVATE /utf-8 /W4)
        if(REALSCRIPT_WARNINGS_AS_ERRORS)
            target_compile_options(realscript_kernel_host_abi_c_tests PRIVATE /WX)
        endif()
    else()
        target_compile_options(
            realscript_kernel_host_abi_c_tests PRIVATE -Wall -Wextra -Wpedantic)
        if(REALSCRIPT_WARNINGS_AS_ERRORS)
            target_compile_options(realscript_kernel_host_abi_c_tests PRIVATE -Werror)
        endif()
    endif()
    add_test(NAME realscript.kernel.host-abi-c COMMAND realscript_kernel_host_abi_c_tests)

    realscript_add_cpp_test(
        realscript_kernel_compiler_frontend_tests
        realscript.kernel.compiler-frontend
        RealScript::Kernel
        tests/kernel_compiler_frontend_tests.cpp)

    realscript_add_cpp_test(
        realscript_kernel_mir_and_bytecode_tests
        realscript.kernel.mir-and-bytecode
        RealScript::Kernel
        tests/kernel_mir_and_bytecode_tests.cpp)

    realscript_add_cpp_test(
        realscript_kernel_runtime_vm_and_gc_tests
        realscript.kernel.runtime-vm-and-gc
        RealScript::Kernel
        tests/kernel_runtime_vm_and_gc_tests.cpp)

    realscript_add_cpp_test(
        realscript_kernel_async_and_fiber_tests
        realscript.kernel.async-and-fiber
        RealScript::Kernel
        tests/kernel_async_and_fiber_tests.cpp)

    realscript_add_cpp_test(
        realscript_kernel_jit_and_hot_reload_tests
        realscript.kernel.jit-and-hot-reload
        RealScript::Kernel
        tests/kernel_jit_and_hot_reload_tests.cpp)
    if(REALSCRIPT_ENABLE_TOOLCHAIN_JIT_TESTS)
        set_target_properties(
            realscript_kernel_jit_and_hot_reload_tests PROPERTIES ENABLE_EXPORTS ON)
        add_dependencies(
            realscript_kernel_jit_and_hot_reload_tests realscript_jit_support_archive)
        target_compile_definitions(
            realscript_kernel_jit_and_hot_reload_tests
            PRIVATE
                REALSCRIPT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
                REALSCRIPT_JIT_COMPILER="${CMAKE_CXX_COMPILER}"
                REALSCRIPT_JIT_INCLUDE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/include"
                REALSCRIPT_JIT_SUPPORT_LIBRARY="$<TARGET_FILE:realscript_jit_support_archive>"
                REALSCRIPT_JIT_CACHE_DIR="${CMAKE_CURRENT_BINARY_DIR}/kernel-jit-cache")
    endif()

    realscript_add_aot_library(
        realscript_kernel_aot_fixture
        PROGRAM_NAME KernelAotSuite
        CPP_NAMESPACE kernel_aot_ns
        QUERY_SYMBOL rs_module_query_kernel_aot
        OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/kernel_aot_suite"
        OPT_LEVEL 2
        SOURCES
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/kernel_aot_suite.rscript")

    realscript_add_aot_library(
        realscript_kernel_aot_trusted_fixture
        PROGRAM_NAME KernelAotTrusted
        CPP_NAMESPACE kernel_aot_trusted_ns
        QUERY_SYMBOL rs_module_query_kernel_aot_trusted
        OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/kernel_aot_trusted"
        OPT_LEVEL 2
        TRUSTED_RELEASE
        SOURCES
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/kernel_aot_trusted.rscript")

    add_executable(realscript_kernel_aot_backend_tests tests/kernel_aot_backend_tests.cpp)
    target_link_libraries(
        realscript_kernel_aot_backend_tests
        PRIVATE
            realscript_kernel_aot_fixture
            realscript_kernel_aot_trusted_fixture
            RealScript::Kernel)
    target_compile_features(realscript_kernel_aot_backend_tests PRIVATE cxx_std_17)
    target_compile_definitions(
        realscript_kernel_aot_backend_tests
        PRIVATE REALSCRIPT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    realscript_enable_cxx_warnings(realscript_kernel_aot_backend_tests)
    add_test(NAME realscript.kernel.aot-backend COMMAND realscript_kernel_aot_backend_tests)

    # Tier-1 L1 Orthogonal Application Suites
    if(REALSCRIPT_ENABLE_DEBUG_SERVICES)
        realscript_add_cpp_test(
            realscript_app_debug_and_dap_tests
            realscript.app.debug-and-dap
            RealScript::Debug
            tests/app_debug_and_dap_tests.cpp)
    endif()

    if(REALSCRIPT_ENABLE_TOOLING)
        realscript_add_cpp_test(
            realscript_app_tooling_and_lsp_tests
            realscript.app.tooling-and-lsp
            RealScript::Tooling
            tests/app_tooling_and_lsp_tests.cpp)
        target_compile_definitions(
            realscript_app_tooling_and_lsp_tests
            PRIVATE REALSCRIPT_RSC_PATH="$<TARGET_FILE:rsc>")
    endif()

    if(REALSCRIPT_ENABLE_EXTENSIBILITY)
        realscript_add_cpp_test(
            realscript_app_reflection_and_mod_host_tests
            realscript.app.reflection-and-mod-host
            RealScript::Extensibility
            tests/app_reflection_and_mod_host_tests.cpp)
    endif()

    if(REALSCRIPT_ENABLE_GAME_SDK)
        realscript_add_cpp_test(
            realscript_app_game_sdk_and_scene_tests
            realscript.app.game-sdk-and-scene
            RealScript::GameSdk
            tests/app_game_sdk_and_scene_tests.cpp)
    endif()

    # CLI & SDK Consumer Verification
    add_test(
        NAME realscript.kernel.cli-smoke
        COMMAND
            rsc
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/kernel_multimodule_model.rscript"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/kernel_multimodule_app.rscript"
            --run
            Kernel.App::main
            --opt-level
            2
            --deterministic
            --digest
            --profile)

    add_test(
        NAME realscript.kernel.benchmark-smoke
        COMMAND
            rsbench
            --entry
            Kernel.App::main
            --iterations
            3
            --warmup
            1
            --opt-level
            2
            --json
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/kernel_multimodule_model.rscript"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/kernel_multimodule_app.rscript")

    add_test(
        NAME realscript.kernel.cli-multimodule
        COMMAND
            ${CMAKE_COMMAND}
            -DREALSCRIPT_RSC=$<TARGET_FILE:rsc>
            -DREALSCRIPT_SOURCE_A=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/kernel_multimodule_model.rscript
            -DREALSCRIPT_SOURCE_B=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/kernel_multimodule_app.rscript
            -DREALSCRIPT_OUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-multimodule/$<CONFIG>
            -P
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/multimodule_cli_test.cmake)

    if(REALSCRIPT_INSTALL_SDK)
        add_test(
            NAME realscript.sdk.install-consumer
            COMMAND
                ${CMAKE_COMMAND}
                -DREALSCRIPT_BUILD_DIR=${CMAKE_BINARY_DIR}
                -DREALSCRIPT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                -DREALSCRIPT_INSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/install-consumer/$<CONFIG>/sdk
                -DREALSCRIPT_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}
                -DREALSCRIPT_CONSUMER_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}/install-consumer/$<CONFIG>/build
                -DREALSCRIPT_CONFIG=$<CONFIG>
                -P
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/install_consumer_test.cmake)
    endif()
endif()

if(REALSCRIPT_INSTALL_SDK)
    set(realscript_install_export_targets
        realscript_slice_foundation_core
        realscript_slice_bytecode_core
        realscript_slice_compiler_core
        realscript_slice_runtime_core
        realscript_slice_native_backends
        realscript_aot_support
        realscript_kernel
        realscript_frontend
        realscript_full
        rsaot
    )
    set(realscript_install_cli_targets rsc rsbench)

    if(REALSCRIPT_ENABLE_DEBUG_SERVICES)
        list(APPEND realscript_install_export_targets realscript_app_debug)
        list(APPEND realscript_install_cli_targets rsdebug)
    endif()
    if(REALSCRIPT_ENABLE_TOOLING)
        list(APPEND realscript_install_export_targets realscript_app_tooling)
        list(APPEND realscript_install_cli_targets rslsp)
    endif()
    if(REALSCRIPT_ENABLE_EXTENSIBILITY)
        list(APPEND realscript_install_export_targets realscript_app_extensibility)
    endif()
    if(REALSCRIPT_ENABLE_GAME_SDK)
        list(APPEND realscript_install_export_targets
            realscript_app_game_sdk
            realscript_game_runtime
            realscript_game_compiler
        )
        list(APPEND realscript_install_cli_targets rsbind)
    endif()

    install(
        TARGETS ${realscript_install_export_targets}
        EXPORT RealScriptTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(
        TARGETS ${realscript_install_cli_targets}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(FILES LICENSE
        DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/RealScript)
    install(FILES cmake/RealScriptAot.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RealScript)
    install(DIRECTORY tools/debuggers/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/RealScript/debuggers)

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
