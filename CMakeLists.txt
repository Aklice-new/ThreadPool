cmake_minimum_required(VERSION 3.16)

project(ThreadPool)

set(CMAKE_CXX_STANDARD 17)

add_executable(test_pool main.cpp ThreadPool.h)

target_link_libraries(test_pool pthread)
