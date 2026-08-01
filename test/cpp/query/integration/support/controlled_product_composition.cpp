#include "query/integration/support/controlled_product_composition.hpp"

#include "connector/support/catalog_test_access.hpp"
#include "connector/support/connector_catalog_test_fixtures.hpp"
#include "connector/support/package_compiler_test_fixtures.hpp"
#include "cuac/query/package_generation_composition.hpp"
#include "runtime/support/loopback_curl_runtime.hpp"

#include <utility>

namespace cuac_test {

ControlledProductComposition BuildControlledProductComposition(uint16_t port, bool predicate_mapping_available) {
	auto connector = predicate_mapping_available ? CompileRepositoryGithubConnectorFixture(CUAC_SOURCE_ROOT)
	                                             : ConnectorCatalogTestAccess::WithoutPredicateMappings(
	                                                   CompileRepositoryGithubConnectorFixture(CUAC_SOURCE_ROOT));
	auto runtime = BuildLoopbackCurlRuntime(port);
	auto executor = runtime->Executor();
	auto package_staging = cuac::BuildPackageGenerationComposition(executor);
	return {std::move(connector), std::move(executor), std::move(package_staging)};
}

} // namespace cuac_test
