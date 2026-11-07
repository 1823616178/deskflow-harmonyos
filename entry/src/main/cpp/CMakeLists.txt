# the minimum version of CMake.
cmake_minimum_required(VERSION 3.16)
project(deskflow_ohospoc)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../../../cmake/MbedTLS.cmake)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

if(DEFINED PACKAGE_FIND_FILE)
    include(${PACKAGE_FIND_FILE})
endif()

include_directories(${NATIVERENDER_ROOT_PATH}
                    ${NATIVERENDER_ROOT_PATH}/include)

add_library(entry SHARED napi_init.cpp
            protocol/proto_util.cpp
            protocol/socket_stream.cpp
            protocol/tls_options.cpp
            protocol/tls_session.cpp
            protocol/deskflow_client.cpp)
set_target_properties(entry PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)
target_link_libraries(entry PUBLIC libace_napi.z.so libohinput.so libhilog_ndk.z.so)
target_link_libraries(entry PRIVATE mbedtls mbedx509 mbedcrypto)
target_compile_definitions(entry PRIVATE DF_WITH_MBEDTLS=1)

# Warn loudly, but do not fail the build on warnings yet: the pre-existing
# NAPI entry points have unused parameters by signature.
target_compile_options(entry PRIVATE -Wall -Wextra)
