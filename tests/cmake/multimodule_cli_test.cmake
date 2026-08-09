foreach(required IN ITEMS
        REALSCRIPT_RSC
        REALSCRIPT_SOURCE_A
        REALSCRIPT_SOURCE_B
        REALSCRIPT_OUTPUT_DIR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

execute_process(
    COMMAND
        "${REALSCRIPT_RSC}"
        "${REALSCRIPT_SOURCE_A}"
        "${REALSCRIPT_SOURCE_B}"
        --emit-bytecode-dir "${REALSCRIPT_OUTPUT_DIR}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR
        "multi-module bytecode emission failed (${result})\n${output}\n${error}")
endif()

foreach(module IN ITEMS Phase5.Model Phase5.App)
    set(artifact "${REALSCRIPT_OUTPUT_DIR}/${module}.rsbc")
    if(NOT EXISTS "${artifact}")
        message(FATAL_ERROR "missing emitted bytecode module: ${artifact}")
    endif()
    file(SIZE "${artifact}" artifact_size)
    if(artifact_size LESS 8)
        message(FATAL_ERROR "emitted bytecode module is truncated: ${artifact}")
    endif()
endforeach()
