# Copyright (C) 2019-2023 Intel Corporation
# SPDX-License-Identifier: MIT

if(UNIX)
    set(OS_SPECIFIC_LIBS pthread rt dl)
  find_package(MPI QUIET COMPONENTS CXX)
  if(MPI_CXX_FOUND)
    list(APPEND OS_SPECIFIC_LIBS MPI::MPI_CXX)
  endif()
else()
    set(OS_SPECIFIC_LIBS "")
endif()

add_lzt_test(
  NAME ze_peer
  GROUP "/perf_tests"
  SOURCES
    ../common/src/ze_app.cpp
    src/ze_peer.cpp
    src/ze_peer_ipc.cpp
    src/ze_peer_unidirectional.cpp
    src/ze_peer_bidirectional.cpp
    src/ze_peer_parallel_multiple_targets.cpp
    src/ze_peer_parallel_pair_targets.cpp
    src/ze_peer_parallel_single_target.cpp
    src/ze_peer_common.cpp
  LINK_LIBRARIES ${OS_SPECIFIC_LIBS}
  KERNELS ze_peer_benchmarks
)

  if(TARGET ze_peer)
    if(MPI_CXX_FOUND)
      target_compile_definitions(ze_peer PRIVATE ZE_PEER_ENABLE_MPI=1)
    else()
      target_compile_definitions(ze_peer PRIVATE ZE_PEER_ENABLE_MPI=0)
    endif()
  endif()
