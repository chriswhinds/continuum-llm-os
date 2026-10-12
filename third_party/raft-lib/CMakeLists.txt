# Copyright 2026 Christopher Hinds, Stratum Labs
# Licensed under the Apache License, Version 2.0 -- see the LICENSE file at the repository root.
# SPDX-License-Identifier: Apache-2.0

# Builds the vendored willemt/raft sources (../raft, MIT/BSD, unmodified) as
# a static library. Kept as its own CMakeLists rather than editing the
# vendored tree, so `third_party/raft` stays a clean drop-in of upstream.
set(RAFT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../raft)

add_library(raft STATIC
  ${RAFT_DIR}/src/raft_server.c
  ${RAFT_DIR}/src/raft_server_properties.c
  ${RAFT_DIR}/src/raft_node.c
  ${RAFT_DIR}/src/raft_log.c
)
target_include_directories(raft PUBLIC ${RAFT_DIR}/include)
target_compile_options(raft PRIVATE -Wno-unused-parameter -Wno-sign-compare)
