include(CMakeParseArguments)

# Generate and compile a RealScript C++17 AOT program as a native static
# library. The target exports the generated include directory so hosts can
# include realscript_aot_generated.h and use either ProgramDescriptor or the
# stable C query symbol.
function(realscript_add_aot_library target)
    set(options NO_LINE_DIRECTIVES)
    set(one_value_args
        PROGRAM_NAME
        CPP_NAMESPACE
        QUERY_SYMBOL
        OUTPUT_DIRECTORY
        OPT_LEVEL
    )
    set(multi_value_args SOURCES)
    cmake_parse_arguments(
        RSAOT
        "${options}"
        "${one_value_args}"
        "${multi_value_args}"
        ${ARGN}
    )

    if(RSAOT_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "realscript_add_aot_library(${target}) received unknown arguments: "
            "${RSAOT_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT TARGET rsaot)
        message(FATAL_ERROR
            "realscript_add_aot_library requires the rsaot executable target")
    endif()
    if(NOT RSAOT_SOURCES)
        message(FATAL_ERROR
            "realscript_add_aot_library(${target}) requires SOURCES")
    endif()

    if(NOT RSAOT_PROGRAM_NAME)
        set(RSAOT_PROGRAM_NAME "${target}")
    endif()
    if(NOT RSAOT_OUTPUT_DIRECTORY)
        set(RSAOT_OUTPUT_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/generated/${target}")
    endif()
    if(NOT DEFINED RSAOT_OPT_LEVEL OR RSAOT_OPT_LEVEL STREQUAL "")
        set(RSAOT_OPT_LEVEL 0)
    endif()
    if(NOT RSAOT_OPT_LEVEL MATCHES "^[012]$")
        message(FATAL_ERROR
            "realscript_add_aot_library(${target}) OPT_LEVEL must be 0, 1, or 2")
    endif()

    set(aot_sources)
    foreach(source IN LISTS RSAOT_SOURCES)
        if(IS_ABSOLUTE "${source}")
            list(APPEND aot_sources "${source}")
        else()
            get_filename_component(
                absolute_source
                "${source}"
                REALPATH
                BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
            )
            list(APPEND aot_sources "${absolute_source}")
        endif()
    endforeach()

    set(generated_header
        "${RSAOT_OUTPUT_DIRECTORY}/realscript_aot_generated.h")
    set(generated_source
        "${RSAOT_OUTPUT_DIRECTORY}/realscript_aot_generated.cpp")
    set(generated_manifest
        "${RSAOT_OUTPUT_DIRECTORY}/realscript_aot_manifest.json")

    set(generator_arguments
        --output-dir "${RSAOT_OUTPUT_DIRECTORY}"
        --program-name "${RSAOT_PROGRAM_NAME}"
        --opt-level "${RSAOT_OPT_LEVEL}"
    )
    if(RSAOT_CPP_NAMESPACE)
        list(APPEND generator_arguments
            --namespace "${RSAOT_CPP_NAMESPACE}")
    endif()
    if(RSAOT_QUERY_SYMBOL)
        list(APPEND generator_arguments
            --query-symbol "${RSAOT_QUERY_SYMBOL}")
    endif()
    if(RSAOT_NO_LINE_DIRECTIVES)
        list(APPEND generator_arguments --no-line-directives)
    endif()
    list(APPEND generator_arguments ${aot_sources})

    add_custom_command(
        OUTPUT
            "${generated_header}"
            "${generated_source}"
            "${generated_manifest}"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${RSAOT_OUTPUT_DIRECTORY}"
        COMMAND $<TARGET_FILE:rsaot> ${generator_arguments}
        COMMAND ${CMAKE_COMMAND} -E touch_nocreate
            "${generated_header}"
            "${generated_source}"
            "${generated_manifest}"
        DEPENDS rsaot ${aot_sources}
        COMMENT "Generating RealScript C++17 AOT target ${target}"
        VERBATIM
    )

    set_source_files_properties(
        "${generated_header}"
        "${generated_source}"
        PROPERTIES GENERATED TRUE
    )
    add_library(${target} STATIC
        "${generated_source}"
        "${generated_header}"
    )
    target_include_directories(
        ${target}
        PUBLIC "${RSAOT_OUTPUT_DIRECTORY}"
    )
    target_link_libraries(${target} PUBLIC RealScript::AotSupport)
    target_compile_features(${target} PUBLIC cxx_std_17)
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

    set_property(
        TARGET ${target}
        PROPERTY REALSCRIPT_AOT_OUTPUT_DIRECTORY "${RSAOT_OUTPUT_DIRECTORY}"
    )
    set_property(
        TARGET ${target}
        PROPERTY REALSCRIPT_AOT_MANIFEST "${generated_manifest}"
    )
endfunction()
