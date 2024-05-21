cmake_minimum_required(VERSION 3.5)

project(rpc VERSION 0.1 LANGUAGES CXX)

include(cmake/common.cmake)

add_library(rpc INTERFACE 
    include/rpc/contract.h
    include/rpc/log.h
    include/rpc/mpsc.h
    include/rpc/rpc.h
    include/rpc/runtime.h
    include/rpc/scope_exit.h
)
set_target_properties(rpc PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
target_include_directories(rpc INTERFACE include)

option(RPC_ABORT_ON_INVARIANT_VIOLATION "Abort on invariant violation" OFF)
if(RPC_ABORT_ON_INVARIANT_VIOLATION)
    target_compile_definitions(rpc INTERFACE RPC_ABORT_ON_INVARIANT_VIOLATION)
endif()

option(RPC_ABORT_ON_TODO "Abort on rpc_todo" OFF)
if(RPC_ABORT_ON_TODO)
    target_compile_definitions(rpc INTERFACE RPC_ABORT_ON_TODO)
endif()

add_executable(test_rpc 
    test/mpsc.cpp
    test/rpc.cpp
    test/runtime.cpp
)
find_package(Catch2 REQUIRED QUIET)
target_link_libraries(test_rpc PRIVATE rpc Catch2::Catch2WithMain)
set_target_build_settings(test_rpc)

add_executable(bench_tasks_in_single_thrad
    bench/tasks_in_single_thread.cpp
)
find_package(Boost REQUIRED COMPONENTS system)
target_link_libraries(bench_tasks_in_single_thrad PRIVATE rpc Boost::boost Boost::system)
set_target_build_settings(bench_tasks_in_single_thrad)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_CLEAN_FILES ${CMAKE_BINARY_DIR}/dcov "${CMAKE_BINARY_DIR}/cov.sh")
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/cov.sh
        COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_SOURCE_DIR}/script/cov.sh ${CMAKE_BINARY_DIR}
        DEPENDS script/cov.sh
    )
    add_custom_target(cov DEPENDS ${CMAKE_BINARY_DIR}/cov.sh DEPENDS test_rpc)
    add_custom_command(TARGET cov POST_BUILD
        COMMAND "${CMAKE_BINARY_DIR}/cov.sh"
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
else()
    message(NOTICE "Build in Debug mode to enable coverage")
endif()
