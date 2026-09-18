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
    private/tui/app.cpp
    private/tui/keymap.cpp
)

target_include_directories(dagent_tui PUBLIC ${CMAKE_SOURCE_DIR}/src/public)
target_compile_features(dagent_tui PUBLIC cxx_std_23)
target_compile_options(dagent_tui PRIVATE -Wall -Wextra)
