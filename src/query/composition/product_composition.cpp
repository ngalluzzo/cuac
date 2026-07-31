#include "cuac/query/product_composition.hpp"

#include "cuac/runtime/http_runtime.hpp"
#include "cuac/query/package_generation_composition.hpp"

#include <utility>

namespace cuac {

ProductComposition BuildProductComposition() {
	auto runtime = InitializeHttpRuntime();
	auto package_staging = BuildPackageGenerationComposition(runtime.executor);
	return {std::move(runtime.executor), std::move(package_staging)};
}

} // namespace cuac
