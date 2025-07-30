# Copyright (c) 2022, Alibaba Group Holding Limited

cmake_minimum_required (VERSION 3.10)
project (rquic)

set (rquic_VERSION_MAJOR 0)
set (rquic_VERSION_MINOR 1)

include(CMakePackageConfigHelpers)
include(CheckCCompilerFlag)
include(GNUInstallDirs)

option (RQC_ENABLE_BBR2 "enable bbr2" OFF)
option (RQC_PROTECT_POOL_MEM "enable write protection for pool memory (for debug)" OFF)
option (RQC_ENABLE_TESTING "Enable Testing" OFF)
option (GCOV "Test Coverage" OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_definitions(-DRQC_DEBUG)
else()
    add_definitions(-DRQC_RELEASE)
endif()

# print only error log
if(RQC_ONLY_ERROR_LOG)
    add_definitions(-DRQC_ONLY_ERROR_LOG)
endif()

# enable event log
if(RQC_ENABLE_EVENT_LOG)
    add_definitions(-DRQC_ENABLE_EVENT_LOG)
endif()

# sendmmsg
if(RQC_SUPPORT_SENDMMSG_BUILD)
    add_definitions(-DRQC_SUPPORT_SENDMMSG)
endif()

if(NOT (CMAKE_C_COMPILER_ID MATCHES "MSVC" OR CMAKE_CXX_COMPILER_ID MATCHES "MSVC"))
    set(CMAKE_C_FLAGS_OPTION "-Werror -Wno-unused -Wno-pointer-sign")
endif()

if (IOS)
    set(CMAKE_C_FLAGS_OPTION "${CMAKE_C_FLAGS_OPTION} -Wno-shorten-64-to-32")
endif ()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -O2 -std=gnu11 -Wall ${CMAKE_C_FLAGS_OPTION}")

if(ASAN)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -w -fsanitize=address -fsanitize-address-use-after-scope -fno-omit-frame-pointer")
endif()

# coverity
if(GCOV STREQUAL "on")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-arcs -ftest-coverage")
endif()

if(CMAKE_SYSTEM_NAME MATCHES "Windows")
    add_definitions(-DRQC_SYS_WINDOWS=1)
    set(CMAKE_C_FLAGS_OPTION " ")
endif()

# configure file
configure_file (
    "${CMAKE_CURRENT_SOURCE_DIR}/rqc_configure.h.in"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/rquic/rqc_configure.h"
)

# common source
set(
    COMMON_SOURCES
    "src/common/rqc_random.c"
    "src/common/rqc_str.c"
    "src/common/rqc_log.c"
    "src/common/rqc_log_event_callback.c"
    "src/common/rqc_time.c"
    "src/common/utils/vint/rqc_discrete_int_parser.c"
    "src/common/utils/vint/rqc_variable_len_int.c"
    "src/common/utils/ringarray/rqc_ring_array.c"
)

# congestion control
set(
    CONGESTION_CONTROL_SOURCES
    "src/congestion_control/rqc_bbr.c"
    "src/congestion_control/rqc_window_filter.c"
    "src/congestion_control/rqc_sample.c"
)

if(RQC_ENABLE_BBR2)
    set(
        CONGESTION_CONTROL_SOURCES
        ${CONGESTION_CONTROL_SOURCES}
        "src/congestion_control/rqc_bbr2.c"
    )
endif()

# transport source
set(
    TRANSPORT_SOURCES
    "src/transport/rqc_cid.c"
    "src/transport/rqc_defs.c"
    "src/transport/rqc_transport_params.c"
    "src/transport/rqc_frame_parser.c"
    "src/transport/rqc_frame.c"
    "src/transport/rqc_packet_in.c"
    "src/transport/rqc_packet_out.c"
    "src/transport/rqc_packet_parser.c"
    "src/transport/rqc_packet.c"
    "src/transport/rqc_pacing.c"
    "src/transport/rqc_send_ctl.c"
    "src/transport/rqc_send_queue.c"
    "src/transport/rqc_recv_record.c"
    "src/transport/rqc_multipath.c"
    "src/transport/rqc_stream.c"
    "src/transport/rqc_conn.c"
    "src/transport/rqc_engine.c"
    "src/transport/rqc_client.c"
    "src/transport/rqc_timer.c"
    "src/transport/rqc_utils.c"
)

# rquic source
set (
    RQC_SOURCE
    ${TRANSPORT_SOURCES}
    ${COMMON_SOURCES}
    ${CONGESTION_CONTROL_SOURCES}
)

# target
add_library(
    rquic
    STATIC
    ${RQC_SOURCE}
)

target_include_directories(rquic
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
)

# testing
if(RQC_ENABLE_TESTING)
    add_subdirectory(demo)
endif(RQC_ENABLE_TESTING)

#install

install(TARGETS rquic EXPORT rquicTargets)

install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
    EXPORT rquicTargets
    NAMESPACE rquic::
    FILE rquicTargets.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rquic
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Config.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/rquicConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rquic
)

install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/rquicConfig.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rquic
)
