set(PICOHTTPPARSER_DIR ${CMAKE_SOURCE_DIR}/third_party/picohttpparser)
set(JSMN_DIR ${CMAKE_SOURCE_DIR}/third_party/jsmn)

add_library(gateway_core STATIC
  src/gateway_config.c
  src/json_util.c
  ${PICOHTTPPARSER_DIR}/picohttpparser.c
)
target_include_directories(gateway_core PUBLIC include src ${PICOHTTPPARSER_DIR} ${JSMN_DIR})
target_link_libraries(gateway_core PUBLIC continuum_common)
target_compile_options(gateway_core PRIVATE -Wno-sign-compare)
target_compile_definitions(gateway_core PUBLIC JSMN_STATIC)

add_executable(api-gatewayd src/main.c)
target_include_directories(api-gatewayd PRIVATE include src ${PICOHTTPPARSER_DIR} ${JSMN_DIR})
target_link_libraries(api-gatewayd PRIVATE gateway_core continuum_common continuum_wire)
target_compile_definitions(api-gatewayd PRIVATE JSMN_STATIC)

if(CONTINUUM_BUILD_TESTS)
  add_executable(json_util_test test/json_util_test.c src/json_util.c)
  target_include_directories(json_util_test PRIVATE src ${JSMN_DIR})
  target_compile_definitions(json_util_test PRIVATE JSMN_STATIC)
  add_test(NAME json_util_test COMMAND json_util_test)

  if(TARGET swapd AND TARGET shard-execd AND TARGET schedulerd AND TARGET gen_toy_weights)
    add_executable(gateway_e2e_test test/gateway_e2e_test.c)
    target_include_directories(gateway_e2e_test PRIVATE include)
    target_link_libraries(gateway_e2e_test PRIVATE continuum_common)
    target_compile_definitions(gateway_e2e_test PRIVATE
      SWAPD_BIN="$<TARGET_FILE:swapd>"
      SHARD_EXECD_BIN="$<TARGET_FILE:shard-execd>"
      SCHEDULERD_BIN="$<TARGET_FILE:schedulerd>"
      API_GATEWAYD_BIN="$<TARGET_FILE:api-gatewayd>"
      GEN_TOY_WEIGHTS_BIN="$<TARGET_FILE:gen_toy_weights>")
    add_test(NAME gateway_e2e_test COMMAND gateway_e2e_test)
    set_tests_properties(gateway_e2e_test PROPERTIES TIMEOUT 30)
  endif()
endif()
