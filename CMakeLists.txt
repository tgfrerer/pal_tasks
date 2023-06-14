cmake_minimum_required(VERSION 3.5)

project(tasks LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SOURCES main.cpp)
set(SOURCES ${SOURCES} src/tasks.cpp)
set(SOURCES ${SOURCES} src/tasks.h)
set(SOURCES ${SOURCES} src/lockfree_ring_buffer.h)

add_executable(tasks ${SOURCES})

install(TARGETS tasks
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
