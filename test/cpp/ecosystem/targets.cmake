add_executable(
  cuac_package_reload_tests
  test/cpp/ecosystem/reload/package_compatibility_tests.cpp)
configure_cuac_cpp_target(cuac_package_reload_tests)
target_include_directories(
  cuac_package_reload_tests
  PRIVATE test/cpp)
target_link_libraries(
  cuac_package_reload_tests
  PRIVATE cuac_package_generation_fixture_service
          cuac_package_reload_service)
