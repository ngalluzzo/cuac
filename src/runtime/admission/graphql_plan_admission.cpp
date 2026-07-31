#include "cuac/internal/runtime/admission/graphql_plan_admission.hpp"
#include "cuac/internal/runtime/resilience/rate_limit_policy_admission.hpp"

#include "cuac/connector/content_digest.hpp"
#include "cuac/internal/runtime/admission/graphql_recipe_admission.hpp"
#include "cuac/internal/runtime/executor/http_scan_executor.hpp"
#include "cuac/internal/runtime/policy/request_validation.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <new>
#include <set>
#include <utility>

namespace cuac {
namespace internal {

namespace {

bool SamePath(const PlannedGraphqlResponsePath &left, const PlannedGraphqlResponsePath &right) {
	return left.segments == right.segments;
}

bool IsPrefixWithOneLeaf(const PlannedGraphqlResponsePath &prefix, const PlannedGraphqlResponsePath &path) {
	return path.segments.size() == prefix.segments.size() + 1 &&
	       std::equal(prefix.segments.begin(), prefix.segments.end(), path.segments.begin());
}

bool IsDisjoint(const PlannedGraphqlResponsePath &left, const PlannedGraphqlResponsePath &right) {
	const auto common = std::min(left.segments.size(), right.segments.size());
	return !std::equal(left.segments.begin(), left.segments.begin() + static_cast<std::ptrdiff_t>(common),
	                   right.segments.begin());
}

bool TryScalarKind(PlannedGraphqlScalarKind planned, ValueKind &kind, const char *&logical) {
	switch (planned) {
	case PlannedGraphqlScalarKind::STRING:
		kind = ValueKind::VARCHAR;
		logical = "VARCHAR";
		return true;
	case PlannedGraphqlScalarKind::INT64:
		kind = ValueKind::BIGINT;
		logical = "BIGINT";
		return true;
	case PlannedGraphqlScalarKind::BOOLEAN:
		kind = ValueKind::BOOLEAN;
		logical = "BOOLEAN";
		return true;
	}
	return false;
}

bool TryOutputType(const PlannedGraphqlResultColumn &planned, OutputValueType &type, std::string &logical) {
	ValueKind kind = ValueKind::VARCHAR;
	const char *scalar_logical = nullptr;
	if (!TryScalarKind(planned.scalar_kind, kind, scalar_logical)) {
		return false;
	}
	if (planned.shape == PlannedResultShape::SCALAR) {
		if (planned.element_nullable) {
			return false;
		}
		type = OutputValueType::Scalar(kind);
		logical = scalar_logical;
		return true;
	}
	if (planned.shape == PlannedResultShape::ARRAY) {
		type = OutputValueType::Array(kind, planned.element_nullable);
		logical = std::string(scalar_logical) + "[]";
		return true;
	}
	return false;
}

bool TryColumnElementKind(PlannedColumnScalarKind planned, ValueKind &kind) {
	switch (planned) {
	case PlannedColumnScalarKind::BOOLEAN:
		kind = ValueKind::BOOLEAN;
		return true;
	case PlannedColumnScalarKind::BIGINT:
		kind = ValueKind::BIGINT;
		return true;
	case PlannedColumnScalarKind::VARCHAR:
		kind = ValueKind::VARCHAR;
		return true;
	case PlannedColumnScalarKind::DOUBLE:
		kind = ValueKind::DOUBLE;
		return true;
	}
	return false;
}

std::string Extractor(const std::vector<std::string> &segments) {
	std::string result = "$";
	for (const auto &segment : segments) {
		result += "." + segment;
	}
	return result;
}

bool HasColumns(const PlannedGraphqlOperation &operation, const std::vector<PlannedColumn> &output) {
	if (operation.result_columns.empty() || operation.result_columns.size() > 256 ||
	    output.size() != operation.result_columns.size()) {
		return false;
	}
	std::set<std::string> names;
	std::set<std::string> paths;
	for (std::size_t index = 0; index < output.size(); index++) {
		const auto &result = operation.result_columns[index];
		const auto &column = output[index];
		OutputValueType type;
		ValueKind output_kind = ValueKind::VARCHAR;
		std::string logical;
		const auto joined_path = Extractor(result.response_path.segments);
		if (!IsSafeLogicalId(result.name) || !names.insert(result.name).second ||
		    !IsSafeGraphqlPath(result.response_path.segments, 1, 2) || !paths.insert(joined_path).second ||
		    !TryOutputType(result, type, logical) || column.name != result.name ||
		    column.shape !=
		        (result.shape == PlannedResultShape::ARRAY ? PlannedColumnShape::ARRAY : PlannedColumnShape::SCALAR) ||
		    column.element_nullable != result.element_nullable ||
		    !TryColumnElementKind(column.element_kind, output_kind) || output_kind != type.element_kind ||
		    column.logical_type != logical || column.nullable != result.nullable || column.extractor != joined_path) {
			return false;
		}
	}
	return true;
}

bool HasVariables(const PlannedGraphqlOperation &operation) {
	if (operation.variables.size() != 2 || !IsGraphqlName(operation.cursor.page_size_variable) ||
	    !IsGraphqlName(operation.cursor.cursor_variable) ||
	    operation.cursor.page_size_variable == operation.cursor.cursor_variable || operation.cursor.page_size == 0) {
		return false;
	}
	const auto &page = operation.variables[0];
	const auto &cursor = operation.variables[1];
	return page.name == operation.cursor.page_size_variable && page.type == PlannedGraphqlVariableType::INT_NON_NULL &&
	       page.source == PlannedGraphqlVariableSource::FIXED_PAGE_SIZE &&
	       page.integer_value == operation.cursor.page_size && cursor.name == operation.cursor.cursor_variable &&
	       cursor.type == PlannedGraphqlVariableType::STRING_NULLABLE &&
	       cursor.source == PlannedGraphqlVariableSource::RUNTIME_CURSOR && cursor.integer_value == 0;
}

bool HasResponseShape(const PlannedGraphqlOperation &operation) {
	const auto &nodes = operation.response.nodes.segments;
	const auto &errors = operation.response.errors.segments;
	const auto &page_info = operation.response.page_info.segments;
	if (!IsSafeGraphqlPath(errors, 1, 1) || errors[0] != "errors" || !IsSafeGraphqlPath(nodes, 3, 18) ||
	    !IsSafeGraphqlPath(page_info, 3, 18) || nodes[0] != "data" || nodes.size() != page_info.size() ||
	    nodes.back() != "nodes" || page_info.back() == "nodes" ||
	    !std::equal(nodes.begin(), nodes.end() - 1, page_info.begin())) {
		return false;
	}
	return IsPrefixWithOneLeaf(operation.response.page_info, operation.cursor.has_next_page) &&
	       IsPrefixWithOneLeaf(operation.response.page_info, operation.cursor.end_cursor);
}

bool HasCursor(const PlannedGraphqlCursor &cursor) {
	return cursor.direction == PlannedGraphqlCursorDirection::FORWARD &&
	       cursor.dependency == PlannedGraphqlCursorDependency::SEQUENTIAL &&
	       cursor.consistency == PlannedGraphqlCursorConsistency::MUTABLE && !cursor.supports_total &&
	       !cursor.supports_resume && cursor.max_concurrent_pages == 1 && cursor.max_pages_per_scan > 0 &&
	       cursor.max_pages_per_scan <= PAGINATION_MAX_PAGES_PER_SCAN &&
	       IsSafeGraphqlPath(cursor.has_next_page.segments, 4, 19) &&
	       IsSafeGraphqlPath(cursor.end_cursor.segments, 4, 19) &&
	       cursor.has_next_page.segments != cursor.end_cursor.segments;
}

bool SameCursor(const PlannedGraphqlCursor &left, const PlannedGraphqlCursor &right) {
	return left.direction == right.direction && left.dependency == right.dependency &&
	       left.consistency == right.consistency && left.supports_total == right.supports_total &&
	       left.supports_resume == right.supports_resume && left.max_concurrent_pages == right.max_concurrent_pages &&
	       left.page_size_variable == right.page_size_variable && left.page_size == right.page_size &&
	       left.cursor_variable == right.cursor_variable && SamePath(left.has_next_page, right.has_next_page) &&
	       SamePath(left.end_cursor, right.end_cursor) && left.max_pages_per_scan == right.max_pages_per_scan;
}

bool SamePageBudgets(const ResourceBudgets &left, const ResourceBudgets &right) {
	return left.request_attempts == right.request_attempts && left.response_bytes == right.response_bytes &&
	       left.header_bytes == right.header_bytes && left.decompressed_bytes == right.decompressed_bytes &&
	       left.decoded_records == right.decoded_records &&
	       left.extracted_string_bytes == right.extracted_string_bytes && left.json_nesting == right.json_nesting &&
	       left.decoded_memory_bytes == right.decoded_memory_bytes && left.batch_rows == right.batch_rows &&
	       left.wall_milliseconds == right.wall_milliseconds && left.concurrency == right.concurrency &&
	       left.serialized_request_body_bytes == right.serialized_request_body_bytes;
}

bool HasBudgets(const ScanPlan &plan, const HttpExecutionProfile &profile) {
	const auto &operation = plan.Operation().Graphql();
	const auto &page = plan.Pagination().PageBudgets();
	const auto &scan = plan.Pagination().ScanBudgets();
	const bool package_generated = operation.document_identity == PlannedGraphqlDocumentIdentity::PACKAGE_GENERATED_V1;
	const auto expected_page_body =
	    package_generated
	        ? std::min(operation.max_serialized_request_body_bytes_per_request, HOST_MAX_SERIALIZED_REQUEST_BODY_BYTES)
	        : operation.max_serialized_request_body_bytes_per_request;
	auto expected_scan_body = operation.max_serialized_request_body_bytes_per_scan;
	if (package_generated) {
		// Recompute Semantics' reachable aggregate instead of trusting either the
		// declared operation ceiling or the plan's copied scan budget.
		const auto attempts_per_step = plan.ResiliencePolicy().max_attempts_per_step;
		if (operation.cursor.max_pages_per_scan == 0 || attempts_per_step == 0 ||
		    operation.cursor.max_pages_per_scan > std::numeric_limits<std::uint64_t>::max() / attempts_per_step) {
			return false;
		}
		const auto aggregate_attempts = operation.cursor.max_pages_per_scan * attempts_per_step;
		if (expected_page_body > std::numeric_limits<std::uint64_t>::max() / aggregate_attempts) {
			return false;
		}
		const auto reachable_scan_body = expected_page_body * aggregate_attempts;
		expected_scan_body = std::min(std::min(operation.max_serialized_request_body_bytes_per_scan,
		                                       PAGINATION_MAX_SERIALIZED_REQUEST_BODY_BYTES_PER_SCAN),
		                              reachable_scan_body);
	}
	const bool coherent_attempts =
	    page.request_attempts != 0 && scan.pages <= std::numeric_limits<std::uint64_t>::max() / page.request_attempts &&
	    scan.request_attempts >= scan.pages && scan.request_attempts <= scan.pages * page.request_attempts;
	return page.IsWithinPaginatedPageBounds() && scan.IsWithinPaginatedScanBounds() &&
	       SamePageBudgets(plan.Budgets(), page) && page.decoded_records <= profile.max_decoded_records &&
	       page.serialized_request_body_bytes > 0 && page.serialized_request_body_bytes == expected_page_body &&
	       scan.serialized_request_body_bytes == expected_scan_body &&
	       scan.pages == operation.cursor.max_pages_per_scan && coherent_attempts &&
	       scan.response_bytes >= page.response_bytes && scan.header_bytes >= page.header_bytes &&
	       scan.decompressed_bytes >= page.decompressed_bytes && scan.decoded_records >= page.decoded_records &&
	       scan.decoded_memory_bytes >= page.decoded_memory_bytes;
}

bool HasRelationalEnvelope(const ScanPlan &plan) {
	const auto &ownership = plan.Ownership();
	return plan.RemotePredicate() == PlannedPredicate::TRUE_FOR_BASE_DOMAIN &&
	       plan.RemoteAccuracy() == RemotePredicateAccuracy::UNSUPPORTED &&
	       plan.ResidualOwner() == RelationalOwner::DUCKDB &&
	       plan.ConditionalInput() == PlannedConditionalInput::NONE && ownership.filter == RelationalOwner::DUCKDB &&
	       ownership.projection == RelationalOwner::DUCKDB && ownership.ordering == RelationalOwner::DUCKDB &&
	       ownership.limit == RelationalOwner::DUCKDB && ownership.offset == RelationalOwner::DUCKDB &&
	       plan.RemoteOrdering() == RelationalDelegation::NONE &&
	       plan.RuntimeOrdering() == RelationalDelegation::NONE && plan.RemoteLimit() == RelationalDelegation::NONE &&
	       plan.RemoteOffset() == RelationalDelegation::NONE && plan.RuntimeLimit() == RelationalDelegation::NONE &&
	       plan.RuntimeOffset() == RelationalDelegation::NONE;
}

// Document identity and base domain are closed Semantics discriminators. They
// are not request or relational authority, but Runtime still rejects unknown
// values so a newly introduced planner profile cannot execute before its
// handoff is reviewed here.
std::vector<std::string> DerivedRecipePath(const PlannedGraphqlGeneratorRecipe &recipe, const std::string &terminal) {
	std::vector<std::string> result;
	result.reserve(recipe.RootPath().size() + 2);
	result.push_back("data");
	result.insert(result.end(), recipe.RootPath().begin(), recipe.RootPath().end());
	result.push_back(terminal);
	return result;
}

bool HasPackageRecipeCorrelation(const PlannedGraphqlOperation &operation) {
	if (!operation.generator_recipe) {
		return false;
	}
	const auto &recipe = *operation.generator_recipe;
	std::string rendered;
	if (!TryRenderPackageGraphqlRecipe(recipe, operation.max_document_bytes, rendered) ||
	    rendered != operation.document || recipe.Variables().size() != 2 || operation.variables.size() != 2 ||
	    recipe.Variables()[0].Name() != operation.variables[0].name ||
	    recipe.Variables()[0].Name() != operation.cursor.page_size_variable ||
	    recipe.Variables()[0].Type() != PlannedGraphqlRecipeVariableType::INT_NON_NULL ||
	    recipe.Variables()[0].Role() != PlannedGraphqlRecipeVariableRole::PAGE_SIZE ||
	    recipe.Variables()[1].Name() != operation.variables[1].name ||
	    recipe.Variables()[1].Name() != operation.cursor.cursor_variable ||
	    recipe.Variables()[1].Type() != PlannedGraphqlRecipeVariableType::STRING_NULLABLE ||
	    recipe.Variables()[1].Role() != PlannedGraphqlRecipeVariableRole::CURSOR ||
	    recipe.Selections().size() != operation.result_columns.size()) {
		return false;
	}
	const auto nodes = DerivedRecipePath(recipe, recipe.NodesField());
	const auto page_info = DerivedRecipePath(recipe, recipe.PageInfoField());
	if (operation.response.nodes.segments != nodes || operation.response.page_info.segments != page_info) {
		return false;
	}
	auto has_next = page_info;
	has_next.push_back(recipe.HasNextPageField());
	auto end_cursor = page_info;
	end_cursor.push_back(recipe.EndCursorField());
	if (operation.cursor.has_next_page.segments != has_next || operation.cursor.end_cursor.segments != end_cursor) {
		return false;
	}
	for (std::size_t index = 0; index < recipe.Selections().size(); index++) {
		if (recipe.Selections()[index].ColumnName() != operation.result_columns[index].name ||
		    recipe.Selections()[index].FieldPath() != operation.result_columns[index].response_path.segments) {
			return false;
		}
	}
	return true;
}

bool HasKnownExecutionProfile(const ScanPlan &plan) {
	const auto &operation = plan.Operation().Graphql();
	return operation.document_identity == PlannedGraphqlDocumentIdentity::PACKAGE_GENERATED_V1 &&
	       plan.Domain() == BaseDomain::GRAPHQL_RELAY_CONNECTION_NODE_OCCURRENCES &&
	       HasPackageRecipeCorrelation(operation);
}

bool HasAuthority(const ScanPlan &plan, const HttpExecutionProfile &profile, bool &requires_bearer, RetryPlan &retry,
                  AdmittedRateLimitPolicy &rate_limit, AdmittedResiliencePolicy &resilience) {
	const auto &operation = plan.Operation().Graphql();
	const auto &obligation = plan.AuthenticationObligation();
	const auto *destination = obligation.Destination();
	const auto &network = plan.Network();
	const bool bearer = plan.Authentication() == FeatureState::ENABLED && plan.SecretReference().IsPresent() &&
	                    obligation.Requirement() == PlannedCredentialRequirement::REQUIRED &&
	                    obligation.LogicalCredential() == "token" &&
	                    obligation.Authenticator() == PlannedAuthenticator::BEARER &&
	                    obligation.Placement() == PlannedCredentialPlacement::AUTHORIZATION_HEADER && destination &&
	                    destination->scheme == operation.origin.scheme && destination->host == operation.origin.host &&
	                    destination->port == operation.origin.port;
	const bool anonymous = plan.Authentication() == FeatureState::DISABLED && !plan.SecretReference().IsPresent() &&
	                       obligation.Requirement() == PlannedCredentialRequirement::NONE &&
	                       obligation.LogicalCredential().empty() &&
	                       obligation.Authenticator() == PlannedAuthenticator::NONE &&
	                       obligation.Placement() == PlannedCredentialPlacement::NONE && destination == nullptr;
	requires_bearer = bearer;
	return plan.Providers() == FeatureState::DISABLED && plan.Cache() == FeatureState::DISABLED &&
	       (bearer || anonymous) && HasExactNetworkCapability(network, operation.origin, profile) &&
	       TryAdmitResiliencePolicies(plan, profile, retry, rate_limit, resilience);
}

bool IsAdmissible(const ScanPlan &plan, const HttpExecutionProfile &profile, bool &requires_bearer, RetryPlan &retry,
                  AdmittedRateLimitPolicy &rate_limit, AdmittedResiliencePolicy &resilience) {
	if (plan.Operation().Protocol() != PlannedProtocol::GRAPHQL ||
	    plan.Pagination().Strategy() != PlannedPaginationStrategy::GRAPHQL_CURSOR) {
		return false;
	}
	const auto &operation = plan.Operation().Graphql();
	std::vector<HttpHeader> headers;
	if (operation.cardinality != PlannedCardinality::ZERO_TO_MANY ||
	    operation.replay_safety != PlannedReplaySafety::SAFE || operation.kind != PlannedGraphqlOperationKind::QUERY ||
	    operation.digest_algorithm != PlannedGraphqlDigestAlgorithm::SHA256 || operation.document.empty() ||
	    operation.max_document_bytes == 0 || operation.max_document_bytes > 64ULL * 1024ULL ||
	    static_cast<uint64_t>(operation.document.size()) > operation.max_document_bytes ||
	    !IsValidUtf8(operation.document) || ComputeSha256Hex(operation.document) != operation.document_digest ||
	    !IsSafeDnsHost(operation.origin.host) || operation.origin.port == 0 || !IsSafeRequestPath(operation.path) ||
	    !HasKnownExecutionProfile(plan) ||
	    !TryCopyFixedHeaders(operation.headers, true, plan.Pagination().PageBudgets().header_bytes, headers) ||
	    !HasVariables(operation) || !HasColumns(operation, plan.OutputColumns()) || !HasCursor(operation.cursor) ||
	    !SameCursor(operation.cursor, plan.Pagination().GraphqlCursor()) ||
	    operation.response.partial_data != PlannedGraphqlPartialDataPolicy::FAIL_ON_ANY_ERROR ||
	    !HasResponseShape(operation) || !IsDisjoint(operation.response.nodes, operation.response.errors) ||
	    !IsDisjoint(operation.response.page_info, operation.response.errors) ||
	    operation.response.nodes.segments == operation.response.page_info.segments) {
		return false;
	}
	return HasBudgets(plan, profile) && HasRelationalEnvelope(plan) &&
	       HasAuthority(plan, profile, requires_bearer, retry, rate_limit, resilience);
}

} // namespace

AdmittedGraphqlRequestProfile::AdmittedGraphqlRequestProfile(const ScanPlan &plan, bool requires_bearer_p,
                                                             RetryPlan retry_p, AdmittedRateLimitPolicy rate_limit_p,
                                                             AdmittedResiliencePolicy resilience_p)
    : method("POST"), scheme("https"), port(plan.Operation().Graphql().origin.port),
      page_size(plan.Operation().Graphql().cursor.page_size),
      max_pages(plan.Operation().Graphql().cursor.max_pages_per_scan),
      max_request_body_bytes(plan.Pagination().PageBudgets().serialized_request_body_bytes),
      max_scan_body_bytes(plan.Pagination().ScanBudgets().serialized_request_body_bytes),
      requires_bearer(requires_bearer_p), page_budgets(plan.Pagination().PageBudgets()),
      scan_budgets(plan.Pagination().ScanBudgets()), retry(retry_p), rate_limit(std::move(rate_limit_p)),
      resilience(resilience_p) {
	page_budgets.request_attempts = resilience.max_attempts_per_step;
	scan_budgets.request_attempts = resilience.max_attempts_per_scan;
	const auto &operation = plan.Operation().Graphql();
	host = operation.origin.host;
	path = operation.path;
	(void)TryCopyFixedHeaders(operation.headers, true, page_budgets.header_bytes, headers);
	document = operation.document;
	for (const auto &column : operation.result_columns) {
		OutputValueType type;
		std::string logical;
		(void)TryOutputType(column, type, logical);
		columns.push_back({column.name, type, column.nullable, column.response_path.segments});
	}
	page_size_variable = operation.cursor.page_size_variable;
	cursor_variable = operation.cursor.cursor_variable;
	nodes_path = operation.response.nodes.segments;
	errors_path = operation.response.errors.segments;
	page_info_path = operation.response.page_info.segments;
	has_next_page_path = operation.cursor.has_next_page.segments;
	end_cursor_path = operation.cursor.end_cursor.segments;
}

const std::string &AdmittedGraphqlRequestProfile::Method() const {
	return method;
}
const std::string &AdmittedGraphqlRequestProfile::Scheme() const {
	return scheme;
}
const std::string &AdmittedGraphqlRequestProfile::Host() const {
	return host;
}
uint16_t AdmittedGraphqlRequestProfile::Port() const {
	return port;
}
const std::string &AdmittedGraphqlRequestProfile::Path() const {
	return path;
}
const std::vector<HttpHeader> &AdmittedGraphqlRequestProfile::Headers() const {
	return headers;
}
const std::string &AdmittedGraphqlRequestProfile::Document() const {
	return document;
}
const std::vector<AdmittedGraphqlColumn> &AdmittedGraphqlRequestProfile::Columns() const {
	return columns;
}
const std::string &AdmittedGraphqlRequestProfile::PageSizeVariable() const {
	return page_size_variable;
}
const std::string &AdmittedGraphqlRequestProfile::CursorVariable() const {
	return cursor_variable;
}
const std::vector<std::string> &AdmittedGraphqlRequestProfile::NodesPath() const {
	return nodes_path;
}
const std::vector<std::string> &AdmittedGraphqlRequestProfile::ErrorsPath() const {
	return errors_path;
}
const std::vector<std::string> &AdmittedGraphqlRequestProfile::PageInfoPath() const {
	return page_info_path;
}
const std::vector<std::string> &AdmittedGraphqlRequestProfile::HasNextPagePath() const {
	return has_next_page_path;
}
const std::vector<std::string> &AdmittedGraphqlRequestProfile::EndCursorPath() const {
	return end_cursor_path;
}
uint64_t AdmittedGraphqlRequestProfile::PageSize() const {
	return page_size;
}
uint64_t AdmittedGraphqlRequestProfile::MaxPages() const {
	return max_pages;
}
uint64_t AdmittedGraphqlRequestProfile::MaxRequestBodyBytes() const {
	return max_request_body_bytes;
}
uint64_t AdmittedGraphqlRequestProfile::MaxScanBodyBytes() const {
	return max_scan_body_bytes;
}
bool AdmittedGraphqlRequestProfile::RequiresBearer() const {
	return requires_bearer;
}
const ResourceBudgets &AdmittedGraphqlRequestProfile::PageBudgets() const {
	return page_budgets;
}
const ScanResourceBudgets &AdmittedGraphqlRequestProfile::ScanBudgets() const {
	return scan_budgets;
}
const RetryPlan &AdmittedGraphqlRequestProfile::RetryPolicy() const {
	return retry;
}
const AdmittedRateLimitPolicy &AdmittedGraphqlRequestProfile::RateLimitPolicy() const {
	return rate_limit;
}
const AdmittedResiliencePolicy &AdmittedGraphqlRequestProfile::ResiliencePolicy() const {
	return resilience;
}

std::unique_ptr<const AdmittedGraphqlRequestProfile> TryAdmitGraphqlPlan(const ScanPlan &plan,
                                                                         const HttpExecutionProfile &profile) {
	try {
		bool requires_bearer = false;
		RetryPlan retry {};
		AdmittedRateLimitPolicy rate_limit {};
		AdmittedResiliencePolicy resilience {};
		if (!IsAdmissible(plan, profile, requires_bearer, retry, rate_limit, resilience)) {
			return {};
		}
		return std::unique_ptr<const AdmittedGraphqlRequestProfile>(
		    new AdmittedGraphqlRequestProfile(plan, requires_bearer, retry, std::move(rate_limit), resilience));
	} catch (const std::bad_alloc &) {
		throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
		                     "GraphQL request profile could not be allocated within its memory budget");
	} catch (const ExecutionError &) {
		return {};
	} catch (...) {
		return {};
	}
}

} // namespace internal
} // namespace cuac
