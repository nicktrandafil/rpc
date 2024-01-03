cmake_minimum_required(VERSION 3.5)

project(rpc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(rpc main.cpp)

find_package(doctest CONFIG REQUIRED)
target_link_libraries(rpc PRIVATE doctest::doctest)
