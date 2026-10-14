add_library(hwdiag_memtest STATIC patterns.cpp)
target_link_libraries(hwdiag_memtest PUBLIC hwdiag_common)

add_executable(memtest main.cpp)
target_link_libraries(memtest PRIVATE hwdiag_memtest)
