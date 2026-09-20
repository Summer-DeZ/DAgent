cmake_minimum_required(VERSION 3.25)
project(DAgent VERSION 0.1.0 LANGUAGES C CXX)

set(DAGENT_DEV_HOME "" CACHE PATH "Default DAGENT_HOME for development builds")

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

install(TARGETS dagent RUNTIME DESTINATION .)
install(FILES home/config.json home/system.md home/compact.md DESTINATION .)
install(FILES home/models.json DESTINATION .
        PERMISSIONS OWNER_READ OWNER_WRITE)
install(DIRECTORY home/themes DESTINATION .)

enable_testing()
add_subdirectory(test)
