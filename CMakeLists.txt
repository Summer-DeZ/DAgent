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

set(DAGENT_BACKEND_PATH "${CMAKE_BINARY_DIR}/src/dagent-backend")
configure_file(cmake/dagent.apparmor.in ${CMAKE_BINARY_DIR}/dagent.apparmor @ONLY)

install(TARGETS dagent dagent-backend RUNTIME DESTINATION .)

# 程序资源：SRT bridge 随二进制版本走，不经过“保留已有用户文件”的 Home 安装逻辑。
install(FILES src/private/exec/srt_bridge.mjs DESTINATION libexec)
configure_file(cmake/install_home.cmake.in ${CMAKE_BINARY_DIR}/install_home.cmake @ONLY)
install(SCRIPT ${CMAKE_BINARY_DIR}/install_home.cmake)
