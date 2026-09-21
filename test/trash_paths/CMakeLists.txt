add_executable(TrashPathsTest
  TrashPathsTest.cpp
)

target_include_directories(TrashPathsTest PRIVATE
  ${REPO_ROOT}/lib/FsHelpers
)

target_link_libraries(TrashPathsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TrashPathsTest)
