cmake_minimum_required(VERSION 3.19)

set(TARGET_NAME cuac)
set(EXTENSION_NAME ${TARGET_NAME}_extension)

project(${TARGET_NAME})

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" CUAC_VERSION)
string(STRIP "${CUAC_VERSION}" CUAC_VERSION)
if(NOT CUAC_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "version.txt must contain one canonical SemVer core version")
endif()

include(cmake/CuacProductSources.cmake)

option(CUAC_PORTABLE_BUILD
       "Build in CUAC's verified Linux development container" OFF)
if(APPLE AND NOT CUAC_PORTABLE_BUILD)
  find_program(CUAC_XCRUN xcrun REQUIRED)
  execute_process(
    COMMAND "${CUAC_XCRUN}" --sdk macosx --show-sdk-path
    RESULT_VARIABLE CUAC_XCRUN_RESULT
    OUTPUT_VARIABLE CUAC_XCRUN_SDK_ROOT
    ERROR_VARIABLE CUAC_XCRUN_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT CUAC_XCRUN_RESULT EQUAL 0 OR CUAC_XCRUN_SDK_ROOT STREQUAL "")
    message(FATAL_ERROR "xcrun could not derive the macOS SDK: ${CUAC_XCRUN_ERROR}")
  endif()
  get_filename_component(CUAC_SDK_ROOT "${CUAC_XCRUN_SDK_ROOT}" REALPATH)
  if(DEFINED CUAC_VERIFIED_SDK_ROOT)
    get_filename_component(
      CUAC_CALLER_SDK_ROOT "${CUAC_VERIFIED_SDK_ROOT}" REALPATH)
    if(NOT CUAC_CALLER_SDK_ROOT STREQUAL CUAC_SDK_ROOT)
      message(FATAL_ERROR
              "verified SDK does not match xcrun: ${CUAC_CALLER_SDK_ROOT} != ${CUAC_SDK_ROOT}")
    endif()
  endif()

  # Native macOS release evidence consumes only verified SDK headers and its
  # text stub. An ambient package-manager curl must never replace them.
  set(CURL_NO_CURL_CMAKE ON CACHE BOOL "Disable ambient curl config packages" FORCE)
  set(CURL_INCLUDE_DIR "${CUAC_SDK_ROOT}/usr/include" CACHE PATH
      "Verified macOS SDK curl include directory" FORCE)
  set(CURL_LIBRARY "${CUAC_SDK_ROOT}/usr/lib/libcurl.4.tbd" CACHE FILEPATH
      "Verified macOS SDK curl text stub" FORCE)
  set(CUAC_REQUIRED_CURL_VERSION "8.7.1")
  set(CUAC_REQUIRED_CURL_VERSION_NUM "0x080701")
  set(CUAC_REQUIRED_CURL_SSL_BACKEND "(SecureTransport) LibreSSL/3.3.6")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CUAC_PORTABLE_BUILD)
  if(NOT "$ENV{CUAC_DEV_CONTAINER}" STREQUAL "1")
    message(FATAL_ERROR
            "CUAC_PORTABLE_BUILD is valid only in the verified development container")
  endif()
  set(CUAC_SDK_ROOT "")
  set(CURL_NO_CURL_CMAKE ON CACHE BOOL "Disable ambient curl config packages" FORCE)
  set(CUAC_REQUIRED_CURL_VERSION "7.88.1")
  set(CUAC_REQUIRED_CURL_VERSION_NUM "0x075801")
  set(CUAC_REQUIRED_CURL_SSL_BACKEND "OpenSSL/3.0.20")
else()
  message(FATAL_ERROR
          "cuac ${CUAC_VERSION} requires the verified Linux development container or an explicit native macOS release build")
endif()

find_package(CURL ${CUAC_REQUIRED_CURL_VERSION} EXACT REQUIRED)
if(DEFINED CURL_VERSION)
  set(CUAC_CURL_VERSION "${CURL_VERSION}")
else()
  set(CUAC_CURL_VERSION "${CURL_VERSION_STRING}")
endif()
if(NOT CUAC_CURL_VERSION STREQUAL CUAC_REQUIRED_CURL_VERSION)
  message(FATAL_ERROR "configured libcurl version drifted: ${CUAC_CURL_VERSION}")
