cmake_minimum_required(VERSION 3.16)
project(esp_ota VERSION 0.1.0 LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
include(CTest)
if(BUILD_TESTING)
    add_executable(eota_update_test components/esp_ota/src/update.c
        components/esp_ota/src/http_deadline.c tests/update_test.c)
    target_include_directories(eota_update_test PRIVATE components/esp_ota/include
        tests/fakes/ota_update tests/fakes)
    target_compile_definitions(eota_update_test PRIVATE _GNU_SOURCE)
    target_compile_options(eota_update_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME update COMMAND eota_update_test)

    find_package(Threads REQUIRED)
    add_executable(eota_http_deadline_test components/esp_ota/src/http_deadline.c
        tests/http_deadline_test.c)
    target_include_directories(eota_http_deadline_test PRIVATE components/esp_ota/src
        tests/fakes/ota_update tests/fakes)
    target_compile_options(eota_http_deadline_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    target_link_libraries(eota_http_deadline_test PRIVATE Threads::Threads)
    add_test(NAME http_deadline COMMAND eota_http_deadline_test)

    add_executable(eota_confirmation_test components/esp_ota/src/ota.c tests/ota_test.c)
    target_include_directories(eota_confirmation_test PRIVATE components/esp_ota/include tests/fakes)
    target_compile_options(eota_confirmation_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME confirmation COMMAND eota_confirmation_test)
endif()
