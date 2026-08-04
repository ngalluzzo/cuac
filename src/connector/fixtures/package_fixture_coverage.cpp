#include "cuac/connector/package_fixture_runner.hpp"

#include "cuac/connector/content_digest.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

namespace cuac {
namespace connector {
namespace internal {

class PackageFixtureCoverageBuilder {
public:
	void Add(const std::string &key, PackageFixtureCoverageScope scope, const std::string &variant,
	         const std::string &relation = std::string(), const std::string &operation = std::string(),
	         const std::string &input = std::string(), const std::string &column = std::string(),
	         const std::string &predicate = std::string(), const std::string &resource = std::string(),
	         const std::string &diagnostic = std::string(), std::uint16_t rate_limit_status = 0,
	         const std::string &rate_limit_header = std::string(),
	         CompiledRateLimitGuidanceFormat rate_limit_format = CompiledRateLimitGuidanceFormat::RETRY_AFTER) {
		if (!IsKey(key) || !unique.insert(key).second) {
			throw std::invalid_argument("compiled package derives an invalid or duplicate fixture coverage key");
		}
		keys.push_back(key);
		entries.push_back({key, scope, variant, relation, operation, input, column, predicate, resource, diagnostic,
		                   rate_limit_status, rate_limit_header, rate_limit_format});
	}

	void Variants(const std::string &prefix, const std::vector<const char *> &variants,
	              PackageFixtureCoverageScope scope, const std::string &relation = std::string(),
	              const std::string &operation = std::string(), const std::string &input = std::string(),
	              const std::string &column = std::string(), const std::string &predicate = std::string(),
	              const std::string &resource = std::string()) {
		for (const auto *variant : variants) {
			Add(prefix + variant, scope, variant, relation, operation, input, column, predicate, resource);
		}
	}

