# Query and Semantics share only the closed protocol-neutral predicate value.
# Keeping it below Query request construction avoids a Query/Semantics target
# cycle while leaving interpretation owned by Semantics.
add_library(
  cuac_relational_predicate_service STATIC
  ${RELATIONAL_PREDICATE_SOURCES})
configure_cuac_cpp_target(cuac_relational_predicate_service)

# The immutable plan is the narrow X-as-a-Service contract consumed by Runtime.
add_library(
  cuac_scan_plan_service STATIC
  ${RELATIONAL_PLAN_VALUE_SOURCES})
configure_cuac_cpp_target(cuac_scan_plan_service)

# Planner construction consumes bounded Connector and Query services. Query
# and Semantics fixtures use this service; Runtime does not.
add_library(
  cuac_relational_planning_service STATIC
  ${RELATIONAL_PLANNER_SOURCES})
configure_cuac_cpp_target(cuac_relational_planning_service)
target_link_libraries(
  cuac_relational_planning_service
  PUBLIC cuac_scan_plan_service
         cuac_relational_predicate_service
         cuac_connector_metadata_service
         cuac_query_request_service)

# Lead composition consumes this generation-bound Semantics provider and
# adapts it to its Query-facing interface. The service owns no catalog,
# publication, Connector-construction, or Query-adapter responsibility.
add_library(
  cuac_package_bound_planning_service STATIC
  ${RELATIONAL_PACKAGE_BOUND_PLANNER_SOURCES})
configure_cuac_cpp_target(cuac_package_bound_planning_service)
target_link_libraries(
  cuac_package_bound_planning_service
  PUBLIC cuac_relational_planning_service)
