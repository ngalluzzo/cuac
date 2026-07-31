#include "cuac/internal/runtime/admission/graphql_recipe_admission.hpp"

#include "cuac/internal/runtime/policy/request_validation.hpp"

#include <cstdint>
#include <limits>
#include <set>
#include <utility>

namespace cuac {
namespace internal {
namespace {

bool IsCanonicalInteger(const std::string &value) noexcept {
	if (value.empty()) {
		return false;
	}
	std::size_t index = value[0] == '-' ? 1 : 0;
	if (index == value.size() || (value[index] == '0' && index + 1 != value.size())) {
		return false;
	}
	const bool negative = value[0] == '-';
	const auto maximum = negative ? (uint64_t(1) << 63U) : static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
	uint64_t magnitude = 0;
	for (; index < value.size(); index++) {
		if (value[index] < '0' || value[index] > '9') {
			return false;
		}
		const auto digit = static_cast<uint64_t>(value[index] - '0');
		if (magnitude > (maximum - digit) / 10) {
			return false;
		}
		magnitude = magnitude * 10 + digit;
	}
	return true;
}

bool ValidateLiteral(const PlannedGraphqlLiteral &literal, std::size_t depth, std::size_t &nodes) {
	if (depth > 32 || nodes == std::numeric_limits<std::size_t>::max() || ++nodes > 100000) {
		return false;
	}
	const bool has_items = !literal.Items().empty();
	const bool has_fields = !literal.Fields().empty();
	switch (literal.Kind()) {
	case PlannedGraphqlLiteralKind::NULL_VALUE:
		return literal.Scalar().empty() && !has_items && !has_fields;
	case PlannedGraphqlLiteralKind::BOOLEAN:
		return (literal.Scalar() == "true" || literal.Scalar() == "false") && !has_items && !has_fields;
	case PlannedGraphqlLiteralKind::INTEGER:
		return IsCanonicalInteger(literal.Scalar()) && !has_items && !has_fields;
	case PlannedGraphqlLiteralKind::STRING:
		return literal.Scalar().size() <= 1048576 && IsValidUtf8(literal.Scalar()) && !has_items && !has_fields;
	case PlannedGraphqlLiteralKind::ENUM_VALUE:
		return IsGraphqlName(literal.Scalar()) && literal.Scalar() != "true" && literal.Scalar() != "false" &&
		       literal.Scalar() != "null" && !has_items && !has_fields;
	case PlannedGraphqlLiteralKind::LIST:
		if (!literal.Scalar().empty() || has_fields || literal.Items().size() > 4096) {
			return false;
		}
		for (const auto &item : literal.Items()) {
			if (!item || !ValidateLiteral(*item, depth + 1, nodes)) {
				return false;
			}
		}
		return true;
	case PlannedGraphqlLiteralKind::OBJECT: {
		if (!literal.Scalar().empty() || has_items || literal.Fields().size() > 4096) {
			return false;
		}
		std::set<std::string> names;
		for (const auto &field : literal.Fields()) {
			if (!IsGraphqlName(field.Name()) || !names.insert(field.Name()).second ||
			    !ValidateLiteral(field.Value(), depth + 1, nodes)) {
				return false;
			}
		}
		return true;
	}
	}
	return false;
}

bool ValidateRecipe(const PlannedGraphqlGeneratorRecipe &recipe) {
	if (recipe.Identity() != PlannedGraphqlGeneratorIdentity::PACKAGE_QUERY_GENERATOR_V1 ||
	    !IsGraphqlName(recipe.OperationName()) || recipe.RootPath().empty() || recipe.RootPath().size() > 16 ||
	    recipe.FixedArguments().size() > 64 || recipe.Selections().empty() || recipe.Selections().size() > 256 ||
	    recipe.NodesField() != "nodes" || !IsGraphqlName(recipe.PageInfoField()) ||
	    !IsGraphqlName(recipe.HasNextPageField()) || !IsGraphqlName(recipe.EndCursorField()) ||
	    recipe.PageInfoField() == recipe.NodesField() || recipe.HasNextPageField() == recipe.EndCursorField()) {
		return false;
	}
	if (recipe.Variables().size() != 2 || recipe.Variables()[0].Role() != PlannedGraphqlRecipeVariableRole::PAGE_SIZE ||
	    recipe.Variables()[0].Type() != PlannedGraphqlRecipeVariableType::INT_NON_NULL ||
	    recipe.Variables()[1].Role() != PlannedGraphqlRecipeVariableRole::CURSOR ||
	    recipe.Variables()[1].Type() != PlannedGraphqlRecipeVariableType::STRING_NULLABLE ||
	    !IsGraphqlName(recipe.Variables()[0].Name()) || !IsGraphqlName(recipe.Variables()[1].Name()) ||
	    !IsGraphqlName(recipe.Variables()[0].ArgumentName()) || !IsGraphqlName(recipe.Variables()[1].ArgumentName()) ||
	    recipe.Variables()[0].Name() == recipe.Variables()[1].Name() ||
	    recipe.Variables()[0].ArgumentName() == recipe.Variables()[1].ArgumentName()) {
		return false;
	}

	std::set<std::string> reserved = {"data",
	                                  "errors",
	                                  recipe.NodesField(),
	                                  recipe.PageInfoField(),
	                                  recipe.HasNextPageField(),
	                                  recipe.EndCursorField()};
	std::set<std::string> roots;
	for (const auto &name : recipe.RootPath()) {
		if (!IsGraphqlName(name) || reserved.count(name) != 0 || !roots.insert(name).second) {
			return false;
		}
	}
	std::set<std::string> arguments = {recipe.Variables()[0].ArgumentName(), recipe.Variables()[1].ArgumentName()};
	std::size_t literal_nodes = 0;
	for (const auto &argument : recipe.FixedArguments()) {
		if (!IsGraphqlName(argument.Name()) || !arguments.insert(argument.Name()).second ||
		    !ValidateLiteral(argument.Value(), 1, literal_nodes)) {
			return false;
		}
	}
	std::set<std::string> paths;
	std::set<std::string> nested_parents;
	std::set<std::string> columns;
	for (const auto &selection : recipe.Selections()) {
		if (!IsSafeLogicalId(selection.ColumnName()) || !columns.insert(selection.ColumnName()).second ||
		    selection.FieldPath().empty() || selection.FieldPath().size() > 2) {
			return false;
		}
		std::string path;
		for (const auto &name : selection.FieldPath()) {
			if (!IsGraphqlName(name)) {
				return false;
			}
			path += (path.empty() ? "" : "\n") + name;
		}
		if (!paths.insert(path).second ||
		    (selection.FieldPath().size() == 2 && !nested_parents.insert(selection.FieldPath()[0]).second) ||
		    (selection.FieldPath().size() == 1 && nested_parents.count(selection.FieldPath()[0]) != 0) ||
		    (selection.FieldPath().size() == 2 && paths.count(selection.FieldPath()[0]) != 0)) {
			return false;
		}
	}
	return true;
}

class BoundedDocument final {
public:
	explicit BoundedDocument(std::size_t maximum_p) : maximum(maximum_p) {
		value.reserve(maximum < 4096 ? maximum : 4096);
	}

