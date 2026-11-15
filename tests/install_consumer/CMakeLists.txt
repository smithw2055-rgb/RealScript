cmake_minimum_required(VERSION 3.20)
project(RealScriptInstallConsumer LANGUAGES C CXX)

find_package(RealScript CONFIG REQUIRED)
enable_testing()

if(NOT RealScript_SLIM_RUNTIME)
    include("${RealScript_AOT_MODULE}")

    realscript_add_aot_library(install_consumer_scripts
        PROGRAM_NAME InstallConsumer
        OPT_LEVEL 2
        SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/consumer.rscript"
    )

    add_executable(realscript_install_consumer main.cpp)
    target_link_libraries(realscript_install_consumer
        PRIVATE install_consumer_scripts RealScript::Full)
    target_compile_features(realscript_install_consumer PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(realscript_install_consumer PRIVATE /utf-8 /W4 /WX /wd4324 /permissive-)
    else()
        target_compile_options(realscript_install_consumer PRIVATE -Wall -Wextra -Werror)
    endif()

    add_test(NAME realscript_install_consumer COMMAND realscript_install_consumer)
endif()

add_executable(realscript_install_slim_consumer slim_consumer.cpp)
target_link_libraries(realscript_install_slim_consumer
    PRIVATE
        RealScript::SlimRuntime
        RealScript::BindGenRuntime
        RealScript::StencilJit
        RealScript::HotReload
        RealScript::AotRuntime
        RealScript::AotSupport)
target_compile_features(realscript_install_slim_consumer PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(realscript_install_slim_consumer PRIVATE /utf-8 /GR- /W4 /WX /wd4324 /permissive-)
else()
    target_compile_options(realscript_install_slim_consumer PRIVATE -fno-rtti -Wall -Wextra -Werror)
endif()
add_test(NAME realscript_install_slim_consumer COMMAND realscript_install_slim_consumer)

if(TARGET RealScript::Runtime)
    add_executable(realscript_install_runtime_c_consumer runtime_only.c)
    set_target_properties(realscript_install_runtime_c_consumer PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO
    )
    target_link_libraries(realscript_install_runtime_c_consumer
        PRIVATE RealScript::Runtime)
    if(MSVC)
        target_compile_options(realscript_install_runtime_c_consumer PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(realscript_install_runtime_c_consumer PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(
        NAME realscript_install_runtime_c_consumer
        COMMAND realscript_install_runtime_c_consumer
    )
endif()

foreach(_rs_expected_target IN ITEMS
        RealScript::Foundation
        RealScript::Bytecode
        RealScript::Runtime
        RealScript::AotRuntime
        RealScript::BindGenRuntime
        RealScript::StencilJit
        RealScript::HotReload
        RealScript::AotSupport
        RealScript::SlimRuntime)
    if(NOT TARGET ${_rs_expected_target})
        message(FATAL_ERROR "Missing exported RealScript target: ${_rs_expected_target}")
    endif()
endforeach()

if(NOT RealScript_SLIM_RUNTIME)
    foreach(_rs_expected_target IN ITEMS
            RealScript::Frontend
            RealScript::Full)
        if(NOT TARGET ${_rs_expected_target})
            message(FATAL_ERROR "Missing exported RealScript target: ${_rs_expected_target}")
        endif()
    endforeach()
endif()
