#include "cuac/internal/connector/compiler/package_compiler_internal.hpp"
#include "cuac/internal/connector/compiler/package_schema_helpers.hpp"

#include "cuac/connector/content_digest.hpp"
#include "cuac/internal/connector/model/compiled_model_builder.hpp"
#include "cuac/internal/connector/model/graphql_query_recipe.hpp"

#include <set>
#include <sstream>
#include <stdexcept>

namespace cuac {
namespace connector {
namespace internal {

namespace {

void AddProfileError(const RelationDeclaration &relation, const OperationDeclaration &operation, const SourceMark &mark,
                     PackageDiagnosticSink &diagnostics) {
	diagnostics.Add(PackageDiagnosticCode::INVALID_GRAPHQL_PROFILE, PackageDiagnosticPhase::COMPILE, mark, "",
	                relation.id.value, operation.id.value);
}

std::string RenderGraphqlString(const std::string &value) {
	static const char hex[] = "0123456789abcdef";
	std::string result;
	result.push_back('"');
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
			break;
		}
	}
	result.push_back('"');
	return result;
}

bool RenderLiteral(const GraphqlLiteralDeclaration &literal, std::string &result) {
	switch (literal.kind) {
	case GraphqlLiteralDeclaration::Kind::NULL_VALUE:
		result = "null";
		return true;
	case GraphqlLiteralDeclaration::Kind::BOOLEAN:
	case GraphqlLiteralDeclaration::Kind::INTEGER:
	case GraphqlLiteralDeclaration::Kind::ENUM_VALUE:
		result = literal.scalar.value;
		return true;
	case GraphqlLiteralDeclaration::Kind::STRING:
		result = RenderGraphqlString(literal.scalar.value);
		return true;
	case GraphqlLiteralDeclaration::Kind::LIST: {
		result = "[";
		for (std::size_t index = 0; index < literal.items.size(); index++) {
			if (!literal.items[index]) {
				return false;
			}
			std::string item;
			if (!RenderLiteral(*literal.items[index], item)) {
				return false;
			}
			result += (index == 0 ? "" : ", ") + item;
		}
		result += "]";
		return true;
	}
	case GraphqlLiteralDeclaration::Kind::OBJECT: {
		std::set<std::string> names;
		result = "{";
		for (std::size_t index = 0; index < literal.fields.size(); index++) {
			const auto &field = literal.fields[index];
			if (!field.value || !names.insert(field.name.value).second) {
				return false;
			}
			std::string value;
			if (!RenderLiteral(*field.value, value)) {
				return false;
			}
			result += (index == 0 ? "" : ", ") + field.name.value + ": " + value;
		}
		result += "}";
		return true;
	}
	}
	return false;
}

std::shared_ptr<const CompiledGraphqlLiteral> CompileLiteral(const GraphqlLiteralDeclaration &literal) {
	using cuac::internal::CompiledModelBuilder;
	switch (literal.kind) {
	case GraphqlLiteralDeclaration::Kind::NULL_VALUE:
		return std::make_shared<const CompiledGraphqlLiteral>(CompiledModelBuilder::GraphqlNullLiteral());
	case GraphqlLiteralDeclaration::Kind::BOOLEAN:
		return std::make_shared<const CompiledGraphqlLiteral>(
		    CompiledModelBuilder::GraphqlBooleanLiteral(literal.scalar.value == "true"));
	case GraphqlLiteralDeclaration::Kind::INTEGER: {
		std::int64_t parsed = 0;
		if (!IsCanonicalSigned(literal.scalar, parsed)) {
			return nullptr;
		}
		return std::make_shared<const CompiledGraphqlLiteral>(CompiledModelBuilder::GraphqlIntegerLiteral(parsed));
	}
	case GraphqlLiteralDeclaration::Kind::STRING:
		return std::make_shared<const CompiledGraphqlLiteral>(
		    CompiledModelBuilder::GraphqlStringLiteral(literal.scalar.value));
	case GraphqlLiteralDeclaration::Kind::ENUM_VALUE:
		return std::make_shared<const CompiledGraphqlLiteral>(
		    CompiledModelBuilder::GraphqlEnumLiteral(literal.scalar.value));
	case GraphqlLiteralDeclaration::Kind::LIST: {
		std::vector<std::shared_ptr<const CompiledGraphqlLiteral>> items;
		for (const auto &item : literal.items) {
			if (!item) {
				return nullptr;
			}
			auto compiled = CompileLiteral(*item);
			if (!compiled) {
				return nullptr;
			}
			items.push_back(std::move(compiled));
		}
		return std::make_shared<const CompiledGraphqlLiteral>(
		    CompiledModelBuilder::GraphqlListLiteral(std::move(items)));
	}
	case GraphqlLiteralDeclaration::Kind::OBJECT: {
		std::vector<CompiledGraphqlObjectField> fields;
		for (const auto &field : literal.fields) {
			if (!field.value) {
				return nullptr;
			}
			auto compiled = CompileLiteral(*field.value);
			if (!compiled) {
				return nullptr;
			}
			fields.push_back(CompiledModelBuilder::GraphqlObjectField(field.name.value, *compiled));
		}
		return std::make_shared<const CompiledGraphqlLiteral>(
		    CompiledModelBuilder::GraphqlObjectLiteral(std::move(fields)));
	}
	}
	return nullptr;
}

bool ValidateSelection(const RelationDeclaration &relation, const OperationDeclaration &operation,
                       PackageDiagnosticSink &diagnostics) {
	const auto &query = operation.graphql_request.query;
	if (query.selection.size() != relation.columns.size()) {
		AddProfileError(relation, operation, query.mark, diagnostics);
		return false;
	}
	std::set<std::string> paths;
	std::set<std::string> nested_parents;
	for (std::size_t index = 0; index < query.selection.size(); index++) {
		const auto &selection = query.selection[index];
		if (selection.column.value != relation.columns[index].id.value || selection.field_path.empty() ||
		    selection.field_path.size() > 2) {
			AddProfileError(relation, operation, selection.mark, diagnostics);
			return false;
		}
		std::string identity;
		for (const auto &segment : selection.field_path) {
			identity += (identity.empty() ? "" : "\n") + segment.value;
		}
		if (!paths.insert(identity).second ||
		    (selection.field_path.size() == 2 && !nested_parents.insert(selection.field_path[0].value).second) ||
		    (selection.field_path.size() == 1 && nested_parents.count(selection.field_path[0].value) != 0)) {
			AddProfileError(relation, operation, selection.mark, diagnostics);
			return false;
		}
		if (selection.field_path.size() == 2 && paths.count(selection.field_path[0].value) != 0) {
			AddProfileError(relation, operation, selection.mark, diagnostics);
			return false;
		}
	}
	return true;
}

bool ValidateNamesAndRoles(const RelationDeclaration &relation, const OperationDeclaration &operation,
                           PackageDiagnosticSink &diagnostics) {
	const auto &request = operation.graphql_request;
	const auto &query = request.query;
	const auto &pagination = request.pagination;
	if (pagination.page_size_variable.value == pagination.cursor_variable.value ||
	    pagination.page_size_argument.value == pagination.cursor_argument.value ||
	    pagination.page_info_field.value == "nodes" ||
	    pagination.has_next_page_field.value == pagination.end_cursor_field.value) {
		AddProfileError(relation, operation, pagination.mark, diagnostics);
		return false;
	}
	std::set<std::string> root_names;
	const std::set<std::string> reserved = {"data",
	                                        "errors",
	                                        "nodes",
	                                        pagination.page_info_field.value,
	                                        pagination.has_next_page_field.value,
	                                        pagination.end_cursor_field.value};
	for (const auto &name : query.root) {
		if (!root_names.insert(name.value).second || reserved.count(name.value) != 0) {
			AddProfileError(relation, operation, name.mark, diagnostics);
			return false;
		}
	}
	std::set<std::string> arguments = {pagination.page_size_argument.value, pagination.cursor_argument.value};
	for (const auto &argument : query.fixed_arguments) {
		if (!arguments.insert(argument.name.value).second || !argument.value) {
			AddProfileError(relation, operation, argument.mark, diagnostics);
			return false;
		}
		std::string ignored;
		if (!RenderLiteral(*argument.value, ignored)) {
			AddProfileError(relation, operation, argument.mark, diagnostics);
			return false;
		}
	}
	return true;
}

// This parser backstop deliberately validates only the closed bytes emitted by
// this renderer: one query, strings with escapes, and balanced GraphQL
// punctuation. It cannot admit a caller-provided document or grant authority.
bool ValidateGeneratedDocument(const std::string &document) {
	if (document.compare(0, 6, "query ") != 0 || document.empty() || document.back() == '\n' ||
	    document.find('\r') != std::string::npos) {
		return false;
	}
	std::vector<char> stack;
	bool string = false;
	bool escaped = false;
	for (std::size_t index = 0; index < document.size(); index++) {
		const char value = document[index];
		if (value == '\n' && index > 0 && (document[index - 1] == ' ' || document[index - 1] == '\t')) {
			return false;
		}
		if (string) {
			if (escaped) {
				escaped = false;
			} else if (value == '\\') {
				escaped = true;
			} else if (value == '"') {
				string = false;
			} else if (static_cast<unsigned char>(value) < 0x20U) {
				return false;
			}
			continue;
		}
		if (value == '"') {
			string = true;
			continue;
		}
		if (value == '{' || value == '(' || value == '[') {
			stack.push_back(value);
		} else if (value == '}' || value == ')' || value == ']') {
			if (stack.empty()) {
				return false;
			}
			const char expected = value == '}' ? '{' : (value == ')' ? '(' : '[');
			if (stack.back() != expected) {
				return false;
			}
			stack.pop_back();
		}
	}
	return !string && stack.empty();
}

} // namespace

bool RenderGraphqlOperation(const RelationDeclaration &relation, const OperationDeclaration &operation,
                            PackageDiagnosticSink &diagnostics, RenderedGraphqlOperation &rendered) {
	if (!operation.graphql || !ValidateNamesAndRoles(relation, operation, diagnostics) ||
	    !ValidateSelection(relation, operation, diagnostics)) {
		return false;
	}
	const auto &request = operation.graphql_request;
	const auto &query = request.query;
	const auto &pagination = request.pagination;
	std::vector<CompiledGraphqlRecipeVariable> variables;
	variables.push_back(cuac::internal::CompiledModelBuilder::GraphqlRecipeVariable(
	    pagination.page_size_variable.value, CompiledGraphqlVariableType::INT_NON_NULL,
	    CompiledGraphqlRecipeVariableRole::PAGE_SIZE, pagination.page_size_argument.value));
	variables.push_back(cuac::internal::CompiledModelBuilder::GraphqlRecipeVariable(
	    pagination.cursor_variable.value, CompiledGraphqlVariableType::STRING_NULLABLE,
	    CompiledGraphqlRecipeVariableRole::CURSOR, pagination.cursor_argument.value));
	std::vector<std::string> root;
	for (const auto &segment : query.root) {
		root.push_back(segment.value);
	}
	std::vector<CompiledGraphqlFixedArgument> arguments;
	for (const auto &argument : query.fixed_arguments) {
		if (!argument.value) {
			AddProfileError(relation, operation, argument.mark, diagnostics);
			return false;
		}
		auto literal = CompileLiteral(*argument.value);
		if (!literal) {
			AddProfileError(relation, operation, argument.mark, diagnostics);
			return false;
		}
		arguments.push_back(cuac::internal::CompiledModelBuilder::GraphqlFixedArgument(argument.name.value, *literal));
	}
	std::vector<CompiledGraphqlSelection> selections;
	for (const auto &selection : query.selection) {
		std::vector<std::string> path;
		for (const auto &segment : selection.field_path) {
			path.push_back(segment.value);
		}
		selections.push_back(
		    cuac::internal::CompiledModelBuilder::GraphqlSelection(selection.column.value, std::move(path)));
	}
	try {
		rendered.query_recipe = cuac::internal::CompiledModelBuilder::GraphqlQueryRecipe(
		    CompiledGraphqlDocumentIdentity::PACKAGE_QUERY_GENERATOR_V1, query.operation_name.value,
		    std::move(variables), std::move(root), std::move(arguments), "nodes", std::move(selections),
		    pagination.page_info_field.value, pagination.has_next_page_field.value, pagination.end_cursor_field.value);
		rendered.document = cuac::internal::RenderCompiledGraphqlQueryRecipe(*rendered.query_recipe);
	} catch (const std::invalid_argument &) {
		AddProfileError(relation, operation, query.mark, diagnostics);
		return false;
	}

	std::uint64_t max_document_bytes = 0;
	if (!IsCanonicalUnsigned(request.max_document_bytes, max_document_bytes) || max_document_bytes > 65536 ||
	    rendered.document.size() > max_document_bytes || !ValidateGeneratedDocument(rendered.document)) {
		AddProfileError(relation, operation, request.max_document_bytes.mark, diagnostics);
		return false;
	}

	rendered.result_columns.reserve(relation.columns.size());
	for (std::size_t index = 0; index < relation.columns.size(); index++) {
		const auto &column = relation.columns[index];
		const auto &selection = query.selection[index];
		const auto &element_type = column.type.value == "ARRAY" ? column.element_type : column.type;
		// RFC 0020: DOUBLE has no GraphQL-side representation today (GraphQL
		// relations render/decode only STRING/INT64/BOOLEAN); reject rather
		// than silently decode a DOUBLE column as STRING, mirroring RFC
		// 0018's precedent of rejecting a REST-only capability on GraphQL
		// operations with a precise compile-time diagnostic instead of a
		// silent runtime mismatch.
		if (element_type.value == "DOUBLE") {
			AddProfileError(relation, operation, element_type.mark, diagnostics);
			return false;
		}
		CompiledGraphqlScalarKind kind = CompiledGraphqlScalarKind::STRING;
		if (element_type.value == "BIGINT") {
			kind = CompiledGraphqlScalarKind::INT64;
		} else if (element_type.value == "BOOLEAN") {
			kind = CompiledGraphqlScalarKind::BOOLEAN;
		}
		bool nullable = false;
		(void)IsPlainBoolean(column.nullable, nullable);
		bool element_nullable = false;
		if (column.type.value == "ARRAY") {
			(void)IsPlainBoolean(column.element_nullable, element_nullable);
		}
		std::vector<std::string> path;
		for (const auto &segment : selection.field_path) {
			path.push_back(segment.value);
		}
		rendered.result_columns.push_back(
		    {column.id.value,
		     kind,
		     nullable,
		     {std::move(path)},
		     column.type.value == "ARRAY" ? CompiledResultShape::ARRAY : CompiledResultShape::SCALAR,
		     element_nullable});
	}

	std::vector<std::string> base = {"data"};
	for (const auto &segment : query.root) {
		base.push_back(segment.value);
	}
	auto nodes = base;
	nodes.push_back("nodes");
	auto page_info = base;
	page_info.push_back(pagination.page_info_field.value);
	auto has_next = page_info;
	has_next.push_back(pagination.has_next_page_field.value);
	auto end_cursor = page_info;
	end_cursor.push_back(pagination.end_cursor_field.value);
	rendered.response = {
	    {std::move(nodes)}, {{"errors"}}, {std::move(page_info)}, CompiledGraphqlPartialDataPolicy::FAIL_ON_ANY_ERROR};
	std::uint64_t page_size = 0;
	std::uint64_t max_pages = 0;
	(void)IsCanonicalUnsigned(pagination.page_size, page_size);
	(void)IsCanonicalUnsigned(pagination.max_pages_per_scan, max_pages);
	rendered.cursor = {CompiledGraphqlCursorDirection::FORWARD,
	                   CompiledGraphqlCursorDependency::SEQUENTIAL,
	                   CompiledGraphqlCursorConsistency::MUTABLE,
	                   false,
	                   false,
	                   1,
	                   pagination.page_size_variable.value,
	                   page_size,
	                   pagination.cursor_variable.value,
	                   {std::move(has_next)},
	                   {std::move(end_cursor)},
	                   max_pages};
	return true;
}

} // namespace internal
} // namespace connector
} // namespace cuac
