# 第三方依赖的集中声明，由根 CMakeLists.txt include。
include(FetchContent)

# 依赖库统一构建静态库，避免影响主程序的链接方式。
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# spdlog 始终从源码构建：系统的 spdlog 基于 fmt，与 SPDLOG_USE_STD_FORMAT 不兼容。
set(SPDLOG_USE_STD_FORMAT ON CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog
    GIT_TAG v1.17.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(spdlog)

# tree-sitter 运行时（C11）：bash 命令分析用。
FetchContent_Declare(tree-sitter
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter
    GIT_TAG v0.27.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(tree-sitter)

# tree-sitter-bash 语法：只编译随 tag 发布的 parser.c/scanner.c。
# 语法仓库自己的 CMake 会在构建时调用 tree-sitter CLI 重新生成 parser.c，
# 那会引入一个完全不必要的 CLI 依赖。SOURCE_SUBDIR 指向不存在的目录，
# MakeAvailable 就只拉取源码、不执行它的 CMake，再在这里自行建目标
# （单参数的 FetchContent_Populate 自 CMake 3.30 起弃用，CMP0169）。
FetchContent_Declare(tree-sitter-bash
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-bash
    GIT_TAG v0.25.1
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _no_cmake
)
FetchContent_MakeAvailable(tree-sitter-bash)
add_library(tree-sitter-bash STATIC
    ${tree-sitter-bash_SOURCE_DIR}/src/parser.c
    ${tree-sitter-bash_SOURCE_DIR}/src/scanner.c
)
target_include_directories(tree-sitter-bash PUBLIC ${tree-sitter-bash_SOURCE_DIR}/bindings/c)
set_target_properties(tree-sitter-bash PROPERTIES C_STANDARD 11)

# 网络隔离用 libseccomp（Ubuntu: apt install libseccomp-dev）。
find_package(PkgConfig REQUIRED)
pkg_check_modules(SECCOMP REQUIRED IMPORTED_TARGET libseccomp)
