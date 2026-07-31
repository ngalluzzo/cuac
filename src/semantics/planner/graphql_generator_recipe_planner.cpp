#include "cuac/internal/semantics/planner/graphql_generator_recipe_planner.hpp"

#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace cuac {
namespace scan_planner_internal {
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
	const bool negative = value[0] == '-';
	const auto maximum =
	    negative ? (std::uint64_t(1) << 63U) : static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
	std::uint64_t magnitude = 0;
	for (; index < value.size(); index++) {
		if (value[index] < '0' || value[index] > '9') {
			return false;
		}
		const auto digit = static_cast<std::uint64_t>(value[index] - '0');
		if (magnitude > (maximum - digit) / 10) {
			return false;
		}
		magnitude = magnitude * 10 + digit;
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
		std::size_t continuation_count = 0;
		std::uint32_t code_point = 0;
		if (first >= 0xc2U && first <= 0xdfU) {
			continuation_count = 1;
			code_point = first & 0x1fU;
		} else if (first >= 0xe0U && first <= 0xefU) {
			continuation_count = 2;
			code_point = first & 0x0fU;
		} else if (first >= 0xf0U && first <= 0xf4U) {
			continuation_count = 3;
			code_point = first & 0x07U;
		} else {
			return false;
		}
		if (index + continuation_count >= value.size()) {
			return false;
		}
		for (std::size_t offset = 1; offset <= continuation_count; offset++) {
			const auto continuation = static_cast<unsigned char>(value[index + offset]);
			if ((continuation & 0xc0U) != 0x80U) {
				return false;
			}
			code_point = (code_point << 6U) | (continuation & 0x3fU);
		}
		if ((continuation_count == 2 && code_point < 0x800U) || (continuation_count == 3 && code_point < 0x10000U) ||
		    (code_point >= 0xd800U && code_point <= 0xdfffU) || code_point > 0x10ffffU) {
			return false;
		}
		index += continuation_count + 1;
	}
	return true;
}

PlannedGraphqlLiteralKind PlanLiteralKind(CompiledGraphqlLiteralKind kind) {
	switch (kind) {
	case CompiledGraphqlLiteralKind::NULL_VALUE:
		return PlannedGraphqlLiteralKind::NULL_VALUE;
	case CompiledGraphqlLiteralKind::BOOLEAN:
		return PlannedGraphqlLiteralKind::BOOLEAN;
	case CompiledGraphqlLiteralKind::INTEGER:
		return PlannedGraphqlLiteralKind::INTEGER;
	case CompiledGraphqlLiteralKind::STRING:
		return PlannedGraphqlLiteralKind::STRING;
	case CompiledGraphqlLiteralKind::ENUM_VALUE:
		return PlannedGraphqlLiteralKind::ENUM_VALUE;
	case CompiledGraphqlLiteralKind::LIST:
		return PlannedGraphqlLiteralKind::LIST;
	case CompiledGraphqlLiteralKind::OBJECT:
		return PlannedGraphqlLiteralKind::OBJECT;
	}
	throw std::logic_error("compiled package GraphQL recipe contains an unknown literal kind");
}

PlannedGraphqlRecipeVariableType PlanVariableType(CompiledGraphqlVariableType type) {
	switch (type) {
	case CompiledGraphqlVariableType::INT_NON_NULL:
		return PlannedGraphqlRecipeVariableType::INT_NON_NULL;
	case CompiledGraphqlVariableType::STRING_NULLABLE:
		return PlannedGraphqlRecipeVariableType::STRING_NULLABLE;
	}
	throw std::logic_error("compiled package GraphQL recipe contains an unknown variable type");
}

PlannedGraphqlRecipeVariableRole PlanVariableRole(CompiledGraphqlRecipeVariableRole role) {
	switch (role) {
	case CompiledGraphqlRecipeVariableRole::PAGE_SIZE:
		return PlannedGraphqlRecipeVariableRole::PAGE_SIZE;
	case CompiledGraphqlRecipeVariableRole::CURSOR:
		return PlannedGraphqlRecipeVariableRole::CURSOR;
	}
	throw std::logic_error("compiled package GraphQL recipe contains an unknown variable role");
}

