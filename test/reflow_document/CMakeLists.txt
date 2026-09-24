add_executable(reflow_document_tests
  ReflowCapabilityTest.cpp
  ReflowDocumentContractTest.cpp
  ReflowReaderProgressTest.cpp
  ReflowSectionSourceTest.cpp
)

target_include_directories(reflow_document_tests BEFORE PRIVATE
  ${REPO_ROOT}/lib/Reflow
)

if(MSVC)
  target_include_directories(reflow_document_tests PRIVATE
    ${REPO_ROOT}
    ${REPO_ROOT}/lib
  )
  target_compile_options(reflow_document_tests PRIVATE /W4)
  target_link_libraries(reflow_document_tests PRIVATE GTest::gtest_main)
else()
  target_link_libraries(reflow_document_tests PRIVATE
    crosspoint_test_common
    GTest::gtest_main
  )
endif()

gtest_discover_tests(reflow_document_tests)
