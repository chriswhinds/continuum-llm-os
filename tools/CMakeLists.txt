# Copyright 2026 Christopher Hinds, Stratum Technologies LLC
# Licensed under the Apache License, Version 2.0 -- see the LICENSE file at the repository root.
# SPDX-License-Identifier: Apache-2.0

add_executable(gen_toy_weights gen_toy_weights.c)
target_link_libraries(gen_toy_weights PRIVATE continuum_toymodel_format)