	PackageFixtureCoverage Finish() {
		std::string framed;
		for (const auto &key : keys) {
			framed += key;
			framed.push_back('\n');
		}
		return PackageFixtureCoverage(std::move(keys), std::move(entries), "sha256." + ComputeSha256Hex(framed));
	}

private:
	static bool IsKey(const std::string &key) {
		if (key.empty() || key.size() > 255 || key.front() < 'a' || key.front() > 'z') {
			return false;
		}
		for (const auto character : key) {
			if (!((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
			      character == '_')) {
				return false;
			}
		}
		return true;
	}

	std::vector<std::string> keys;
	std::vector<PackageFixtureCoverageEntry> entries;
	std::set<std::string> unique;
};

} // namespace internal

namespace {

using CoverageBuilder = internal::PackageFixtureCoverageBuilder;

bool IsCredentialRelation(const CompiledRelation &relation) {
	return relation.Authentication().Requirement() == CompiledCredentialRequirement::REQUIRED;
}

void AddOperationCoverage(CoverageBuilder &coverage, const CompiledConnector &connector) {
	for (const auto &relation : connector.Relations()) {
		for (const auto &operation : relation.Operations()) {
			coverage.Variants("operation_" + relation.Name() + "_" + operation.name + "_", {"success"},
			                  PackageFixtureCoverageScope::OPERATION, relation.Name(), operation.name);
		}
	}
	for (const auto &relation : connector.Relations()) {
		if (!IsCredentialRelation(relation)) {
			for (const auto &operation : relation.Operations()) {
				coverage.Variants("auth_" + relation.Name() + "_" + operation.name + "_", {"anonymous"},
				                  PackageFixtureCoverageScope::AUTHENTICATION, relation.Name(), operation.name);
			}
		}
	}
	for (const auto &relation : connector.Relations()) {
		if (IsCredentialRelation(relation)) {
			for (const auto &operation : relation.Operations()) {
				coverage.Variants("auth_" + relation.Name() + "_" + operation.name + "_",
				                  {"bearer_present", "bearer_missing"}, PackageFixtureCoverageScope::AUTHENTICATION,
				                  relation.Name(), operation.name);
			}
		}
	}
}

void AddInputCoverage(CoverageBuilder &coverage, const CompiledConnector &connector) {
	for (const auto &relation : connector.Relations()) {
		for (const auto &input : relation.Inputs()) {
			coverage.Variants("input_" + relation.Name() + "_" + input.Name() + "_", {"bound_value", "unbound"},
			                  PackageFixtureCoverageScope::INPUT, relation.Name(), "", input.Name());
		}
	}
	for (const auto &relation : connector.Relations()) {
		for (const auto &input : relation.Inputs()) {
			if (input.Default().HasDefault()) {
				coverage.Variants("input_" + relation.Name() + "_" + input.Name() + "_", {"default_applied"},
				                  PackageFixtureCoverageScope::INPUT, relation.Name(), "", input.Name());
			}
		}
	}
	for (const auto &relation : connector.Relations()) {
		for (const auto &input : relation.Inputs()) {
			if (input.Nullable()) {
				coverage.Variants("input_" + relation.Name() + "_" + input.Name() + "_", {"bound_null_omitted"},
				                  PackageFixtureCoverageScope::INPUT, relation.Name(), "", input.Name());
			}
		}
	}
	for (const auto &relation : connector.Relations()) {
		for (const auto &input : relation.Inputs()) {
			if (!input.Nullable()) {
				coverage.Variants("input_" + relation.Name() + "_" + input.Name() + "_", {"bound_null_rejected"},
				                  PackageFixtureCoverageScope::INPUT, relation.Name(), "", input.Name());
			}
		}
	}
}

void AddColumnCoverage(CoverageBuilder &coverage, const CompiledConnector &connector) {
	for (const auto &relation : connector.Relations()) {
		for (const auto &column : relation.Columns()) {
			coverage.Variants("column_" + relation.Name() + "_" + column.name + "_",
			                  {"success", "type_mismatch_rejected"}, PackageFixtureCoverageScope::COLUMN,
			                  relation.Name(), "", "", column.name);
		}
	}
	for (const auto &relation : connector.Relations()) {
		for (const auto &column : relation.Columns()) {
			if (column.nullable) {
				coverage.Variants("column_" + relation.Name() + "_" + column.name + "_", {"null"},
				                  PackageFixtureCoverageScope::COLUMN, relation.Name(), "", "", column.name);
			}
		}
	}
	for (const auto &relation : connector.Relations()) {
		for (const auto &column : relation.Columns()) {
			if (!column.nullable) {
				coverage.Variants("column_" + relation.Name() + "_" + column.name + "_",
				                  {"missing_rejected", "null_rejected"}, PackageFixtureCoverageScope::COLUMN,
				                  relation.Name(), "", "", column.name);
			}
		}
	}
	for (const auto &relation : connector.Relations()) {
		for (const auto &column : relation.Columns()) {
			if (column.Shape() == CompiledColumnShape::SCALAR && column.ElementType() == CompiledScalarType::BIGINT) {
				coverage.Variants(
				    "column_" + relation.Name() + "_" + column.name + "_",
				    {"minimum", "maximum", "underflow_rejected", "overflow_rejected", "fraction_rejected"},
				    PackageFixtureCoverageScope::COLUMN, relation.Name(), "", "", column.name);
			}
		}
	}
	for (const auto &relation : connector.Relations()) {
		for (const auto &column : relation.Columns()) {
			if (column.Shape() == CompiledColumnShape::SCALAR && column.ElementType() == CompiledScalarType::VARCHAR) {
				coverage.Variants("column_" + relation.Name() + "_" + column.name + "_",
				                  {"string_budget_boundary", "string_budget_one_over_rejected"},
				                  PackageFixtureCoverageScope::COLUMN, relation.Name(), "", "", column.name);
			}
		}
	}
	for (const auto &relation : connector.Relations()) {
		for (const auto &column : relation.Columns()) {
			if (column.Shape() == CompiledColumnShape::SCALAR && column.ElementType() == CompiledScalarType::DOUBLE) {
				// RFC 0020: no underflow_rejected (underflow to a subnormal
				// or exact zero is a legitimate, accepted result, not an
				// error) and no fraction_rejected (DOUBLE accepts
				// fractions); magnitude_overflow_rejected replaces BIGINT's
				// overflow_rejected for a JSON number too large to
				// represent as any finite double.
				coverage.Variants("column_" + relation.Name() + "_" + column.name + "_",
				                  {"minimum", "maximum", "magnitude_overflow_rejected"},
				                  PackageFixtureCoverageScope::COLUMN, relation.Name(), "", "", column.name);
			}
		}
	}
}

void AddArrayCoverage(CoverageBuilder &coverage, const CompiledConnector &connector) {
	for (const auto &relation : connector.Relations()) {
		for (const auto &operation : relation.Operations()) {
			for (const auto &column : relation.Columns()) {
				if (column.Shape() != CompiledColumnShape::ARRAY) {
					continue;
				}
				const auto prefix = "array_" + relation.Name() + "_" + operation.name + "_" + column.name + "_";
				coverage.Variants(prefix,
				                  {"success", "empty", "singleton", "ordered_duplicates", "outer_type_rejected",
				                   "element_type_rejected", "nested_rejected", "late_invalid_element_rejected",
				                   "decode_cancellation", "query_transfer_cancellation"},
				                  PackageFixtureCoverageScope::ARRAY, relation.Name(), operation.name, "", column.name);
				coverage.Variants(prefix, {column.ElementNullable() ? "null_element" : "null_element_rejected"},
				                  PackageFixtureCoverageScope::ARRAY, relation.Name(), operation.name, "", column.name);
				if (column.ElementType() == CompiledScalarType::BIGINT) {
					coverage.Variants(
					    prefix, {"minimum", "maximum", "underflow_rejected", "overflow_rejected", "fraction_rejected"},
					    PackageFixtureCoverageScope::ARRAY, relation.Name(), operation.name, "", column.name);
				} else if (column.ElementType() == CompiledScalarType::VARCHAR) {
					coverage.Variants(prefix, {"string_budget_boundary", "string_budget_one_over_rejected"},
					                  PackageFixtureCoverageScope::ARRAY, relation.Name(), operation.name, "",
					                  column.name);
				} else if (column.ElementType() == CompiledScalarType::DOUBLE) {
					coverage.Variants(prefix,
					                  {"zero", "negative_zero", "largest_finite", "smallest_normal",
					                   "smallest_subnormal", "magnitude_overflow_rejected"},
					                  PackageFixtureCoverageScope::ARRAY, relation.Name(), operation.name, "",
					                  column.name);
				}
			}
		}
	}
}

void AddSelectionAndPredicateCoverage(CoverageBuilder &coverage, const CompiledConnector &connector) {
	for (const auto &relation : connector.Relations()) {
		if (relation.Operations().size() > 1) {
			for (const auto &operation : relation.Operations()) {
				coverage.Variants("selection_" + relation.Name() + "_" + operation.name + "_", {"selected"},
				                  PackageFixtureCoverageScope::OPERATION_SELECTION, relation.Name(), operation.name);
			}
		}
	}
	for (const auto &relation : connector.Relations()) {
		if (relation.Operations().size() > 1) {
			coverage.Variants("selection_" + relation.Name() + "_",
			                  {"highest_rank_tie_rejected", "no_candidate_rejected", "fallback_selected"},
			                  PackageFixtureCoverageScope::RELATION_SELECTION, relation.Name());
		}
	}
	for (const auto &relation : connector.Relations()) {
		std::set<std::string> predicate_ids;
		for (const auto &predicate : relation.PredicateMappings()) {
			if (!predicate_ids.insert(predicate.Name()).second) {
				continue;
			}
			coverage.Variants("predicate_" + relation.Name() + "_" + predicate.Name() + "_",
			                  {"positive", "non_true", "duplicates", "unavailable_structure_local"},
			                  PackageFixtureCoverageScope::PREDICATE, relation.Name(), "", "", "", predicate.Name());
		}
	}
}

void AddProtocolCoverage(CoverageBuilder &coverage, const CompiledConnector &connector) {
	for (const auto &relation : connector.Relations()) {
		for (const auto &operation : relation.Operations()) {
			if (operation.Protocol() == CompiledProtocol::REST &&
			    operation.Rest().pagination.Strategy() == CompiledPaginationStrategy::DISABLED) {
				coverage.Variants("pagination_" + relation.Name() + "_" + operation.name + "_",
				                  {"single_page_termination"}, PackageFixtureCoverageScope::PAGINATION, relation.Name(),
				                  operation.name);
			}
		}
	}
	for (const auto &relation : connector.Relations()) {
		for (const auto &operation : relation.Operations()) {
			if (operation.Protocol() == CompiledProtocol::REST &&
			    operation.Rest().pagination.Strategy() == CompiledPaginationStrategy::LINK_HEADER) {
				coverage.Variants("pagination_" + relation.Name() + "_" + operation.name + "_",
				                  {"first_page", "multi_page", "termination", "encoded_target",
				                   "malformed_target_rejected", "replayed_target_rejected", "max_pages_exhausted"},
				                  PackageFixtureCoverageScope::PAGINATION, relation.Name(), operation.name);
			}
		}
	}
	for (const auto &relation : connector.Relations()) {
		for (const auto &operation : relation.Operations()) {
			if (operation.Protocol() == CompiledProtocol::REST &&
			    operation.Rest().pagination.Strategy() == CompiledPaginationStrategy::RESPONSE_NEXT_URL) {
				coverage.Variants("pagination_" + relation.Name() + "_" + operation.name + "_",
				                  {"first_page", "multi_page", "termination", "encoded_target",
				                   "malformed_target_rejected", "replayed_target_rejected", "max_pages_exhausted",
				                   "next_field_wrong_type_rejected"},
				                  PackageFixtureCoverageScope::PAGINATION, relation.Name(), operation.name);
			}
		}
	}
	for (const auto &relation : connector.Relations()) {
		for (const auto &operation : relation.Operations()) {
			if (operation.Protocol() == CompiledProtocol::REST &&
			    operation.Rest().pagination.Strategy() == CompiledPaginationStrategy::SHORT_PAGE) {
				// RFC 0019: short_page has no external continuation signal, so
				// it correctly omits every target-validation variant
				// (encoded_target/malformed_target_rejected/replayed_target_rejected)
				// link_next and response_next require.
				coverage.Variants("pagination_" + relation.Name() + "_" + operation.name + "_",
				                  {"first_page", "multi_page", "termination_on_short_page", "termination_on_empty_page",
				                   "exact_multiple_page_boundary", "max_pages_exhausted"},
				                  PackageFixtureCoverageScope::PAGINATION, relation.Name(), operation.name);
			}
		}
	}
	for (const auto &relation : connector.Relations()) {
		for (const auto &operation : relation.Operations()) {
			if (operation.Protocol() == CompiledProtocol::REST &&
			    operation.Rest().pagination.Strategy() == CompiledPaginationStrategy::RESPONSE_CURSOR) {
				// RFC 0029: an opaque token cannot be compared against a locally
				// reconstructed expectation, so this strategy has no
				// encoded_target/malformed_target_rejected/replayed_target_rejected
				// variant. What replaces them is proof of confinement: the first
				// page omits the parameter, the token is encoded, it is bounded,
				// a repeat is terminal, and it never reaches explanation or
				// cache identity.
				coverage.Variants("pagination_" + relation.Name() + "_" + operation.name + "_",
				                  {"first_page_omits_cursor", "cursor_transition", "multi_page",
				                   "termination_empty_cursor", "termination_absent_cursor", "termination_null_cursor",
				                   "cursor_wrong_type_rejected", "empty_page_with_cursor_continues",
				                   "repeated_cursor_rejected", "reserved_character_cursor_encoded",
				                   "cursor_byte_budget_boundary", "cursor_byte_budget_one_over_rejected",
				                   "max_pages_exhausted", "cursor_at_page_ceiling_resource_failure",
				                   "cursor_absent_from_explanation", "cursor_absent_from_cache_identity"},
				                  PackageFixtureCoverageScope::PAGINATION, relation.Name(), operation.name);
			}
		}
	}
	for (const auto &relation : connector.Relations()) {
		for (const auto &operation : relation.Operations()) {
			if (operation.Protocol() == CompiledProtocol::GRAPHQL) {
				coverage.Variants("pagination_" + relation.Name() + "_" + operation.name + "_",
				                  {"first_page", "multi_page", "termination", "cursor_transition",
				                   "missing_cursor_rejected", "repeated_cursor_rejected", "max_pages_exhausted"},
				                  PackageFixtureCoverageScope::PAGINATION, relation.Name(), operation.name);
			}
		}
	}
	for (const auto &relation : connector.Relations()) {
		for (const auto &operation : relation.Operations()) {
			if (operation.Protocol() == CompiledProtocol::GRAPHQL) {
				coverage.Variants(
				    "graphql_" + relation.Name() + "_" + operation.name + "_",
				    {"document_identity", "serialized_body_identity", "errors_rejected", "response_role_rejected"},
				    PackageFixtureCoverageScope::GRAPHQL, relation.Name(), operation.name);
			}
		}
	}
}

void AddResourceCoverage(CoverageBuilder &coverage, const CompiledConnector &connector) {
	static const char *RELATION_RESOURCES[] = {"max_response_bytes_per_page", "max_response_bytes_per_scan",
	                                           "max_records_per_page", "max_records_per_scan",
	                                           "max_extracted_string_bytes"};
	for (const auto &relation : connector.Relations()) {
		for (const auto *resource : RELATION_RESOURCES) {
			coverage.Variants("resource_" + relation.Name() + "_" + resource + "_", {"boundary", "one_over_rejected"},
			                  PackageFixtureCoverageScope::RELATION_RESOURCE, relation.Name(), "", "", "", "",
			                  resource);
		}
	}
	static const char *GRAPHQL_RESOURCES[] = {"max_document_bytes", "max_serialized_body_bytes_per_request",
	                                          "max_serialized_body_bytes_per_scan"};
	for (const auto &relation : connector.Relations()) {
		for (const auto &operation : relation.Operations()) {
			if (operation.Protocol() != CompiledProtocol::GRAPHQL) {
				continue;
			}
			for (const auto *resource : GRAPHQL_RESOURCES) {
				coverage.Variants("resource_" + relation.Name() + "_" + operation.name + "_" + resource + "_",
				                  {"boundary", "one_over_rejected"}, PackageFixtureCoverageScope::GRAPHQL_RESOURCE,
				                  relation.Name(), operation.name, "", "", "", resource);
			}
		}
	}
}

const char *RateLimitModeVariant(const CompiledRateLimitPolicy &policy) {
	if (!policy.Declared()) {
		return "mode_disabled";
	}
	switch (policy.mode) {
	case CompiledRateLimitMode::FAIL:
		return "mode_fail";
	case CompiledRateLimitMode::WAIT:
		return "mode_wait";
	case CompiledRateLimitMode::WAIT_IF_DEADLINE_ALLOWS:
		return "mode_wait_if_deadline_allows";
	}
	throw std::invalid_argument("compiled package contains an unknown rate-limit mode");
}

const char *RateLimitFormatVariant(CompiledRateLimitGuidanceFormat format) {
	switch (format) {
	case CompiledRateLimitGuidanceFormat::RETRY_AFTER:
		return "retry_after";
	case CompiledRateLimitGuidanceFormat::DELTA_SECONDS:
		return "delta_seconds";
	case CompiledRateLimitGuidanceFormat::UNIX_SECONDS:
		return "unix_seconds";
	}
	throw std::invalid_argument("compiled package contains an unknown rate-limit guidance format");
}

void AddRateLimitCoverage(CoverageBuilder &coverage, const CompiledConnector &connector) {
	for (const auto &relation : connector.Relations()) {
		for (const auto &operation : relation.Operations()) {
			const auto &policy = operation.RateLimitPolicy();
			const auto prefix = "rate_limit_" + relation.Name() + "_" + operation.name + "_";
			const auto *mode = RateLimitModeVariant(policy);
			coverage.Add(prefix + mode, PackageFixtureCoverageScope::RATE_LIMIT, mode, relation.Name(), operation.name);
			if (policy.Declared()) {
				for (const auto status : policy.statuses) {
					const auto variant = "status_" + std::to_string(status);
					coverage.Add(prefix + variant, PackageFixtureCoverageScope::RATE_LIMIT, variant, relation.Name(),
					             operation.name, "", "", "", "", "", status);
				}
				for (std::size_t index = 0; index < policy.guidance.size(); index++) {
					const auto &guidance = policy.guidance[index];
					const auto variant =
					    "guidance_" + std::to_string(index) + "_" + RateLimitFormatVariant(guidance.format);
					coverage.Add(prefix + variant, PackageFixtureCoverageScope::RATE_LIMIT, variant, relation.Name(),
					             operation.name, "", "", "", "", "", 0, guidance.header_name, guidance.format);
				}
				if (!policy.remaining_quota_header.empty()) {
					coverage.Add(prefix + "remaining_present", PackageFixtureCoverageScope::RATE_LIMIT,
					             "remaining_present", relation.Name(), operation.name, "", "", "", "", "", 0,
					             policy.remaining_quota_header);
				}
				if (!policy.remote_bucket_header.empty()) {
					coverage.Add(prefix + "remote_bucket_present", PackageFixtureCoverageScope::RATE_LIMIT,
					             "remote_bucket_present", relation.Name(), operation.name, "", "", "", "", "", 0,
					             policy.remote_bucket_header);
				}
			}
			coverage.Add(prefix + "safe_explanation", PackageFixtureCoverageScope::RATE_LIMIT, "safe_explanation",
			             relation.Name(), operation.name);
		}
	}
}

void AddLifecycleAndDiagnosticCoverage(CoverageBuilder &coverage, const CompiledConnector &connector) {
	coverage.Variants("compiler_cancellation_",
	                  {"source_enumeration", "source_read", "yaml_parse", "reference_validation",
	                   "generation_validation", "publication_wait"},
	                  PackageFixtureCoverageScope::COMPILER_CANCELLATION);
	for (const auto &relation : connector.Relations()) {
		for (const auto &operation : relation.Operations()) {
			coverage.Variants("runtime_cancellation_" + relation.Name() + "_" + operation.name + "_",
			                  {"before_request", "transport", "decode", "page_boundary", "stream_close"},
			                  PackageFixtureCoverageScope::RUNTIME_CANCELLATION, relation.Name(), operation.name);
		}
	}
	coverage.Variants("source_identity_",
	                  {"copied_root", "byte_change", "symlink_rejected", "hardlink_rejected", "entry_change_rejected",
	                   "unlisted_relation_rejected", "case_collision_rejected"},
	                  PackageFixtureCoverageScope::SOURCE_IDENTITY);
	coverage.Variants("reload_",
	                  {"exact_no_op", "compatible_patch", "compatible_minor", "version_reuse_rejected",
	                   "downgrade_rejected", "incompatible_rejected"},
	                  PackageFixtureCoverageScope::RELOAD);
	static const char *DIAGNOSTICS[] = {"unsupported_spec",  "unsupported_dialect", "malformed_yaml",
	                                    "unknown_field",     "missing_field",       "duplicate_id",
	                                    "invalid_reference", "invalid_identifier",  "invalid_type",
	                                    "invalid_extractor", "reserved_input",      "unsupported_declaration",
	                                    "invalid_selector",  "invalid_predicate",   "invalid_graphql_profile",
	                                    "policy_widening",   "resource_exhausted",  "package_identity",
	                                    "fixture_mismatch",  "incompatible_reload", "publication_conflict"};
	for (const auto *code : DIAGNOSTICS) {
		std::string diagnostic = "CUAC_";
		for (const auto character : std::string(code)) {
			diagnostic.push_back(character >= 'a' && character <= 'z' ? static_cast<char>(character - 'a' + 'A')
			                                                          : character);
		}
		coverage.Add(std::string("diagnostic_cuac_") + code, PackageFixtureCoverageScope::DIAGNOSTIC, code, "", "", "",
		             "", "", "", diagnostic);
	}
}

} // namespace

PackageFixtureCoverage::PackageFixtureCoverage(std::vector<std::string> required_keys_p,
                                               std::vector<PackageFixtureCoverageEntry> entries_p,
                                               std::string ordered_digest_p)
    : required_keys(std::move(required_keys_p)), entries(std::move(entries_p)),
      ordered_digest(std::move(ordered_digest_p)) {
}

const std::vector<std::string> &PackageFixtureCoverage::RequiredKeys() const noexcept {
	return required_keys;
}

const std::vector<PackageFixtureCoverageEntry> &PackageFixtureCoverage::Entries() const noexcept {
	return entries;
}

const std::string &PackageFixtureCoverage::OrderedDigest() const noexcept {
	return ordered_digest;
}

PackageFixtureCoverage DerivePackageFixtureCoverage(const CompiledPackageGeneration &generation) {
	const auto &spec = generation.Identity().SpecIdentifier();
	if (spec != "cuac/v1") {
		throw std::invalid_argument("fixture coverage requires one compiled cuac/v1 package generation");
	}
	CoverageBuilder coverage;
	const auto &connector = generation.Connector();
	AddOperationCoverage(coverage, connector);
	AddInputCoverage(coverage, connector);
	AddColumnCoverage(coverage, connector);
	AddArrayCoverage(coverage, connector);
	AddSelectionAndPredicateCoverage(coverage, connector);
	AddProtocolCoverage(coverage, connector);
	AddResourceCoverage(coverage, connector);
	AddRateLimitCoverage(coverage, connector);
	AddLifecycleAndDiagnosticCoverage(coverage, connector);
	return coverage.Finish();
}

} // namespace connector
} // namespace cuac
