idf_component_register(SRCS "src/ota.c" "src/update.c" "src/http_deadline.c" "src/http_transport.c"
    INCLUDE_DIRS "include"
    REQUIRES app_update esp_partition esp_http_client esp_timer mbedtls bootloader_support
        tcp_transport lwip)
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_build_get_property(sdk_python PYTHON)
    idf_build_get_property(sdk_path IDF_PATH)
    execute_process(COMMAND "${sdk_python}" "${CMAKE_CURRENT_LIST_DIR}/tools/check_sdk.py"
        --path "${sdk_path}"
        RESULT_VARIABLE sdk_result ERROR_VARIABLE sdk_error OUTPUT_QUIET)
    if(NOT sdk_result STREQUAL "0")
        message(FATAL_ERROR "${sdk_error}")
    endif()
endif()
