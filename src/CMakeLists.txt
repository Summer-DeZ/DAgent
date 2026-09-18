# 最底层公共库：日志、dotenv 密钥、文本工具、JSON 脱敏。
add_library(dagent_base STATIC
    private/base/log.cpp
    private/base/dotenv.cpp
    private/base/text.cpp
    private/base/json.cpp
)

target_include_directories(dagent_base PUBLIC ${CMAKE_SOURCE_DIR}/src/public)
target_compile_features(dagent_base PUBLIC cxx_std_23)
target_compile_options(dagent_base PRIVATE -Wall -Wextra)
target_link_libraries(dagent_base PUBLIC spdlog::spdlog)

add_library(dagent_tui STATIC
    private/tui/terminal.cpp
    private/tui/grapheme.cpp
    private/tui/surface.cpp
    private/tui/present.cpp
    private/tui/layout.cpp
    private/tui/widget.cpp
    private/tui/document.cpp
    private/tui/wrap.cpp
    private/tui/markdown.cpp
    private/tui/syntax.cpp
    private/tui/input.cpp
    private/tui/runtime.cpp
    private/tui/keymap.cpp
    private/tui/theme.cpp
)

target_include_directories(dagent_tui PUBLIC ${CMAKE_SOURCE_DIR}/src/public)
target_compile_features(dagent_tui PUBLIC cxx_std_23)
target_compile_options(dagent_tui PRIVATE -Wall -Wextra)

# 应用层：在冻结的 TUI 框架之上构建的界面与配置（主题文件加载等）。
add_library(dagent_ui STATIC
    private/ui/theme_config.cpp
)

target_link_libraries(dagent_ui PUBLIC dagent_tui)
target_compile_options(dagent_ui PRIVATE -Wall -Wextra)

# 网络层：libcurl 薄封装（HTTP 整包/流式、取消、超时）与 SSE 解析，不含任何模型协议。
add_library(dagent_net STATIC
    private/net/http.cpp
    private/net/sse.cpp
)

target_include_directories(dagent_net PUBLIC ${CMAKE_SOURCE_DIR}/src/public)
target_compile_features(dagent_net PUBLIC cxx_std_23)
target_compile_options(dagent_net PRIVATE -Wall -Wextra)
target_link_libraries(dagent_net PRIVATE CURL::libcurl)

# 子进程与沙箱：一次性命令、长期子进程、bash 命令分析、Landlock + seccomp。
add_library(dagent_exec STATIC
    private/exec/detail.cpp
    private/exec/process.cpp
    private/exec/child.cpp
    private/exec/shell.cpp
    private/exec/sandbox.cpp
)

target_include_directories(dagent_exec PUBLIC ${CMAKE_SOURCE_DIR}/src/public)
target_compile_features(dagent_exec PUBLIC cxx_std_23)
target_compile_options(dagent_exec PRIVATE -Wall -Wextra)
# Boost.Process v2 内部用 std::filesystem（本机没有编译好的 Boost.Filesystem）。
target_compile_definitions(dagent_exec PRIVATE BOOST_PROCESS_USE_STD_FS=1)
target_link_libraries(dagent_exec
    PUBLIC dagent_base
    PRIVATE Boost::headers PkgConfig::SECCOMP tree-sitter tree-sitter-bash Threads::Threads
)

# 工作区：文件原语、ripgrep 搜索、unified diff、项目上下文（git/AGENTS.md/模板）。
add_library(dagent_workspace STATIC
    private/workspace/files.cpp
    private/workspace/search.cpp
    private/workspace/diff.cpp
    private/workspace/context.cpp
)

target_include_directories(dagent_workspace PUBLIC ${CMAKE_SOURCE_DIR}/src/public)
target_compile_features(dagent_workspace PUBLIC cxx_std_23)
target_compile_options(dagent_workspace PRIVATE -Wall -Wextra)
target_link_libraries(dagent_workspace
    PUBLIC dagent_base
    PRIVATE dagent_exec pantor::inja
)
