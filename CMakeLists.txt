cmake_minimum_required(VERSION 3.20)
project(hwdiag VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
# CMake's Release default is -O3; the benchmarks are specified at -O2.
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "Release flags" FORCE)

option(HWDIAG_WERROR "Treat compiler warnings as errors" ON)
option(HWDIAG_SANITIZE "Build with -fsanitize=address,undefined" OFF)
option(HWDIAG_BUILD_TESTS "Build the GoogleTest suite" ON)

# Every executable lands in build/bin, where the Python harness looks for it.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(HWDIAG_WARNING_FLAGS -Wall -Wextra)
if(HWDIAG_WERROR)
  list(APPEND HWDIAG_WARNING_FLAGS -Werror)
endif()
set(HWDIAG_SANITIZE_FLAGS "")
if(HWDIAG_SANITIZE)
  set(HWDIAG_SANITIZE_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer
                            -fno-sanitize-recover=undefined)
endif()

add_library(hwdiag_flags INTERFACE)
target_compile_options(hwdiag_flags INTERFACE ${HWDIAG_WARNING_FLAGS} ${HWDIAG_SANITIZE_FLAGS})
target_link_options(hwdiag_flags INTERFACE ${HWDIAG_SANITIZE_FLAGS})

# Build metadata recorded in every JSON result.
set(HWDIAG_GIT_COMMIT "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE _hwdiag_commit
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _hwdiag_git_rc)
  if(_hwdiag_git_rc EQUAL 0 AND _hwdiag_commit)
    set(HWDIAG_GIT_COMMIT ${_hwdiag_commit})
  endif()
endif()
string(TOUPPER "${CMAKE_BUILD_TYPE}" _hwdiag_bt)
string(JOIN " " HWDIAG_COMPILER_FLAGS "-std=c++20" ${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_hwdiag_bt}}
       ${HWDIAG_WARNING_FLAGS} ${HWDIAG_SANITIZE_FLAGS})
string(STRIP "${HWDIAG_COMPILER_FLAGS}" HWDIAG_COMPILER_FLAGS)
configure_file(src/common/build_info.hpp.in ${CMAKE_BINARY_DIR}/generated/build_info.hpp @ONLY)

find_package(Threads REQUIRED)

add_subdirectory(src/common)
add_subdirectory(src/inventory)

if(HWDIAG_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests/cpp)
endif()
