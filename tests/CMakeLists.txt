enable_language(CXX)

if(CMAKE_SYSTEM_NAME MATCHES "Windows")
    set(RQC_TESTS_POSIX OFF)
else()
    set(RQC_TESTS_POSIX ON)
endif()

include(FetchContent)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_Declare(
    googletest
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    URL https://github.com/google/googletest/releases/download/v1.15.0/googletest-1.15.0.tar.gz
)
FetchContent_MakeAvailable(googletest)

if(RQC_TESTS_POSIX)
    find_package(Threads REQUIRED)
endif()

add_executable(rqc_unit_tests
    rqc_unit_tests.c
    rqc_unit_tests.cc
)

target_compile_features(rqc_unit_tests PRIVATE cxx_std_17)

target_include_directories(rqc_unit_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
)

target_link_libraries(rqc_unit_tests
    PRIVATE
        rquic
        GTest::gtest_main
)

if(RQC_TESTS_POSIX)
    target_link_libraries(rqc_unit_tests
        PRIVATE
        m
        dl
        Threads::Threads
    )
elseif(WIN32)
    target_link_libraries(rqc_unit_tests PRIVATE ws2_32)
endif()

include(GoogleTest)
gtest_discover_tests(rqc_unit_tests)

if(TARGET demo_server AND TARGET demo_client)
    add_test(
        NAME rqc_demo_e2e
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/e2e_demo.sh"
                "$<TARGET_FILE:demo_server>"
                "$<TARGET_FILE:demo_client>"
    )
endif()
