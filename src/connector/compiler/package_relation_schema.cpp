#include "cuac/internal/connector/compiler/package_relation_schema_parts.hpp"

#include <set>

namespace cuac {
namespace connector {
namespace internal {

namespace {

void RequireValue(const LocatedText &value, const char *expected, PackageDiagnosticCode code,
                  PackageDiagnosticSink &diagnostics) {
	if (value.value != expected) {
		diagnostics.Add(code, PackageDiagnosticPhase::SCHEMA, value.mark);
	}
}

void RequireIdentifier(const LocatedText &value, PackageDiagnosticSink &diagnostics) {
	if (!IsIdentifier(value.value)) {
		diagnostics.Add(PackageDiagnosticCode::INVALID_IDENTIFIER, PackageDiagnosticPhase::SCHEMA, value.mark);
	}
}

ColumnDeclaration DecodeColumn(const SchemaReader &reader) {
	ColumnDeclaration column;
	reader.RequireMapping({"id", "type", "element_type", "element_nullable", "nullable", "extract"},
	                      {"id", "type", "nullable", "extract"});
	column.id = reader.Text("id");
	column.type = reader.Text("type");
	column.element_type = reader.Text("element_type", false);
	column.element_nullable = reader.Text("element_nullable", false);
	column.nullable = reader.Text("nullable");
	column.extract = reader.Text("extract");
	column.mark = reader.Mark();
	RequireIdentifier(column.id, reader.Diagnostics());
	if (column.type.value == "ARRAY") {
		if (reader.Field("element_type") == nullptr) {
			reader.Diagnostics().Add(PackageDiagnosticCode::MISSING_FIELD, PackageDiagnosticPhase::SCHEMA,
			                         column.element_type.mark);
		} else if (column.element_type.value != "BOOLEAN" && column.element_type.value != "BIGINT" &&
		           column.element_type.value != "VARCHAR" && column.element_type.value != "DOUBLE" &&
		           column.element_type.value != "TIMESTAMPTZ") {
			reader.Diagnostics().Add(PackageDiagnosticCode::INVALID_TYPE, PackageDiagnosticPhase::SCHEMA,
			                         column.element_type.mark);
		}
		if (reader.Field("element_nullable") == nullptr) {
			reader.Diagnostics().Add(PackageDiagnosticCode::MISSING_FIELD, PackageDiagnosticPhase::SCHEMA,
			                         column.element_nullable.mark);
		} else {
			bool element_nullable = false;
			if (!IsPlainBoolean(column.element_nullable, element_nullable)) {
				reader.Diagnostics().Add(PackageDiagnosticCode::INVALID_TYPE, PackageDiagnosticPhase::SCHEMA,
				                         column.element_nullable.mark);
			}
		}
	} else if (column.type.value != "BOOLEAN" && column.type.value != "BIGINT" && column.type.value != "VARCHAR" &&
	           column.type.value != "DOUBLE" && column.type.value != "TIMESTAMPTZ") {
		reader.Diagnostics().Add(PackageDiagnosticCode::INVALID_TYPE, PackageDiagnosticPhase::SCHEMA, column.type.mark);
	} else {
		if (reader.Field("element_type") != nullptr) {
			reader.Diagnostics().Add(PackageDiagnosticCode::UNKNOWN_FIELD, PackageDiagnosticPhase::SCHEMA,
			                         column.element_type.mark);
		}
		if (reader.Field("element_nullable") != nullptr) {
			reader.Diagnostics().Add(PackageDiagnosticCode::UNKNOWN_FIELD, PackageDiagnosticPhase::SCHEMA,
			                         column.element_nullable.mark);
		}
	}
	bool nullable = false;
	if (!IsPlainBoolean(column.nullable, nullable)) {
		reader.Diagnostics().Add(PackageDiagnosticCode::INVALID_TYPE, PackageDiagnosticPhase::SCHEMA,
		                         column.nullable.mark);
	}
	if (!IsExtractor(column.extract.value, false)) {
		reader.Diagnostics().Add(PackageDiagnosticCode::INVALID_EXTRACTOR, PackageDiagnosticPhase::SCHEMA,
		                         column.extract.mark);
	}
	return column;
}

DefaultDeclaration DecodeDefault(const SchemaReader &reader) {
	DefaultDeclaration result;
	result.present = true;
	result.mark = reader.Mark();
	reader.RequireMapping({"kind", "value"}, {"kind"});
	result.kind = reader.Text("kind");
	result.value = reader.Text("value", false);
	if (result.kind.value == "null") {
		if (reader.Field("value") != nullptr) {
			reader.Diagnostics().Add(PackageDiagnosticCode::UNKNOWN_FIELD, PackageDiagnosticPhase::SCHEMA,
			                         result.value.mark);
		}
	} else if (result.kind.value == "value") {
		if (reader.Field("value") == nullptr) {
			reader.Diagnostics().Add(PackageDiagnosticCode::MISSING_FIELD, PackageDiagnosticPhase::SCHEMA,
			                         result.value.mark);
		}
	} else {
		reader.Diagnostics().Add(PackageDiagnosticCode::INVALID_TYPE, PackageDiagnosticPhase::SCHEMA, result.kind.mark);
	}
	return result;
}

InputDeclaration DecodeInput(const SchemaReader &reader) {
	InputDeclaration input;
	reader.RequireMapping({"id", "type", "nullable", "default"}, {"id", "type", "nullable"});
	input.id = reader.Text("id");
	input.type = reader.Text("type");
	input.nullable = reader.Text("nullable");
	input.mark = reader.Mark();
	input.default_value.present = false;
	if (reader.Field("default") != nullptr) {
		input.default_value = DecodeDefault(reader.Child("default"));
	}
	RequireIdentifier(input.id, reader.Diagnostics());
	if (input.id.value == "secret") {
		reader.Diagnostics().Add(PackageDiagnosticCode::RESERVED_INPUT, PackageDiagnosticPhase::SCHEMA, input.id.mark);
	}
	if (input.type.value != "BOOLEAN" && input.type.value != "BIGINT" && input.type.value != "VARCHAR" &&
	    input.type.value != "DOUBLE" && input.type.value != "TIMESTAMPTZ") {
		reader.Diagnostics().Add(PackageDiagnosticCode::INVALID_TYPE, PackageDiagnosticPhase::SCHEMA, input.type.mark);
	}
	bool nullable = false;
	if (!IsPlainBoolean(input.nullable, nullable)) {
		reader.Diagnostics().Add(PackageDiagnosticCode::INVALID_TYPE, PackageDiagnosticPhase::SCHEMA,
		                         input.nullable.mark);
	}
	return input;
}

AuthDeclaration DecodeAuth(const SchemaReader &reader) {
	AuthDeclaration auth;
	reader.RequireMapping({"mode", "credential"}, {"mode"});
	auth.mode = reader.Text("mode");
	auth.credential = reader.Text("credential", false);
	auth.mark = reader.Mark();
	if (auth.mode.value == "anonymous") {
		if (reader.Field("credential") != nullptr) {
			reader.Diagnostics().Add(PackageDiagnosticCode::UNKNOWN_FIELD, PackageDiagnosticPhase::SCHEMA,
			                         auth.credential.mark);
		}
	} else if (auth.mode.value == "credential") {
		if (reader.Field("credential") == nullptr) {
			reader.Diagnostics().Add(PackageDiagnosticCode::MISSING_FIELD, PackageDiagnosticPhase::SCHEMA,
			                         auth.credential.mark);
		} else {
			RequireIdentifier(auth.credential, reader.Diagnostics());
		}
	} else {
		reader.Diagnostics().Add(PackageDiagnosticCode::UNSUPPORTED_DECLARATION, PackageDiagnosticPhase::SCHEMA,
		                         auth.mode.mark);
	}
	return auth;
}

ResourceDeclaration DecodeResources(const SchemaReader &reader) {
	ResourceDeclaration resources;
	reader.RequireMapping({"max_response_bytes_per_page", "max_response_bytes_per_scan", "max_records_per_page",
	                       "max_records_per_scan", "max_extracted_string_bytes"},
	                      {"max_response_bytes_per_page", "max_response_bytes_per_scan", "max_records_per_page",
	                       "max_records_per_scan", "max_extracted_string_bytes"});
	resources.max_response_bytes_per_page = reader.Text("max_response_bytes_per_page");
	resources.max_response_bytes_per_scan = reader.Text("max_response_bytes_per_scan");
	resources.max_records_per_page = reader.Text("max_records_per_page");
	resources.max_records_per_scan = reader.Text("max_records_per_scan");
	resources.max_extracted_string_bytes = reader.Text("max_extracted_string_bytes");
	resources.mark = reader.Mark();
	std::uint64_t ignored = 0;
	const LocatedText *values[] = {&resources.max_response_bytes_per_page, &resources.max_response_bytes_per_scan,
	                               &resources.max_records_per_page, &resources.max_records_per_scan,
	                               &resources.max_extracted_string_bytes};
	for (const auto *value : values) {
		if (!IsCanonicalUnsigned(*value, ignored)) {
			reader.Diagnostics().Add(PackageDiagnosticCode::INVALID_TYPE, PackageDiagnosticPhase::SCHEMA, value->mark);
		}
	}
	return resources;
}

template <class Declaration, class Decode>
void DecodeUniqueSequence(const SchemaReader &reader, const char *field, std::size_t minimum, std::size_t maximum,
                          Decode decode, std::vector<Declaration> &target, const std::string &relation,
                          PackageDiagnosticSink &diagnostics) {
	const auto *sequence = reader.Sequence(field, minimum, maximum);
	if (sequence == nullptr) {
		return;
	}
	std::set<std::string> ids;
	for (std::size_t index = 0; index < sequence->Size(); index++) {
		auto value = decode(
		    reader.Child(sequence->SequenceValue(index), "." + std::string(field) + "[" + std::to_string(index) + "]"));
		if (!ids.insert(value.id.value).second) {
			diagnostics.Add(PackageDiagnosticCode::DUPLICATE_ID, PackageDiagnosticPhase::SCHEMA, value.id.mark, "",
			                relation);
		}
		target.push_back(std::move(value));
	}
}

} // namespace

bool DecodeRelationSchema(const std::string &file, const FailsafeYamlNode &root, PackageDiagnosticSink &diagnostics,
                          const std::string &expected_spec_identifier, RelationDeclaration &relation) {
	SchemaReader reader(file, root, "$", diagnostics);
	if (!reader.RequireMapping({"api_version", "kind", "id", "schema", "columns", "inputs", "auth", "resources",
	                            "operations", "predicates"},
	                           {"api_version", "kind", "id", "schema", "columns", "auth", "resources", "operations"})) {
		return false;
	}
	relation.api_version = reader.Text("api_version");
	relation.kind = reader.Text("kind");
	relation.id = reader.Text("id");
	relation.schema = reader.Text("schema");
	relation.auth = DecodeAuth(reader.Child("auth"));
	relation.resources = DecodeResources(reader.Child("resources"));
	relation.mark = reader.Mark();
	if (relation.api_version.value != "cuac/v1" || relation.api_version.value != expected_spec_identifier) {
		diagnostics.Add(PackageDiagnosticCode::UNSUPPORTED_SPEC, PackageDiagnosticPhase::SCHEMA,
		                relation.api_version.mark);
	}
	RequireValue(relation.kind, "relation", PackageDiagnosticCode::UNSUPPORTED_DECLARATION, diagnostics);
	RequireIdentifier(relation.id, diagnostics);
	RequireValue(relation.schema, "static", PackageDiagnosticCode::UNSUPPORTED_DECLARATION, diagnostics);

	DecodeUniqueSequence<ColumnDeclaration>(reader, "columns", 1, 256, DecodeColumn, relation.columns,
	                                        relation.id.value, diagnostics);
	DecodeUniqueSequence<InputDeclaration>(reader, "inputs", 0, 128, DecodeInput, relation.inputs, relation.id.value,
	                                       diagnostics);
	DecodeUniqueSequence<OperationDeclaration>(
	    reader, "operations", 1, 64,
	    [](const SchemaReader &operation_reader) { return DecodeOperationSchema(operation_reader); },
	    relation.operations, relation.id.value, diagnostics);
	DecodeUniqueSequence<PredicateDeclaration>(reader, "predicates", 0, 64, DecodePredicateSchema, relation.predicates,
	                                           relation.id.value, diagnostics);
	return diagnostics.Empty();
}

} // namespace internal
} // namespace connector
} // namespace cuac
