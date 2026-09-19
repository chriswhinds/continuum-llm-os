add_library(continuum_common STATIC
  src/clog.c
  src/hashtable.c
  src/minheap.c
  src/sock_util.c
)
target_include_directories(continuum_common PUBLIC include)
target_link_libraries(continuum_common PUBLIC pthread)
