cmake_minimum_required(VERSION 3.5)

project(tasks LANGUAGES CXX)

set(TASKS_VARIANT 0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SOURCES main.cpp)
set(SOURCES ${SOURCES} raytracer.cpp )


if(TASKS_VARIANT EQUAL 0)
add_compile_definitions(TASKS_LEGACY)
    set(SOURCES ${SOURCES} src/tasks.cpp)
elseif(TASKS_VARIANT EQUAL 1)
add_compile_definitions(TASKS_THREAD_AFFINITY)
    set(SOURCES ${SOURCES} src/tasks_ta.cpp)
elseif(TASKS_VARIANT EQUAL 2)
add_compile_definitions(TASKS_TA_LL)
    set(SOURCES ${SOURCES} src/tasks_ta_ll.cpp)
endif(TASKS_VARIANT)

set(SOURCES ${SOURCES} src/tasks.h)
set(SOURCES ${SOURCES} src/lockfree_ring_buffer.h)

add_executable(tasks ${SOURCES})

install(TARGETS tasks
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