void ValidateSourceEnvelope(const CompiledGraphqlQueryRecipe &recipe) {
	if (!IsName(recipe.OperationName()) || recipe.Variables().size() != 2 || recipe.RootPath().empty() ||
	    recipe.RootPath().size() > 16 || recipe.FixedArguments().size() > 64 || recipe.Selections().empty() ||
	    recipe.Selections().size() > 256 || !IsName(recipe.NodesField()) || !IsName(recipe.PageInfoField()) ||
	    !IsName(recipe.HasNextPageField()) || !IsName(recipe.EndCursorField())) {
		throw std::logic_error("compiled package GraphQL recipe exceeds its copy envelope");
	}
	for (const auto &variable : recipe.Variables()) {
		if (!IsName(variable.Name()) || !IsName(variable.ArgumentName())) {
			throw std::logic_error("compiled package GraphQL variable exceeds its copy envelope");
		}
		(void)PlanVariableType(variable.Type());
		(void)PlanVariableRole(variable.Role());
	}
	for (const auto &root : recipe.RootPath()) {
		if (!IsName(root)) {
			throw std::logic_error("compiled package GraphQL root exceeds its copy envelope");
		}
	}
	for (const auto &argument : recipe.FixedArguments()) {
		if (!IsName(argument.Name())) {
			throw std::logic_error("compiled package GraphQL argument exceeds its copy envelope");
		}
	}
	for (const auto &selection : recipe.Selections()) {
		if (!IsColumnName(selection.ColumnName()) || selection.FieldPath().empty() ||
		    selection.FieldPath().size() > 2) {
			throw std::logic_error("compiled package GraphQL selection exceeds its copy envelope");
		}
		for (const auto &field : selection.FieldPath()) {
			if (!IsName(field)) {
				throw std::logic_error("compiled package GraphQL field exceeds its copy envelope");
			}
		}
	}
}

void ValidateLiteral(const PlannedGraphqlLiteral &literal, std::size_t depth, std::size_t &nodes) {
	if (depth > 32 || nodes == std::numeric_limits<std::size_t>::max() || ++nodes > 100000) {
		throw std::logic_error("planned package GraphQL literal exceeds its structural budget");
	}
	const bool has_items = !literal.Items().empty();
	const bool has_fields = !literal.Fields().empty();
	switch (literal.Kind()) {
	case PlannedGraphqlLiteralKind::NULL_VALUE:
		if (!literal.Scalar().empty() || has_items || has_fields) {
			throw std::logic_error("planned package GraphQL NULL literal is contradictory");
		}
		return;
	case PlannedGraphqlLiteralKind::BOOLEAN:
		if ((literal.Scalar() != "true" && literal.Scalar() != "false") || has_items || has_fields) {
			throw std::logic_error("planned package GraphQL BOOLEAN literal is contradictory");
		}
		return;
	case PlannedGraphqlLiteralKind::INTEGER:
		if (!IsCanonicalInteger(literal.Scalar()) || has_items || has_fields) {
			throw std::logic_error("planned package GraphQL INTEGER literal is contradictory");
		}
		return;
	case PlannedGraphqlLiteralKind::STRING:
		if (!IsValidUtf8(literal.Scalar()) || literal.Scalar().size() > 1048576 || has_items || has_fields) {
			throw std::logic_error("planned package GraphQL STRING literal is contradictory");
		}
		return;
	case PlannedGraphqlLiteralKind::ENUM_VALUE:
		if (!IsName(literal.Scalar()) || literal.Scalar() == "true" || literal.Scalar() == "false" ||
		    literal.Scalar() == "null" || has_items || has_fields) {
			throw std::logic_error("planned package GraphQL enum literal is contradictory");
		}
		return;
	case PlannedGraphqlLiteralKind::LIST:
		if (!literal.Scalar().empty() || has_fields || literal.Items().size() > 4096) {
			throw std::logic_error("planned package GraphQL list literal is contradictory");
		}
		for (const auto &item : literal.Items()) {
			if (!item) {
				throw std::logic_error("planned package GraphQL list contains an empty item");
			}
			ValidateLiteral(*item, depth + 1, nodes);
		}
		return;
	case PlannedGraphqlLiteralKind::OBJECT: {
		if (!literal.Scalar().empty() || has_items || literal.Fields().size() > 4096) {
			throw std::logic_error("planned package GraphQL object literal is contradictory");
		}
		std::set<std::string> names;
		for (const auto &field : literal.Fields()) {
			if (!IsName(field.Name()) || !names.insert(field.Name()).second) {
				throw std::logic_error("planned package GraphQL object contains an invalid field");
			}
			ValidateLiteral(field.Value(), depth + 1, nodes);
		}
		return;
	}
	}
	throw std::logic_error("planned package GraphQL literal contains an unknown kind");
}

