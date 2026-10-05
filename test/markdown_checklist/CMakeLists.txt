add_executable(MarkdownChecklistTest
  MarkdownChecklistTest.cpp
  SyncedNoteTest.cpp
  ${REPO_ROOT}/src/util/MarkdownChecklist.cpp
  ${REPO_ROOT}/src/util/SyncedNote.cpp
)
target_include_directories(MarkdownChecklistTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Memory
)
target_link_libraries(MarkdownChecklistTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(MarkdownChecklistTest)
