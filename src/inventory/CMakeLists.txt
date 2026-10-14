add_library(hwdiag_inventory STATIC inventory.cpp)
target_link_libraries(hwdiag_inventory PUBLIC hwdiag_common)

add_executable(inventory main.cpp)
target_link_libraries(inventory PRIVATE hwdiag_inventory)
