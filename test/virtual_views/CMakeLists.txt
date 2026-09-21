add_executable(VirtualViewsTest
  VirtualViewsTest.cpp
  ${REPO_ROOT}/src/activities/home/VirtualViews.cpp
)
target_include_directories(VirtualViewsTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/FsHelpers
)
target_link_libraries(VirtualViewsTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(VirtualViewsTest)
