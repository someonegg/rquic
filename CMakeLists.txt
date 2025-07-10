# Copyright (c) 2022, Alibaba Group Holding Limited

cmake_minimum_required (VERSION 3.10)
project (xquic)

set (xquic_VERSION_MAJOR 0)
set (xquic_VERSION_MINOR 1)
set (CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option (XQC_ENABLE_BBR2 "enable bbr2" OFF)
option (XQC_PROTECT_POOL_MEM "enable write protection for pool memory (for debug)" OFF)
option (XQC_ENABLE_TESTING "Enable Testing" OFF)
option (GCOV "Test Coverage" OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()


# Users can use both SSL_DIR or SSL_PATH to specify the ssl path to find the ssl
# module, if both are defined, SSL_DIR will be used.
if(NOT SSL_DIR)
    if(NOT SSL_PATH)
        # at least SSL_DIR or SSL_PATH shall be set
        message(FATAL_ERROR "SSL_PATH not set")
    else()
        set(SSL_DIR ${SSL_PATH})
    endif()
endif()

find_package(SSL)
if(NOT SSL_FOUND)
    message(FATAL_ERROR "ssl module not found")
endif(NOT SSL_FOUND)

if(SSL_DYNAMIC)
    message(NOTICE "-- Building with dynamic libssl")
    set(SSL_LIB_PATH ${SSL_LIBRARIES})
else()
    message(NOTICE "-- Building with static libssl")
    set(SSL_LIB_PATH ${SSL_LIBRARIES_STATIC})
endif()
set(SSL_INC_PATH ${SSL_INCLUDE_DIR})

MESSAGE("-- SSL_PATH: ${SSL_PATH}")
MESSAGE("-- SSL_LIB_PATH: ${SSL_LIB_PATH}")
MESSAGE("-- SSL_INC_PATH: ${SSL_INC_PATH}")


# C_FLAGS
if(PLATFORM STREQUAL "mac32")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -m32")
endif()

if(NOT (CMAKE_C_COMPILER_ID MATCHES "MSVC" OR CMAKE_CXX_COMPILER_ID MATCHES "MSVC"))
    set(CMAKE_C_FLAGS_OPTION "-Werror -Wno-unused -Wno-pointer-sign -Wno-format-security ")
endif()

# C_FLAGS
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -O2 -std=gnu11 -Wall ${CMAKE_C_FLAGS_OPTION} -DNDEBUG_PRINT -DNPRINT_MALLOC ")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS} -g -O0 -std=gnu11 -Wall ${CMAKE_C_FLAGS_OPTION} -DNDEBUG_PRINT -DNPRINT_MALLOC -DXQC_DEBUG ")

if(ASAN)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -w -fsanitize=address -fsanitize-address-use-after-scope -fno-omit-frame-pointer")
endif()


if(CMAKE_SYSTEM_NAME MATCHES "Windows")
    add_definitions(-DXQC_SYS_WINDOWS=1)
    set(CMAKE_C_FLAGS_OPTION " ")
endif()


# configure file
configure_file (
    "${CMAKE_CURRENT_SOURCE_DIR}/xqc_configure.h.in"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/xquic/xqc_configure.h"
)

include_directories(include
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${SSL_INC_PATH}
    ${CMAKE_SOURCE_DIR}
)


# print tls traffic secret in keylog
if(XQC_PRINT_SECRET)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DXQC_PRINT_SECRET")
endif()

# compat with the stateless reset before version v1.6.0
if(XQC_COMPAT_GENERATE_SR_PKT)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DXQC_COMPAT_GENERATE_SR_PKT")
endif()

# print only error log
if(XQC_ONLY_ERROR_LOG)
    set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -DXQC_ONLY_ERROR_LOG ")
endif()

# enable event log
if(XQC_ENABLE_EVENT_LOG)
    set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -DXQC_ENABLE_EVENT_LOG ")
endif()

# sendmmsg
if(XQC_SUPPORT_SENDMMSG_BUILD)
    add_definitions(-DXQC_SUPPORT_SENDMMSG)
endif()

# coverity
if(GCOV STREQUAL "on")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-arcs -ftest-coverage")
endif()


# Transport source
set(
    TRANSPORT_SOURCES
    "src/transport/xqc_engine.c"
    "src/transport/xqc_conn.c"
    "src/transport/xqc_client.c"
    "src/transport/xqc_cid.c"
    "src/transport/xqc_packet_parser.c"
    "src/transport/xqc_frame_parser.c"
    "src/transport/xqc_stream.c"
    "src/transport/xqc_packet_out.c"
    "src/transport/xqc_packet_in.c"
    "src/transport/xqc_send_ctl.c"
    "src/transport/xqc_send_queue.c"
    "src/transport/xqc_packet.c"
    "src/transport/xqc_frame.c"
    "src/transport/xqc_recv_record.c"
    "src/transport/xqc_pacing.c"
    "src/transport/xqc_utils.c"
    "src/transport/xqc_multipath.c"
    "src/transport/xqc_defs.c"
    "src/transport/xqc_transport_params.c"
    "src/transport/xqc_timer.c"
)


# TLS source
set (
    TLS_SOURCE
    "src/tls/xqc_hkdf.c"
    "src/tls/xqc_null_crypto.c"
    "src/tls/xqc_crypto.c"
    "src/tls/xqc_tls_ctx.c"
    "src/tls/xqc_tls.c"
    "src/tls/boringssl/xqc_hkdf_impl.c"
    "src/tls/boringssl/xqc_crypto_impl.c"
    "src/tls/boringssl/xqc_ssl_if_impl.c"
)

# common source
set(
    COMMON_SOURCES
    "src/common/xqc_random.c"
    "src/common/xqc_str.c"
    "src/common/xqc_log.c"
    "src/common/xqc_log_event_callback.c"
    "src/common/xqc_time.c"
    "src/common/utils/vint/xqc_discrete_int_parser.c"
    "src/common/utils/vint/xqc_variable_len_int.c"
    "src/common/utils/ringarray/xqc_ring_array.c"
)

# congestion control
set(
    CONGESTION_CONTROL_SOURCES
    "src/congestion_control/xqc_bbr.c"
    "src/congestion_control/xqc_window_filter.c"
    "src/congestion_control/xqc_sample.c"
)

if(XQC_ENABLE_BBR2)
    set(
        CONGESTION_CONTROL_SOURCES
        ${CONGESTION_CONTROL_SOURCES}
        "src/congestion_control/xqc_bbr2.c"
    )
endif()


# xquic source
set (
    XQC_SOURCE
    ${XQC_SOURCE}
    ${TRANSPORT_SOURCES}
    ${TLS_SOURCE}
    ${COMMON_SOURCES}
    ${CONGESTION_CONTROL_SOURCES}
)


# target
add_library(
    xquic
    STATIC
    ${XQC_SOURCE}
)


# testing
if(XQC_ENABLE_TESTING)
    add_subdirectory(demo)
endif(XQC_ENABLE_TESTING)
