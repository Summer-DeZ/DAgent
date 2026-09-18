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