endif()
if(APPLE AND NOT CUAC_PORTABLE_BUILD)
  get_target_property(
    CUAC_CURL_LOCATION CURL::libcurl IMPORTED_LOCATION)
  if(NOT CUAC_CURL_LOCATION STREQUAL CURL_LIBRARY)
    message(FATAL_ERROR
            "CURL::libcurl resolved outside the verified SDK stub: ${CUAC_CURL_LOCATION}")
  endif()
else()
  set(CUAC_CURL_LOCATION "${CURL_LIBRARIES}")
endif()
get_target_property(
  CUAC_CURL_INCLUDE_DIRS CURL::libcurl INTERFACE_INCLUDE_DIRECTORIES)
list(LENGTH CUAC_CURL_INCLUDE_DIRS CUAC_CURL_INCLUDE_COUNT)
if(APPLE AND NOT CUAC_PORTABLE_BUILD AND
   (NOT CUAC_CURL_INCLUDE_COUNT EQUAL 1 OR
    NOT CUAC_CURL_INCLUDE_DIRS STREQUAL CURL_INCLUDE_DIR))
  message(FATAL_ERROR
          "CURL::libcurl includes resolved outside the verified SDK: ${CUAC_CURL_INCLUDE_DIRS}")
endif()
if(CUAC_PORTABLE_BUILD)
  set(CUAC_DEPENDENCY_POLICY "container")
else()
  set(CUAC_DEPENDENCY_POLICY "macos-sdk")
endif()
file(
  WRITE "${CMAKE_CURRENT_BINARY_DIR}/cuac_native_dependencies.json"
  "{\n"
  "  \"curl_include_dir\": \"${CUAC_CURL_INCLUDE_DIRS}\",\n"
  "  \"curl_library\": \"${CUAC_CURL_LOCATION}\",\n"
  "  \"curl_no_curl_cmake\": true,\n"
  "  \"curl_target\": \"CURL::libcurl\",\n"
  "  \"curl_version\": \"${CUAC_CURL_VERSION}\",\n"
  "  \"dependency_policy\": \"${CUAC_DEPENDENCY_POLICY}\",\n"
  "  \"sdk_root\": \"${CUAC_SDK_ROOT}\"\n"
  "}\n")

find_package(Threads REQUIRED)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/cuac/internal/runtime/build")
configure_file(
  cmake/CuacDependencyIdentity.hpp.in
  "${CMAKE_CURRENT_BINARY_DIR}/generated/cuac/internal/runtime/build/dependency_identity.hpp"
  @ONLY)

include_directories(src/include "${CMAKE_CURRENT_BINARY_DIR}/generated")

# Each topology team owns its production and focused-test inventory. The root
# build composes those bounded inventories into product and integration targets.
include(src/connector/sources.cmake)
include(src/ecosystem/sources.cmake)
include(src/query/sources.cmake)
include(src/semantics/sources.cmake)
include(src/runtime/sources.cmake)
include(test/cpp/connector/sources.cmake)
include(test/cpp/query/sources.cmake)
include(test/cpp/semantics/sources.cmake)
include(test/cpp/runtime/sources.cmake)
set(PRODUCTION_SOURCES
    ${CONNECTOR_METADATA_SOURCES}
    ${CONNECTOR_LOCAL_PACKAGE_CUSTODY_SOURCES}
    ${CONNECTOR_PACKAGE_YAML_SOURCES}
    ${CONNECTOR_PACKAGE_SOURCE_SOURCES}
    ${CONNECTOR_PACKAGE_COMPILER_SOURCES}
    ${CONNECTOR_PACKAGE_FIXTURE_SOURCES}
    ${ECOSYSTEM_PACKAGE_RELOAD_SOURCES}
    ${QUERY_REQUEST_SOURCES}
    ${RELATIONAL_PLANNING_SOURCES}
    ${REMOTE_RUNTIME_INTERFACE_SOURCES}
    ${REMOTE_RUNTIME_GENERATION_SOURCES}
    ${REMOTE_RUNTIME_SOURCES}
    ${QUERY_PRODUCT_COMPOSITION_SOURCES}
    ${QUERY_DUCKDB_SECRET_SOURCES}
    ${QUERY_DUCKDB_ADAPTER_SOURCES})
