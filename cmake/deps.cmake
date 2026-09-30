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
    SOURCE_SUBDIR _no_cmake
)
FetchContent_MakeAvailable(tree-sitter)
file(GLOB TREE_SITTER_SOURCES CONFIGURE_DEPENDS ${tree-sitter_SOURCE_DIR}/lib/src/*.c)
list(REMOVE_ITEM TREE_SITTER_SOURCES ${tree-sitter_SOURCE_DIR}/lib/src/lib.c)
add_library(tree-sitter STATIC ${TREE_SITTER_SOURCES})
target_include_directories(tree-sitter PRIVATE ${tree-sitter_SOURCE_DIR}/lib/src
                                  PUBLIC ${tree-sitter_SOURCE_DIR}/lib/include)
target_compile_definitions(tree-sitter PRIVATE
    _POSIX_C_SOURCE=200112L _DEFAULT_SOURCE _BSD_SOURCE _DARWIN_C_SOURCE)
set_target_properties(tree-sitter PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)

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

# nlohmann/json 已经 vendor 在 src/public/lib/nlohmann/json.hpp，这里暴露成 inja 认识的
# 接口目标（用 third_party/include + <nlohmann/json.hpp> 的引用方式），避免 inja 再拉一份
# 内嵌副本：两份 nlohmann 出现在同一个 TU 里会撞 include guard 和 ODR。
add_library(nlohmann_json INTERFACE)
add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
target_include_directories(nlohmann_json INTERFACE ${CMAKE_SOURCE_DIR}/src/public/lib)
target_compile_features(nlohmann_json INTERFACE cxx_std_17)

# 模板渲染（Jinja2 语法），system prompt 的条件与循环用。header-only。
set(INJA_USE_EMBEDDED_JSON OFF CACHE BOOL "" FORCE)
set(INJA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(INJA_INSTALL OFF CACHE BOOL "" FORCE)
set(INJA_EXPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(inja
    GIT_REPOSITORY https://github.com/pantor/inja
    GIT_TAG v3.5.0
    GIT_SHALLOW TRUE
    FIND_PACKAGE_ARGS 3.5 NAMES inja
)
FetchContent_MakeAvailable(inja)

# 命令行解析（header-only）：子命令、互斥选项与环境变量回退。
set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLI11_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11
    GIT_TAG v2.7.2
    GIT_SHALLOW TRUE
    FIND_PACKAGE_ARGS 2.7 NAMES CLI11
)
FetchContent_MakeAvailable(CLI11)

# Agent Skills YAML frontmatter. No dependency test or utility targets.
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG 0.8.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(yaml-cpp)

# Runtime archive integrity.
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
