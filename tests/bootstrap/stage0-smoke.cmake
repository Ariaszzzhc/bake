if(NOT DEFINED BAKE_BINARY OR NOT DEFINED SMOKE_DIR)
    message(FATAL_ERROR "BAKE_BINARY and SMOKE_DIR are required")
endif()

file(REMOVE_RECURSE "${SMOKE_DIR}")
file(MAKE_DIRECTORY "${SMOKE_DIR}")
file(WRITE "${SMOKE_DIR}/main.c"
    "#include <stdio.h>\nint main(void) { puts(\"stage0 compile ok\"); return 0; }\n")

execute_process(
    COMMAND "${BAKE_BINARY}" --help
    RESULT_VARIABLE help_result
    OUTPUT_VARIABLE help_output
    ERROR_VARIABLE help_error
)
if(NOT help_result EQUAL 0 OR NOT help_output MATCHES "bootstrap binary")
    message(FATAL_ERROR "Stage0 help failed (${help_result}): ${help_output}${help_error}")
endif()

execute_process(
    COMMAND "${BAKE_BINARY}" cc "${SMOKE_DIR}/main.c" -o "${SMOKE_DIR}/smoke"
    RESULT_VARIABLE compile_result
    OUTPUT_VARIABLE compile_output
    ERROR_VARIABLE compile_error
)
if(NOT compile_result EQUAL 0 OR NOT EXISTS "${SMOKE_DIR}/smoke")
    message(FATAL_ERROR "Stage0 compile failed (${compile_result}): ${compile_output}${compile_error}")
endif()

execute_process(
    COMMAND "${SMOKE_DIR}/smoke"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error
)
if(NOT run_result EQUAL 0 OR NOT run_output MATCHES "stage0 compile ok")
    message(FATAL_ERROR "Compiled fixture failed (${run_result}): ${run_output}${run_error}")
endif()

file(REMOVE_RECURSE "${SMOKE_DIR}")
