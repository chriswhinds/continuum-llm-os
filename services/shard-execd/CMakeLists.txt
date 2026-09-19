add_library(shard_execd_core STATIC
  src/shard_config.c
  src/toy_model.c
)
target_include_directories(shard_execd_core PUBLIC include)
target_link_libraries(shard_execd_core PUBLIC continuum_common continuum_pageclient continuum_toymodel_format m)

add_executable(shard-execd src/main.c)
target_include_directories(shard-execd PRIVATE include)
target_link_libraries(shard-execd PRIVATE shard_execd_core continuum_common continuum_wire continuum_kvcache continuum_pageclient m)

if(CONTINUUM_BUILD_TESTS)
  add_executable(toy_model_test test/toy_model_test.c)
  target_include_directories(toy_model_test PRIVATE include)
  target_link_libraries(toy_model_test PRIVATE shard_execd_core continuum_common continuum_wire continuum_pagetable continuum_pageclient continuum_toymodel_format pthread m)
  target_compile_definitions(toy_model_test PRIVATE
    SWAPD_BIN="$<TARGET_FILE:swapd>"
    GEN_TOY_WEIGHTS_BIN="$<TARGET_FILE:gen_toy_weights>")
  add_test(NAME toy_model_test COMMAND toy_model_test)
  set_tests_properties(toy_model_test PROPERTIES TIMEOUT 30)

  add_executable(shard_execd_e2e_test test/shard_execd_e2e_test.c)
  target_include_directories(shard_execd_e2e_test PRIVATE include)
  target_link_libraries(shard_execd_e2e_test PRIVATE continuum_common continuum_wire pthread)
  target_compile_definitions(shard_execd_e2e_test PRIVATE
    SWAPD_BIN="$<TARGET_FILE:swapd>"
    SHARD_EXECD_BIN="$<TARGET_FILE:shard-execd>"
    GEN_TOY_WEIGHTS_BIN="$<TARGET_FILE:gen_toy_weights>")
  add_test(NAME shard_execd_e2e_test COMMAND shard_execd_e2e_test)
  set_tests_properties(shard_execd_e2e_test PROPERTIES TIMEOUT 30)
endif()
