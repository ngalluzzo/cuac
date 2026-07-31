# The protocol-neutral request is Query Experience's provider API to
# Relational Semantics and its own DuckDB adapter.
add_library(
  cuac_query_request_service STATIC
  ${QUERY_REQUEST_SOURCES})
configure_cuac_cpp_target(cuac_query_request_service)
target_link_libraries(
  cuac_query_request_service
  PUBLIC cuac_connector_metadata_service
         cuac_relational_predicate_service)

# Query's DuckDB value-boundary service consumes only the immutable plan and
# Runtime row interfaces. It owns strict nullability/kind enforcement and has
# no protocol decoder or provider construction dependency.
add_library(
  cuac_query_typed_value_adapter_service STATIC
  src/query/duckdb/adapter/typed_value_adapter.cpp)
configure_cuac_cpp_target(cuac_query_typed_value_adapter_service)
target_link_libraries(
  cuac_query_typed_value_adapter_service
  PUBLIC cuac_scan_plan_service
         cuac_runtime_interface_service
         duckdb_static)

# Query's credential provider/storage service owns the complete DuckDB-specific
# implementation behind Runtime's provider-neutral snapshot API. Focused
# consumers link this service and cannot compile or construct its private
# concrete secret, persistent codec, or descriptor-custody implementation.
add_library(
  cuac_query_credential_service STATIC
  ${QUERY_DUCKDB_SECRET_SOURCES})
configure_cuac_cpp_target(cuac_query_credential_service)
target_link_libraries(
  cuac_query_credential_service
  PUBLIC cuac_runtime_interface_service
         duckdb_static)

# Query's bounded package-catalog provider owns the actual DuckDB publication
# path. Consumers link this target; they do not list or include its private
# production sources. Runtime and Connector implementations remain behind the
# public QueryPackageStagingService port.
add_library(
  cuac_query_package_catalog_service STATIC
  ${QUERY_PACKAGE_CATALOG_SOURCES}
  ${QUERY_DUCKDB_ADAPTER_SUPPORT_SOURCES})
configure_cuac_cpp_target(cuac_query_package_catalog_service)
target_link_libraries(
  cuac_query_package_catalog_service
  PUBLIC cuac_query_request_service
         cuac_scan_plan_service
         cuac_runtime_interface_service
         duckdb_static
  PRIVATE cuac_query_credential_service)

# Lead product composition adapts bounded Connector, Semantics, and Runtime
# generation services to Query's staging port. It owns no DuckDB catalog or
# transport implementation and accepts only Runtime's ScanExecutor interface.
add_library(
  cuac_package_generation_composition_service STATIC
  ${QUERY_PACKAGE_GENERATION_COMPOSITION_SOURCES})
configure_cuac_cpp_target(cuac_package_generation_composition_service)
target_link_libraries(
  cuac_package_generation_composition_service
  PUBLIC cuac_query_request_service
         cuac_package_compiler_service
         cuac_package_reload_service
         cuac_package_bound_planning_service
         cuac_runtime_generation_service
         cuac_runtime_interface_service)
