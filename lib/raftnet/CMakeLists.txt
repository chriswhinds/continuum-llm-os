add_library(continuum_raftnet STATIC
  src/raftnet.c
)
target_include_directories(continuum_raftnet PUBLIC include)
target_link_libraries(continuum_raftnet PUBLIC raft continuum_wire continuum_common pthread)

if(CONTINUUM_BUILD_TESTS)
  add_executable(raftnet_test test/raftnet_test.c)
  target_link_libraries(raftnet_test PRIVATE continuum_raftnet)
  add_test(NAME raftnet_test COMMAND raftnet_test)
  set_tests_properties(raftnet_test PROPERTIES TIMEOUT 30)
endif()
