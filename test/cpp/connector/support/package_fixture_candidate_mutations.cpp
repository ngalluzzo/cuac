#include "package_fixture_candidate_internal.hpp"

#include "cuac/internal/connector/source/compiled_local_package.hpp"
#include "cuac/internal/connector/source/failsafe_yaml.hpp"
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

const FailsafeYamlNode &Required(const FailsafeYamlNode &node, const char *field) {
	if (node.Type() != FailsafeYamlNode::Kind::MAPPING) {
		throw std::logic_error("compiled package source shape no longer matches its generation");
	}
	const auto *value = node.Find(field);
	if (value == nullptr) {
		throw std::logic_error("compiled package source field no longer matches its generation");
	}
	return *value;
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

void ReplaceMappingEntry(SemanticSourceFile &source, const FailsafeYamlNode &mapping, const std::string &field,
                         const std::string &replacement) {
	if (mapping.Type() != FailsafeYamlNode::Kind::MAPPING) {
		throw std::logic_error("compiled package source mapping entry has an invalid retained shape");
	}
	for (std::size_t index = 0; index < mapping.Size(); index++) {
		if (mapping.MappingKey(index) != field) {
			continue;
		}
		const auto key_offset = static_cast<std::size_t>(mapping.MappingKeySpan(index).begin.byte_offset);
		const auto value_end = static_cast<std::size_t>(mapping.MappingValue(index).Span().end.byte_offset);
		const auto preceding_newline = key_offset == 0 ? std::string::npos : source.bytes.rfind('\n', key_offset - 1);
		const auto line_begin = preceding_newline == std::string::npos ? 0 : preceding_newline + 1;
		const auto line_end_offset = source.bytes.find('\n', value_end);
		const auto line_end = line_end_offset == std::string::npos ? source.bytes.size() : line_end_offset;
		if (line_begin > line_end || line_end > source.bytes.size()) {
			throw std::logic_error("compiled package source mapping entry has an invalid retained span");
		}
		const auto indentation = source.bytes.substr(line_begin, key_offset - line_begin);
		source.bytes.replace(line_begin, line_end - line_begin, indentation + replacement);
		return;
	}
	throw std::logic_error("compiled package source mapping entry no longer matches its retained field");
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

void MutateReload(std::vector<SemanticSourceFile> &files, const std::string &variant,
                  const std::string &package_version, PackageCancellation &cancellation) {
	auto &manifest = FindSource(files, "connector.yaml");
	if (variant == "version_reuse_rejected") {
		manifest.bytes += "\n# cuac closed fixture identity variant\n";
		return;
	}
	const auto root = ParseSource(manifest, cancellation);
	if (variant == "incompatible_rejected") {
		const auto &id = Required(root, "id");
		const auto replacement =
		    id.Scalar() == "fixture_incompatible" ? "fixture_incompatible_alt" : "fixture_incompatible";
		ReplaceScalar(manifest, id, replacement);
		return;
	}
	const auto &version_node = Required(root, "version");
	if (variant != "compatible_patch" && variant != "compatible_minor" && variant != "downgrade_rejected") {
		throw std::invalid_argument("coverage entry is not a closed reload source variant");
	}
	ReplaceScalar(manifest, version_node, package_version);
}

const CompiledOperation &FindGraphqlOperation(const CompiledPackageGeneration &generation,
                                              const PackageFixtureCoverageEntry &entry) {
	const auto *relation = generation.Connector().FindRelation(entry.relation);
	if (relation == nullptr) {
		throw std::invalid_argument("GraphQL fixture candidate relation is absent from the generation");
	}
	for (const auto &operation : relation->Operations()) {
		if (operation.name == entry.operation && operation.Protocol() == CompiledProtocol::GRAPHQL) {
			return operation;
		}
	}
	throw std::invalid_argument("GraphQL fixture candidate operation is absent from the generation");
}

const FailsafeYamlNode &FindOperationNode(const FailsafeYamlNode &root, const std::string &operation) {
	const auto &operations = Required(root, "operations");
	if (operations.Type() != FailsafeYamlNode::Kind::SEQUENCE) {
		throw std::logic_error("compiled relation operation source is no longer a sequence");
	}
	for (std::size_t index = 0; index < operations.Size(); index++) {
		const auto &candidate = operations.SequenceValue(index);
		const auto &id = Required(candidate, "id");
		if (id.Type() == FailsafeYamlNode::Kind::SCALAR && id.Scalar() == operation) {
			return candidate;
		}
	}
	throw std::logic_error("compiled GraphQL operation is absent from retained source");
}

void MutateGraphqlDocumentLimit(std::vector<SemanticSourceFile> &files, const CompiledPackageGeneration &generation,
                                const PackageFixtureCoverageEntry &entry, PackageCancellation &cancellation) {
	if (entry.resource != "max_document_bytes" ||
	    (entry.variant != "boundary" && entry.variant != "one_over_rejected")) {
		throw std::invalid_argument("coverage entry is not a closed GraphQL document-limit variant");
	}
	const auto &operation = FindGraphqlOperation(generation, entry);
	const auto document_bytes = operation.Graphql().document.size();
	if (document_bytes == 0 || (entry.variant == "one_over_rejected" && document_bytes == 1)) {
		throw std::logic_error("compiled GraphQL document cannot exercise the requested limit variant");
	}
	auto &relation_source = FindSource(files, "relations/" + entry.relation + ".yaml");
	const auto root = ParseSource(relation_source, cancellation);
	const auto &operation_node = FindOperationNode(root, entry.operation);
	const auto &request = Required(operation_node, "request");
	const auto &limit = Required(request, "max_document_bytes");
	const auto value = entry.variant == "boundary" ? document_bytes : document_bytes - 1;
	ReplaceScalar(relation_source, limit, std::to_string(value));
}

void MutateSelectionNoCandidate(std::vector<SemanticSourceFile> &files, const CompiledPackageGeneration &generation,
                                const PackageFixtureCoverageEntry &entry, PackageCancellation &cancellation) {
	if (entry.variant != "no_candidate_rejected") {
		throw std::invalid_argument("coverage entry is not the closed no-candidate selection variant");
	}
	const auto *relation = generation.Connector().FindRelation(entry.relation);
	if (relation == nullptr || relation->Operations().size() < 2) {
		throw std::invalid_argument("no-candidate source variant requires a selectable relation");
	}
	std::string variant_input = "fixture_variant_required";
	for (std::size_t suffix = 2;; suffix++) {
		bool unique = true;
		for (const auto &input : relation->Inputs()) {
			if (input.Name() == variant_input) {
				unique = false;
				break;
			}
		}
		if (unique) {
			break;
		}
		variant_input = "fixture_variant_required_" + std::to_string(suffix);
	}
	auto &relation_source = FindSource(files, "relations/" + entry.relation + ".yaml");
	const auto root = ParseSource(relation_source, cancellation);
	for (auto operation = relation->Operations().rbegin(); operation != relation->Operations().rend(); operation++) {
		const auto &operation_node = FindOperationNode(root, operation->name);
		const auto replacement = "when: {required_inputs: [input." + variant_input + "]}";
		if (operation_node.Find("fallback") != nullptr) {
			ReplaceMappingEntry(relation_source, operation_node, "fallback", replacement);
		} else if (operation_node.Find("when") != nullptr) {
			ReplaceMappingEntry(relation_source, operation_node, "when", replacement);
		} else {
			throw std::logic_error("compiled selectable operation lost its selector source");
		}
	}
	const auto amended_root = ParseSource(relation_source, cancellation);
	const auto input_bytes = "  - id: " + variant_input + "\n    type: BOOLEAN\n    nullable: false\n\n";
	InsertBeforeMappingEntry(relation_source, amended_root, "auth",
	                         amended_root.Find("inputs") == nullptr ? "inputs:\n" + input_bytes : input_bytes);
}

} // namespace

std::vector<SemanticSourceFile> BuildFixtureCandidateSources(const CompiledLocalPackage &active,
                                                             const PackageFixtureCoverageEntry &coverage_entry,
                                                             PackageCancellation &cancellation) {
	CheckCancellation(cancellation);
	if (!active.IsValid()) {
		throw std::invalid_argument("fixture source candidate requires a valid active package");
	}
	auto files = cuac::internal::CompiledLocalPackageAccess::Source(active).Files();
	if (coverage_entry.scope == PackageFixtureCoverageScope::GRAPHQL_RESOURCE) {
		MutateGraphqlDocumentLimit(files, active.Generation(), coverage_entry, cancellation);
	} else if (coverage_entry.scope == PackageFixtureCoverageScope::SOURCE_IDENTITY) {
		if (coverage_entry.variant == "byte_change") {
			FindSource(files, "connector.yaml").bytes += "\n# cuac closed fixture byte-change variant\n";
		} else if (coverage_entry.variant != "copied_root" && coverage_entry.variant != "symlink_rejected" &&
		           coverage_entry.variant != "hardlink_rejected" && coverage_entry.variant != "entry_change_rejected" &&
		           coverage_entry.variant != "unlisted_relation_rejected" &&
		           coverage_entry.variant != "case_collision_rejected") {
			throw std::invalid_argument("coverage entry is not a closed source-identity variant");
		}
	} else if (coverage_entry.scope == PackageFixtureCoverageScope::RELATION_SELECTION) {
		MutateSelectionNoCandidate(files, active.Generation(), coverage_entry, cancellation);
	} else if (coverage_entry.scope == PackageFixtureCoverageScope::COMPILER_CANCELLATION) {
		if (coverage_entry.variant != "source_enumeration" && coverage_entry.variant != "source_read" &&
		    coverage_entry.variant != "yaml_parse" && coverage_entry.variant != "reference_validation" &&
		    coverage_entry.variant != "generation_validation") {
			throw std::invalid_argument("coverage entry is not a Connector-owned compiler-cancellation variant");
		}
	} else {
		throw std::invalid_argument("coverage entry has no Connector-owned source candidate");
	}
	CheckCancellation(cancellation);
	return files;
}

std::vector<SemanticSourceFile> BuildFixtureReloadSources(const CompiledLocalPackage &current,
                                                          const PackageFixtureCoverageEntry &coverage_entry,
                                                          const std::string &package_version,
                                                          PackageCancellation &cancellation) {
	CheckCancellation(cancellation);
	if (!current.IsValid() || coverage_entry.scope != PackageFixtureCoverageScope::RELOAD ||
	    coverage_entry.variant == "exact_no_op") {
		throw std::invalid_argument("fixture reload source requires a changed closed reload variant");
	}
	auto files = cuac::internal::CompiledLocalPackageAccess::Source(current).Files();
	MutateReload(files, coverage_entry.variant, package_version, cancellation);
	CheckCancellation(cancellation);
	return files;
}

} // namespace internal
} // namespace connector
} // namespace cuac
