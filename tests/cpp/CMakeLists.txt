find_package(GTest CONFIG QUIET)
if(NOT GTest_FOUND)
  find_package(GTest QUIET)
endif()
if(NOT GTest_FOUND)
  message(STATUS "GoogleTest not installed; fetching v1.14.0")
  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)

set(HWDIAG_TEST_SOURCES
  test_common.cpp
  test_inventory.cpp)

add_executable(hwdiag_tests ${HWDIAG_TEST_SOURCES})
target_link_libraries(hwdiag_tests PRIVATE hwdiag_inventory GTest::gtest_main)
target_compile_definitions(hwdiag_tests PRIVATE
  HWDIAG_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
gtest_discover_tests(hwdiag_tests DISCOVERY_TIMEOUT 60)
