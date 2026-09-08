if(NOT DEFINED BAKE_BINARY)
    message(FATAL_ERROR "BAKE_BINARY is required")
endif()

execute_process(
    COMMAND "${BAKE_BINARY}" lsp
    RESULT_VARIABLE command_result
    OUTPUT_VARIABLE command_output
    ERROR_VARIABLE command_error
)
set(combined_output "${command_output}${command_error}")
if(command_result EQUAL 0)
    message(FATAL_ERROR "Stage0 accepted the lsp command")
endif()
if(NOT combined_output MATCHES "command 'lsp' is not part of the bootstrap binary")
    message(FATAL_ERROR "Stage0 rejection was imprecise: ${combined_output}")
endif()
