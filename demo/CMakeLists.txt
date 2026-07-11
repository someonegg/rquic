if(CMAKE_SYSTEM_NAME MATCHES "Windows")
    message(STATUS "demo is a POSIX socket demo and is not built on Windows")
    return()
endif()

set(
    DEMO_SERVER_SOURCES
    "demo_common.c"
    "demo_event.c"
    "demo_hq.c"
    "demo_udp.c"
    "demo_server.c"
)

set(
    DEMO_CLIENT_SOURCES
    "demo_common.c"
    "demo_event.c"
    "demo_hq.c"
    "demo_udp.c"
    "demo_client.c"
)

include_directories(
    "${CMAKE_SOURCE_DIR}/"
    "${CMAKE_SOURCE_DIR}/include"
    ${CMAKE_SOURCE_DIR}
)

set(DEMO_DEPEND_LIBS
    rquic
    m
    dl
    stdc++
    pthread
)

add_executable(demo_server ${DEMO_SERVER_SOURCES})
add_executable(demo_client ${DEMO_CLIENT_SOURCES})

target_link_libraries(demo_server ${DEMO_DEPEND_LIBS})
target_link_libraries(demo_client ${DEMO_DEPEND_LIBS})