void ValidateRecipe(const PlannedGraphqlGeneratorRecipe &recipe) {
	if (recipe.Identity() != PlannedGraphqlGeneratorIdentity::PACKAGE_QUERY_GENERATOR_V1 ||
	    !IsName(recipe.OperationName()) || recipe.RootPath().empty() || recipe.RootPath().size() > 16 ||
	    recipe.FixedArguments().size() > 64 || recipe.Selections().empty() || recipe.Selections().size() > 256 ||
	    recipe.NodesField() != "nodes" || !IsName(recipe.PageInfoField()) || !IsName(recipe.HasNextPageField()) ||
	    !IsName(recipe.EndCursorField()) || recipe.PageInfoField() == recipe.NodesField() ||
	    recipe.HasNextPageField() == recipe.EndCursorField()) {
		throw std::logic_error("planned package GraphQL recipe has an invalid envelope");
	}
	if (recipe.Variables().size() != 2 || recipe.Variables()[0].Role() != PlannedGraphqlRecipeVariableRole::PAGE_SIZE ||
	    recipe.Variables()[0].Type() != PlannedGraphqlRecipeVariableType::INT_NON_NULL ||
	    recipe.Variables()[1].Role() != PlannedGraphqlRecipeVariableRole::CURSOR ||
	    recipe.Variables()[1].Type() != PlannedGraphqlRecipeVariableType::STRING_NULLABLE ||
	    !IsName(recipe.Variables()[0].Name()) || !IsName(recipe.Variables()[1].Name()) ||
	    !IsName(recipe.Variables()[0].ArgumentName()) || !IsName(recipe.Variables()[1].ArgumentName()) ||
	    recipe.Variables()[0].Name() == recipe.Variables()[1].Name() ||
	    recipe.Variables()[0].ArgumentName() == recipe.Variables()[1].ArgumentName()) {
		throw std::logic_error("planned package GraphQL recipe has invalid ordered variables");
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
			throw std::logic_error("planned package GraphQL recipe has an aliased root path");
		}
	}
	std::set<std::string> arguments = {recipe.Variables()[0].ArgumentName(), recipe.Variables()[1].ArgumentName()};
	std::size_t literal_nodes = 0;
	for (const auto &argument : recipe.FixedArguments()) {
		if (!IsName(argument.Name()) || !arguments.insert(argument.Name()).second) {
			throw std::logic_error("planned package GraphQL recipe has a duplicate argument");
		}
		ValidateLiteral(argument.Value(), 1, literal_nodes);
	}
	std::set<std::string> paths;
	std::set<std::string> nested_parents;
	std::set<std::string> columns;
	for (const auto &selection : recipe.Selections()) {
		if (!IsColumnName(selection.ColumnName()) || !columns.insert(selection.ColumnName()).second ||
		    selection.FieldPath().empty() || selection.FieldPath().size() > 2) {
			throw std::logic_error("planned package GraphQL recipe has an invalid selection");
		}
		std::string path;
		for (const auto &name : selection.FieldPath()) {
			if (!IsName(name)) {
				throw std::logic_error("planned package GraphQL recipe has an invalid field name");
			}
			path += (path.empty() ? "" : "\n") + name;
		}
		if (!paths.insert(path).second ||
		    (selection.FieldPath().size() == 2 && !nested_parents.insert(selection.FieldPath()[0]).second) ||
		    (selection.FieldPath().size() == 1 && nested_parents.count(selection.FieldPath()[0]) != 0) ||
		    (selection.FieldPath().size() == 2 && paths.count(selection.FieldPath()[0]) != 0)) {
			throw std::logic_error("planned package GraphQL recipe has overlapping selections");
		}
	}
}

