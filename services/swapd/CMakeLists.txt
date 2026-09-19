add_library(swapd_core STATIC
  src/swapd_config.c
  src/paging_engine.c
)
target_include_directories(swapd_core PUBLIC include)
target_link_libraries(swapd_core PUBLIC continuum_common continuum_wire continuum_pagetable pthread)

add_executable(swapd src/main.c)
target_include_directories(swapd PRIVATE include)
target_link_libraries(swapd PRIVATE swapd_core continuum_common continuum_wire continuum_pagetable pthread)

if(CONTINUUM_BUILD_TESTS)
  add_executable(paging_engine_test test/paging_engine_test.c)
  target_include_directories(paging_engine_test PRIVATE include)
  target_link_libraries(paging_engine_test PRIVATE swapd_core continuum_common continuum_wire continuum_pagetable pthread)
  add_test(NAME paging_engine_test COMMAND paging_engine_test)
endif()
