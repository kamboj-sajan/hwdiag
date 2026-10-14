add_library(hwdiag_perfctr STATIC perfctr.cpp)
target_link_libraries(hwdiag_perfctr PUBLIC hwdiag_common)

add_executable(perfcheck perfcheck.cpp)
target_link_libraries(perfcheck PRIVATE hwdiag_perfctr hwdiag_memlat)
