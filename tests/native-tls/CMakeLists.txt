if(NOT DEFINED ENV{IDF_PATH})
    message(FATAL_ERROR "EOTA_REAL_HTTPS_TEST requires IDF_PATH")
endif()
set(EOTA_IDF_MBEDTLS "$ENV{IDF_PATH}/components/mbedtls")
if(NOT EXISTS "${EOTA_IDF_MBEDTLS}/mbedtls/CMakeLists.txt")
    message(FATAL_ERROR "IDF_PATH has no Mbed TLS component")
endif()
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/components/esp_ota/tools/check_sdk.py"
    --path "$ENV{IDF_PATH}"
    RESULT_VARIABLE eota_sdk_result
    OUTPUT_VARIABLE eota_sdk_output
    ERROR_VARIABLE eota_sdk_error)
if(NOT eota_sdk_result EQUAL 0)
    message(FATAL_ERROR "${eota_sdk_error}")
endif()
message(STATUS "${eota_sdk_output}")

set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(GEN_FILES OFF CACHE BOOL "" FORCE)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
include_directories("${EOTA_IDF_MBEDTLS}/port/include"
    "${EOTA_IDF_MBEDTLS}/mbedtls/tf-psa-crypto/drivers/builtin/include"
    "${CMAKE_SOURCE_DIR}/tests/fakes/native-tls")
add_subdirectory("${EOTA_IDF_MBEDTLS}/mbedtls"
    "${CMAKE_CURRENT_BINARY_DIR}/mbedtls")

add_executable(eota_real_https_client
    ${CMAKE_SOURCE_DIR}/components/esp_ota/src/http_transport.c
    ${CMAKE_SOURCE_DIR}/components/esp_ota/src/http_deadline.c
    ${CMAKE_SOURCE_DIR}/tests/real_https_client.c)
target_include_directories(eota_real_https_client PRIVATE
    "${EOTA_IDF_MBEDTLS}/mbedtls/include"
    "${EOTA_IDF_MBEDTLS}/mbedtls/tf-psa-crypto/include"
    ${CMAKE_SOURCE_DIR}/components/esp_ota/src
    ${CMAKE_SOURCE_DIR}/tests/fakes/transport
    ${CMAKE_SOURCE_DIR}/tests/fakes/ota_update
    ${CMAKE_SOURCE_DIR}/tests/fakes)
target_compile_options(eota_real_https_client PRIVATE -Wall -Wextra -Werror -UNDEBUG)
target_link_libraries(eota_real_https_client PRIVATE mbedtls mbedx509
    tfpsacrypto Threads::Threads)
find_program(EOTA_OPENSSL_EXECUTABLE openssl)
if(NOT EOTA_OPENSSL_EXECUTABLE)
    message(FATAL_ERROR "EOTA_REAL_HTTPS_TEST requires the openssl CLI")
endif()
add_test(NAME real_https COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/tests/real_https_loopback.py"
    "$<TARGET_FILE:eota_real_https_client>" "${EOTA_OPENSSL_EXECUTABLE}")
set_tests_properties(real_https PROPERTIES TIMEOUT 10)
