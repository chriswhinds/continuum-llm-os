# Copyright 2026 Christopher Hinds, Stratum Technologies LLC
# Licensed under the Apache License, Version 2.0 -- see the LICENSE file at the repository root.
# SPDX-License-Identifier: Apache-2.0

add_library(continuum_common STATIC
  src/clog.c
  src/hashtable.c
  src/minheap.c
  src/sock_util.c
)
target_include_directories(continuum_common PUBLIC include)
target_link_libraries(continuum_common PUBLIC pthread)
