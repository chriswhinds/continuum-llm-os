add_library(schedulerd_core STATIC
  src/scheduler_config.c
)
target_include_directories(schedulerd_core PUBLIC include)
target_link_libraries(schedulerd_core PUBLIC continuum_common)

add_executable(schedulerd src/main.c)
target_include_directories(schedulerd PRIVATE include)
target_link_libraries(schedulerd PRIVATE schedulerd_core continuum_common continuum_wire)

if(CONTINUUM_BUILD_TESTS)
  add_executable(scheduler_config_test test/scheduler_config_test.c src/scheduler_config.c)
  target_include_directories(scheduler_config_test PRIVATE include)
  target_link_libraries(scheduler_config_test PRIVATE continuum_common)
  add_test(NAME scheduler_config_test COMMAND scheduler_config_test)

  if(TARGET swapd AND TARGET shard-execd AND TARGET gen_toy_weights)
    add_executable(scheduler_e2e_test test/scheduler_e2e_test.c)
    target_include_directories(scheduler_e2e_test PRIVATE include)
    target_link_libraries(scheduler_e2e_test PRIVATE continuum_common continuum_wire)
    target_compile_definitions(scheduler_e2e_test PRIVATE
      SWAPD_BIN="$<TARGET_FILE:swapd>"
      SHARD_EXECD_BIN="$<TARGET_FILE:shard-execd>"
      SCHEDULERD_BIN="$<TARGET_FILE:schedulerd>"
      GEN_TOY_WEIGHTS_BIN="$<TARGET_FILE:gen_toy_weights>")
    add_test(NAME scheduler_e2e_test COMMAND scheduler_e2e_test)
    set_tests_properties(scheduler_e2e_test PROPERTIES TIMEOUT 30)
  endif()
endif()
