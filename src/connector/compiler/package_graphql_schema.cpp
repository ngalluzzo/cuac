#include "cuac/internal/connector/compiler/package_relation_schema_parts.hpp"

namespace cuac {
namespace connector {
namespace internal {

namespace {

void RequireValue(const LocatedText &value, const char *expected, PackageDiagnosticCode code,
                  PackageDiagnosticPhase phase, PackageDiagnosticSink &diagnostics) {
	if (value.value != expected) {
		diagnostics.Add(code, phase, value.mark);
	}
}

void RequireIdentifier(const LocatedText &value, PackageDiagnosticSink &diagnostics) {
	if (!IsIdentifier(value.value)) {
		diagnostics.Add(PackageDiagnosticCode::INVALID_IDENTIFIER, PackageDiagnosticPhase::SCHEMA, value.mark);
	}
}

void RequireGraphqlName(const LocatedText &value, PackageDiagnosticSink &diagnostics) {
	if (!IsGraphqlName(value.value)) {
		diagnostics.Add(PackageDiagnosticCode::INVALID_GRAPHQL_PROFILE, PackageDiagnosticPhase::COMPILE, value.mark);
	}
}

std::shared_ptr<GraphqlLiteralDeclaration> DecodeLiteral(const SchemaReader &reader, std::uint64_t depth) {
	auto literal = std::make_shared<GraphqlLiteralDeclaration>();
	literal->mark = reader.Mark();
	if (depth > 32 || !reader.RequireMapping({"null", "boolean", "integer", "string", "enum", "list", "object"}, {})) {
		reader.Diagnostics().Add(PackageDiagnosticCode::INVALID_GRAPHQL_PROFILE, PackageDiagnosticPhase::COMPILE,
		                         reader.Mark());
		return literal;
	}
	std::vector<std::string> present;
	for (const auto *name : {"null", "boolean", "integer", "string", "enum", "list", "object"}) {
		if (reader.Field(name) != nullptr) {
			present.push_back(name);
		}
	}
	if (present.size() != 1) {
		reader.Diagnostics().Add(PackageDiagnosticCode::INVALID_TYPE, PackageDiagnosticPhase::SCHEMA, reader.Mark());
		return literal;
	}
	const auto &kind = present.front();
	if (kind == "list") {
		literal->kind = GraphqlLiteralDeclaration::Kind::LIST;
		const auto *items = reader.Sequence("list", 0, 4096);
		if (items != nullptr) {
			for (std::size_t index = 0; index < items->Size(); index++) {
				literal->items.push_back(DecodeLiteral(
				    reader.Child(items->SequenceValue(index), ".list[" + std::to_string(index) + "]"), depth + 1));
			}
		}
		return literal;
	}
	if (kind == "object") {
		literal->kind = GraphqlLiteralDeclaration::Kind::OBJECT;
		const auto *fields = reader.Sequence("object", 0, 4096);
		if (fields != nullptr) {
			for (std::size_t index = 0; index < fields->Size(); index++) {
				auto field_reader =
				    reader.Child(fields->SequenceValue(index), ".object[" + std::to_string(index) + "]");
				field_reader.RequireMapping({"name", "value"}, {"name", "value"});
				GraphqlObjectFieldDeclaration field;
				field.name = field_reader.Text("name");
				field.mark = field_reader.Mark();
				RequireGraphqlName(field.name, reader.Diagnostics());
				if (field_reader.Field("value") != nullptr) {
					field.value = DecodeLiteral(field_reader.Child("value"), depth + 1);
				}
				literal->fields.push_back(std::move(field));
			}
		}
		return literal;
	}
	literal->scalar = reader.Text(kind);
	if (kind == "null") {
		literal->kind = GraphqlLiteralDeclaration::Kind::NULL_VALUE;
		RequireValue(literal->scalar, "true", PackageDiagnosticCode::INVALID_TYPE, PackageDiagnosticPhase::SCHEMA,
		             reader.Diagnostics());
	} else if (kind == "boolean") {
		literal->kind = GraphqlLiteralDeclaration::Kind::BOOLEAN;
		bool ignored = false;
		if (!IsPlainBoolean(literal->scalar, ignored)) {
			reader.Diagnostics().Add(PackageDiagnosticCode::INVALID_TYPE, PackageDiagnosticPhase::SCHEMA,
			                         literal->scalar.mark);
		}
	} else if (kind == "integer") {
		literal->kind = GraphqlLiteralDeclaration::Kind::INTEGER;
		std::int64_t ignored = 0;
		if (!IsCanonicalSigned(literal->scalar, ignored)) {
			reader.Diagnostics().Add(PackageDiagnosticCode::INVALID_TYPE, PackageDiagnosticPhase::SCHEMA,
			                         literal->scalar.mark);
		}
	} else if (kind == "string") {
		literal->kind = GraphqlLiteralDeclaration::Kind::STRING;
		if (literal->scalar.style != FailsafeYamlNode::ScalarStyle::DOUBLE_QUOTED) {
			reader.Diagnostics().Add(PackageDiagnosticCode::INVALID_TYPE, PackageDiagnosticPhase::SCHEMA,
			                         literal->scalar.mark);
		}
	} else {
		literal->kind = GraphqlLiteralDeclaration::Kind::ENUM_VALUE;
		RequireGraphqlName(literal->scalar, reader.Diagnostics());
	}
	return literal;
}

GraphqlQueryDeclaration DecodeQuery(const SchemaReader &reader) {
	GraphqlQueryDeclaration query;
	reader.RequireMapping({"operation_name", "root", "fixed_arguments", "selection"},
	                      {"operation_name", "root", "fixed_arguments", "selection"});
	query.operation_name = reader.Text("operation_name");
	query.root = reader.TextSequence("root", 1, 16);
	query.mark = reader.Mark();
	RequireGraphqlName(query.operation_name, reader.Diagnostics());
	for (const auto &name : query.root) {
		RequireGraphqlName(name, reader.Diagnostics());
	}
	const auto *arguments = reader.Sequence("fixed_arguments", 0, 64);
	if (arguments != nullptr) {
		for (std::size_t index = 0; index < arguments->Size(); index++) {
			auto child =
			    reader.Child(arguments->SequenceValue(index), ".fixed_arguments[" + std::to_string(index) + "]");
			child.RequireMapping({"name", "value"}, {"name", "value"});
			GraphqlFixedArgumentDeclaration argument;
			argument.name = child.Text("name");
			argument.mark = child.Mark();
			RequireGraphqlName(argument.name, reader.Diagnostics());
			if (child.Field("value") != nullptr) {
				argument.value = DecodeLiteral(child.Child("value"), 1);
			}
			query.fixed_arguments.push_back(std::move(argument));
		}
	}
	const auto *selections = reader.Sequence("selection", 1, 256);
	if (selections != nullptr) {
		for (std::size_t index = 0; index < selections->Size(); index++) {
			auto child = reader.Child(selections->SequenceValue(index), ".selection[" + std::to_string(index) + "]");
			child.RequireMapping({"column", "field_path"}, {"column", "field_path"});
			GraphqlSelectionDeclaration selection;
			selection.column = child.Text("column");
			selection.field_path = child.TextSequence("field_path", 1, 2);
			selection.mark = child.Mark();
			RequireIdentifier(selection.column, reader.Diagnostics());
			for (const auto &name : selection.field_path) {
				RequireGraphqlName(name, reader.Diagnostics());
			}
			query.selection.push_back(std::move(selection));
		}
	}
	return query;
}

GraphqlPaginationDeclaration DecodePagination(const SchemaReader &reader) {
	GraphqlPaginationDeclaration pagination;
	reader.RequireMapping({"strategy", "dependency", "consistency", "page_size_argument", "page_size_variable",
	                       "page_size", "cursor_argument", "cursor_variable", "page_info_field", "has_next_page_field",
	                       "end_cursor_field", "max_pages_per_scan", "max_concurrent_pages"},
	                      {"strategy", "dependency", "consistency", "page_size_argument", "page_size_variable",
	                       "page_size", "cursor_argument", "cursor_variable", "page_info_field", "has_next_page_field",
	                       "end_cursor_field", "max_pages_per_scan", "max_concurrent_pages"});
	pagination.strategy = reader.Text("strategy");
	pagination.dependency = reader.Text("dependency");
	pagination.consistency = reader.Text("consistency");
	pagination.page_size_argument = reader.Text("page_size_argument");
	pagination.page_size_variable = reader.Text("page_size_variable");
	pagination.page_size = reader.Text("page_size");
	pagination.cursor_argument = reader.Text("cursor_argument");
	pagination.cursor_variable = reader.Text("cursor_variable");
	pagination.page_info_field = reader.Text("page_info_field");
	pagination.has_next_page_field = reader.Text("has_next_page_field");
	pagination.end_cursor_field = reader.Text("end_cursor_field");
	pagination.max_pages_per_scan = reader.Text("max_pages_per_scan");
	pagination.max_concurrent_pages = reader.Text("max_concurrent_pages");
	pagination.mark = reader.Mark();
	RequireValue(pagination.strategy, "relay_forward", PackageDiagnosticCode::UNSUPPORTED_DECLARATION,
	             PackageDiagnosticPhase::SCHEMA, reader.Diagnostics());
	RequireValue(pagination.dependency, "sequential", PackageDiagnosticCode::UNSUPPORTED_DECLARATION,
	             PackageDiagnosticPhase::SCHEMA, reader.Diagnostics());
	RequireValue(pagination.consistency, "mutable", PackageDiagnosticCode::UNSUPPORTED_DECLARATION,
	             PackageDiagnosticPhase::SCHEMA, reader.Diagnostics());
	RequireValue(pagination.max_concurrent_pages, "1", PackageDiagnosticCode::POLICY_WIDENING,
	             PackageDiagnosticPhase::COMPILE, reader.Diagnostics());
	for (const auto *name : {&pagination.page_size_argument, &pagination.page_size_variable,
	                         &pagination.cursor_argument, &pagination.cursor_variable, &pagination.page_info_field,
	                         &pagination.has_next_page_field, &pagination.end_cursor_field}) {
		RequireGraphqlName(*name, reader.Diagnostics());
	}
	std::uint64_t ignored = 0;
	if (!IsCanonicalUnsigned(pagination.page_size, ignored) ||
	    !IsCanonicalUnsigned(pagination.max_pages_per_scan, ignored)) {
		reader.Diagnostics().Add(PackageDiagnosticCode::INVALID_TYPE, PackageDiagnosticPhase::SCHEMA, pagination.mark);
	}
	return pagination;
}

} // namespace

GraphqlRequestDeclaration DecodeGraphqlRequestSchema(const SchemaReader &reader) {
	GraphqlRequestDeclaration request;
	reader.RequireMapping(
	    {"protocol", "endpoint", "headers", "query", "pagination", "partial_data", "max_document_bytes",
	     "max_serialized_body_bytes_per_request", "max_serialized_body_bytes_per_scan"},
	    {"protocol", "endpoint", "headers", "query", "pagination", "partial_data", "max_document_bytes",
	     "max_serialized_body_bytes_per_request", "max_serialized_body_bytes_per_scan"});
	request.protocol = reader.Text("protocol");
	auto endpoint = reader.Child("endpoint");
	endpoint.RequireMapping({"origin", "path"}, {"origin", "path"});
	request.origin = DecodeHttpOrigin(endpoint.Child("origin"));
	request.path = endpoint.Text("path");
	request.headers = DecodeHttpHeaders(reader);
	request.query = DecodeQuery(reader.Child("query"));
	request.pagination = DecodePagination(reader.Child("pagination"));
	request.partial_data = reader.Text("partial_data");
	request.max_document_bytes = reader.Text("max_document_bytes");
	request.max_serialized_body_bytes_per_request = reader.Text("max_serialized_body_bytes_per_request");
	request.max_serialized_body_bytes_per_scan = reader.Text("max_serialized_body_bytes_per_scan");
	request.mark = reader.Mark();
	RequireValue(request.protocol, "graphql", PackageDiagnosticCode::UNSUPPORTED_DECLARATION,
	             PackageDiagnosticPhase::SCHEMA, reader.Diagnostics());
	RequireValue(request.partial_data, "fail_on_any_error", PackageDiagnosticCode::UNSUPPORTED_DECLARATION,
	             PackageDiagnosticPhase::SCHEMA, reader.Diagnostics());
	if (!IsFixedPath(request.path.value)) {
		reader.Diagnostics().Add(PackageDiagnosticCode::POLICY_WIDENING, PackageDiagnosticPhase::COMPILE,
		                         request.path.mark);
	}
	std::uint64_t ignored = 0;
	for (const auto *value : {&request.max_document_bytes, &request.max_serialized_body_bytes_per_request,
	                          &request.max_serialized_body_bytes_per_scan}) {
		if (!IsCanonicalUnsigned(*value, ignored)) {
			reader.Diagnostics().Add(PackageDiagnosticCode::INVALID_TYPE, PackageDiagnosticPhase::SCHEMA, value->mark);
		}
	}
	return request;
}

} // namespace internal
} // namespace connector
} // namespace cuac
