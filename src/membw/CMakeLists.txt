add_library(hwdiag_membw STATIC stream.cpp kernels.cpp)
target_link_libraries(hwdiag_membw PUBLIC hwdiag_common hwdiag_perfctr Threads::Threads)
# Keep the copy loop a loop. GCC (loop distribution) and Clang (loop idiom
# recognition) would otherwise turn it into a memcpy call, and glibc's memcpy
# uses non-temporal stores for large copies, which skips the write-allocate
# read that STREAM's copy measures.
set_source_files_properties(kernels.cpp PROPERTIES COMPILE_OPTIONS
  "$<$<CXX_COMPILER_ID:GNU>:-fno-tree-loop-distribute-patterns>;$<$<CXX_COMPILER_ID:Clang>:-fno-builtin>")

add_executable(membw main.cpp)
target_link_libraries(membw PRIVATE hwdiag_membw hwdiag_inventory)
