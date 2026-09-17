add_executable(tui_tests
    main.cpp
    grapheme_test.cpp
    terminal_test.cpp
    surface_test.cpp
    present_test.cpp
    layout_test.cpp
    widget_test.cpp
    frame_test.cpp
    document_test.cpp
    input_test.cpp
    app_test.cpp
)

target_link_libraries(tui_tests PRIVATE dagent_tui Boost::headers)
# 用例用指定初始化器构造 FrameOptions，未写出的成员依赖默认值是有意为之。
target_compile_options(tui_tests PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

add_test(NAME tui COMMAND tui_tests)
