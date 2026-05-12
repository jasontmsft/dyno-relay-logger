# Copyright (c) Microsoft
#
# Licensed under the Apache License, Version 2.0

cmake_minimum_required(VERSION 3.16)
project(DynoRelayLogger VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_POSITION_INDEPENDENT_CODE True)
set(CMAKE_VERBOSE_MAKEFILE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─── Dependencies ───
find_package(Threads REQUIRED)

# glog - static build from local third_party
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(WITH_GFLAGS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/glog EXCLUDE_FROM_ALL)

# gflags - static build from local third_party
set(GFLAGS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/gflags EXCLUDE_FROM_ALL)

# nlohmann_json
set(JSON_BuildTests OFF CACHE INTERNAL "")
add_subdirectory(third_party/json EXCLUDE_FROM_ALL)

# fmt - static build from local third_party
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/fmt EXCLUDE_FROM_ALL)

# curl (for Azure metadata) - static build from local third_party
# Disable all optional features not needed for Azure IMDS metadata fetch
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_TESTS ON CACHE BOOL "" FORCE)
set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)
set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/curl EXCLUDE_FROM_ALL)

# Create CURL::libcurl alias so Azure SDK's find_package(CURL) path works
if(NOT TARGET CURL::libcurl)
  add_library(CURL::libcurl ALIAS libcurl_static)
endif()
set(CURL_FOUND TRUE CACHE BOOL "" FORCE)
set(CURL_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/curl/include" CACHE STRING "" FORCE)
# FindCURL.cmake checks these singular-form variables via find_path/find_library;
# pre-setting them prevents the system search that fails without libcurl-dev.
set(CURL_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/curl/include" CACHE PATH "" FORCE)
set(CURL_LIBRARY "${CMAKE_CURRENT_BINARY_DIR}/third_party/curl/lib/libcurl${CMAKE_STATIC_LIBRARY_SUFFIX}" CACHE FILEPATH "" FORCE)

# OpenSSL (needed by Azure SDK TLS)
find_package(OpenSSL REQUIRED)

# Azure SDK for C++ (for AeHubs sink — Event Hubs via native Azure SDK)
set(ENV{AZURE_SDK_DISABLE_AUTO_VCPKG} "1")
set(AZ_ALL_LIBRARIES ON CACHE BOOL "" FORCE)
set(BUILD_TRANSPORT_CURL ON CACHE BOOL "" FORCE)
set(BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
list(APPEND CMAKE_MODULE_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/azure-sdk-for-cpp/cmake-modules")
add_subdirectory(third_party/azure-sdk-for-cpp/sdk/core/azure-core EXCLUDE_FROM_ALL)
add_subdirectory(third_party/azure-sdk-for-cpp/sdk/core/azure-core-amqp EXCLUDE_FROM_ALL)
add_subdirectory(third_party/azure-sdk-for-cpp/sdk/eventhubs/azure-messaging-eventhubs EXCLUDE_FROM_ALL)
add_subdirectory(third_party/azure-sdk-for-cpp/sdk/identity/azure-identity EXCLUDE_FROM_ALL)
add_subdirectory(third_party/azure-sdk-for-cpp/sdk/keyvault/azure-security-keyvault-secrets EXCLUDE_FROM_ALL)

# ─── DynoRelayLogger library ───
add_library(dynorelaylogger_lib
    src/AeHubsClient.cpp
    src/AeHubsClient.h
    src/GatherSystemInfo.cpp
    src/GatherSystemInfo.h
    src/Heartbeat.cpp
    src/Heartbeat.h
    src/MdsdClient.cpp
    src/MdsdClient.h
    src/MetricSink.cpp
    src/MetricSink.h
    src/DynoRelayServer.cpp
    src/DynoRelayServer.h
    src/StatsCollector.cpp
    src/StatsCollector.h
    src/rpc/SimpleJsonServer.cpp
    src/rpc/SimpleJsonServer.h
)

target_include_directories(dynorelaylogger_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(UUID REQUIRED uuid)

target_link_libraries(dynorelaylogger_lib PUBLIC
    glog::glog
    gflags::gflags
    nlohmann_json::nlohmann_json
    fmt::fmt
    Azure::azure-messaging-eventhubs
    Azure::azure-identity
    Azure::azure-security-keyvault-secrets
    CURL::libcurl_static
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    ${UUID_LIBRARIES}
)

# ─── DynoRelayLogger executable ───
add_executable(dynorelaylogger src/main.cpp)
target_link_libraries(dynorelaylogger PRIVATE dynorelaylogger_lib)
# System shared OpenSSL must come last to resolve symbols from static libs
target_link_libraries(dynorelaylogger PRIVATE ssl crypto)

# ─── DynoRelayLoggerInfo executable (JSON RPC client) ───
add_executable(dynorelayloggerinfo src/dynorelayloggerinfo.cpp)
target_link_libraries(dynorelayloggerinfo PRIVATE nlohmann_json::nlohmann_json Threads::Threads)

# ─── Example: CPU logger ───
add_executable(cpu_logger examples/cpu_logger.cpp)
target_link_libraries(cpu_logger PRIVATE gflags::gflags nlohmann_json::nlohmann_json Threads::Threads)
