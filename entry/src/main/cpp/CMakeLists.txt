# the minimum version of CMake.
cmake_minimum_required(VERSION 3.5.0)
project(deskflow_ohospoc)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

if(DEFINED PACKAGE_FIND_FILE)
    include(${PACKAGE_FIND_FILE})
endif()

include_directories(${NATIVERENDER_ROOT_PATH}
                    ${NATIVERENDER_ROOT_PATH}/include)

add_library(entry SHARED napi_init.cpp
            protocol/proto_util.cpp
            protocol/socket_stream.cpp
            protocol/deskflow_client.cpp)
set_target_properties(entry PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)
target_link_libraries(entry PUBLIC libace_napi.z.so libohinput.so libhilog_ndk.z.so)

# Warn loudly, but do not fail the build on warnings yet: the pre-existing
# NAPI entry points have unused parameters by signature.
target_compile_options(entry PRIVATE -Wall -Wextra)
