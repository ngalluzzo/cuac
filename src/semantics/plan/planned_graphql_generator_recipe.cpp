#include "cuac/semantics/planned_graphql_generator_recipe.hpp"

#include <stdexcept>
#include <utility>

namespace cuac {

PlannedGraphqlObjectField::PlannedGraphqlObjectField(std::string name_p,
                                                     std::shared_ptr<const PlannedGraphqlLiteral> value_p)
    : name(std::move(name_p)), value(std::move(value_p)) {
	if (!value) {
		throw std::invalid_argument("planned GraphQL object field lacks a value");
	}
}

const std::string &PlannedGraphqlObjectField::Name() const noexcept {
	return name;
}

const PlannedGraphqlLiteral &PlannedGraphqlObjectField::Value() const {
	return *value;
}

PlannedGraphqlLiteral::PlannedGraphqlLiteral(PlannedGraphqlLiteralKind kind_p, std::string scalar_p,
                                             std::vector<std::shared_ptr<const PlannedGraphqlLiteral>> items_p,
                                             std::vector<PlannedGraphqlObjectField> fields_p)
    : kind(kind_p), scalar(std::move(scalar_p)), items(std::move(items_p)), fields(std::move(fields_p)) {
}

PlannedGraphqlLiteralKind PlannedGraphqlLiteral::Kind() const noexcept {
	return kind;
}

const std::string &PlannedGraphqlLiteral::Scalar() const noexcept {
	return scalar;
}

const std::vector<std::shared_ptr<const PlannedGraphqlLiteral>> &PlannedGraphqlLiteral::Items() const noexcept {
	return items;
}

const std::vector<PlannedGraphqlObjectField> &PlannedGraphqlLiteral::Fields() const noexcept {
	return fields;
}

PlannedGraphqlFixedArgument::PlannedGraphqlFixedArgument(std::string name_p,
                                                         std::shared_ptr<const PlannedGraphqlLiteral> value_p)
    : name(std::move(name_p)), value(std::move(value_p)) {
	if (!value) {
		throw std::invalid_argument("planned GraphQL fixed argument lacks a value");
	}
}

const std::string &PlannedGraphqlFixedArgument::Name() const noexcept {
	return name;
}

const PlannedGraphqlLiteral &PlannedGraphqlFixedArgument::Value() const {
	return *value;
}

PlannedGraphqlRecipeVariable::PlannedGraphqlRecipeVariable(std::string name_p, PlannedGraphqlRecipeVariableType type_p,
                                                           PlannedGraphqlRecipeVariableRole role_p,
                                                           std::string argument_name_p)
    : name(std::move(name_p)), type(type_p), role(role_p), argument_name(std::move(argument_name_p)) {
}

const std::string &PlannedGraphqlRecipeVariable::Name() const noexcept {
	return name;
}

PlannedGraphqlRecipeVariableType PlannedGraphqlRecipeVariable::Type() const noexcept {
	return type;
}

PlannedGraphqlRecipeVariableRole PlannedGraphqlRecipeVariable::Role() const noexcept {
	return role;
}

const std::string &PlannedGraphqlRecipeVariable::ArgumentName() const noexcept {
	return argument_name;
}

PlannedGraphqlSelection::PlannedGraphqlSelection(std::string column_name_p, std::vector<std::string> field_path_p)
    : column_name(std::move(column_name_p)), field_path(std::move(field_path_p)) {
}

const std::string &PlannedGraphqlSelection::ColumnName() const noexcept {
	return column_name;
}

const std::vector<std::string> &PlannedGraphqlSelection::FieldPath() const noexcept {
	return field_path;
}

PlannedGraphqlGeneratorRecipe::PlannedGraphqlGeneratorRecipe(
    PlannedGraphqlGeneratorIdentity identity_p, std::string operation_name_p,
    std::vector<PlannedGraphqlRecipeVariable> variables_p, std::vector<std::string> root_path_p,
    std::vector<PlannedGraphqlFixedArgument> fixed_arguments_p, std::string nodes_field_p,
    std::vector<PlannedGraphqlSelection> selections_p, std::string page_info_field_p, std::string has_next_page_field_p,
    std::string end_cursor_field_p)
    : identity(identity_p), operation_name(std::move(operation_name_p)), variables(std::move(variables_p)),
      root_path(std::move(root_path_p)), fixed_arguments(std::move(fixed_arguments_p)),
      nodes_field(std::move(nodes_field_p)), selections(std::move(selections_p)),
      page_info_field(std::move(page_info_field_p)), has_next_page_field(std::move(has_next_page_field_p)),
      end_cursor_field(std::move(end_cursor_field_p)) {
}

PlannedGraphqlGeneratorIdentity PlannedGraphqlGeneratorRecipe::Identity() const noexcept {
	return identity;
}

const std::string &PlannedGraphqlGeneratorRecipe::OperationName() const noexcept {
	return operation_name;
}

const std::vector<PlannedGraphqlRecipeVariable> &PlannedGraphqlGeneratorRecipe::Variables() const noexcept {
	return variables;
}

const std::vector<std::string> &PlannedGraphqlGeneratorRecipe::RootPath() const noexcept {
	return root_path;
}

const std::vector<PlannedGraphqlFixedArgument> &PlannedGraphqlGeneratorRecipe::FixedArguments() const noexcept {
	return fixed_arguments;
}

const std::string &PlannedGraphqlGeneratorRecipe::NodesField() const noexcept {
	return nodes_field;
}

const std::vector<PlannedGraphqlSelection> &PlannedGraphqlGeneratorRecipe::Selections() const noexcept {
	return selections;
}

const std::string &PlannedGraphqlGeneratorRecipe::PageInfoField() const noexcept {
	return page_info_field;
}

const std::string &PlannedGraphqlGeneratorRecipe::HasNextPageField() const noexcept {
	return has_next_page_field;
}

const std::string &PlannedGraphqlGeneratorRecipe::EndCursorField() const noexcept {
	return end_cursor_field;
}

} // namespace cuac
