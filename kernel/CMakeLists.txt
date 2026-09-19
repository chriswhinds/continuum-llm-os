add_executable(continuumd
  src/main.c
  src/config.c
  src/supervisor.c
  src/cgroup_util.c
)
target_include_directories(continuumd PRIVATE include)
target_link_libraries(continuumd PRIVATE continuum_common)

if(CONTINUUM_BUILD_TESTS)
  add_executable(supervisor_test
    test/supervisor_test.c
    src/config.c
    src/supervisor.c
    src/cgroup_util.c
  )
  target_include_directories(supervisor_test PRIVATE include)
  target_link_libraries(supervisor_test PRIVATE continuum_common)
  add_test(NAME supervisor_test COMMAND supervisor_test)
endif()
