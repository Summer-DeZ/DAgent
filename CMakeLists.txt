cmake_minimum_required(VERSION 3.25)
project(DAgent VERSION 0.1.0 LANGUAGES C CXX)

set(DAGENT_DEV_HOME "" CACHE PATH "Default DAGENT_HOME for development builds")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(DAGENT_PLATFORM posix)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(DAGENT_PLATFORM win)
else()
    message(FATAL_ERROR "Unsupported target system: ${CMAKE_SYSTEM_NAME}.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    find_package(Boost 1.83 REQUIRED COMPONENTS process)
    find_package(Iconv MODULE REQUIRED)
else()
    find_package(Boost 1.83 REQUIRED)
endif()
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

include(cmake/deps.cmake)

add_subdirectory(src)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(DAGENT_BACKEND_PATH "${CMAKE_BINARY_DIR}/src/dagent-backend")
    configure_file(cmake/dagent.apparmor.in ${CMAKE_BINARY_DIR}/dagent.apparmor @ONLY)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    install(TARGETS dagent dagent-backend RUNTIME_DEPENDENCY_SET dagent_runtime RUNTIME DESTINATION .)
    if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
        set(DAGENT_RUNTIME_DEPENDENCY_DIRECTORIES DIRECTORIES
            "$<IF:$<CONFIG:Debug>,${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin,${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin>")
    else()
        set(DAGENT_RUNTIME_DEPENDENCY_DIRECTORIES "")
    endif()
    install(RUNTIME_DEPENDENCY_SET dagent_runtime
        # VC 运行时（含 Debug 变体）由 VC Redistributable 或开发环境提供，不随安装目录分发。
        PRE_EXCLUDE_REGEXES "api-ms-win-.*" "ext-ms-.*" "hvsifiletrust" "pdmutilities"
            "[Vv][Cc][Rr][Uu][Nn][Tt][Ii][Mm][Ee]140.*" "[Mm][Ss][Vv][Cc][Pp]140.*" "[Uu][Cc][Rr][Tt][Bb][Aa][Ss][Ee].*"
            "[Cc][Oo][Nn][Cc][Rr][Tt]140.*" "[Vv][Cc][Cc][Oo][Rr][Ll][Ii][Bb]140.*"
        POST_EXCLUDE_REGEXES ".*[\\\\/][Ss]ystem32[\\\\/].*" ".*[\\\\/]WinSxS[\\\\/].*"
        ${DAGENT_RUNTIME_DEPENDENCY_DIRECTORIES}
        DESTINATION .)
else()
    install(TARGETS dagent dagent-backend RUNTIME DESTINATION .)
endif()

# 程序资源：SRT bridge 随二进制版本走，不经过“保留已有用户文件”的 Home 安装逻辑。
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    install(FILES src/private/exec/srt_bridge.mjs src/private/app/searxng_server.py DESTINATION libexec)
    install(PROGRAMS src/private/exec/srt_bwrap.sh DESTINATION libexec)
endif()
configure_file(cmake/install_home.cmake.in ${CMAKE_BINARY_DIR}/install_home.cmake @ONLY)
install(SCRIPT ${CMAKE_BINARY_DIR}/install_home.cmake)
