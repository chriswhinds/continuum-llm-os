add_library(continuum_wire STATIC
  src/wire.c
)
target_include_directories(continuum_wire PUBLIC include)
target_link_libraries(continuum_wire PUBLIC continuum_common)

if(CONTINUUM_BUILD_TESTS)
  add_executable(wire_test test/wire_test.c)
  target_link_libraries(wire_test PRIVATE continuum_wire continuum_common)
  add_test(NAME wire_test COMMAND wire_test)
endif()
