cmake_minimum_required(VERSION 3.20)
project(RealScriptInstallConsumer LANGUAGES CXX)

find_package(RealScript CONFIG REQUIRED)
include("${RealScript_AOT_MODULE}")

realscript_add_aot_library(install_consumer_scripts
    PROGRAM_NAME InstallConsumer
    OPT_LEVEL 2
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/consumer.rs"
)

add_executable(realscript_install_consumer main.cpp)
target_link_libraries(realscript_install_consumer
    PRIVATE install_consumer_scripts RealScript::Full)
target_compile_features(realscript_install_consumer PRIVATE cxx_std_17)