set(CONTROLLED_PRODUCT_SOURCES
    ${CONNECTOR_METADATA_SOURCES}
    ${CONNECTOR_LOCAL_PACKAGE_CUSTODY_SOURCES}
    ${CONNECTOR_PACKAGE_YAML_SOURCES}
    ${CONNECTOR_PACKAGE_SOURCE_SOURCES}
    ${CONNECTOR_PACKAGE_COMPILER_SOURCES}
    ${CONNECTOR_PACKAGE_FIXTURE_SOURCES}
    ${ECOSYSTEM_PACKAGE_RELOAD_SOURCES}
    ${CONNECTOR_TEST_SERVICE_SOURCES}
    ${CONNECTOR_PACKAGE_COMPILER_TEST_SERVICE_SOURCES}
    ${QUERY_REQUEST_SOURCES}
    ${RELATIONAL_PLANNING_SOURCES}
    ${REMOTE_RUNTIME_INTERFACE_SOURCES}
    ${REMOTE_RUNTIME_GENERATION_SOURCES}
    ${REMOTE_RUNTIME_SOURCES}
    ${QUERY_PACKAGE_GENERATION_COMPOSITION_SOURCES}
    ${QUERY_DUCKDB_SECRET_SOURCES}
    ${QUERY_PACKAGE_CATALOG_SOURCES}
    test/cpp/query/support/controlled_table_function_adapter.cpp
    ${REMOTE_RUNTIME_LOOPBACK_PRODUCT_SOURCES}
    ${QUERY_CONTROLLED_COMPOSITION_SOURCES})

build_static_extension(${TARGET_NAME} ${PRODUCTION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${PRODUCTION_SOURCES})
# This private artifact is a deterministic product oracle, not an installable
# extension. Its entry/composition replace the installed entry/composition and
# are intentionally omitted from every install rule.
set(DUCKDB_EXTENSION_CUAC_CONTROLLED_EXT_VERSION "${CUAC_VERSION}")
build_loadable_extension(cuac_controlled " " ${CONTROLLED_PRODUCT_SOURCES})

function(configure_cuac_cpp_target target)
  set_target_properties(
    ${target}
    PROPERTIES CXX_STANDARD 11
               CXX_STANDARD_REQUIRED YES
               CXX_EXTENSIONS NO)
  # DuckDB 1.5.4 headers intentionally contain unused default parameters.
  # Keep project warnings fatal while exempting that upstream header pattern.
  target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
endfunction()

add_executable(
  cuac_native_dependency_identity
  test/cpp/native_dependency_identity.cpp)
configure_cuac_cpp_target(cuac_native_dependency_identity)
target_link_libraries(
  cuac_native_dependency_identity
  CURL::libcurl)

foreach(target ${TARGET_NAME}_extension ${TARGET_NAME}_loadable_extension)
  configure_cuac_cpp_target(${target})
  target_link_libraries(${target} CURL::libcurl Threads::Threads)
endforeach()

configure_cuac_cpp_target(cuac_controlled_loadable_extension)
target_compile_definitions(
  cuac_controlled_loadable_extension
  PRIVATE CUAC_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
  cuac_controlled_loadable_extension
  PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/private")
target_include_directories(cuac_controlled_loadable_extension PRIVATE test/cpp)
target_link_libraries(
  cuac_controlled_loadable_extension
  CURL::libcurl
  Threads::Threads)

# Package-owned targets make source and test dependencies reviewable without a
# shared CMake editing hotspot. Provider services precede their consumers.
include(src/connector/targets.cmake)
include(src/ecosystem/targets.cmake)
include(src/query/targets.cmake)
include(src/semantics/targets.cmake)
include(src/runtime/targets.cmake)
include(test/cpp/connector/targets.cmake)
include(test/cpp/ecosystem/targets.cmake)
include(test/cpp/semantics/targets.cmake)
include(test/cpp/runtime/targets.cmake)
include(test/cpp/query/targets.cmake)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

# Keep this as the literal final command. CMake executes deferred calls in
# scheduling order, so the product-graph observer must follow every possible
# deferred target mutation in this directory.
cuac_defer_product_source_record(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/cuac_product_sources.json"
  PUBLIC_STATIC "${TARGET_NAME}_extension"
  PUBLIC_LOADABLE "${TARGET_NAME}_loadable_extension"
  CONTROLLED "cuac_controlled_loadable_extension")
