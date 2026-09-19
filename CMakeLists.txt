cmake_minimum_required(VERSION 3.16)
project(continuum C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# ---------------------------------------------------------------------------
# Build options
# ---------------------------------------------------------------------------
option(CONTINUUM_STATIC     "Link daemons fully static (needs a static libc, e.g. musl)" OFF)
option(CONTINUUM_TLS        "Build api-gatewayd with mbedTLS support (off: put TLS in front instead)" OFF)
option(CONTINUUM_NEON       "Build shard-execd's kernels with ARM NEON (auto-detected on aarch64)" AUTO)
option(CONTINUUM_BUILD_TESTS "Build the lib/ unit tests" ON)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(CONTINUUM_TARGET_ARM TRUE)
else()
  set(CONTINUUM_TARGET_ARM FALSE)
endif()

add_compile_definitions(_DEFAULT_SOURCE _FILE_OFFSET_BITS=64)
add_compile_options(-Wall -Wextra -Wno-unused-parameter -g)
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
  add_compile_options(-O2)
endif()

if(CONTINUUM_STATIC)
  add_link_options(-static)
endif()

# Reference hardware per ARCH-002 §01 is a Raspberry Pi 5 (Cortex-A76). Only
# apply the tuning flag when actually cross/native-building for aarch64 --
# it is invalid on the x86_64 dev/CI build this repo also supports.
if(CONTINUUM_TARGET_ARM)
  add_compile_options(-mcpu=cortex-a76)
endif()

include_directories(${CMAKE_SOURCE_DIR}/lib/wire/include)
include_directories(${CMAKE_SOURCE_DIR}/lib/kvcache/include)
include_directories(${CMAKE_SOURCE_DIR}/lib/raftnet/include)
include_directories(${CMAKE_SOURCE_DIR}/lib/common/include)

enable_testing()

add_subdirectory(lib/common)
add_subdirectory(lib/wire)
add_subdirectory(lib/kvcache)
add_subdirectory(third_party/raft-lib)
add_subdirectory(lib/raftnet)

# Added incrementally as each is implemented -- see README.md's build status table.
if(EXISTS ${CMAKE_SOURCE_DIR}/kernel/CMakeLists.txt)
  add_subdirectory(kernel)
endif()
foreach(svc node-agentd swapd shard-execd page-directoryd membershipd schedulerd api-gatewayd consoled)
  if(EXISTS ${CMAKE_SOURCE_DIR}/services/${svc}/CMakeLists.txt)
    add_subdirectory(services/${svc})
  endif()
endforeach()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/CMakeLists.txt)
  add_subdirectory(tools)
endif()

message(STATUS "Continuum OS build configuration")
message(STATUS "  Target arch        : ${CMAKE_SYSTEM_PROCESSOR} (ARM tuning: ${CONTINUUM_TARGET_ARM})")
message(STATUS "  Static link         : ${CONTINUUM_STATIC}")
message(STATUS "  TLS in api-gatewayd : ${CONTINUUM_TLS}")