class BoundedDocument final {
public:
	explicit BoundedDocument(std::size_t maximum_p) : maximum(maximum_p) {
		value.reserve(maximum < 4096 ? maximum : 4096);
	}

	void Append(const std::string &part) {
		if (part.size() > maximum - value.size()) {
			throw std::logic_error("planned package GraphQL document exceeds its rendered-byte budget");
		}
		value += part;
	}

	void Append(const char *part) {
		Append(std::string(part));
	}

	void Push(char part) {
		if (value.size() == maximum) {
			throw std::logic_error("planned package GraphQL document exceeds its rendered-byte budget");
		}
		value.push_back(part);
	}

	std::string Release() {
		return std::move(value);
	}

private:
	std::size_t maximum;
	std::string value;
};

void RenderString(const std::string &value, BoundedDocument &result) {
	static const char hex[] = "0123456789abcdef";
	result.Push('"');
	for (const auto character : value) {
		const auto byte = static_cast<unsigned char>(character);
		switch (byte) {
		case '"':
			result.Append("\\\"");
			break;
		case '\\':
			result.Append("\\\\");
			break;
		case '\b':
			result.Append("\\b");
			break;
		case '\f':
			result.Append("\\f");
			break;
		case '\n':
			result.Append("\\n");
			break;
		case '\r':
			result.Append("\\r");
			break;
		case '\t':
			result.Append("\\t");
			break;
		default:
			if (byte < 0x20U) {
				result.Append("\\u00");
				result.Push(hex[(byte >> 4U) & 0x0fU]);
				result.Push(hex[byte & 0x0fU]);
			} else {
				result.Push(character);
			}
		}
	}
	result.Push('"');
}

void RenderLiteral(const PlannedGraphqlLiteral &literal, BoundedDocument &result) {
	switch (literal.Kind()) {
	case PlannedGraphqlLiteralKind::NULL_VALUE:
		result.Append("null");
		return;
	case PlannedGraphqlLiteralKind::BOOLEAN:
	case PlannedGraphqlLiteralKind::INTEGER:
	case PlannedGraphqlLiteralKind::ENUM_VALUE:
		result.Append(literal.Scalar());
		return;
	case PlannedGraphqlLiteralKind::STRING:
		RenderString(literal.Scalar(), result);
		return;
	case PlannedGraphqlLiteralKind::LIST:
		result.Push('[');
		for (std::size_t index = 0; index < literal.Items().size(); index++) {
			if (index != 0) {
				result.Append(", ");
			}
			RenderLiteral(*literal.Items()[index], result);
		}
		result.Push(']');
		return;
	case PlannedGraphqlLiteralKind::OBJECT:
		result.Push('{');
		for (std::size_t index = 0; index < literal.Fields().size(); index++) {
			if (index != 0) {
				result.Append(", ");
			}
			result.Append(literal.Fields()[index].Name());
			result.Append(": ");
			RenderLiteral(literal.Fields()[index].Value(), result);
		}
		result.Push('}');
		return;
	}
	throw std::logic_error("planned package GraphQL literal contains an unknown kind");
}

