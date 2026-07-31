#include "cuac/internal/connector/model/graphql_query_recipe.hpp"

#include "cuac/internal/connector/model/compiled_model_builder.hpp"

#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace cuac {

namespace {

bool IsName(const std::string &value) {
	if (value.empty() || value.size() > 255 || (value.size() >= 2 && value[0] == '_' && value[1] == '_')) {
		return false;
	}
	const auto first = value[0];
	if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_')) {
		return false;
	}
	for (std::size_t index = 1; index < value.size(); index++) {
		const auto character = value[index];
		if (!((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
		      (character >= '0' && character <= '9') || character == '_')) {
			return false;
		}
	}
	return true;
}

bool IsColumnName(const std::string &value) {
	if (value.empty() || value.size() > 63 || value[0] < 'a' || value[0] > 'z') {
		return false;
	}
	for (const auto character : value) {
		if (!((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_')) {
			return false;
		}
	}
	return true;
}

bool IsCanonicalInteger(const std::string &value) {
	if (value.empty()) {
		return false;
	}
	std::size_t index = value[0] == '-' ? 1 : 0;
	if (index == value.size() || (value[index] == '0' && index + 1 != value.size())) {
		return false;
	}
	for (; index < value.size(); index++) {
		if (value[index] < '0' || value[index] > '9') {
			return false;
		}
	}
	return true;
}

bool IsValidUtf8(const std::string &value) {
	std::size_t index = 0;
	while (index < value.size()) {
		const auto first = static_cast<unsigned char>(value[index]);
		if (first <= 0x7fU) {
			index++;
			continue;
		}
		std::size_t count = 0;
		std::uint32_t codepoint = 0;
		if (first >= 0xc2U && first <= 0xdfU) {
			count = 1;
			codepoint = first & 0x1fU;
		} else if (first >= 0xe0U && first <= 0xefU) {
			count = 2;
			codepoint = first & 0x0fU;
		} else if (first >= 0xf0U && first <= 0xf4U) {
			count = 3;
			codepoint = first & 0x07U;
		} else {
			return false;
		}
		if (index + count >= value.size()) {
			return false;
		}
		for (std::size_t offset = 1; offset <= count; offset++) {
			const auto continuation = static_cast<unsigned char>(value[index + offset]);
			if ((continuation & 0xc0U) != 0x80U) {
				return false;
			}
			codepoint = (codepoint << 6U) | (continuation & 0x3fU);
		}
		if ((count == 2 && codepoint < 0x800U) || (count == 3 && codepoint < 0x10000U) ||
		    (codepoint >= 0xd800U && codepoint <= 0xdfffU) || codepoint > 0x10ffffU) {
			return false;
		}
		index += count + 1;
	}
	return true;
}

void ValidateLiteral(const CompiledGraphqlLiteral &literal, std::size_t depth, std::size_t &nodes) {
	if (depth > 32 || nodes == std::numeric_limits<std::size_t>::max() || ++nodes > 100000) {
		throw std::invalid_argument("compiled GraphQL literal exceeds its structural budget");
	}
	const bool has_items = !literal.Items().empty();
	const bool has_fields = !literal.Fields().empty();
	switch (literal.Kind()) {
	case CompiledGraphqlLiteralKind::NULL_VALUE:
		if (!literal.Scalar().empty() || has_items || has_fields) {
			throw std::invalid_argument("compiled GraphQL NULL literal is contradictory");
		}
		return;
	case CompiledGraphqlLiteralKind::BOOLEAN:
		if ((literal.Scalar() != "true" && literal.Scalar() != "false") || has_items || has_fields) {
			throw std::invalid_argument("compiled GraphQL BOOLEAN literal is contradictory");
		}
		return;
	case CompiledGraphqlLiteralKind::INTEGER:
		if (!IsCanonicalInteger(literal.Scalar()) || has_items || has_fields) {
			throw std::invalid_argument("compiled GraphQL INTEGER literal is contradictory");
		}
		return;
	case CompiledGraphqlLiteralKind::STRING:
		if (!IsValidUtf8(literal.Scalar()) || has_items || has_fields || literal.Scalar().size() > 1048576) {
			throw std::invalid_argument("compiled GraphQL STRING literal is contradictory");
		}
		return;
	case CompiledGraphqlLiteralKind::ENUM_VALUE:
		if (!IsName(literal.Scalar()) || literal.Scalar() == "true" || literal.Scalar() == "false" ||
		    literal.Scalar() == "null" || has_items || has_fields) {
			throw std::invalid_argument("compiled GraphQL enum literal is contradictory");
		}
		return;
	case CompiledGraphqlLiteralKind::LIST:
		if (!literal.Scalar().empty() || has_fields || literal.Items().size() > 4096) {
			throw std::invalid_argument("compiled GraphQL list literal is contradictory");
		}
		for (const auto &item : literal.Items()) {
			if (!item) {
				throw std::invalid_argument("compiled GraphQL list contains an empty item");
			}
			ValidateLiteral(*item, depth + 1, nodes);
		}
		return;
	case CompiledGraphqlLiteralKind::OBJECT: {
		if (!literal.Scalar().empty() || has_items || literal.Fields().size() > 4096) {
			throw std::invalid_argument("compiled GraphQL object literal is contradictory");
		}
		std::set<std::string> names;
		for (const auto &field : literal.Fields()) {
			if (!IsName(field.Name()) || !names.insert(field.Name()).second) {
				throw std::invalid_argument("compiled GraphQL object contains an invalid field");
			}
			ValidateLiteral(field.Value(), depth + 1, nodes);
		}
		return;
	}
	}
	throw std::invalid_argument("compiled GraphQL literal contains an unknown kind");
}

std::string RenderString(const std::string &value) {
	static const char hex[] = "0123456789abcdef";
	std::string result = "\"";
	for (const auto character : value) {
		const auto byte = static_cast<unsigned char>(character);
		switch (byte) {
		case '"':
			result += "\\\"";
			break;
		case '\\':
			result += "\\\\";
			break;
		case '\b':
			result += "\\b";
			break;
		case '\f':
			result += "\\f";
			break;
		case '\n':
			result += "\\n";
			break;
		case '\r':
			result += "\\r";
			break;
		case '\t':
			result += "\\t";
			break;
		default:
			if (byte < 0x20U) {
				result += "\\u00";
				result.push_back(hex[(byte >> 4U) & 0x0fU]);
				result.push_back(hex[byte & 0x0fU]);
			} else {
				result.push_back(character);
			}
		}
	}
	result.push_back('"');
	return result;
}

std::string RenderLiteral(const CompiledGraphqlLiteral &literal) {
	switch (literal.Kind()) {
	case CompiledGraphqlLiteralKind::NULL_VALUE:
		return "null";
	case CompiledGraphqlLiteralKind::BOOLEAN:
	case CompiledGraphqlLiteralKind::INTEGER:
	case CompiledGraphqlLiteralKind::ENUM_VALUE:
		return literal.Scalar();
	case CompiledGraphqlLiteralKind::STRING:
		return RenderString(literal.Scalar());
	case CompiledGraphqlLiteralKind::LIST: {
		std::string result = "[";
		for (std::size_t index = 0; index < literal.Items().size(); index++) {
			result += (index == 0 ? "" : ", ") + RenderLiteral(*literal.Items()[index]);
		}
		return result + "]";
	}
	case CompiledGraphqlLiteralKind::OBJECT: {
		std::string result = "{";
		for (std::size_t index = 0; index < literal.Fields().size(); index++) {
			result += (index == 0 ? "" : ", ") + literal.Fields()[index].Name() + ": " +
			          RenderLiteral(literal.Fields()[index].Value());
		}
		return result + "}";
	}
	}
	throw std::invalid_argument("compiled GraphQL literal contains an unknown kind");
}

std::string Indent(std::size_t depth) {
	return std::string(depth * 2, ' ');
}

} // namespace

CompiledGraphqlObjectField::CompiledGraphqlObjectField(std::string name_p,
                                                       std::shared_ptr<const CompiledGraphqlLiteral> value_p)
    : name(std::move(name_p)), value(std::move(value_p)) {
	if (!value) {
		throw std::invalid_argument("compiled GraphQL object field lacks a value");
	}
}

const std::string &CompiledGraphqlObjectField::Name() const {
	return name;
}

const CompiledGraphqlLiteral &CompiledGraphqlObjectField::Value() const {
	return *value;
}

CompiledGraphqlLiteral::CompiledGraphqlLiteral(CompiledGraphqlLiteralKind kind_p, std::string scalar_p,
                                               std::vector<std::shared_ptr<const CompiledGraphqlLiteral>> items_p,
                                               std::vector<CompiledGraphqlObjectField> fields_p)
    : kind(kind_p), scalar(std::move(scalar_p)), items(std::move(items_p)), fields(std::move(fields_p)) {
}

CompiledGraphqlLiteralKind CompiledGraphqlLiteral::Kind() const {
	return kind;
}

const std::string &CompiledGraphqlLiteral::Scalar() const {
	return scalar;
}

const std::vector<std::shared_ptr<const CompiledGraphqlLiteral>> &CompiledGraphqlLiteral::Items() const {
	return items;
}

const std::vector<CompiledGraphqlObjectField> &CompiledGraphqlLiteral::Fields() const {
	return fields;
}

CompiledGraphqlFixedArgument::CompiledGraphqlFixedArgument(std::string name_p,
                                                           std::shared_ptr<const CompiledGraphqlLiteral> value_p)
    : name(std::move(name_p)), value(std::move(value_p)) {
	if (!value) {
		throw std::invalid_argument("compiled GraphQL fixed argument lacks a value");
	}
}

const std::string &CompiledGraphqlFixedArgument::Name() const {
	return name;
}

const CompiledGraphqlLiteral &CompiledGraphqlFixedArgument::Value() const {
	return *value;
}

CompiledGraphqlRecipeVariable::CompiledGraphqlRecipeVariable(std::string name_p, CompiledGraphqlVariableType type_p,
                                                             CompiledGraphqlRecipeVariableRole role_p,
                                                             std::string argument_name_p)
    : name(std::move(name_p)), type(type_p), role(role_p), argument_name(std::move(argument_name_p)) {
}

const std::string &CompiledGraphqlRecipeVariable::Name() const {
	return name;
}

CompiledGraphqlVariableType CompiledGraphqlRecipeVariable::Type() const {
	return type;
}

CompiledGraphqlRecipeVariableRole CompiledGraphqlRecipeVariable::Role() const {
	return role;
}

const std::string &CompiledGraphqlRecipeVariable::ArgumentName() const {
	return argument_name;
}

CompiledGraphqlSelection::CompiledGraphqlSelection(std::string column_name_p, std::vector<std::string> field_path_p)
    : column_name(std::move(column_name_p)), field_path(std::move(field_path_p)) {
}

const std::string &CompiledGraphqlSelection::ColumnName() const {
	return column_name;
}

const std::vector<std::string> &CompiledGraphqlSelection::FieldPath() const {
	return field_path;
}

CompiledGraphqlQueryRecipe::CompiledGraphqlQueryRecipe(
    CompiledGraphqlDocumentIdentity identity_p, std::string operation_name_p,
    std::vector<CompiledGraphqlRecipeVariable> variables_p, std::vector<std::string> root_path_p,
    std::vector<CompiledGraphqlFixedArgument> fixed_arguments_p, std::string nodes_field_p,
    std::vector<CompiledGraphqlSelection> selections_p, std::string page_info_field_p,
    std::string has_next_page_field_p, std::string end_cursor_field_p)
    : identity(identity_p), operation_name(std::move(operation_name_p)), variables(std::move(variables_p)),
      root_path(std::move(root_path_p)), fixed_arguments(std::move(fixed_arguments_p)),
      nodes_field(std::move(nodes_field_p)), selections(std::move(selections_p)),
      page_info_field(std::move(page_info_field_p)), has_next_page_field(std::move(has_next_page_field_p)),
      end_cursor_field(std::move(end_cursor_field_p)) {
	internal::ValidateCompiledGraphqlQueryRecipe(*this);
}

CompiledGraphqlDocumentIdentity CompiledGraphqlQueryRecipe::Identity() const {
	return identity;
}

const std::string &CompiledGraphqlQueryRecipe::OperationName() const {
	return operation_name;
}

const std::vector<CompiledGraphqlRecipeVariable> &CompiledGraphqlQueryRecipe::Variables() const {
	return variables;
}

const std::vector<std::string> &CompiledGraphqlQueryRecipe::RootPath() const {
	return root_path;
}

const std::vector<CompiledGraphqlFixedArgument> &CompiledGraphqlQueryRecipe::FixedArguments() const {
	return fixed_arguments;
}

const std::string &CompiledGraphqlQueryRecipe::NodesField() const {
	return nodes_field;
}

const std::vector<CompiledGraphqlSelection> &CompiledGraphqlQueryRecipe::Selections() const {
	return selections;
}

const std::string &CompiledGraphqlQueryRecipe::PageInfoField() const {
	return page_info_field;
}

const std::string &CompiledGraphqlQueryRecipe::HasNextPageField() const {
	return has_next_page_field;
}

const std::string &CompiledGraphqlQueryRecipe::EndCursorField() const {
	return end_cursor_field;
}

const CompiledGraphqlQueryRecipe &CompiledGraphqlOperation::QueryRecipe() const {
	if (!query_recipe) {
		throw std::logic_error("compiled GraphQL operation has no query recipe");
	}
	return *query_recipe;
}

namespace internal {

void ValidateCompiledGraphqlQueryRecipe(const CompiledGraphqlQueryRecipe &recipe) {
	if (recipe.Identity() != CompiledGraphqlDocumentIdentity::PACKAGE_QUERY_GENERATOR_V1 ||
	    !IsName(recipe.OperationName()) || recipe.RootPath().empty() || recipe.RootPath().size() > 16 ||
	    recipe.FixedArguments().size() > 64 || recipe.Selections().empty() || recipe.Selections().size() > 256 ||
	    recipe.NodesField() != "nodes" || !IsName(recipe.PageInfoField()) || !IsName(recipe.HasNextPageField()) ||
	    !IsName(recipe.EndCursorField()) || recipe.PageInfoField() == recipe.NodesField() ||
	    recipe.HasNextPageField() == recipe.EndCursorField()) {
		throw std::invalid_argument("compiled GraphQL query recipe has an invalid envelope");
	}
	if (recipe.Variables().size() != 2 ||
	    recipe.Variables()[0].Role() != CompiledGraphqlRecipeVariableRole::PAGE_SIZE ||
	    recipe.Variables()[0].Type() != CompiledGraphqlVariableType::INT_NON_NULL ||
	    recipe.Variables()[1].Role() != CompiledGraphqlRecipeVariableRole::CURSOR ||
	    recipe.Variables()[1].Type() != CompiledGraphqlVariableType::STRING_NULLABLE ||
	    !IsName(recipe.Variables()[0].Name()) || !IsName(recipe.Variables()[1].Name()) ||
	    !IsName(recipe.Variables()[0].ArgumentName()) || !IsName(recipe.Variables()[1].ArgumentName()) ||
	    recipe.Variables()[0].Name() == recipe.Variables()[1].Name() ||
	    recipe.Variables()[0].ArgumentName() == recipe.Variables()[1].ArgumentName()) {
		throw std::invalid_argument("compiled GraphQL query recipe has invalid ordered variables");
	}
	std::set<std::string> reserved = {"data",
	                                  "errors",
	                                  recipe.NodesField(),
	                                  recipe.PageInfoField(),
	                                  recipe.HasNextPageField(),
	                                  recipe.EndCursorField()};
	std::set<std::string> roots;
	for (const auto &name : recipe.RootPath()) {
		if (!IsName(name) || reserved.count(name) != 0 || !roots.insert(name).second) {
			throw std::invalid_argument("compiled GraphQL query recipe has an aliased root path");
		}
	}
	std::set<std::string> arguments = {recipe.Variables()[0].ArgumentName(), recipe.Variables()[1].ArgumentName()};
	std::size_t literal_nodes = 0;
	for (const auto &argument : recipe.FixedArguments()) {
		if (!IsName(argument.Name()) || !arguments.insert(argument.Name()).second) {
			throw std::invalid_argument("compiled GraphQL query recipe has a duplicate argument");
		}
		ValidateLiteral(argument.Value(), 1, literal_nodes);
	}
	std::set<std::string> paths;
	std::set<std::string> nested_parents;
	std::set<std::string> columns;
	for (const auto &selection : recipe.Selections()) {
		if (!IsColumnName(selection.ColumnName()) || !columns.insert(selection.ColumnName()).second ||
		    selection.FieldPath().empty() || selection.FieldPath().size() > 2) {
			throw std::invalid_argument("compiled GraphQL query recipe has an invalid selection");
		}
		std::string path;
		for (const auto &name : selection.FieldPath()) {
			if (!IsName(name)) {
				throw std::invalid_argument("compiled GraphQL query recipe has an invalid field name");
			}
			path += (path.empty() ? "" : "\n") + name;
		}
		if (!paths.insert(path).second ||
		    (selection.FieldPath().size() == 2 && !nested_parents.insert(selection.FieldPath()[0]).second) ||
		    (selection.FieldPath().size() == 1 && nested_parents.count(selection.FieldPath()[0]) != 0) ||
		    (selection.FieldPath().size() == 2 && paths.count(selection.FieldPath()[0]) != 0)) {
			throw std::invalid_argument("compiled GraphQL query recipe has overlapping selections");
		}
	}
}

std::string RenderCompiledGraphqlQueryRecipe(const CompiledGraphqlQueryRecipe &recipe) {
	ValidateCompiledGraphqlQueryRecipe(recipe);
	const auto &page = recipe.Variables()[0];
	const auto &cursor = recipe.Variables()[1];
	std::ostringstream document;
	document << "query " << recipe.OperationName() << "($" << page.Name() << ": Int!, $" << cursor.Name()
	         << ": String) {\n";
	for (std::size_t index = 0; index + 1 < recipe.RootPath().size(); index++) {
		document << Indent(index + 1) << recipe.RootPath()[index] << " {\n";
	}
	const auto connection_depth = recipe.RootPath().size();
	document << Indent(connection_depth) << recipe.RootPath().back() << "(\n";
	document << Indent(connection_depth + 1) << page.ArgumentName() << ": $" << page.Name() << "\n";
	document << Indent(connection_depth + 1) << cursor.ArgumentName() << ": $" << cursor.Name() << "\n";
	for (const auto &argument : recipe.FixedArguments()) {
		document << Indent(connection_depth + 1) << argument.Name() << ": " << RenderLiteral(argument.Value()) << "\n";
	}
	document << Indent(connection_depth) << ") {\n";
	document << Indent(connection_depth + 1) << recipe.NodesField() << " {\n";
	for (const auto &selection : recipe.Selections()) {
		document << Indent(connection_depth + 2) << selection.FieldPath()[0];
		if (selection.FieldPath().size() == 2) {
			document << " { " << selection.FieldPath()[1] << " }";
		}
		document << "\n";
	}
	document << Indent(connection_depth + 1) << "}\n";
	document << Indent(connection_depth + 1) << recipe.PageInfoField() << " { " << recipe.HasNextPageField() << ' '
	         << recipe.EndCursorField() << " }\n";
	for (std::size_t depth = connection_depth; depth > 0; depth--) {
		document << Indent(depth) << "}\n";
	}
	document << '}';
	return document.str();
}

CompiledGraphqlLiteral CompiledModelBuilder::GraphqlNullLiteral() {
	return CompiledGraphqlLiteral(CompiledGraphqlLiteralKind::NULL_VALUE, "", {}, {});
}

CompiledGraphqlLiteral CompiledModelBuilder::GraphqlBooleanLiteral(bool value) {
	return CompiledGraphqlLiteral(CompiledGraphqlLiteralKind::BOOLEAN, value ? "true" : "false", {}, {});
}

CompiledGraphqlLiteral CompiledModelBuilder::GraphqlIntegerLiteral(std::int64_t value) {
	return CompiledGraphqlLiteral(CompiledGraphqlLiteralKind::INTEGER, std::to_string(value), {}, {});
}

CompiledGraphqlLiteral CompiledModelBuilder::GraphqlStringLiteral(std::string value) {
	return CompiledGraphqlLiteral(CompiledGraphqlLiteralKind::STRING, std::move(value), {}, {});
}

CompiledGraphqlLiteral CompiledModelBuilder::GraphqlEnumLiteral(std::string value) {
	return CompiledGraphqlLiteral(CompiledGraphqlLiteralKind::ENUM_VALUE, std::move(value), {}, {});
}

CompiledGraphqlLiteral
CompiledModelBuilder::GraphqlListLiteral(std::vector<std::shared_ptr<const CompiledGraphqlLiteral>> items) {
	return CompiledGraphqlLiteral(CompiledGraphqlLiteralKind::LIST, "", std::move(items), {});
}

CompiledGraphqlObjectField CompiledModelBuilder::GraphqlObjectField(std::string name, CompiledGraphqlLiteral value) {
	return CompiledGraphqlObjectField(std::move(name),
	                                  std::make_shared<const CompiledGraphqlLiteral>(std::move(value)));
}

CompiledGraphqlLiteral CompiledModelBuilder::GraphqlObjectLiteral(std::vector<CompiledGraphqlObjectField> fields) {
	return CompiledGraphqlLiteral(CompiledGraphqlLiteralKind::OBJECT, "", {}, std::move(fields));
}

CompiledGraphqlFixedArgument CompiledModelBuilder::GraphqlFixedArgument(std::string name,
                                                                        CompiledGraphqlLiteral value) {
	return CompiledGraphqlFixedArgument(std::move(name),
	                                    std::make_shared<const CompiledGraphqlLiteral>(std::move(value)));
}

CompiledGraphqlRecipeVariable CompiledModelBuilder::GraphqlRecipeVariable(std::string name,
                                                                          CompiledGraphqlVariableType type,
                                                                          CompiledGraphqlRecipeVariableRole role,
                                                                          std::string argument_name) {
	return CompiledGraphqlRecipeVariable(std::move(name), type, role, std::move(argument_name));
}

CompiledGraphqlSelection CompiledModelBuilder::GraphqlSelection(std::string column_name,
                                                                std::vector<std::string> field_path) {
	return CompiledGraphqlSelection(std::move(column_name), std::move(field_path));
}

std::shared_ptr<const CompiledGraphqlQueryRecipe> CompiledModelBuilder::GraphqlQueryRecipe(
    CompiledGraphqlDocumentIdentity identity, std::string operation_name,
    std::vector<CompiledGraphqlRecipeVariable> variables, std::vector<std::string> root_path,
    std::vector<CompiledGraphqlFixedArgument> fixed_arguments, std::string nodes_field,
    std::vector<CompiledGraphqlSelection> selections, std::string page_info_field, std::string has_next_page_field,
    std::string end_cursor_field) {
	return std::shared_ptr<const CompiledGraphqlQueryRecipe>(new CompiledGraphqlQueryRecipe(
	    identity, std::move(operation_name), std::move(variables), std::move(root_path), std::move(fixed_arguments),
	    std::move(nodes_field), std::move(selections), std::move(page_info_field), std::move(has_next_page_field),
	    std::move(end_cursor_field)));
}

} // namespace internal
} // namespace cuac
