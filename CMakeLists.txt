cmake_minimum_required(VERSION 3.20)

project(RealScript VERSION 0.1.0 LANGUAGES CXX)

option(REALSCRIPT_BUILD_TESTS "Build RealScript conformance tests" ON)
option(REALSCRIPT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

add_library(realscript_frontend STATIC
    src/text/Text.cpp
    src/diagnostics/Diagnostic.cpp
    src/syntax/Syntax.cpp
    src/semantic/Semantic.cpp
    src/mir/Mir.cpp
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
    add_test(NAME realscript.frontend COMMAND realscript_tests)
endif()
