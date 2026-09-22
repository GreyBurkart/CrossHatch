add_executable(BleRemoteTest
  BleRemoteTest.cpp
  ${REPO_ROOT}/src/ble/BleRemoteAction.cpp
  ${REPO_ROOT}/src/ble/BleRemoteCore.cpp
)
target_include_directories(BleRemoteTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
)
target_link_libraries(BleRemoteTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(BleRemoteTest)
