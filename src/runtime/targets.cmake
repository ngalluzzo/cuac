# Runtime publishes narrow link targets so consumers take only the service
# layer they exercise. None of these targets imports provider-private sources.
add_library(
  cuac_runtime_interface_service STATIC
  ${REMOTE_RUNTIME_INTERFACE_SOURCES})
configure_cuac_cpp_target(cuac_runtime_interface_service)

# Runtime's immutable generation registry is a bounded lifecycle service. It
# consumes Connector's opaque local-package custody and Package Ecosystem's
# reload decision, but has no source acquisition, YAML, compiler, Query,
# catalog, or DuckDB dependency.
add_library(
  cuac_runtime_generation_service STATIC
  ${REMOTE_RUNTIME_GENERATION_SOURCES})
configure_cuac_cpp_target(cuac_runtime_generation_service)
target_link_libraries(
  cuac_runtime_generation_service
  PUBLIC cuac_runtime_interface_service
         cuac_local_package_custody_service
         cuac_package_reload_service)

# Runtime's reactive resilience foundation is independently testable and owns
# no protocol, transport, credential, ScanPlan, Query, or DuckDB dependency.
add_library(
  cuac_runtime_resilience_service STATIC
  ${REMOTE_RUNTIME_RESILIENCE_SOURCES})
configure_cuac_cpp_target(cuac_runtime_resilience_service)
target_link_libraries(
  cuac_runtime_resilience_service
  PRIVATE Threads::Threads)

# Runtime's executor-local admission provider owns only structural identities,
# bounded counters, queues, and move-only release authority. It consumes the
# injected steady-clock interface from resilience but exposes no retry, cache,
# protocol, transport, credential, Query, or DuckDB implementation.
add_library(
  cuac_runtime_admission_service STATIC
  ${REMOTE_RUNTIME_ADMISSION_SOURCES})
configure_cuac_cpp_target(cuac_runtime_admission_service)
target_link_libraries(
  cuac_runtime_admission_service
  PRIVATE cuac_runtime_resilience_service
          Threads::Threads)

# Complete-result caching is a separate Runtime capability. It consumes the
# immutable plan/cache policy, stream interface, and non-queuing cache-resident
# admission authority, but it does not own request admission, transport,
# credentials, or protocol execution.
add_library(
  cuac_runtime_cache_service STATIC
  ${REMOTE_RUNTIME_CACHE_SOURCES})
configure_cuac_cpp_target(cuac_runtime_cache_service)
target_link_libraries(
  cuac_runtime_cache_service
  PUBLIC cuac_runtime_interface_service
         cuac_scan_plan_service
  PRIVATE cuac_runtime_admission_service)

add_library(
  cuac_runtime_executor_service STATIC
  ${REMOTE_RUNTIME_EXECUTOR_SOURCES})
configure_cuac_cpp_target(cuac_runtime_executor_service)
target_link_libraries(
  cuac_runtime_executor_service
  PUBLIC cuac_runtime_interface_service
         cuac_scan_plan_service
         cuac_content_digest_service
  PRIVATE cuac_runtime_admission_service
          cuac_runtime_cache_service
          cuac_runtime_resilience_service)
