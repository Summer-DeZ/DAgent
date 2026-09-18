add_executable(tui_tests
    main.cpp
    app_test.cpp
    input_test.cpp
)

target_link_libraries(tui_tests PRIVATE dagent_tui Boost::headers)
target_compile_options(tui_tests PRIVATE -Wall -Wextra)

add_test(NAME tui COMMAND tui_tests)
