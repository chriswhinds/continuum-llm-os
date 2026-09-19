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

  if(TARGET continuum_pageclient)
    add_executable(pageclient_test ${CMAKE_SOURCE_DIR}/lib/pageclient/test/pageclient_test.c)
    target_link_libraries(pageclient_test PRIVATE continuum_pageclient continuum_common continuum_wire continuum_pagetable pthread)
    target_compile_definitions(pageclient_test PRIVATE SWAPD_BIN="$<TARGET_FILE:swapd>")
    add_test(NAME pageclient_test COMMAND pageclient_test)
    set_tests_properties(pageclient_test PROPERTIES TIMEOUT 20)
  endif()
endif()
