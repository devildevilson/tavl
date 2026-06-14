set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG        v2.5.2
  GIT_PROGRESS   TRUE
)
FetchContent_MakeAvailable(doctest)
unset(CMAKE_POLICY_VERSION_MINIMUM)

set(TAVL_TEST_SRC
  doctest_main.cpp

  format/test_format_examples.cpp

  features/test_lexer.cpp
  features/test_parser.cpp
  features/test_serialize.cpp
  features/test_deserialize.cpp
  features/test_chrono.cpp

  errors/test_errors.cpp

  extensions/test_pair_ast.cpp
  extensions/test_tag_ast.cpp
  extensions/test_math_ast.cpp
)

add_executable(tavl_tests ${TAVL_TEST_SRC})
target_link_libraries(tavl_tests PRIVATE tavl doctest::doctest)
target_include_directories(tavl_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# tavl demo
target_compile_definitions(tavl_tests PRIVATE
  TAVL_FORMAT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/format"
)

include("${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")
doctest_discover_tests(tavl_tests)
