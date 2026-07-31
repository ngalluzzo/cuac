# Same-v1 reload classification is an ecosystem policy, not connector
# authoring or runtime publication. Consumers receive one immutable decision.
add_library(
  cuac_package_reload_service STATIC
  ${ECOSYSTEM_PACKAGE_RELOAD_SOURCES})
configure_cuac_cpp_target(cuac_package_reload_service)
target_link_libraries(
  cuac_package_reload_service
  PUBLIC cuac_connector_metadata_service)
