add_library(continuum_pageclient STATIC
  src/pageclient.c
)
target_include_directories(continuum_pageclient PUBLIC include)
target_link_libraries(continuum_pageclient PUBLIC continuum_common continuum_wire continuum_pagetable)

# pageclient_test needs the swapd binary to exist as a real subprocess, so
# it's registered from services/swapd/CMakeLists.txt (processed after this
# file, once the `swapd` target is actually defined) rather than here.
