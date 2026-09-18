# 第三方依赖的集中声明，由根 CMakeLists.txt include。
include(FetchContent)

# spdlog 始终从源码构建：系统的 spdlog 基于 fmt，与 SPDLOG_USE_STD_FORMAT 不兼容。
set(SPDLOG_USE_STD_FORMAT ON CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog
    GIT_TAG v1.17.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(spdlog)
