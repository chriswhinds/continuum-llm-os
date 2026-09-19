add_library(continuum_pagetable STATIC
  src/shm_pagetable.c
  src/dram_pool.c
)
target_include_directories(continuum_pagetable PUBLIC include)
target_link_libraries(continuum_pagetable PUBLIC pthread rt)

if(CONTINUUM_BUILD_TESTS)
  add_executable(pagetable_test test/pagetable_test.c)
  target_link_libraries(pagetable_test PRIVATE continuum_pagetable)
  add_test(NAME pagetable_test COMMAND pagetable_test)
endif()
