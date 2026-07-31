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

} // namespace

PredicateDeclaration DecodePredicateSchema(const SchemaReader &reader) {
	PredicateDeclaration predicate;
	reader.RequireMapping(
	    {"id", "column", "operator", "literal", "conditional_input", "operations", "accuracy", "occurrence_fixtures"},
	    {"id", "column", "operator", "literal", "conditional_input", "operations", "accuracy", "occurrence_fixtures"});
	predicate.id = reader.Text("id");
	predicate.column = reader.Text("column");
	predicate.predicate_operator = reader.Text("operator");
	predicate.conditional_input = reader.Text("conditional_input");
	predicate.operations = reader.TextSequence("operations", 1, 64);
	predicate.accuracy = reader.Text("accuracy");
	predicate.mark = reader.Mark();
	auto literal = reader.Child("literal");
	literal.RequireMapping({"type", "value"}, {"type", "value"});
	predicate.literal_type = literal.Text("type");
	predicate.literal_value = literal.Text("value");
	auto fixtures = reader.Child("occurrence_fixtures");
	fixtures.RequireMapping({"matching", "false_or_null", "duplicates"}, {"matching", "false_or_null", "duplicates"});
	predicate.matching_fixture = fixtures.Text("matching");
	predicate.false_or_null_fixture = fixtures.Text("false_or_null");
	predicate.duplicates_fixture = fixtures.Text("duplicates");
	for (const auto *id : {&predicate.id, &predicate.column, &predicate.conditional_input, &predicate.matching_fixture,
	                       &predicate.false_or_null_fixture, &predicate.duplicates_fixture}) {
		RequireIdentifier(*id, reader.Diagnostics());
	}
	for (const auto &operation : predicate.operations) {
		RequireIdentifier(operation, reader.Diagnostics());
	}
	RequireValue(predicate.predicate_operator, "eq", PackageDiagnosticCode::UNSUPPORTED_DECLARATION,
	             PackageDiagnosticPhase::SCHEMA, reader.Diagnostics());
	if (predicate.literal_type.value != "BOOLEAN" && predicate.literal_type.value != "BIGINT" &&
	    predicate.literal_type.value != "VARCHAR" && predicate.literal_type.value != "DOUBLE") {
		reader.Diagnostics().Add(PackageDiagnosticCode::INVALID_TYPE, PackageDiagnosticPhase::SCHEMA,
		                         predicate.literal_type.mark);
	}
	if (predicate.accuracy.value != "exact" && predicate.accuracy.value != "superset") {
		reader.Diagnostics().Add(PackageDiagnosticCode::INVALID_PREDICATE, PackageDiagnosticPhase::COMPILE,
		                         predicate.accuracy.mark);
	}
	return predicate;
}

} // namespace internal
} // namespace connector
} // namespace cuac
