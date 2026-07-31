file(READ "${CMAKE_CURRENT_LIST_DIR}/version.txt" CUAC_VERSION)
string(STRIP "${CUAC_VERSION}" CUAC_VERSION)

if(NOT CUAC_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "version.txt must contain one canonical SemVer core version")
endif()

duckdb_extension_load(cuac
  SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
  EXTENSION_VERSION "${CUAC_VERSION}")
