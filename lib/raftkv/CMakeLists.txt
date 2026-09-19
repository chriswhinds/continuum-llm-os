add_library(continuum_raftkv STATIC
  src/raftkv.c
  src/raftkv_daemon_config.c
)
target_include_directories(continuum_raftkv PUBLIC include)
target_link_libraries(continuum_raftkv PUBLIC continuum_raftnet continuum_common pthread)

if(CONTINUUM_BUILD_TESTS)
  add_executable(raftkv_test test/raftkv_test.c)
  target_link_libraries(raftkv_test PRIVATE continuum_raftkv)
  add_test(NAME raftkv_test COMMAND raftkv_test)
  set_tests_properties(raftkv_test PROPERTIES TIMEOUT 30)
endif()