void AppendIndent(BoundedDocument &document, std::size_t depth) {
	document.Append(std::string(depth * 2, ' '));
}

std::string RenderRecipe(const PlannedGraphqlGeneratorRecipe &recipe, std::size_t max_rendered_bytes) {
	const auto &page = recipe.Variables()[0];
	const auto &cursor = recipe.Variables()[1];
	BoundedDocument document(max_rendered_bytes);
	document.Append("query ");
	document.Append(recipe.OperationName());
	document.Append("($");
	document.Append(page.Name());
	document.Append(": Int!, $");
	document.Append(cursor.Name());
	document.Append(": String) {\n");
	for (std::size_t index = 0; index + 1 < recipe.RootPath().size(); index++) {
		AppendIndent(document, index + 1);
		document.Append(recipe.RootPath()[index]);
		document.Append(" {\n");
	}
	const auto connection_depth = recipe.RootPath().size();
	AppendIndent(document, connection_depth);
	document.Append(recipe.RootPath().back());
	document.Append("(\n");
	AppendIndent(document, connection_depth + 1);
	document.Append(page.ArgumentName());
	document.Append(": $");
	document.Append(page.Name());
	document.Push('\n');
	AppendIndent(document, connection_depth + 1);
	document.Append(cursor.ArgumentName());
	document.Append(": $");
	document.Append(cursor.Name());
	document.Push('\n');
	for (const auto &argument : recipe.FixedArguments()) {
		AppendIndent(document, connection_depth + 1);
		document.Append(argument.Name());
		document.Append(": ");
		RenderLiteral(argument.Value(), document);
		document.Push('\n');
	}
	AppendIndent(document, connection_depth);
	document.Append(") {\n");
	AppendIndent(document, connection_depth + 1);
	document.Append(recipe.NodesField());
	document.Append(" {\n");
	for (const auto &selection : recipe.Selections()) {
		AppendIndent(document, connection_depth + 2);
		document.Append(selection.FieldPath()[0]);
		if (selection.FieldPath().size() == 2) {
			document.Append(" { ");
			document.Append(selection.FieldPath()[1]);
			document.Append(" }");
		}
		document.Push('\n');
	}
	AppendIndent(document, connection_depth + 1);
	document.Append("}\n");
	AppendIndent(document, connection_depth + 1);
	document.Append(recipe.PageInfoField());
	document.Append(" { ");
	document.Append(recipe.HasNextPageField());
	document.Push(' ');
	document.Append(recipe.EndCursorField());
	document.Append(" }\n");
	for (std::size_t depth = connection_depth; depth > 0; depth--) {
		AppendIndent(document, depth);
		document.Append("}\n");
	}
	document.Push('}');
	return document.Release();
}

} // namespace

