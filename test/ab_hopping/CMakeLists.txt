# Reuse PlatformIO's pinned dependency when available; clean native-only builds
# fetch the same version as the firmware.
file(GLOB ARDUINOJSON_CACHED_DIRS "${REPO_ROOT}/.pio/libdeps/*/ArduinoJson/src")
find_path(AB_ARDUINOJSON_INCLUDE_DIR ArduinoJson.h PATHS ${ARDUINOJSON_CACHED_DIRS} NO_DEFAULT_PATH)
if(NOT AB_ARDUINOJSON_INCLUDE_DIR)
  FetchContent_Declare(arduinojson
    GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
    GIT_TAG v7.4.2
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(arduinojson)
  set(AB_ARDUINOJSON_INCLUDE_DIR "${arduinojson_SOURCE_DIR}/src")
endif()

add_executable(DocumentSlotsTest
  DocumentSlotsTest.cpp
  ${REPO_ROOT}/src/util/DocumentSlots.cpp
)
target_include_directories(DocumentSlotsTest PRIVATE
  ${REPO_ROOT}/src
  ${AB_ARDUINOJSON_INCLUDE_DIR}
)
target_link_libraries(DocumentSlotsTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(DocumentSlotsTest)