	bool Append(const std::string &part) {
		if (value.size() > maximum || part.size() > maximum - value.size()) {
			return false;
		}
		value += part;
		return true;
	}

	bool Append(const char *part) {
		return Append(std::string(part));
	}

	bool Push(char part) {
		if (value.size() >= maximum) {
			return false;
		}
		value.push_back(part);
		return true;
	}

	std::string Release() {
		return std::move(value);
	}

private:
	const std::size_t maximum;
	std::string value;
};

bool RenderString(const std::string &value, BoundedDocument &result) {
	static const char HEX[] = "0123456789abcdef";
	if (!result.Push('"')) {
		return false;
	}
	for (const auto character : value) {
		const auto byte = static_cast<unsigned char>(character);
		switch (byte) {
		case '"':
			if (!result.Append("\\\""))
				return false;
			break;
		case '\\':
			if (!result.Append("\\\\"))
				return false;
			break;
		case '\b':
			if (!result.Append("\\b"))
				return false;
			break;
		case '\f':
			if (!result.Append("\\f"))
				return false;
			break;
		case '\n':
			if (!result.Append("\\n"))
				return false;
			break;
		case '\r':
			if (!result.Append("\\r"))
				return false;
			break;
		case '\t':
			if (!result.Append("\\t"))
				return false;
			break;
		default:
			if (byte < 0x20U) {
				if (!result.Append("\\u00") || !result.Push(HEX[(byte >> 4U) & 0x0fU]) ||
				    !result.Push(HEX[byte & 0x0fU])) {
					return false;
				}
			} else if (!result.Push(character)) {
				return false;
			}
		}
	}
	return result.Push('"');
}

bool RenderLiteral(const PlannedGraphqlLiteral &literal, BoundedDocument &result) {
	switch (literal.Kind()) {
	case PlannedGraphqlLiteralKind::NULL_VALUE:
		return result.Append("null");
	case PlannedGraphqlLiteralKind::BOOLEAN:
	case PlannedGraphqlLiteralKind::INTEGER:
	case PlannedGraphqlLiteralKind::ENUM_VALUE:
		return result.Append(literal.Scalar());
	case PlannedGraphqlLiteralKind::STRING:
		return RenderString(literal.Scalar(), result);
	case PlannedGraphqlLiteralKind::LIST:
		if (!result.Push('['))
			return false;
		for (std::size_t index = 0; index < literal.Items().size(); index++) {
			if ((index != 0 && !result.Append(", ")) || !RenderLiteral(*literal.Items()[index], result))
				return false;
		}
		return result.Push(']');
	case PlannedGraphqlLiteralKind::OBJECT:
		if (!result.Push('{'))
			return false;
		for (std::size_t index = 0; index < literal.Fields().size(); index++) {
			if ((index != 0 && !result.Append(", ")) || !result.Append(literal.Fields()[index].Name()) ||
			    !result.Append(": ") || !RenderLiteral(literal.Fields()[index].Value(), result))
				return false;
		}
		return result.Push('}');
	}
	return false;
}

bool AppendIndent(BoundedDocument &document, std::size_t depth) {
	return document.Append(std::string(depth * 2, ' '));
}

bool RenderRecipe(const PlannedGraphqlGeneratorRecipe &recipe, std::size_t maximum, std::string &output) {
	const auto &page = recipe.Variables()[0];
	const auto &cursor = recipe.Variables()[1];
	BoundedDocument document(maximum);
	if (!document.Append("query ") || !document.Append(recipe.OperationName()) || !document.Append("($") ||
	    !document.Append(page.Name()) || !document.Append(": Int!, $") || !document.Append(cursor.Name()) ||
	    !document.Append(": String) {\n"))
		return false;
	for (std::size_t index = 0; index + 1 < recipe.RootPath().size(); index++) {
		if (!AppendIndent(document, index + 1) || !document.Append(recipe.RootPath()[index]) ||
		    !document.Append(" {\n"))
			return false;
	}
	const auto connection_depth = recipe.RootPath().size();
	if (!AppendIndent(document, connection_depth) || !document.Append(recipe.RootPath().back()) ||
	    !document.Append("(\n") || !AppendIndent(document, connection_depth + 1) ||
	    !document.Append(page.ArgumentName()) || !document.Append(": $") || !document.Append(page.Name()) ||
	    !document.Push('\n') || !AppendIndent(document, connection_depth + 1) ||
	    !document.Append(cursor.ArgumentName()) || !document.Append(": $") || !document.Append(cursor.Name()) ||
	    !document.Push('\n'))
		return false;
	for (const auto &argument : recipe.FixedArguments()) {
		if (!AppendIndent(document, connection_depth + 1) || !document.Append(argument.Name()) ||
		    !document.Append(": ") || !RenderLiteral(argument.Value(), document) || !document.Push('\n'))
			return false;
	}
	if (!AppendIndent(document, connection_depth) || !document.Append(") {\n") ||
	    !AppendIndent(document, connection_depth + 1) || !document.Append(recipe.NodesField()) ||
	    !document.Append(" {\n"))
		return false;
	for (const auto &selection : recipe.Selections()) {
		if (!AppendIndent(document, connection_depth + 2) || !document.Append(selection.FieldPath()[0]))
			return false;
		if (selection.FieldPath().size() == 2 &&
		    (!document.Append(" { ") || !document.Append(selection.FieldPath()[1]) || !document.Append(" }")))
			return false;
		if (!document.Push('\n'))
			return false;
	}
	if (!AppendIndent(document, connection_depth + 1) || !document.Append("}\n") ||
	    !AppendIndent(document, connection_depth + 1) || !document.Append(recipe.PageInfoField()) ||
	    !document.Append(" { ") || !document.Append(recipe.HasNextPageField()) || !document.Push(' ') ||
	    !document.Append(recipe.EndCursorField()) || !document.Append(" }\n"))
		return false;
	for (std::size_t depth = connection_depth; depth > 0; depth--) {
		if (!AppendIndent(document, depth) || !document.Append("}\n"))
			return false;
	}
	if (!document.Push('}'))
		return false;
	output = document.Release();
	return true;
}

} // namespace

bool TryRenderPackageGraphqlRecipe(const PlannedGraphqlGeneratorRecipe &recipe, uint64_t max_rendered_bytes,
                                   std::string &document) {
	document.clear();
	if (max_rendered_bytes == 0 || max_rendered_bytes > 65536 ||
	    max_rendered_bytes > static_cast<uint64_t>(std::numeric_limits<std::size_t>::max()) ||
	    !ValidateRecipe(recipe)) {
		return false;
	}
	return RenderRecipe(recipe, static_cast<std::size_t>(max_rendered_bytes), document);
}

} // namespace internal
} // namespace cuac
