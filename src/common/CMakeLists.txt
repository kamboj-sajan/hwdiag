add_library(hwdiag_common STATIC
  args.cpp
  affinity.cpp
  hostinfo.cpp
  json.cpp
  memory.cpp
  result.cpp
  sysfs.cpp
  timing.cpp)
target_include_directories(hwdiag_common PUBLIC ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(hwdiag_common PUBLIC hwdiag_flags)
