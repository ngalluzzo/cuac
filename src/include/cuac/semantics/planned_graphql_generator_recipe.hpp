#pragma once

#include <memory>
#include <string>
#include <vector>

namespace cuac_test {
class ScanPlanTestAccess;
}

namespace cuac {

namespace scan_planner_internal {
class GraphqlGeneratorRecipePlanner;
}

enum class PlannedGraphqlGeneratorIdentity { PACKAGE_QUERY_GENERATOR_V1 };
enum class PlannedGraphqlLiteralKind { NULL_VALUE, BOOLEAN, INTEGER, STRING, ENUM_VALUE, LIST, OBJECT };
enum class PlannedGraphqlRecipeVariableType { INT_NON_NULL, STRING_NULLABLE };
enum class PlannedGraphqlRecipeVariableRole { PAGE_SIZE, CURSOR };

class PlannedGraphqlLiteral;

// One immutable ordered object member in a Semantics-owned GraphQL literal.
// Values are deep-copied from Connector's compiled recipe and share only
// immutable planned storage across prepared and concurrent execution.
class PlannedGraphqlObjectField {
public:
	PlannedGraphqlObjectField(const PlannedGraphqlObjectField &) = default;
	PlannedGraphqlObjectField(PlannedGraphqlObjectField &&) = default;
	PlannedGraphqlObjectField &operator=(const PlannedGraphqlObjectField &) = delete;
	PlannedGraphqlObjectField &operator=(PlannedGraphqlObjectField &&) = delete;

	const std::string &Name() const noexcept;
	const PlannedGraphqlLiteral &Value() const;

private:
	friend class scan_planner_internal::GraphqlGeneratorRecipePlanner;
	friend class cuac_test::ScanPlanTestAccess;
	PlannedGraphqlObjectField(std::string name, std::shared_ptr<const PlannedGraphqlLiteral> value);

	std::string name;
	std::shared_ptr<const PlannedGraphqlLiteral> value;
};

// Closed recursive literal authority for canonical package query rendering.
// Exactly one payload shape is active for each Kind. The value owns no source
// text, variable interpolation, credential, request, response, or Runtime state.
class PlannedGraphqlLiteral {
public:
	PlannedGraphqlLiteral(const PlannedGraphqlLiteral &) = default;
	PlannedGraphqlLiteral(PlannedGraphqlLiteral &&) = default;
	PlannedGraphqlLiteral &operator=(const PlannedGraphqlLiteral &) = delete;
	PlannedGraphqlLiteral &operator=(PlannedGraphqlLiteral &&) = delete;

	PlannedGraphqlLiteralKind Kind() const noexcept;
	const std::string &Scalar() const noexcept;
	const std::vector<std::shared_ptr<const PlannedGraphqlLiteral>> &Items() const noexcept;
	const std::vector<PlannedGraphqlObjectField> &Fields() const noexcept;

private:
	friend class scan_planner_internal::GraphqlGeneratorRecipePlanner;
	friend class cuac_test::ScanPlanTestAccess;
	PlannedGraphqlLiteral(PlannedGraphqlLiteralKind kind, std::string scalar,
	                      std::vector<std::shared_ptr<const PlannedGraphqlLiteral>> items,
	                      std::vector<PlannedGraphqlObjectField> fields);

	PlannedGraphqlLiteralKind kind;
	std::string scalar;
	std::vector<std::shared_ptr<const PlannedGraphqlLiteral>> items;
	std::vector<PlannedGraphqlObjectField> fields;
};

class PlannedGraphqlFixedArgument {
public:
	PlannedGraphqlFixedArgument(const PlannedGraphqlFixedArgument &) = default;
	PlannedGraphqlFixedArgument(PlannedGraphqlFixedArgument &&) = default;
	PlannedGraphqlFixedArgument &operator=(const PlannedGraphqlFixedArgument &) = delete;
	PlannedGraphqlFixedArgument &operator=(PlannedGraphqlFixedArgument &&) = delete;

	const std::string &Name() const noexcept;
	const PlannedGraphqlLiteral &Value() const;

private:
	friend class scan_planner_internal::GraphqlGeneratorRecipePlanner;
	friend class cuac_test::ScanPlanTestAccess;
	PlannedGraphqlFixedArgument(std::string name, std::shared_ptr<const PlannedGraphqlLiteral> value);

