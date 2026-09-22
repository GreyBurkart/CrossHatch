add_executable(MarkdownReaderLayoutTest
  MarkdownReaderLayoutTest.cpp
  ${REPO_ROOT}/src/util/MarkdownReaderLayout.cpp
  ${REPO_ROOT}/src/util/MarkdownParser.cpp
)
target_include_directories(MarkdownReaderLayoutTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Memory
)
target_link_libraries(MarkdownReaderLayoutTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(MarkdownReaderLayoutTest)
