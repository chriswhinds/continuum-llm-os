add_executable(node-agentd
  src/main.c
  src/node_agent_config.c
  src/cpu_load.c
)
target_include_directories(node-agentd PRIVATE include)
target_link_libraries(node-agentd PRIVATE continuum_common continuum_wire continuum_pagetable pthread)

if(CONTINUUM_BUILD_TESTS)
  add_executable(node_agent_config_test test/node_agent_config_test.c src/node_agent_config.c)
  target_include_directories(node_agent_config_test PRIVATE include)
  target_link_libraries(node_agent_config_test PRIVATE continuum_common)
  add_test(NAME node_agent_config_test COMMAND node_agent_config_test)

  add_executable(node_agentd_e2e_test test/node_agentd_e2e_test.c)
  target_include_directories(node_agentd_e2e_test PRIVATE include)
  target_link_libraries(node_agentd_e2e_test PRIVATE continuum_common continuum_wire continuum_pagetable pthread)
  target_compile_definitions(node_agentd_e2e_test PRIVATE NODE_AGENTD_BIN="$<TARGET_FILE:node-agentd>")
  add_test(NAME node_agentd_e2e_test COMMAND node_agentd_e2e_test)
  set_tests_properties(node_agentd_e2e_test PROPERTIES TIMEOUT 20)
endif()
