add_executable(tui_tests
    main.cpp
    render_test.cpp
    document_test.cpp
    runtime_test.cpp
)

target_link_libraries(tui_tests PRIVATE dagent_tui Boost::headers)
target_compile_options(tui_tests PRIVATE -Wall -Wextra)
# 一致性测试直接读 Unicode 官方数据文件（随测试目录入库）。
target_compile_definitions(tui_tests PRIVATE
    DAGENT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

add_test(NAME tui COMMAND tui_tests)
