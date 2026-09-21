add_executable(LibraryPinsTest LibraryPinsTest.cpp ${REPO_ROOT}/src/util/LibraryPins.cpp)
target_include_directories(LibraryPinsTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs ${REPO_ROOT}/src/util)
target_link_libraries(LibraryPinsTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(LibraryPinsTest)