	std::string name;
	std::shared_ptr<const PlannedGraphqlLiteral> value;
};

class PlannedGraphqlRecipeVariable {
public:
	PlannedGraphqlRecipeVariable(const PlannedGraphqlRecipeVariable &) = default;
	PlannedGraphqlRecipeVariable(PlannedGraphqlRecipeVariable &&) = default;
	PlannedGraphqlRecipeVariable &operator=(const PlannedGraphqlRecipeVariable &) = delete;
	PlannedGraphqlRecipeVariable &operator=(PlannedGraphqlRecipeVariable &&) = delete;

	const std::string &Name() const noexcept;
	PlannedGraphqlRecipeVariableType Type() const noexcept;
	PlannedGraphqlRecipeVariableRole Role() const noexcept;
	const std::string &ArgumentName() const noexcept;

private:
	friend class scan_planner_internal::GraphqlGeneratorRecipePlanner;
	friend class cuac_test::ScanPlanTestAccess;
	PlannedGraphqlRecipeVariable(std::string name, PlannedGraphqlRecipeVariableType type,
	                             PlannedGraphqlRecipeVariableRole role, std::string argument_name);

	std::string name;
	PlannedGraphqlRecipeVariableType type;
	PlannedGraphqlRecipeVariableRole role;
	std::string argument_name;
};

class PlannedGraphqlSelection {
public:
	PlannedGraphqlSelection(const PlannedGraphqlSelection &) = default;
	PlannedGraphqlSelection(PlannedGraphqlSelection &&) = default;
	PlannedGraphqlSelection &operator=(const PlannedGraphqlSelection &) = delete;
	PlannedGraphqlSelection &operator=(PlannedGraphqlSelection &&) = delete;

	const std::string &ColumnName() const noexcept;
	const std::vector<std::string> &FieldPath() const noexcept;

private:
	friend class scan_planner_internal::GraphqlGeneratorRecipePlanner;
	friend class cuac_test::ScanPlanTestAccess;
	PlannedGraphqlSelection(std::string column_name, std::vector<std::string> field_path);

	std::string column_name;
	std::vector<std::string> field_path;
};

// Immutable Semantics-owned generator recipe. It is a field-by-field copy of
// Connector's closed package recipe, independently validated and rendered by
// Semantics before entering a ScanPlan. Runtime may trust only this planned
// value plus the correlated planned operation; Connector's type and renderer
// are not execution authority. The recipe is deterministic, thread-safe, and
// owns no mutable cursor, cancellation, close, credential, or network state.
class PlannedGraphqlGeneratorRecipe {
public:
	PlannedGraphqlGeneratorRecipe(const PlannedGraphqlGeneratorRecipe &) = default;
	PlannedGraphqlGeneratorRecipe(PlannedGraphqlGeneratorRecipe &&) = default;
	PlannedGraphqlGeneratorRecipe &operator=(const PlannedGraphqlGeneratorRecipe &) = delete;
	PlannedGraphqlGeneratorRecipe &operator=(PlannedGraphqlGeneratorRecipe &&) = delete;

	PlannedGraphqlGeneratorIdentity Identity() const noexcept;
	const std::string &OperationName() const noexcept;
	const std::vector<PlannedGraphqlRecipeVariable> &Variables() const noexcept;
	const std::vector<std::string> &RootPath() const noexcept;
	const std::vector<PlannedGraphqlFixedArgument> &FixedArguments() const noexcept;
	const std::string &NodesField() const noexcept;
	const std::vector<PlannedGraphqlSelection> &Selections() const noexcept;
	const std::string &PageInfoField() const noexcept;
	const std::string &HasNextPageField() const noexcept;
	const std::string &EndCursorField() const noexcept;

private:
	friend class scan_planner_internal::GraphqlGeneratorRecipePlanner;
	friend class cuac_test::ScanPlanTestAccess;
	PlannedGraphqlGeneratorRecipe(PlannedGraphqlGeneratorIdentity identity, std::string operation_name,
	                              std::vector<PlannedGraphqlRecipeVariable> variables,
	                              std::vector<std::string> root_path,
	                              std::vector<PlannedGraphqlFixedArgument> fixed_arguments, std::string nodes_field,
	                              std::vector<PlannedGraphqlSelection> selections, std::string page_info_field,
	                              std::string has_next_page_field, std::string end_cursor_field);

	PlannedGraphqlGeneratorIdentity identity;
	std::string operation_name;
	std::vector<PlannedGraphqlRecipeVariable> variables;
	std::vector<std::string> root_path;
	std::vector<PlannedGraphqlFixedArgument> fixed_arguments;
	std::string nodes_field;
	std::vector<PlannedGraphqlSelection> selections;
	std::string page_info_field;
	std::string has_next_page_field;
	std::string end_cursor_field;
};

} // namespace cuac
