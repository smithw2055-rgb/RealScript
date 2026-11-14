cmake_minimum_required(VERSION 3.20)
project(RealScriptInstallConsumer LANGUAGES C CXX)

find_package(RealScript CONFIG REQUIRED)
include("${RealScript_AOT_MODULE}")
enable_testing()

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
