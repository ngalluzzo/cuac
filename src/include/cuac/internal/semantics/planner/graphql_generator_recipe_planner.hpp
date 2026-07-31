#pragma once

#include "cuac/connector/compiled_protocol_operation.hpp"
#include "cuac/semantics/planned_graphql_generator_recipe.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace cuac_test {
class GraphqlGeneratorRecipePlannerTestAccess;
}

namespace cuac {
namespace scan_planner_internal {

struct PlannedGraphqlGeneratorRecipeResult {
	std::shared_ptr<const PlannedGraphqlGeneratorRecipe> recipe;
	std::string rendered_document;
};

// Relational Semantics' independent compiler-to-plan boundary for package
// GraphQL query generators. It copies every public compiled field into a
// distinct planned representation, validates that copy, and renders only from
// planned facts. It performs no I/O and never calls Connector's renderer.
class GraphqlGeneratorRecipePlanner final {
public:
	static PlannedGraphqlGeneratorRecipeResult Plan(const CompiledGraphqlQueryRecipe &source,
	                                                std::uint64_t max_rendered_bytes);

private:
	friend class cuac_test::GraphqlGeneratorRecipePlannerTestAccess;
	static std::shared_ptr<const PlannedGraphqlLiteral> CopyLiteral(const CompiledGraphqlLiteral &source,
	                                                                std::size_t depth, std::size_t &nodes);
};

} // namespace scan_planner_internal
} // namespace cuac
