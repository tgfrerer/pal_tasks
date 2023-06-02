cmake_minimum_required(VERSION 3.5)

project(coro LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SOURCES main.cpp)
set(SOURCES ${SOURCES} tasks.cpp)
set(SOURCES ${SOURCES} tasks.h)
set(SOURCES ${SOURCES} lockfree_ring_buffer.h)

add_executable(coro ${SOURCES})

install(TARGETS coro
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
