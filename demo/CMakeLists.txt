find_package(LibEvent)
if(NOT LIBEVENT_FOUND)
    message(FATAL_ERROR "libevent with required version not found")
endif()

set(
    HQ_SOURCES
    "rqc_hq_ctx.c"
    "rqc_hq_conn.c"
    "rqc_hq_request.c"
)

set(
    DEMO_CLIENT_SOURCES
    ${HQ_SOURCES}
    "demo_client.c"
)

set(
    DEMO_SERVER_SOURCES
    ${HQ_SOURCES}
    "demo_server.c"
)

if(${CMAKE_SYSTEM_NAME} MATCHES "Windows")
    set(GETOPT_SOURCES
        getopt.c
    )

    set(DEMO_SERVER_SOURCES
        ${DEMO_SERVER_SOURCES}
        ${GETOPT_SOURCES}
    )

    set(DEMO_CLIENT_SOURCES
        ${DEMO_CLIENT_SOURCES}
        ${GETOPT_SOURCES}
    )
endif()

include_directories(
    "${CMAKE_SOURCE_DIR}/"
    "${CMAKE_SOURCE_DIR}/include"
    ${CMAKE_SOURCE_DIR}
    ${CUNIT_INCLUDE_DIRS}
    ${LIBEVENT_INCLUDE_DIR}
)

# link libraries
if(CMAKE_SYSTEM_NAME MATCHES "Windows")
    set(APP_DEPEND_LIBS
        rquic
        ${LIBEVENT_LIBRARY}
        -lm
    )
else()
    set(APP_DEPEND_LIBS
        rquic
        ${LIBEVENT_LIBRARY}
        m
        dl
        stdc++
        pthread
    )
endif()

add_executable(demo_server ${DEMO_SERVER_SOURCES})
add_executable(demo_client ${DEMO_CLIENT_SOURCES})

target_link_libraries(demo_server ${APP_DEPEND_LIBS})
target_link_libraries(demo_client ${APP_DEPEND_LIBS})
