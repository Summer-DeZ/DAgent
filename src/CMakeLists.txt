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
    private/ui/strings.cpp
    private/ui/display.cpp
    private/ui/theme_config.cpp
    private/ui/shell.cpp
    private/ui/transcript.cpp
    private/ui/prompt_input.cpp
    private/ui/status_line.cpp
    private/ui/approval.cpp
    private/ui/center.cpp
    private/ui/toast.cpp
    private/ui/prompt_box.cpp
    private/ui/completion.cpp
    private/ui/panel.cpp
    private/ui/side_panel.cpp
)

target_link_libraries(dagent_ui PUBLIC dagent_tui dagent_agent)
target_compile_options(dagent_ui PRIVATE -Wall -Wextra)
target_compile_definitions(dagent_ui PRIVATE DAGENT_VERSION="${PROJECT_VERSION}")

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

# 会话存储：JSONL 追加写入、blob 外置、脱敏、列目录与回放。
add_library(dagent_session STATIC
    private/session/session.cpp
)

target_include_directories(dagent_session PUBLIC ${CMAKE_SOURCE_DIR}/src/public)
target_compile_features(dagent_session PUBLIC cxx_std_23)
target_compile_options(dagent_session PRIVATE -Wall -Wextra)
target_compile_definitions(dagent_session PRIVATE DAGENT_VERSION="${PROJECT_VERSION}")
target_link_libraries(dagent_session
    PUBLIC dagent_base
)

# MCP 客户端：JSON-RPC 2.0，stdio 走 dagent_exec 的 Child，Streamable HTTP 走 dagent_net。
add_library(dagent_mcp STATIC
    private/mcp/client.cpp
    private/mcp/detail.cpp
)

target_include_directories(dagent_mcp PUBLIC ${CMAKE_SOURCE_DIR}/src/public)
target_compile_features(dagent_mcp PUBLIC cxx_std_23)
target_compile_options(dagent_mcp PRIVATE -Wall -Wextra)
target_compile_definitions(dagent_mcp PRIVATE DAGENT_VERSION="${PROJECT_VERSION}")
target_link_libraries(dagent_mcp
    PUBLIC dagent_base dagent_exec dagent_net
)

# 工具层：把外围模块包装成模型能调用的工具（read/write/edit/bash/grep/glob + MCP）。
# 依赖 base、exec、workspace、mcp；不依赖 agent（核心反过来依赖它，会成环）。
add_library(tools STATIC
    private/tools/context.cpp
    private/tools/detail.cpp
    private/tools/registry.cpp
    private/tools/view.cpp
    private/tools/read.cpp
    private/tools/write.cpp
    private/tools/edit.cpp
    private/tools/bash.cpp
    private/tools/grep.cpp
    private/tools/glob.cpp
    private/tools/todo.cpp
    private/tools/mcp.cpp
)

target_include_directories(tools PUBLIC ${CMAKE_SOURCE_DIR}/src/public)
target_compile_features(tools PUBLIC cxx_std_23)
target_compile_options(tools PRIVATE -Wall -Wextra)
target_link_libraries(tools
    PUBLIC dagent_base dagent_exec dagent_workspace dagent_mcp
)

# 入口层：配置加载与命令行解析。认识所有模块的 Options，其他模块都不依赖它。
add_library(dagent_app STATIC
    private/app/config.cpp
    private/app/cli.cpp
)

target_include_directories(dagent_app PUBLIC ${CMAKE_SOURCE_DIR}/src/public)
target_compile_features(dagent_app PUBLIC cxx_std_23)
target_compile_options(dagent_app PRIVATE -Wall -Wextra)
target_compile_definitions(dagent_app PRIVATE DAGENT_VERSION="${PROJECT_VERSION}")
target_link_libraries(dagent_app
    PUBLIC dagent_agent dagent_base dagent_exec dagent_workspace dagent_net dagent_session dagent_mcp tools
    PRIVATE CLI11::CLI11
)

# 核心 agent：中立消息模型、LLM 编解码、事件接口与一轮循环。
file(READ ${CMAKE_SOURCE_DIR}/prompts/system.md DAGENT_PROMPT_SYSTEM)
file(READ ${CMAKE_SOURCE_DIR}/prompts/compact.md DAGENT_PROMPT_COMPACT)
configure_file(${CMAKE_SOURCE_DIR}/cmake/prompts.cpp.in ${CMAKE_BINARY_DIR}/generated/prompts.cpp @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${CMAKE_SOURCE_DIR}/prompts/system.md ${CMAKE_SOURCE_DIR}/prompts/compact.md)

add_library(dagent_agent STATIC
    private/agent/llm.cpp
    private/agent/provider.cpp
    private/agent/provider_chat.cpp
    private/agent/provider_anthropic.cpp
    private/agent/provider_ollama.cpp
    private/agent/events.cpp
    private/agent/model.cpp
    private/agent/conversation.cpp
    private/agent/compaction.cpp
    private/agent/mcp_hub.cpp
    private/agent/prompt.cpp
    private/agent/record.cpp
    private/agent/permission.cpp
    private/agent/agent.cpp
    private/agent/dispatch.cpp
    private/agent/headless.cpp
    ${CMAKE_BINARY_DIR}/generated/prompts.cpp
)

target_include_directories(dagent_agent PUBLIC ${CMAKE_SOURCE_DIR}/src/public)
target_compile_features(dagent_agent PUBLIC cxx_std_23)
target_compile_options(dagent_agent PRIVATE -Wall -Wextra)
target_link_libraries(dagent_agent
    PUBLIC dagent_base dagent_net dagent_exec dagent_workspace dagent_session dagent_mcp tools
)

# 可执行文件：只编 main.cpp，其余代码都在库里（docs/design/agent.md §1）。
add_executable(dagent private/agent/main.cpp)

target_compile_features(dagent PRIVATE cxx_std_23)
target_compile_options(dagent PRIVATE -Wall -Wextra)
target_link_libraries(dagent PRIVATE dagent_app dagent_ui)
