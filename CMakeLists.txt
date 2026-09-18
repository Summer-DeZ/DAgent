cmake_minimum_required(VERSION 3.25)
project(DAgent VERSION 0.1.0 LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The current file-tool backend requires Linux.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Boost 1.83 REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

include(cmake/deps.cmake)

add_subdirectory(src)

enable_testing()
add_subdirectory(test)
