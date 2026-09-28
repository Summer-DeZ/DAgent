# 最底层公共库：日志、文本工具、JSON 脱敏。
add_library(dagent_base STATIC
    private/base/log.cpp
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

# 应用层：在 TUI 框架之上构建的界面（只持 Client、协议 DTO 与页面状态）。
add_library(dagent_ui STATIC
    private/ui/strings.cpp
    private/ui/display.cpp
    private/ui/theme_config.cpp
    private/ui/projection.cpp
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
    private/ui/model_dialog.cpp
    private/ui/side_panel.cpp
)

target_link_libraries(dagent_ui PUBLIC dagent_tui dagent_client dagent_protocol dagent_base)
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

# 会话存储：单个 SQLite 数据库、脱敏、按 cwd 列表与顺序回放。
add_library(dagent_storage STATIC
    private/storage/storage.cpp
    public/lib/sqlite/sqlite3.c
)

target_include_directories(dagent_storage PUBLIC ${CMAKE_SOURCE_DIR}/src/public)
target_compile_features(dagent_storage PUBLIC cxx_std_23)
target_compile_options(dagent_storage PRIVATE -Wall -Wextra)
target_compile_definitions(dagent_storage PRIVATE
    DAGENT_VERSION="${PROJECT_VERSION}"
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_DQS=0
    SQLITE_THREADSAFE=1
)
target_link_libraries(dagent_storage
    PUBLIC dagent_base dagent_agent
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
# 依赖 agent（中立工具契约）、base、exec、workspace、mcp；核心不反向依赖 tools。
add_library(tools STATIC
    private/tools/context.cpp
    private/tools/detail.cpp
    private/tools/registry.cpp
    private/tools/read.cpp
    private/tools/write.cpp
    private/tools/edit.cpp
    private/tools/bash.cpp
    private/tools/grep.cpp
    private/tools/glob.cpp
    private/tools/mcp.cpp
    private/tools/mcp_hub.cpp
)

target_include_directories(tools PUBLIC ${CMAKE_SOURCE_DIR}/src/public)
target_compile_features(tools PUBLIC cxx_std_23)
target_compile_options(tools PRIVATE -Wall -Wextra)
target_link_libraries(tools
    PUBLIC dagent_agent dagent_base dagent_exec dagent_workspace dagent_mcp
)

# 前端入口层：命令行解析、安装根、前端启动与输出适配。其他模块都不依赖它。
add_library(dagent_app_cli STATIC
    private/app/cli.cpp
    private/app/interrupts.cpp
    private/app/launcher.cpp
    private/app/output.cpp
    private/app/paths.cpp
    private/app/run.cpp
)

target_include_directories(dagent_app_cli PUBLIC ${CMAKE_SOURCE_DIR}/src/public)
target_compile_features(dagent_app_cli PUBLIC cxx_std_23)
target_compile_options(dagent_app_cli PRIVATE -Wall -Wextra)
target_compile_definitions(dagent_app_cli PRIVATE DAGENT_VERSION="${PROJECT_VERSION}")
if(DAGENT_DEV_HOME)
    target_compile_definitions(dagent_app_cli PRIVATE DAGENT_DEV_HOME="${DAGENT_DEV_HOME}")
endif()
target_link_libraries(dagent_app_cli
    PUBLIC dagent_client dagent_protocol dagent_base
    PRIVATE CLI11::CLI11
)

# 核心 agent：中立消息模型、动作/控制契约、Session/Run/循环、调度、权限与记录语义。
add_library(dagent_agent STATIC
    private/agent/skills.cpp
    private/agent/action.cpp
    private/agent/control.cpp
    private/agent/catalog.cpp
    private/agent/committer.cpp
    private/agent/record_codec.cpp
    private/agent/history.cpp
    private/agent/recovery.cpp
    private/agent/session.cpp
    private/agent/compaction.cpp
    private/agent/dispatch.cpp
    private/agent/turn_runner.cpp
    private/agent/tokens.cpp
    private/agent/tool_data.cpp
    private/agent/conversation.cpp
    private/agent/events.cpp
    private/agent/permission.cpp
)

target_include_directories(dagent_agent PUBLIC ${CMAKE_SOURCE_DIR}/src/public)
target_compile_features(dagent_agent PUBLIC cxx_std_23)
target_compile_options(dagent_agent PRIVATE -Wall -Wextra)
target_link_libraries(dagent_agent PUBLIC dagent_base)

# llm 适配：Model 实际请求、provider 编解码、HTTP/SSE；实现核心 ModelSession 端口。
add_library(dagent_llm STATIC
    private/llm/model.cpp
    private/llm/provider.cpp
    private/llm/provider_chat.cpp
    private/llm/provider_anthropic.cpp
    private/llm/provider_ollama.cpp
)

target_include_directories(dagent_llm PUBLIC ${CMAKE_SOURCE_DIR}/src/public)
target_compile_features(dagent_llm PUBLIC cxx_std_23)
target_compile_options(dagent_llm PRIVATE -Wall -Wextra)
target_link_libraries(dagent_llm
    PUBLIC dagent_agent dagent_net
    PRIVATE CURL::libcurl
)

# protocol：线上 DTO/编解码/错误码，纯数据；只依赖 base。
add_library(dagent_protocol STATIC
    private/protocol/dto.cpp
    private/protocol/rpc.cpp
)

target_include_directories(dagent_protocol PUBLIC ${CMAKE_SOURCE_DIR}/src/public)
target_compile_features(dagent_protocol PUBLIC cxx_std_23)
target_compile_options(dagent_protocol PRIVATE -Wall -Wextra)
target_link_libraries(dagent_protocol PUBLIC dagent_base)

# ipc：socketpair FD 包装、JSON 行分帧与后端进程启动。
add_library(dagent_ipc STATIC
    private/ipc/channel.cpp
)

target_include_directories(dagent_ipc PUBLIC ${CMAKE_SOURCE_DIR}/src/public)
target_compile_features(dagent_ipc PUBLIC cxx_std_23)
target_compile_options(dagent_ipc PRIVATE -Wall -Wextra)
target_link_libraries(dagent_ipc PUBLIC dagent_base)

# client：RPC 匹配、DTO 状态与请求/事件转交。
add_library(dagent_client STATIC
    private/client/client.cpp
)

target_include_directories(dagent_client PUBLIC ${CMAKE_SOURCE_DIR}/src/public)
target_compile_features(dagent_client PUBLIC cxx_std_23)
target_compile_options(dagent_client PRIVATE -Wall -Wextra)
target_link_libraries(dagent_client PUBLIC dagent_protocol dagent_ipc dagent_base)

# runtime：会话控制、交互代理、子执行与只读查询协调；只经核心端口访问具体实现。
add_library(dagent_runtime STATIC
    private/runtime/interaction.cpp
    private/runtime/controller.cpp
    private/runtime/subagent.cpp
    private/runtime/runtime.cpp
)

target_include_directories(dagent_runtime PUBLIC ${CMAKE_SOURCE_DIR}/src/public)
target_compile_features(dagent_runtime PUBLIC cxx_std_23)
target_compile_options(dagent_runtime PRIVATE -Wall -Wextra)
target_link_libraries(dagent_runtime PUBLIC dagent_agent)

# 后端配置与具体适配对象装配：SessionFactory/ConfigurationGateway/QueryGateway 的实现。
add_library(dagent_app_config STATIC
    private/app/skills.cpp
    private/app/toolchain.cpp
    private/app/config.cpp
    private/app/bootstrap.cpp
    private/app/assembly.cpp
    private/app/session_assembly.cpp
    private/app/configuration.cpp
    private/app/queries.cpp
    private/app/prompt.cpp
    private/app/history.cpp
)

target_include_directories(dagent_app_config PUBLIC ${CMAKE_SOURCE_DIR}/src/public)
target_compile_features(dagent_app_config PUBLIC cxx_std_23)
target_compile_options(dagent_app_config PRIVATE -Wall -Wextra)
target_compile_definitions(dagent_app_config PRIVATE DAGENT_VERSION="${PROJECT_VERSION}")
if(DAGENT_DEV_HOME)
    target_compile_definitions(dagent_app_config PRIVATE DAGENT_DEV_HOME="${DAGENT_DEV_HOME}")
endif()
target_link_libraries(dagent_app_config
    PUBLIC dagent_runtime dagent_llm tools dagent_storage dagent_workspace dagent_exec dagent_mcp
           dagent_base
    PRIVATE yaml-cpp::yaml-cpp CURL::libcurl OpenSSL::Crypto
)

# backend：RPC 方法适配与事件 DTO 转换；不链接 TUI。
add_library(dagent_backend STATIC
    private/backend/convert.cpp
    private/backend/publisher.cpp
    private/backend/backend.cpp
)

target_include_directories(dagent_backend PUBLIC ${CMAKE_SOURCE_DIR}/src/public)
target_compile_features(dagent_backend PUBLIC cxx_std_23)
target_compile_options(dagent_backend PRIVATE -Wall -Wextra)
target_compile_definitions(dagent_backend PRIVATE DAGENT_VERSION="${PROJECT_VERSION}")
target_link_libraries(dagent_backend
    PUBLIC dagent_runtime dagent_protocol dagent_ipc dagent_base
)

# 后端可执行文件：与前端同安装目录，只接受 --ipc-fd；入口在 app，把配置装配注入 backend。
add_executable(dagent-backend private/app/backend_main.cpp)

target_compile_features(dagent-backend PRIVATE cxx_std_23)
target_compile_options(dagent-backend PRIVATE -Wall -Wextra)
target_link_libraries(dagent-backend PRIVATE dagent_backend dagent_app_config)

# 可执行文件：只编 main.cpp，其余代码都在库里（docs/design/agent.md §1）。
add_executable(dagent private/app/main.cpp)

target_compile_features(dagent PRIVATE cxx_std_23)
target_compile_options(dagent PRIVATE -Wall -Wextra)
target_link_libraries(dagent PRIVATE dagent_app_cli dagent_ui)
