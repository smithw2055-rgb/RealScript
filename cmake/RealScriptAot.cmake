include(CMakeParseArguments)

# Generate and compile a RealScript C++17 AOT program as a native static
# library. The target exports the generated include directory so hosts can
# include realscript_aot_generated.h and use either ProgramDescriptor or the
# stable C query symbol.
function(realscript_add_aot_library target)
    set(options
        NO_LINE_DIRECTIVES
        NO_SHARED_GENERICS
        TRUSTED_RELEASE
        CACHE_STATS
        NO_PROJECT_CONFIG
    )
    set(one_value_args
        PROGRAM_NAME
        CPP_NAMESPACE
        QUERY_SYMBOL
        OUTPUT_DIRECTORY
        CACHE_DIR
        OPT_LEVEL
        PROJECT_CONFIG
    )
    set(multi_value_args
        SOURCES
        MODULE_PACKS
        COMPILER_PLUGINS
    )
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
    if(TARGET rsaot)
        set(realscript_rsaot_target rsaot)
    elseif(TARGET RealScript::rsaot)
        set(realscript_rsaot_target RealScript::rsaot)
    else()
        message(FATAL_ERROR
            "realscript_add_aot_library requires the rsaot executable target "
            "from either the RealScript source tree or an installed SDK")
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
    if(RSAOT_NO_SHARED_GENERICS)
        list(APPEND generator_arguments --no-shared-generics)
    endif()
    if(RSAOT_TRUSTED_RELEASE)
        list(APPEND generator_arguments --trusted-release)
    endif()
    if(RSAOT_CACHE_DIR)
        list(APPEND generator_arguments --cache-dir "${RSAOT_CACHE_DIR}")
    endif()
    if(RSAOT_CACHE_STATS)
        list(APPEND generator_arguments --cache-stats)
    endif()
    set(aot_extra_depends)
    if(RSAOT_NO_PROJECT_CONFIG)
        list(APPEND generator_arguments --no-project-config)
    elseif(RSAOT_PROJECT_CONFIG)
        if(IS_ABSOLUTE "${RSAOT_PROJECT_CONFIG}")
            set(resolved_project_config "${RSAOT_PROJECT_CONFIG}")
        else()
            get_filename_component(
                resolved_project_config
                "${RSAOT_PROJECT_CONFIG}"
                REALPATH
                BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
            )
        endif()
        list(APPEND generator_arguments --project "${resolved_project_config}")
        list(APPEND aot_extra_depends "${resolved_project_config}")
    endif()
    foreach(pack_entry IN LISTS RSAOT_MODULE_PACKS)
        if(IS_ABSOLUTE "${pack_entry}" OR "${pack_entry}" MATCHES "^\\$<")
            set(resolved_pack "${pack_entry}")
        else()
            get_filename_component(
                resolved_pack
                "${pack_entry}"
                REALPATH
                BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
            )
        endif()
        list(APPEND generator_arguments --pack "${resolved_pack}")
        if("${resolved_pack}" MATCHES "^\\$<")
            list(APPEND aot_extra_depends "${resolved_pack}")
        elseif(IS_DIRECTORY "${resolved_pack}")
            file(GLOB_RECURSE pack_manifest_files CONFIGURE_DEPENDS "${resolved_pack}/*")
            list(APPEND aot_extra_depends ${pack_manifest_files})
        else()
            list(APPEND aot_extra_depends "${resolved_pack}")
        endif()
    endforeach()
    foreach(plugin_entry IN LISTS RSAOT_COMPILER_PLUGINS)
        if(IS_ABSOLUTE "${plugin_entry}" OR "${plugin_entry}" MATCHES "^\\$<")
            set(resolved_plugin "${plugin_entry}")
        else()
            get_filename_component(
                resolved_plugin
                "${plugin_entry}"
                REALPATH
                BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
            )
        endif()
        list(APPEND generator_arguments --plugin "${resolved_plugin}")
        list(APPEND aot_extra_depends "${resolved_plugin}")
    endforeach()
    list(APPEND generator_arguments ${aot_sources})

    add_custom_command(
        OUTPUT
            "${generated_header}"
            "${generated_source}"
            "${generated_manifest}"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${RSAOT_OUTPUT_DIRECTORY}"
        COMMAND $<TARGET_FILE:${realscript_rsaot_target}> ${generator_arguments}
        COMMAND ${CMAKE_COMMAND} -E touch_nocreate
            "${generated_header}"
            "${generated_source}"
            "${generated_manifest}"
        DEPENDS ${realscript_rsaot_target} ${aot_sources} ${aot_extra_depends}
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
