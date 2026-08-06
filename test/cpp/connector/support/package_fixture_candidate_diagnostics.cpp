#include "package_fixture_candidate_internal.hpp"
#include "cuac/internal/connector/source/compiled_local_package.hpp"

#include "cuac/internal/connector/source/failsafe_yaml.hpp"

#include <sstream>
#include <stdexcept>

namespace cuac {
namespace connector {
namespace internal {
namespace {

void CheckCancellation(PackageCancellation &cancellation) {
	if (cancellation.IsCancellationRequested()) {
		throw PackageCompilationCancelled();
	}
}

SemanticSourceFile &FindSource(std::vector<SemanticSourceFile> &files, const std::string &path) {
	for (auto &file : files) {
		if (file.path == path) {
			return file;
		}
	}
	throw std::logic_error("compiled package source snapshot is incomplete");
}

FailsafeYamlNode ParseSource(const SemanticSourceFile &source, PackageCancellation &cancellation) {
	FailsafeYamlBudget budget(FailsafeYamlLimits::V1());
	try {
		return ParseFailsafeYaml(source.path, source.bytes, budget, cancellation);
	} catch (const FailsafeYamlError &error) {
		if (error.Code() == FailsafeYamlErrorCode::CANCELLED) {
			throw PackageCompilationCancelled();
		}
		throw std::logic_error("compiled package retained invalid semantic YAML");
	}
}

const FailsafeYamlNode &Required(const FailsafeYamlNode &node, const std::string &field) {
	if (node.Type() != FailsafeYamlNode::Kind::MAPPING) {
		throw std::logic_error("compiled package source shape no longer matches its generation");
	}
	const auto *value = node.Find(field);
	if (value == nullptr) {
		throw std::logic_error("compiled package source field no longer matches its generation");
	}
	return *value;
}

const FailsafeYamlNode &FindEntry(const FailsafeYamlNode &root, const std::string &sequence_field,
                                  const std::string &id) {
	const auto &sequence = Required(root, sequence_field);
	if (sequence.Type() != FailsafeYamlNode::Kind::SEQUENCE) {
		throw std::logic_error("compiled package source collection is no longer a sequence");
	}
	for (std::size_t index = 0; index < sequence.Size(); index++) {
		const auto &candidate = sequence.SequenceValue(index);
		const auto &candidate_id = Required(candidate, "id");
		if (candidate_id.Type() == FailsafeYamlNode::Kind::SCALAR && candidate_id.Scalar() == id) {
			return candidate;
		}
	}
	throw std::logic_error("compiled package source entry is absent from retained source");
}

void ReplaceScalar(SemanticSourceFile &source, const FailsafeYamlNode &node, const std::string &replacement) {
	if (node.Type() != FailsafeYamlNode::Kind::SCALAR || node.Span().begin.byte_offset > node.Span().end.byte_offset ||
	    node.Span().end.byte_offset > source.bytes.size()) {
		throw std::logic_error("compiled package source scalar has an invalid retained span");
	}
	source.bytes.replace(static_cast<std::size_t>(node.Span().begin.byte_offset),
	                     static_cast<std::size_t>(node.Span().end.byte_offset - node.Span().begin.byte_offset),
	                     replacement);
}

void RemoveMappingEntry(SemanticSourceFile &source, const FailsafeYamlNode &mapping, const std::string &field) {
	if (mapping.Type() != FailsafeYamlNode::Kind::MAPPING) {
		throw std::logic_error("compiled package source root is no longer a mapping");
	}
	for (std::size_t index = 0; index < mapping.Size(); index++) {
		if (mapping.MappingKey(index) != field) {
			continue;
		}
		const auto key_offset = static_cast<std::size_t>(mapping.MappingKeySpan(index).begin.byte_offset);
		const auto value_end = static_cast<std::size_t>(mapping.MappingValue(index).Span().end.byte_offset);
		const auto prior_newline = key_offset == 0 ? std::string::npos : source.bytes.rfind('\n', key_offset - 1);
		const auto begin = prior_newline == std::string::npos ? 0 : prior_newline + 1;
		const auto next_newline = source.bytes.find('\n', value_end);
		const auto end = next_newline == std::string::npos ? source.bytes.size() : next_newline + 1;
		if (begin > end || end > source.bytes.size()) {
			throw std::logic_error("compiled package mapping entry has an invalid retained span");
		}
		source.bytes.erase(begin, end - begin);
		return;
	}
	throw std::logic_error("compiled package source mapping field is absent");
}

void InsertBeforeMappingEntry(SemanticSourceFile &source, const FailsafeYamlNode &mapping, const std::string &field,
                              const std::string &bytes) {
	if (mapping.Type() != FailsafeYamlNode::Kind::MAPPING) {
		throw std::logic_error("compiled package source root is no longer a mapping");
	}
	for (std::size_t index = 0; index < mapping.Size(); index++) {
		if (mapping.MappingKey(index) == field) {
			const auto key_offset = static_cast<std::size_t>(mapping.MappingKeySpan(index).begin.byte_offset);
			const auto prior_newline = key_offset == 0 ? std::string::npos : source.bytes.rfind('\n', key_offset - 1);
			const auto begin = prior_newline == std::string::npos ? 0 : prior_newline + 1;
			source.bytes.insert(begin, bytes);
			return;
		}
	}
	throw std::logic_error("compiled package source insertion field is absent");
}

void InsertAfterScalarLine(SemanticSourceFile &source, const FailsafeYamlNode &scalar, const std::string &bytes) {
	if (scalar.Type() != FailsafeYamlNode::Kind::SCALAR || scalar.Span().end.byte_offset > source.bytes.size()) {
		throw std::logic_error("compiled package source scalar has an invalid insertion span");
	}
	const auto end = static_cast<std::size_t>(scalar.Span().end.byte_offset);
	const auto newline = source.bytes.find('\n', end);
	const auto insertion = newline == std::string::npos ? source.bytes.size() : newline + 1;
	source.bytes.insert(insertion, bytes);
}

void InsertAfterMappingEntry(SemanticSourceFile &source, const FailsafeYamlNode &mapping, const std::string &field,
                             const std::string &bytes) {
	if (mapping.Type() != FailsafeYamlNode::Kind::MAPPING) {
		throw std::logic_error("compiled package source root is no longer a mapping");
	}
	for (std::size_t index = 0; index < mapping.Size(); index++) {
		if (mapping.MappingKey(index) != field) {
			continue;
		}
		const auto value_end = static_cast<std::size_t>(mapping.MappingValue(index).Span().end.byte_offset);
		if (value_end > source.bytes.size()) {
			throw std::logic_error("compiled package source mapping entry has an invalid insertion span");
		}
		const auto newline = source.bytes.find('\n', value_end);
		auto insertion = newline == std::string::npos ? source.bytes.size() : newline + 1;
		if (insertion == source.bytes.size() && !source.bytes.empty() && source.bytes.back() != '\n') {
			source.bytes.push_back('\n');
			insertion = source.bytes.size();
		}
		source.bytes.insert(insertion, bytes);
		return;
	}
	throw std::logic_error("compiled package source insertion field is absent");
}

std::string ScalarTypeName(CompiledScalarType type) {
	switch (type) {
	case CompiledScalarType::BOOLEAN:
		return "BOOLEAN";
	case CompiledScalarType::BIGINT:
		return "BIGINT";
	case CompiledScalarType::VARCHAR:
		return "VARCHAR";
	case CompiledScalarType::DOUBLE:
		return "DOUBLE";
	case CompiledScalarType::TIMESTAMPTZ:
		return "TIMESTAMPTZ";
	}
	throw std::logic_error("compiled column has an unknown scalar type");
}

std::string ScalarLiteral(CompiledScalarType type) {
	switch (type) {
	case CompiledScalarType::BOOLEAN:
		return "true";
	case CompiledScalarType::BIGINT:
		return "0";
	case CompiledScalarType::VARCHAR:
		return "fixture";
	case CompiledScalarType::DOUBLE:
		return "0.0";
	case CompiledScalarType::TIMESTAMPTZ:
		return "1970-01-01T00:00:00.000000Z";
	}
	throw std::logic_error("compiled column has an unknown scalar type");
}

std::string UniqueInputId(const CompiledRelation &relation) {
	std::string value = "fixture_variant_input";
	for (std::size_t suffix = 2;; suffix++) {
		bool found = false;
		for (const auto &input : relation.Inputs()) {
			found = found || input.Name() == value;
		}
		if (!found) {
			return value;
		}
		value = "fixture_variant_input_" + std::to_string(suffix);
	}
}

std::string UniqueOperationId(const CompiledRelation &relation) {
	std::string value = "fixture_variant_graphql";
	for (std::size_t suffix = 2;; suffix++) {
		bool found = false;
		for (const auto &operation : relation.Operations()) {
			found = found || operation.name == value;
		}
		if (!found) {
			return value;
		}
		value = "fixture_variant_graphql_" + std::to_string(suffix);
	}
}

const CompiledOperation &FirstRestOperation(const CompiledRelation &relation) {
	for (const auto &operation : relation.Operations()) {
		if (operation.Protocol() == CompiledProtocol::REST) {
			return operation;
		}
	}
	throw std::logic_error("diagnostic REST augmentation lost its operation");
}

const CompiledRelation &RelationWithRest(const CompiledPackageGeneration &generation) {
	for (const auto &relation : generation.Connector().Relations()) {
		for (const auto &operation : relation.Operations()) {
			if (operation.Protocol() == CompiledProtocol::REST) {
				return relation;
			}
		}
	}
	throw std::invalid_argument("diagnostic source variant requires a REST operation");
}

void AppendDuplicateColumn(SemanticSourceFile &source, const CompiledRelation &relation,
                           PackageCancellation &cancellation) {
	const auto root = ParseSource(source, cancellation);
	const auto &column = relation.Columns().front();
	std::ostringstream bytes;
	bytes << "  - id: " << column.name << "\n"
	      << "    type: " << ScalarTypeName(column.ScalarType()) << "\n"
	      << "    nullable: " << (column.nullable ? "true" : "false") << "\n"
	      << "    extract: " << column.extractor << "\n";
	InsertAfterMappingEntry(source, root, "columns", bytes.str());
}

void AddInvalidPredicate(SemanticSourceFile &source, const CompiledRelation &relation) {
	const auto &operation = FirstRestOperation(relation);
	const auto &column = relation.Columns().front();
	std::ostringstream bytes;
	bytes << "\npredicates:\n"
	      << "  - id: fixture_variant_predicate\n"
	      << "    column: " << column.name << "\n"
	      << "    operator: eq\n"
	      << "    literal:\n"
	      << "      type: " << ScalarTypeName(column.ScalarType()) << "\n"
	      << "      value: " << ScalarLiteral(column.ScalarType()) << "\n"
	      << "    conditional_input: fixture_variant_conditional\n"
	      << "    operations: [" << operation.name << "]\n"
	      << "    accuracy: approximate\n"
	      << "    occurrence_fixtures:\n"
	      << "      matching: fixture_variant_matching\n"
	      << "      false_or_null: fixture_variant_false_or_null\n"
	      << "      duplicates: fixture_variant_duplicates\n";
	source.bytes += bytes.str();
}

void AddInvalidGraphqlOperation(SemanticSourceFile &source, const CompiledRelation &relation,
                                PackageCancellation &cancellation) {
	const auto &rest = FirstRestOperation(relation);
	const auto &column = relation.Columns().front();
	const auto operation_id = UniqueOperationId(relation);
	const CompiledOperation *fallback = nullptr;
	for (const auto &operation : relation.Operations()) {
		if (operation.fallback) {
			fallback = &operation;
			break;
		}
	}
	if (fallback != nullptr) {
		const auto input_id = UniqueInputId(relation);
		auto root = ParseSource(source, cancellation);
		const auto &fallback_node = FindEntry(root, "operations", fallback->name);
		RemoveMappingEntry(source, fallback_node, "fallback");
		root = ParseSource(source, cancellation);
		const auto &amended_fallback = FindEntry(root, "operations", fallback->name);
		InsertAfterScalarLine(source, Required(amended_fallback, "id"),
		                      "    when: {required_inputs: [input." + input_id + "]}\n");
		root = ParseSource(source, cancellation);
		const auto input_bytes = "  - id: " + input_id + "\n    type: BOOLEAN\n    nullable: false\n";
		if (root.Find("inputs") == nullptr) {
			InsertBeforeMappingEntry(source, root, "auth", "inputs:\n" + input_bytes + "\n");
		} else {
			InsertAfterMappingEntry(source, root, "inputs", input_bytes);
		}
	}
	const auto root = ParseSource(source, cancellation);
	std::ostringstream bytes;
	bytes << "  - id: " << operation_id << "\n"
	      << "    fallback: true\n"
	      << "    cardinality: many\n"
	      << "    replay_safety: safe\n"
	      << "    request:\n"
	      << "      protocol: graphql\n"
	      << "      endpoint:\n"
	      << "        origin:\n"
	      << "          scheme: https\n"
	      << "          host: " << rest.Rest().request.origin.host.Value() << "\n"
	      << "          port: " << rest.Rest().request.origin.port << "\n"
	      << "        path: /fixture-variant\n"
	      << "      headers: []\n"
	      << "      query:\n"
	      << "        operation_name: 1Invalid\n"
	      << "        root: [fixtureVariant]\n"
	      << "        fixed_arguments: []\n"
	      << "        selection:\n"
	      << "          - {column: " << column.name << ", field_path: [value]}\n"
	      << "      pagination:\n"
	      << "        strategy: relay_forward\n"
	      << "        dependency: sequential\n"
	      << "        consistency: mutable\n"
	      << "        page_size_argument: first\n"
	      << "        page_size_variable: pageSize\n"
	      << "        page_size: 1\n"
	      << "        cursor_argument: after\n"
	      << "        cursor_variable: cursor\n"
	      << "        page_info_field: pageInfo\n"
	      << "        has_next_page_field: hasNextPage\n"
	      << "        end_cursor_field: endCursor\n"
	      << "        max_pages_per_scan: 1\n"
	      << "        max_concurrent_pages: 1\n"
	      << "      partial_data: fail_on_any_error\n"
	      << "      max_document_bytes: 4096\n"
	      << "      max_serialized_body_bytes_per_request: 8192\n"
	      << "      max_serialized_body_bytes_per_scan: 8192\n";
	InsertAfterMappingEntry(source, root, "operations", bytes.str());
}

const CompiledRelation &FirstRelation(const CompiledPackageGeneration &generation) {
	const auto &relations = generation.Connector().Relations();
	if (relations.empty()) {
		throw std::logic_error("compiled package has no relation");
	}
	return relations.front();
}

const CompiledRelation &RelationWithColumns(const CompiledPackageGeneration &generation, std::size_t minimum) {
	for (const auto &relation : generation.Connector().Relations()) {
		if (relation.Columns().size() >= minimum) {
			return relation;
		}
	}
	throw std::invalid_argument("diagnostic source variant requires more compiled columns");
}

const CompiledRelation &RelationWithGraphql(const CompiledPackageGeneration &generation) {
	for (const auto &relation : generation.Connector().Relations()) {
		for (const auto &operation : relation.Operations()) {
			if (operation.Protocol() == CompiledProtocol::GRAPHQL) {
				return relation;
			}
		}
	}
	throw std::invalid_argument("diagnostic source variant requires a GraphQL operation");
}

const CompiledRelation &RelationWithFallback(const CompiledPackageGeneration &generation) {
	for (const auto &relation : generation.Connector().Relations()) {
		for (const auto &operation : relation.Operations()) {
			if (operation.fallback) {
				return relation;
			}
		}
	}
	throw std::invalid_argument("diagnostic source variant requires a fallback operation");
}

const CompiledRelation &RelationWithPredicate(const CompiledPackageGeneration &generation) {
	for (const auto &relation : generation.Connector().Relations()) {
		if (!relation.PredicateMappings().empty()) {
			return relation;
		}
	}
	throw std::invalid_argument("diagnostic source variant requires a predicate mapping");
}

SemanticSourceFile &RelationSource(std::vector<SemanticSourceFile> &files, const CompiledRelation &relation) {
	return FindSource(files, "relations/" + relation.Name() + ".yaml");
}

std::string UppercaseFirst(std::string value) {
	for (auto &character : value) {
		if (character >= 'a' && character <= 'z') {
			character = static_cast<char>(character - 'a' + 'A');
			return value;
		}
	}
	throw std::invalid_argument("diagnostic invalid-identifier variant requires a lowercase identifier");
}

void MutateCompilerDiagnostic(std::vector<SemanticSourceFile> &files, const CompiledPackageGeneration &generation,
                              const std::string &diagnostic, PackageCancellation &cancellation) {
	auto &manifest_source = FindSource(files, "connector.yaml");
	const auto manifest = ParseSource(manifest_source, cancellation);
	if (diagnostic == "CUAC_UNSUPPORTED_SPEC") {
		ReplaceScalar(manifest_source, Required(manifest, "api_version"), "cuac/unsupported");
	} else if (diagnostic == "CUAC_UNSUPPORTED_DIALECT") {
		ReplaceScalar(manifest_source, Required(manifest, "extractor_dialect"), "cuac/unsupported");
	} else if (diagnostic == "CUAC_MALFORMED_YAML") {
		ReplaceScalar(manifest_source, Required(manifest, "api_version"), "[");
	} else if (diagnostic == "CUAC_UNKNOWN_FIELD") {
		manifest_source.bytes += "\nfixture_unknown_field: true\n";
	} else if (diagnostic == "CUAC_MISSING_FIELD") {
		const auto &relation = FirstRelation(generation);
		auto &source = RelationSource(files, relation);
		const auto root = ParseSource(source, cancellation);
		RemoveMappingEntry(source, root, "columns");
	} else if (diagnostic == "CUAC_DUPLICATE_ID") {
		const auto &relation = RelationWithColumns(generation, 1);
		auto &source = RelationSource(files, relation);
		if (relation.Columns().size() == 1) {
			AppendDuplicateColumn(source, relation, cancellation);
		} else {
			const auto root = ParseSource(source, cancellation);
			const auto &columns = Required(root, "columns");
			ReplaceScalar(source, Required(columns.SequenceValue(1), "id"), relation.Columns()[0].name);
		}
	} else if (diagnostic == "CUAC_INVALID_REFERENCE") {
		const auto &relation = FirstRelation(generation);
		auto &source = RelationSource(files, relation);
		const auto root = ParseSource(source, cancellation);
		const auto &auth = Required(root, "auth");
		const auto &mode = Required(auth, "mode");
		if (mode.Scalar() == "anonymous") {
			ReplaceScalar(source, mode, "credential");
			InsertAfterScalarLine(source, mode, "  credential: fixture_missing_credential\n");
		} else {
			ReplaceScalar(source, Required(auth, "credential"), "fixture_missing_credential");
		}
	} else if (diagnostic == "CUAC_INVALID_IDENTIFIER") {
		ReplaceScalar(manifest_source, Required(manifest, "id"), UppercaseFirst(generation.Identity().ConnectorId()));
	} else if (diagnostic == "CUAC_INVALID_TYPE") {
		const auto &relation = RelationWithColumns(generation, 1);
		auto &source = RelationSource(files, relation);
		const auto root = ParseSource(source, cancellation);
		ReplaceScalar(source, Required(Required(root, "columns").SequenceValue(0), "type"), "INTEGER");
	} else if (diagnostic == "CUAC_INVALID_EXTRACTOR") {
		const auto &relation = RelationWithColumns(generation, 1);
		auto &source = RelationSource(files, relation);
		const auto root = ParseSource(source, cancellation);
		ReplaceScalar(source, Required(Required(root, "columns").SequenceValue(0), "extract"),
		              "fixture_invalid_extractor");
	} else if (diagnostic == "CUAC_RESERVED_INPUT") {
		const CompiledRelation *relation = nullptr;
		for (const auto &candidate : generation.Connector().Relations()) {
			if (!candidate.Inputs().empty()) {
				relation = &candidate;
				break;
			}
		}
		if (relation == nullptr) {
			relation = &FirstRelation(generation);
		}
		auto &source = RelationSource(files, *relation);
		const auto root = ParseSource(source, cancellation);
		if (!relation->Inputs().empty()) {
			ReplaceScalar(source, Required(FindEntry(root, "inputs", relation->Inputs()[0].Name()), "id"), "secret");
		} else {
			InsertBeforeMappingEntry(source, root, "auth",
			                         "inputs:\n  - id: secret\n    type: VARCHAR\n    nullable: false\n\n");
		}
	} else if (diagnostic == "CUAC_UNSUPPORTED_DECLARATION") {
		ReplaceScalar(manifest_source, Required(manifest, "kind"), "unsupported");
	} else if (diagnostic == "CUAC_INVALID_SELECTOR") {
		const CompiledRelation *relation = nullptr;
		try {
			relation = &RelationWithFallback(generation);
		} catch (const std::invalid_argument &) {
			relation = &FirstRelation(generation);
		}
		auto &source = RelationSource(files, *relation);
		const auto root = ParseSource(source, cancellation);
		bool mutated = false;
		for (const auto &operation : relation->Operations()) {
			if (!operation.fallback) {
				continue;
			}
			ReplaceScalar(source, Required(FindEntry(root, "operations", operation.name), "fallback"), "false");
			mutated = true;
			break;
		}
		if (!mutated) {
			const auto &operation = relation->Operations().front();
			InsertAfterScalarLine(source, Required(FindEntry(root, "operations", operation.name), "id"),
			                      "    fallback: true\n");
		}
	} else if (diagnostic == "CUAC_INVALID_PREDICATE") {
		const CompiledRelation *relation = nullptr;
		try {
			relation = &RelationWithPredicate(generation);
		} catch (const std::invalid_argument &) {
			relation = &RelationWithRest(generation);
		}
		auto &source = RelationSource(files, *relation);
		if (relation->PredicateMappings().empty()) {
			AddInvalidPredicate(source, *relation);
		} else {
			const auto root = ParseSource(source, cancellation);
			ReplaceScalar(source,
			              Required(FindEntry(root, "predicates", relation->PredicateMappings()[0].Name()), "accuracy"),
			              "approximate");
		}
	} else if (diagnostic == "CUAC_INVALID_GRAPHQL_PROFILE") {
		const CompiledRelation *relation = nullptr;
		try {
			relation = &RelationWithGraphql(generation);
		} catch (const std::invalid_argument &) {
			relation = &RelationWithRest(generation);
		}
		auto &source = RelationSource(files, *relation);
		bool mutated = false;
		for (const auto &operation : relation->Operations()) {
			if (operation.Protocol() != CompiledProtocol::GRAPHQL) {
				continue;
			}
			const auto root = ParseSource(source, cancellation);
			const auto &operation_node = FindEntry(root, "operations", operation.name);
			ReplaceScalar(source, Required(Required(Required(operation_node, "request"), "query"), "operation_name"),
			              "1Invalid");
			mutated = true;
			break;
		}
		if (!mutated) {
			AddInvalidGraphqlOperation(source, *relation, cancellation);
		}
	} else if (diagnostic == "CUAC_POLICY_WIDENING") {
		ReplaceScalar(manifest_source, Required(Required(manifest, "network_policy"), "private_addresses"), "allow");
	} else if (diagnostic == "CUAC_INCOMPATIBLE_RELOAD") {
		ReplaceScalar(manifest_source, Required(manifest, "id"),
		              generation.Identity().ConnectorId() == "fixture_incompatible" ? "fixture_incompatible_alt"
		                                                                            : "fixture_incompatible");
	} else if (diagnostic != "CUAC_RESOURCE_EXHAUSTED" && diagnostic != "CUAC_PACKAGE_IDENTITY" &&
	           diagnostic != "CUAC_FIXTURE_MISMATCH") {
		throw std::invalid_argument("coverage entry is not a Connector-owned diagnostic variant");
	}
}

} // namespace

std::vector<SemanticSourceFile> BuildFixtureDiagnosticSources(const CompiledLocalPackage &active,
                                                              const PackageFixtureCoverageEntry &coverage_entry,
                                                              PackageCancellation &cancellation) {
	CheckCancellation(cancellation);
	if (!active.IsValid() || coverage_entry.scope != PackageFixtureCoverageScope::DIAGNOSTIC) {
		throw std::invalid_argument("fixture diagnostic candidate requires a valid package and diagnostic entry");
	}
	auto files = cuac::internal::CompiledLocalPackageAccess::Source(active).Files();
	MutateCompilerDiagnostic(files, active.Generation(), coverage_entry.diagnostic, cancellation);
	CheckCancellation(cancellation);
	return files;
}

} // namespace internal
} // namespace connector
} // namespace cuac
