foreach(required IN ITEMS
        REALSCRIPT_BUILD_DIR
        REALSCRIPT_SOURCE_DIR
        REALSCRIPT_INSTALL_PREFIX
        REALSCRIPT_INSTALL_LIBDIR
        REALSCRIPT_CONSUMER_BUILD_DIR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

if(NOT DEFINED REALSCRIPT_CONFIG OR REALSCRIPT_CONFIG STREQUAL "")
    set(REALSCRIPT_CONFIG Debug)
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${REALSCRIPT_BUILD_DIR}"
        --config "${REALSCRIPT_CONFIG}"
        --prefix "${REALSCRIPT_INSTALL_PREFIX}"
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_error
)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR
        "RealScript SDK install failed (${install_result})\n"
        "${install_output}\n${install_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${REALSCRIPT_SOURCE_DIR}/tests/install_consumer"
        -B "${REALSCRIPT_CONSUMER_BUILD_DIR}"
        "-DRealScript_DIR=${REALSCRIPT_INSTALL_PREFIX}/${REALSCRIPT_INSTALL_LIBDIR}/cmake/RealScript"
        "-DCMAKE_BUILD_TYPE=${REALSCRIPT_CONFIG}"
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_error
)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR
        "installed SDK consumer configure failed (${configure_result})\n"
        "${configure_output}\n${configure_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${REALSCRIPT_CONSUMER_BUILD_DIR}"
        --config "${REALSCRIPT_CONFIG}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR
        "installed SDK consumer build failed (${build_result})\n"
        "${build_output}\n${build_error}")
endif()
