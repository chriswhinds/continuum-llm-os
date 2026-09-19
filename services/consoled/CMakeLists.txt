# consoled originally targeted civetweb (ARCH-002 §02) for its HTTP
# surface, but the vendored third_party/civetweb snapshot (an unpinned
# clone of upstream HEAD) turned out to have a genuine syntax error deep
# in its request-parsing code -- not something introduced by this build,
# a broken commit in the tree that was cloned. consoled's actual HTTP
# surface is two GET routes with no bodies, websockets, or TLS, so rather
# than chase a working civetweb commit, it reuses picohttpparser -- the
# same real, already-working dependency api-gatewayd uses for a
# considerably larger HTTP surface -- with a small hand-rolled response
# writer in place of civetweb's server loop. See main.c's module comment.
set(PICOHTTPPARSER_DIR ${CMAKE_SOURCE_DIR}/third_party/picohttpparser)

add_library(consoled_core STATIC
  src/console_config.c
  src/telemetry_store.c
  ${PICOHTTPPARSER_DIR}/picohttpparser.c
)
target_include_directories(consoled_core PUBLIC include src ${PICOHTTPPARSER_DIR})
target_link_libraries(consoled_core PUBLIC continuum_common)
target_compile_options(consoled_core PRIVATE -Wno-sign-compare)

add_executable(consoled src/main.c)
target_include_directories(consoled PRIVATE include src ${PICOHTTPPARSER_DIR})
target_link_libraries(consoled PRIVATE consoled_core continuum_common continuum_wire pthread)

if(CONTINUUM_BUILD_TESTS)
  add_executable(telemetry_store_test test/telemetry_store_test.c src/telemetry_store.c)
  target_include_directories(telemetry_store_test PRIVATE src)
  target_link_libraries(telemetry_store_test PRIVATE continuum_common pthread)
  add_test(NAME telemetry_store_test COMMAND telemetry_store_test)

  if(TARGET node-agentd AND TARGET swapd AND TARGET gen_toy_weights)
    add_executable(console_e2e_test test/console_e2e_test.c)
    target_link_libraries(console_e2e_test PRIVATE continuum_common continuum_wire)
    target_compile_definitions(console_e2e_test PRIVATE
      CONSOLED_BIN="$<TARGET_FILE:consoled>"
      NODE_AGENTD_BIN="$<TARGET_FILE:node-agentd>"
      SWAPD_BIN="$<TARGET_FILE:swapd>"
      GEN_TOY_WEIGHTS_BIN="$<TARGET_FILE:gen_toy_weights>")
    add_test(NAME console_e2e_test COMMAND console_e2e_test)
    set_tests_properties(console_e2e_test PROPERTIES TIMEOUT 20)
  endif()
endif()
