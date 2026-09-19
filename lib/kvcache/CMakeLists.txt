add_library(continuum_kvcache STATIC
  src/kvcache.c
)
target_include_directories(continuum_kvcache PUBLIC include)

if(CONTINUUM_BUILD_TESTS)
  add_executable(kvcache_test test/kvcache_test.c)
  target_link_libraries(kvcache_test PRIVATE continuum_kvcache)
  add_test(NAME kvcache_test COMMAND kvcache_test)
endif()