std::shared_ptr<const PlannedGraphqlLiteral>
GraphqlGeneratorRecipePlanner::CopyLiteral(const CompiledGraphqlLiteral &source, std::size_t depth,
                                           std::size_t &nodes) {
	if (depth > 32 || nodes == std::numeric_limits<std::size_t>::max() || ++nodes > 100000) {
		throw std::logic_error("compiled package GraphQL literal exceeds its copy budget");
	}
	const auto kind = PlanLiteralKind(source.Kind());
	const bool has_items = !source.Items().empty();
	const bool has_fields = !source.Fields().empty();
	if ((kind == PlannedGraphqlLiteralKind::STRING &&
	     (!IsValidUtf8(source.Scalar()) || source.Scalar().size() > 1048576)) ||
	    (kind == PlannedGraphqlLiteralKind::INTEGER && !IsCanonicalInteger(source.Scalar())) ||
	    (kind == PlannedGraphqlLiteralKind::ENUM_VALUE && (!IsName(source.Scalar()) || source.Scalar() == "true" ||
	                                                       source.Scalar() == "false" || source.Scalar() == "null")) ||
	    (kind == PlannedGraphqlLiteralKind::BOOLEAN && source.Scalar() != "true" && source.Scalar() != "false") ||
	    ((kind == PlannedGraphqlLiteralKind::NULL_VALUE || kind == PlannedGraphqlLiteralKind::LIST ||
	      kind == PlannedGraphqlLiteralKind::OBJECT) &&
	     !source.Scalar().empty()) ||
	    (kind != PlannedGraphqlLiteralKind::LIST && has_items) ||
	    (kind != PlannedGraphqlLiteralKind::OBJECT && has_fields) || source.Items().size() > 4096 ||
	    source.Fields().size() > 4096) {
		throw std::logic_error("compiled package GraphQL literal is invalid before copy");
	}
	std::vector<std::shared_ptr<const PlannedGraphqlLiteral>> items;
	items.reserve(source.Items().size());
	for (const auto &item : source.Items()) {
		if (!item) {
			throw std::logic_error("compiled package GraphQL recipe contains an empty literal item");
		}
		items.push_back(CopyLiteral(*item, depth + 1, nodes));
	}
	std::vector<PlannedGraphqlObjectField> fields;
	fields.reserve(source.Fields().size());
	std::set<std::string> field_names;
	for (const auto &field : source.Fields()) {
		if (!IsName(field.Name()) || !field_names.insert(field.Name()).second) {
			throw std::logic_error("compiled package GraphQL object field is invalid before copy");
		}
		fields.push_back(PlannedGraphqlObjectField(field.Name(), CopyLiteral(field.Value(), depth + 1, nodes)));
	}
	return std::shared_ptr<const PlannedGraphqlLiteral>(
	    new PlannedGraphqlLiteral(kind, source.Scalar(), std::move(items), std::move(fields)));
}

PlannedGraphqlGeneratorRecipeResult GraphqlGeneratorRecipePlanner::Plan(const CompiledGraphqlQueryRecipe &source,
                                                                        std::uint64_t max_rendered_bytes) {
	if (source.Identity() != CompiledGraphqlDocumentIdentity::PACKAGE_QUERY_GENERATOR_V1 || max_rendered_bytes == 0 ||
	    max_rendered_bytes > 65536) {
		throw std::logic_error("compiled GraphQL recipe is not the package query generator identity");
	}
	ValidateSourceEnvelope(source);
	std::vector<PlannedGraphqlRecipeVariable> variables;
	variables.reserve(source.Variables().size());
	for (const auto &variable : source.Variables()) {
		variables.push_back(PlannedGraphqlRecipeVariable(variable.Name(), PlanVariableType(variable.Type()),
		                                                 PlanVariableRole(variable.Role()), variable.ArgumentName()));
	}
	std::vector<PlannedGraphqlFixedArgument> arguments;
	arguments.reserve(source.FixedArguments().size());
	std::size_t literal_nodes = 0;
	for (const auto &argument : source.FixedArguments()) {
		arguments.push_back(
		    PlannedGraphqlFixedArgument(argument.Name(), CopyLiteral(argument.Value(), 1, literal_nodes)));
	}
	std::vector<PlannedGraphqlSelection> selections;
	selections.reserve(source.Selections().size());
	for (const auto &selection : source.Selections()) {
		selections.push_back(PlannedGraphqlSelection(selection.ColumnName(), selection.FieldPath()));
	}
	auto planned = std::shared_ptr<const PlannedGraphqlGeneratorRecipe>(new PlannedGraphqlGeneratorRecipe(
	    PlannedGraphqlGeneratorIdentity::PACKAGE_QUERY_GENERATOR_V1, source.OperationName(), std::move(variables),
	    source.RootPath(), std::move(arguments), source.NodesField(), std::move(selections), source.PageInfoField(),
	    source.HasNextPageField(), source.EndCursorField()));
	ValidateRecipe(*planned);
	return {planned, RenderRecipe(*planned, static_cast<std::size_t>(max_rendered_bytes))};
}

} // namespace scan_planner_internal
} // namespace cuac
