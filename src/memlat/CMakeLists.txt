add_library(hwdiag_memlat STATIC chain.cpp measure.cpp)
target_link_libraries(hwdiag_memlat PUBLIC hwdiag_common)

add_executable(memlat main.cpp)
target_link_libraries(memlat PRIVATE hwdiag_memlat hwdiag_inventory)
