# Copyright (c) 2022, Alibaba Group Holding Limited

cmake_minimum_required (VERSION 3.10)

project(rquic
    VERSION 1.0.0
    DESCRIPTION "Trimmed QUIC transport library"
    LANGUAGES C
)

include(CMakePackageConfigHelpers)
include(CheckCCompilerFlag)
include(GNUInstallDirs)

option (RQC_ENABLE_BBR2 "enable bbr2" OFF)
option (RQC_PROTECT_POOL_MEM "enable write protection for pool memory (for debug)" OFF)
option (RQC_ENABLE_DEMO "Enable Demo" OFF)
option (RQC_ENABLE_TESTING "Enable Testing" OFF)

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

if(MSVC)
    set(CMAKE_C_FLAGS_OPTION "/W1")
    add_compile_options("/source-charset:utf-8")
else()
    set(CMAKE_C_FLAGS_OPTION "-Wall -Werror -Wno-unused -Wno-pointer-sign")
    if(IOS)
        set(CMAKE_C_FLAGS_OPTION "${CMAKE_C_FLAGS_OPTION} -Wno-shorten-64-to-32")
    endif()
endif()

if (WIN32)
    add_definitions(-DWIN32_LEAN_AND_MEAN)
    add_definitions(-D_WIN32_WINNT=0x0600)
endif ()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -O2 -std=gnu11 ${CMAKE_C_FLAGS_OPTION}")

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

# demo and testing
if(RQC_ENABLE_TESTING)
    set(RQC_ENABLE_DEMO ON CACHE BOOL "Enable Demo" FORCE)
endif()

if(RQC_ENABLE_DEMO)
    add_subdirectory(demo)
endif()

if(RQC_ENABLE_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

#install

install(TARGETS rquic EXPORT RQUICTargets)

install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
    EXPORT RQUICTargets
    NAMESPACE RQUIC::
    FILE RQUICTargets.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RQUIC
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Config.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/RQUICConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RQUIC
)

install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/RQUICConfig.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RQUIC
)
