cmake_minimum_required(VERSION 3.5)

project(rpc VERSION 0.1 LANGUAGES CXX)

include(cmake/common.cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(rpc main.cpp)

find_package(doctest CONFIG REQUIRED)
target_link_libraries(rpc PRIVATE doctest::doctest)
set_target_build_settings(rpc)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_CLEAN_FILES ${CMAKE_BINARY_DIR}/dcov)
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/cov.sh
        COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_SOURCE_DIR}/scripts/cov.sh ${CMAKE_BINARY_DIR}
        DEPENDS scripts/cov.sh
    )
    add_custom_target(cov DEPENDS ${CMAKE_BINARY_DIR}/cov.sh DEPENDS rpc)
    add_custom_command(TARGET cov POST_BUILD
        COMMAND ./cov.sh
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
else()
    message(NOTICE "Build in Debug mode to enable coverage")
endif()
