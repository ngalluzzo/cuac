#include "cuac/semantics/scan_planner.hpp"

#include "cuac/internal/semantics/predicate/predicate_classifier.hpp"
#include "cuac/internal/semantics/planner/scan_plan_builder.hpp"
#include "cuac/internal/semantics/planner/scan_planner.hpp"

#include "cuac/semantics/cache_policy.hpp"
#include "cuac/connector/package_semver.hpp"

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <utility>

namespace cuac {

namespace {

PlannedColumnScalarKind PlanColumnKind(CompiledScalarType type) {
	switch (type) {
	case CompiledScalarType::BOOLEAN:
		return PlannedColumnScalarKind::BOOLEAN;
	case CompiledScalarType::BIGINT:
		return PlannedColumnScalarKind::BIGINT;
	case CompiledScalarType::VARCHAR:
		return PlannedColumnScalarKind::VARCHAR;
	case CompiledScalarType::DOUBLE:
		return PlannedColumnScalarKind::DOUBLE;
	}
	throw std::logic_error("compiled output column contains an unknown element type");
}

PlannedColumnShape PlanColumnShape(CompiledColumnShape shape) {
	switch (shape) {
	case CompiledColumnShape::SCALAR:
		return PlannedColumnShape::SCALAR;
	case CompiledColumnShape::ARRAY:
		return PlannedColumnShape::ARRAY;
	}
	throw std::logic_error("compiled output column contains an unknown shape");
}

PlannedOperationReplayClass PlanReplayClass(CompiledOperationReplayClass replay_class) {
	switch (replay_class) {
	case CompiledOperationReplayClass::NON_REPLAYABLE:
		return PlannedOperationReplayClass::NON_REPLAYABLE;
	case CompiledOperationReplayClass::REPLAYABLE_READ:
		return PlannedOperationReplayClass::REPLAYABLE_READ;
	case CompiledOperationReplayClass::REPLAYABLE_WITH_IDEMPOTENCY_MECHANISM:
		return PlannedOperationReplayClass::REPLAYABLE_WITH_IDEMPOTENCY_MECHANISM;
	case CompiledOperationReplayClass::UNKNOWN:
		return PlannedOperationReplayClass::UNKNOWN;
	}
	throw std::logic_error("compiled operation contains an unknown replay class");
}

} // namespace

namespace {

void AppendDomain(std::string &buffer, BaseDomain domain) {
	switch (domain) {
	case BaseDomain::JSON_PATH_RECORDS:
		buffer.append("json_path");
		break;
	case BaseDomain::ROOT_ARRAY_RECORDS:
		buffer.append("root_array");
		break;
	case BaseDomain::PAGINATED_JSON_PATH_RECORDS:
		buffer.append("pag_json_path");
		break;
	case BaseDomain::PAGINATED_ROOT_ARRAY_RECORDS:
		buffer.append("pag_root_array");
		break;
	case BaseDomain::GRAPHQL_RELAY_CONNECTION_NODE_OCCURRENCES:
		buffer.append("gql_relay");
		break;
	case BaseDomain::SUCCESSFUL_ROOT_OBJECT:
		buffer.append("root_object");
		break;
	}
}

void AppendPredicate(std::string &buffer, PlannedPredicate predicate, RemotePredicateAccuracy accuracy,
                     PlannedPredicate residual, PlannedConditionalInput conditional) {
	const char *pred = "true";
	switch (predicate) {
	case PlannedPredicate::TRUE_FOR_BASE_DOMAIN:
		pred = "true";
		break;
	case PlannedPredicate::TYPED_EQUALITY:
		pred = "typed_eq";
		break;
	case PlannedPredicate::COMPLETE_DUCKDB_FILTER:
		pred = "full_filter";
		break;
	}
	const char *acc = "unsupported";
	switch (accuracy) {
	case RemotePredicateAccuracy::UNSUPPORTED:
		acc = "unsupported";
		break;
	case RemotePredicateAccuracy::SUPERSET:
		acc = "superset";
		break;
	case RemotePredicateAccuracy::EXACT:
		acc = "exact";
		break;
	}
	const char *res = "true";
	switch (residual) {
	case PlannedPredicate::TRUE_FOR_BASE_DOMAIN:
		res = "true";
		break;
	case PlannedPredicate::COMPLETE_DUCKDB_FILTER:
		res = "full_filter";
		break;
	default:
		res = "other";
		break;
	}
	const char *cond = "none";
	switch (conditional) {
	case PlannedConditionalInput::NONE:
		cond = "none";
		break;
	case PlannedConditionalInput::REST_QUERY_BINDING:
		cond = "rest_bind";
		break;
	}
	buffer.append("|pred=")
	    .append(pred)
	    .append("|acc=")
	    .append(acc)
	    .append("|res=")
	    .append(res)
	    .append("|cond=")
	    .append(cond);
}

std::string BuildCanonicalRepresentation(const ScanPlan &plan, const ScanRequest &request) {
	std::string buffer;
	const auto &gen = request.generation_identity;
	buffer.append("gen=package|si=")
	    .append(gen.spec_identifier)
	    .append("|ci=")
	    .append(gen.connector_id)
	    .append("|pv=")
	    .append(gen.package_version)
	    .append("|pd=")
	    .append(gen.package_digest);
	buffer.append("|conn=")
	    .append(plan.ConnectorName())
	    .append("|ver=")
	    .append(plan.ConnectorVersion())
	    .append("|rel=")
	    .append(plan.RelationName())
	    .append("|dom=");
	AppendDomain(buffer, plan.Domain());
	buffer.append("|proto=");
	const auto &op = plan.Operation();
	if (op.Protocol() == PlannedProtocol::REST) {
		const auto &rest = op.Rest();
		buffer.append("rest|path=")
		    .append(rest.path)
		    .append("|origin=")
		    .append(rest.origin.scheme == PlannedUrlScheme::HTTPS ? "https" : "http")
		    .append("://")
		    .append(rest.origin.host)
		    .append(":")
		    .append(std::to_string(rest.origin.port));
	} else {
		const auto &gql = op.Graphql();
		buffer.append("gql|endpoint=")
		    .append(gql.origin.scheme == PlannedUrlScheme::HTTPS ? "https" : "http")
		    .append("://")
		    .append(gql.origin.host)
		    .append(":")
		    .append(std::to_string(gql.origin.port))
		    .append(gql.path)
		    .append("|digest=")
		    .append(gql.document_digest);
	}
	AppendPredicate(buffer, plan.RemotePredicate(), plan.RemoteAccuracy(), plan.ResidualPredicate(),
	                plan.ConditionalInput());
	if (plan.TypedEquality() != nullptr) {
		const auto *eq = plan.TypedEquality();
		buffer.append("|eq_col=")
		    .append(eq->ColumnName())
		    .append("|eq_proof=")
		    .append(eq->ProofIdentity())
		    .append("|eq_dom=")
		    .append(eq->BaseDomainIdentity());
	}
	buffer.append("|cols=");
	for (std::size_t i = 0; i < plan.OutputColumns().size(); ++i) {
		if (i > 0) {
			buffer.append(",");
		}
		const auto &col = plan.OutputColumns()[i];
		buffer.append(col.name)
		    .append(":")
		    .append(col.extractor)
		    .append(col.shape == PlannedColumnShape::SCALAR ? ":s" : ":a")
		    .append(col.nullable ? ":n" : ":nn");
	}
	buffer.append("|auth=").append(plan.Authentication() == FeatureState::ENABLED ? "req" : "anon");
	buffer.append("|pag=");
	switch (plan.Pagination().Strategy()) {
	case PlannedPaginationStrategy::DISABLED:
		buffer.append("off");
		break;
	case PlannedPaginationStrategy::LINK_HEADER:
		buffer.append("link");
		break;
	case PlannedPaginationStrategy::RESPONSE_NEXT_URL:
		buffer.append("resp_next");
		break;
	case PlannedPaginationStrategy::GRAPHQL_CURSOR:
		buffer.append("gql_cursor");
		break;
	case PlannedPaginationStrategy::SHORT_PAGE:
		buffer.append("short");
		break;
	case PlannedPaginationStrategy::RESPONSE_CURSOR:
		// Distinct canonical tag: this string is part of plan identity, so a
		// cursor traversal must never hash equal to any page-numbered plan.
		buffer.append("resp_cursor");
		break;
	}
	return buffer;
}

std::size_t HashCanonical(const std::string &canonical) {
	return std::hash<std::string> {}(canonical);
}

} // namespace

PlanningError::PlanningError(PlanningErrorCode code_p, std::string safe_message)
    : std::logic_error(std::move(safe_message)), code(code_p) {
}

PlanningErrorCode PlanningError::Code() const noexcept {
	return code;
}

ScanPlan ScanPlanBuilder::Build(const CompiledConnector &connector, const ScanRequest &request,
                                const CompiledGenerationHandle &generation_handle) {
	using namespace scan_planner_internal;

	const auto selected = ValidateAndSelectOperation(connector, request);
	const auto &relation = *selected.relation;
	const auto &operation = *selected.operation;
	const auto predicate_decision = predicate_classifier::Classify(relation, operation, request);

	ScanPlan result;
	result.connector_name = connector.ConnectorName();
	result.connector_version = connector.Version();
	result.relation_name = relation.Name();
	// Package relation snapshots include author scalar defaults and predicate
	// literals, so the executable plan keeps only stable structural provenance.
	result.source_snapshot = "relation=" + relation.Name() + ";operation=" + operation.name;
	CompiledHttpOrigin operation_origin = operation.Protocol() == CompiledProtocol::REST
	                                          ? operation.Rest().request.origin
	                                          : operation.Graphql().endpoint_origin;
	if (operation.Protocol() == CompiledProtocol::REST) {
		const auto &rest = operation.Rest();
		result.domain = PlanBaseDomain(rest.response_source, rest.pagination.Strategy());
		auto planned = BuildRestOperation(relation, operation, selected.resolved_inputs, predicate_decision);
		result.operation =
		    std::make_shared<const PlannedProtocolOperation>(PlannedProtocolOperation::FromRest(std::move(planned)));
	} else {
		result.domain = BaseDomain::GRAPHQL_RELAY_CONNECTION_NODE_OCCURRENCES;
		result.operation = std::make_shared<const PlannedProtocolOperation>(
		    PlannedProtocolOperation::FromGraphql(PlanGraphqlOperation(operation)));
	}

	for (const auto &column : relation.Columns()) {
		result.output_columns.push_back({column.name, column.logical_type, column.nullable, column.extractor,
		                                 PlanColumnShape(column.Shape()), PlanColumnKind(column.ElementType()),
		                                 column.ElementNullable()});
	}
	result.remote_predicate = predicate_decision.remote_predicate;
	result.remote_accuracy = predicate_decision.remote_accuracy;
	result.residual_predicate = predicate_decision.residual_predicate;
	result.residual_owner = predicate_decision.residual_owner;
	result.conditional_input = predicate_decision.conditional_input;
	if (predicate_decision.typed_equality.present) {
		const auto &equality = predicate_decision.typed_equality;
		result.typed_equality = std::shared_ptr<const PlannedEqualityPredicate>(new PlannedEqualityPredicate(
		    equality.column_name, PlannedPredicateOperator::EQUALS, equality.kind, equality.boolean_value,
		    equality.bigint_value, equality.varchar_value, equality.double_value, equality.conditional_input_id,
		    equality.proof_identity, equality.base_domain_identity, equality.occurrence_preservation));
	}
	result.predicate_category = predicate_decision.category;
	result.predicate_reason = predicate_decision.reason_code;
	result.ownership = {RelationalOwner::DUCKDB, RelationalOwner::DUCKDB, RelationalOwner::DUCKDB,
	                    RelationalOwner::DUCKDB, RelationalOwner::DUCKDB};
	result.remote_ordering = RelationalDelegation::NONE;
	result.runtime_ordering = RelationalDelegation::NONE;
	result.remote_limit = RelationalDelegation::NONE;
	result.remote_offset = RelationalDelegation::NONE;
	result.runtime_limit = RelationalDelegation::NONE;
	result.runtime_offset = RelationalDelegation::NONE;
	result.providers = FeatureState::DISABLED;
	result.replay_class = PlanReplayClass(operation.ReplayClass());
	const auto &retry_recommendation = operation.RetryRecommendation();
	const auto retry_attempts_per_step =
	    retry_recommendation.Enabled() ? retry_recommendation.max_attempts_per_step : 1;
	result.retry = retry_recommendation.Enabled() ? FeatureState::ENABLED : FeatureState::DISABLED;
	result.retry_policy = {
	    retry_attempts_per_step, retry_attempts_per_step,
	    retry_recommendation.Enabled() ? retry_recommendation.max_delay_milliseconds : 0,
	    retry_recommendation.Enabled() ? retry_recommendation.max_cumulative_waiting_milliseconds_per_scan : 0};
	const auto &compiled_rate_limit = operation.RateLimitPolicy();
	result.rate_limit = compiled_rate_limit.Declared() ? FeatureState::ENABLED : FeatureState::DISABLED;
	result.rate_limit_policy.declared = compiled_rate_limit.Declared();
	result.rate_limit_policy.mode = PlanRateLimitMode(compiled_rate_limit.mode);
	result.rate_limit_policy.statuses = compiled_rate_limit.statuses;
	result.rate_limit_policy.operation_family = compiled_rate_limit.operation_family;
	result.rate_limit_policy.scope = PlanRateLimitPrincipalScope(compiled_rate_limit.scope);
	result.rate_limit_policy.guidance.clear();
	result.rate_limit_policy.guidance.reserve(compiled_rate_limit.guidance.size());
	for (const auto &guidance : compiled_rate_limit.guidance) {
		result.rate_limit_policy.guidance.push_back(
		    {guidance.header_name, PlanRateLimitGuidanceFormat(guidance.format)});
	}
	result.rate_limit_policy.remaining_quota_header = compiled_rate_limit.remaining_quota_header;
	result.rate_limit_policy.remote_bucket_header = compiled_rate_limit.remote_bucket_header;
	result.rate_limit_policy.max_attempts_per_step = compiled_rate_limit.max_attempts_per_step;
	result.rate_limit_policy.max_delay_milliseconds = compiled_rate_limit.max_delay_milliseconds;
	result.rate_limit_policy.max_cumulative_waiting_milliseconds_per_scan =
	    compiled_rate_limit.max_cumulative_waiting_milliseconds_per_scan;
	result.rate_limit_policy.package_major_version =
	    compiled_rate_limit.Declared() ? PackageSemVer::Parse(connector.Version()).Major() : 0;
	const auto rate_limit_attempts_per_step =
	    compiled_rate_limit.WaitingEnabled() ? compiled_rate_limit.max_attempts_per_step : 1;
	const auto attempts_per_step = std::max(retry_attempts_per_step, rate_limit_attempts_per_step);
	result.resilience_policy = {attempts_per_step, attempts_per_step,
	                            BoundedSum(result.retry_policy.max_cumulative_waiting_milliseconds_per_scan,
	                                       result.rate_limit_policy.max_cumulative_waiting_milliseconds_per_scan,
	                                       RESILIENCE_MAX_CUMULATIVE_WAITING_MILLISECONDS_PER_SCAN,
	                                       "aggregate resilience waiting scope")};
	result.cache = FeatureState::DISABLED;
	result.secret_reference = request.secret_reference.IsPresent()
	                              ? PlannedSecretReference(request.secret_reference.Name())
	                              : PlannedSecretReference();
	const auto &authentication = relation.Authentication();
	if (authentication.Requirement() == CompiledCredentialRequirement::NONE) {
		result.authentication = FeatureState::DISABLED;
	} else {
		result.authentication = FeatureState::ENABLED;
		result.authentication_obligation.requirement = PlannedCredentialRequirement::REQUIRED;
		result.authentication_obligation.logical_credential = authentication.LogicalCredential();
		result.authentication_obligation.authenticator = PlanAuthenticator(authentication.Authenticator());
		result.authentication_obligation.placement = PlanCredentialPlacement(authentication.Placement());
		result.authentication_obligation.placement_name = authentication.PlacementName();
		result.authentication_obligation.has_destination = true;
		const auto &destination = *authentication.Destination();
		result.authentication_obligation.destination = {PlanUrlScheme(destination.scheme), destination.host.Value(),
		                                                destination.port};
	}

	// The selected plan narrows catalog-wide network policy to the one operation
	// origin. It never grants another relation's host or scheme to this scan.
	result.network = {{UrlSchemeName(operation_origin.scheme)},
	                  {operation_origin.host.Value()},
	                  operation_origin.port,
	                  false,
	                  false,
	                  false,
	                  connector.NetworkPolicy().loopback_addresses_enabled};
	const auto &ceilings = relation.ResourceCeilings();
	if (operation.Protocol() == CompiledProtocol::REST &&
	    operation.Rest().pagination.Strategy() == CompiledPaginationStrategy::DISABLED) {
		const auto response_bytes =
		    ceilings.HasResponseByteNarrowing()
		        ? std::min(ceilings.MaxResponseBytesPerPage(), connector.NetworkPolicy().max_response_bytes)
		        : connector.NetworkPolicy().max_response_bytes;
		result.budgets = {attempts_per_step,
		                  std::min(response_bytes, HOST_MAX_RESPONSE_BYTES),
		                  HOST_MAX_HEADER_BYTES,
		                  HOST_MAX_DECOMPRESSED_BYTES,
		                  std::min(ceilings.MaxRecordsPerPage(), HOST_MAX_DECODED_RECORDS),
		                  std::min(ceilings.MaxExtractedStringBytes(), HOST_MAX_EXTRACTED_STRING_BYTES),
		                  HOST_MAX_JSON_NESTING,
		                  HOST_MAX_DECODED_MEMORY_BYTES,
		                  OUTPUT_BATCH_ROWS,
		                  MAX_EXECUTION_MILLISECONDS,
		                  HOST_MAX_CONCURRENCY,
		                  0};
		if (!result.budgets.IsWithinLiveRestBounds()) {
			throw std::logic_error("selected relation produced an invalid single-response resource envelope");
		}
		result.retry_policy.max_attempts_per_scan = retry_attempts_per_step;
		result.resilience_policy.max_attempts_per_scan = attempts_per_step;
	} else if (operation.Protocol() == CompiledProtocol::REST) {
		const auto &rest = operation.Rest();
		const auto &compiled_pagination = rest.pagination;
		result.pagination.strategy = PlanPaginationStrategy(compiled_pagination.Strategy());
		result.pagination.dependency = PlanPageDependency(compiled_pagination.Dependency());
		result.pagination.consistency = PlanPageConsistency(compiled_pagination.Consistency());
		result.pagination.link_relation = PlanLinkRelation(compiled_pagination.LinkRelation());
		result.pagination.target_scope = PlanTargetScope(compiled_pagination.TargetScope());
		result.pagination.supports_total = compiled_pagination.SupportsTotal();
		result.pagination.supports_resume = compiled_pagination.SupportsResume();
		if (compiled_pagination.Strategy() == CompiledPaginationStrategy::RESPONSE_NEXT_URL) {
			result.pagination.next_url_path = compiled_pagination.NextUrlPath();
		}
		if (compiled_pagination.Strategy() == CompiledPaginationStrategy::RESPONSE_CURSOR) {
			// RFC 0029: a cursor traversal has no page number, so it fills its
			// own continuation target instead of the page-number target. Both
			// carry the operation's own origin and path, which is what keeps a
			// received token from ever influencing where the request goes.
			result.pagination.cursor_target = {
			    {PlanUrlScheme(rest.request.origin.scheme), rest.request.origin.host.Value(), rest.request.origin.port},
			    rest.request.path,
			    compiled_pagination.CursorPath(),
			    compiled_pagination.CursorParameter(),
			    compiled_pagination.MaxCursorBytes()};
		} else {
			result.pagination.target = {
			    {PlanUrlScheme(rest.request.origin.scheme), rest.request.origin.host.Value(), rest.request.origin.port},
			    rest.request.path,
			    compiled_pagination.PageSizeParameter(),
			    compiled_pagination.PageSize(),
			    compiled_pagination.PageNumberParameter(),
			    compiled_pagination.FirstPage(),
			    compiled_pagination.PageIncrement()};
		}

		const auto page_response_bytes =
		    std::min(std::min(ceilings.MaxResponseBytesPerPage(), connector.NetworkPolicy().max_response_bytes),
		             PAGINATION_MAX_RESPONSE_BYTES_PER_PAGE);
		result.pagination.page_budgets = {
		    attempts_per_step,
		    page_response_bytes,
		    PAGINATION_MAX_HEADER_BYTES_PER_PAGE,
		    PAGINATION_MAX_DECOMPRESSED_BYTES_PER_PAGE,
		    std::min(ceilings.MaxRecordsPerPage(), PAGINATION_MAX_DECODED_RECORDS_PER_PAGE),
		    std::min(ceilings.MaxExtractedStringBytes(), PAGINATION_MAX_EXTRACTED_STRING_BYTES),
		    PAGINATION_MAX_JSON_NESTING,
		    PAGINATION_MAX_DECODED_MEMORY_BYTES,
		    PAGINATION_OUTPUT_BATCH_ROWS,
		    PAGINATION_MAX_EXECUTION_MILLISECONDS,
		    PAGINATION_MAX_CONCURRENCY,
		    0};

		const auto max_pages = compiled_pagination.MaxPagesPerScan();
		const auto aggregate_attempts = BoundedProduct(
		    max_pages, attempts_per_step, RETRY_MAX_REQUEST_ATTEMPTS_PER_SCAN, "aggregate request-attempt scope");
		result.retry_policy.max_attempts_per_scan =
		    BoundedProduct(max_pages, retry_attempts_per_step, RETRY_MAX_REQUEST_ATTEMPTS_PER_SCAN,
		                   "ordinary retry request-attempt scope");
		result.resilience_policy.max_attempts_per_scan = aggregate_attempts;
		result.pagination.scan_budgets = {
		    aggregate_attempts,
		    max_pages,
		    std::min(ceilings.MaxResponseBytesPerScan(), PAGINATION_MAX_RESPONSE_BYTES_PER_SCAN),
		    BoundedProduct(PAGINATION_MAX_HEADER_BYTES_PER_PAGE, max_pages, PAGINATION_MAX_HEADER_BYTES_PER_SCAN,
		                   "aggregate header-byte scope"),
		    BoundedProduct(PAGINATION_MAX_DECOMPRESSED_BYTES_PER_PAGE, max_pages,
		                   PAGINATION_MAX_DECOMPRESSED_BYTES_PER_SCAN, "aggregate decompressed-byte scope"),
		    std::min(ceilings.MaxRecordsPerScan(), PAGINATION_MAX_DECODED_RECORDS_PER_SCAN),
		    std::min(ceilings.MaxExtractedStringBytes(), PAGINATION_MAX_EXTRACTED_STRING_BYTES),
		    PAGINATION_MAX_JSON_NESTING,
		    PAGINATION_MAX_DECODED_MEMORY_BYTES,
		    PAGINATION_OUTPUT_BATCH_ROWS,
		    PAGINATION_MAX_EXECUTION_MILLISECONDS,
		    PAGINATION_MAX_CONCURRENCY,
		    0};
		result.budgets = result.pagination.page_budgets;
		if (!result.pagination.PageBudgets().IsWithinPaginatedPageBounds() ||
		    !result.pagination.ScanBudgets().IsWithinPaginatedScanBounds() ||
		    result.pagination.ScanBudgets().response_bytes < result.pagination.PageBudgets().response_bytes ||
		    result.pagination.ScanBudgets().header_bytes < result.pagination.PageBudgets().header_bytes ||
		    result.pagination.ScanBudgets().decompressed_bytes < result.pagination.PageBudgets().decompressed_bytes ||
		    result.pagination.ScanBudgets().decoded_records < result.pagination.PageBudgets().decoded_records) {
			throw std::logic_error("selected relation produced an invalid paginated page or scan resource envelope");
		}
	} else {
		const auto &graphql = operation.Graphql();
		result.pagination.strategy = PlannedPaginationStrategy::GRAPHQL_CURSOR;
		result.pagination.graphql_cursor = result.Operation().Graphql().cursor;
		const auto page_response_bytes =
		    std::min(std::min(ceilings.MaxResponseBytesPerPage(), connector.NetworkPolicy().max_response_bytes),
		             PAGINATION_MAX_RESPONSE_BYTES_PER_PAGE);
		const auto max_pages = graphql.cursor.max_pages_per_scan;
		const auto page_serialized_body_bytes =
		    std::min(graphql.max_serialized_request_body_bytes_per_request, HOST_MAX_SERIALIZED_REQUEST_BODY_BYTES);
		const auto aggregate_attempts = BoundedProduct(
		    max_pages, attempts_per_step, RETRY_MAX_REQUEST_ATTEMPTS_PER_SCAN, "aggregate request-attempt scope");
		result.retry_policy.max_attempts_per_scan =
		    BoundedProduct(max_pages, retry_attempts_per_step, RETRY_MAX_REQUEST_ATTEMPTS_PER_SCAN,
		                   "ordinary retry request-attempt scope");
		result.resilience_policy.max_attempts_per_scan = aggregate_attempts;
		// A narrower effective page cannot spend the declared scan ceiling after
		// the cursor reaches its page limit. Keep aggregate authority reachable.
		const auto reachable_scan_serialized_body_bytes = BoundedProduct(
		    page_serialized_body_bytes, aggregate_attempts, PAGINATION_MAX_SERIALIZED_REQUEST_BODY_BYTES_PER_SCAN,
		    "aggregate serialized request-body scope");
		result.pagination.page_budgets = {
		    attempts_per_step,
		    page_response_bytes,
		    PAGINATION_MAX_HEADER_BYTES_PER_PAGE,
		    PAGINATION_MAX_DECOMPRESSED_BYTES_PER_PAGE,
		    std::min(ceilings.MaxRecordsPerPage(), PAGINATION_MAX_DECODED_RECORDS_PER_PAGE),
		    std::min(ceilings.MaxExtractedStringBytes(), PAGINATION_MAX_EXTRACTED_STRING_BYTES),
		    PAGINATION_MAX_JSON_NESTING,
		    PAGINATION_MAX_DECODED_MEMORY_BYTES,
		    PAGINATION_OUTPUT_BATCH_ROWS,
		    PAGINATION_MAX_EXECUTION_MILLISECONDS,
		    PAGINATION_MAX_CONCURRENCY,
		    page_serialized_body_bytes};
		result.pagination.scan_budgets = {
		    aggregate_attempts,
		    max_pages,
		    std::min(ceilings.MaxResponseBytesPerScan(), PAGINATION_MAX_RESPONSE_BYTES_PER_SCAN),
		    BoundedProduct(PAGINATION_MAX_HEADER_BYTES_PER_PAGE, max_pages, PAGINATION_MAX_HEADER_BYTES_PER_SCAN,
		                   "aggregate header-byte scope"),
		    BoundedProduct(PAGINATION_MAX_DECOMPRESSED_BYTES_PER_PAGE, max_pages,
		                   PAGINATION_MAX_DECOMPRESSED_BYTES_PER_SCAN, "aggregate decompressed-byte scope"),
		    std::min(ceilings.MaxRecordsPerScan(), PAGINATION_MAX_DECODED_RECORDS_PER_SCAN),
		    std::min(ceilings.MaxExtractedStringBytes(), PAGINATION_MAX_EXTRACTED_STRING_BYTES),
		    PAGINATION_MAX_JSON_NESTING,
		    PAGINATION_MAX_DECODED_MEMORY_BYTES,
		    PAGINATION_OUTPUT_BATCH_ROWS,
		    PAGINATION_MAX_EXECUTION_MILLISECONDS,
		    PAGINATION_MAX_CONCURRENCY,
		    std::min(graphql.max_serialized_request_body_bytes_per_scan, reachable_scan_serialized_body_bytes)};
		result.budgets = result.pagination.page_budgets;
		if (!result.pagination.PageBudgets().IsWithinPaginatedPageBounds() ||
		    !result.pagination.ScanBudgets().IsWithinPaginatedScanBounds() ||
		    result.pagination.ScanBudgets().serialized_request_body_bytes <
		        result.pagination.PageBudgets().serialized_request_body_bytes ||
		    result.pagination.ScanBudgets().decoded_records < result.pagination.PageBudgets().decoded_records) {
			throw std::logic_error("selected GraphQL operation produced an invalid cursor resource envelope");
		}
	}

	if (result.domain == BaseDomain::GRAPHQL_RELAY_CONNECTION_NODE_OCCURRENCES) {
		result.classification_reason =
		    predicate_decision.reason +
		    "; package-generated Relay connection nodes define a duplicate-preserving mutable occurrence bag; "
		    "sequential cursor traversal grants no DuckDB ordering or snapshot; resource ceilings grant no limit or "
		    "truncation authority; DuckDB retains every relational operator";
	} else if (result.domain == BaseDomain::PAGINATED_JSON_PATH_RECORDS ||
	           result.domain == BaseDomain::PAGINATED_ROOT_ARRAY_RECORDS) {
		result.classification_reason =
		    predicate_decision.reason +
		    "; accepted sequential page records define one duplicate-preserving mutable base-domain bag; traversal "
		    "order is not DuckDB ordering; page and scan ceilings are not limits; DuckDB retains all relational "
		    "operators";
	} else if (result.domain == BaseDomain::SUCCESSFUL_ROOT_OBJECT) {
		result.classification_reason =
		    predicate_decision.reason +
		    "; successful root object defines exactly one base row; source cardinality is not a limit; DuckDB retains "
		    "all relational operators";
	} else if (result.domain == BaseDomain::ROOT_ARRAY_RECORDS) {
		result.classification_reason =
		    predicate_decision.reason +
		    "; selected complete root-array occurrences define one duplicate-preserving base-domain bag; DuckDB "
		    "retains all relational operators";
	} else {
		result.classification_reason =
		    predicate_decision.reason +
		    "; selected JSON-path records define the complete base domain; DuckDB retains all relational operators";
	}
	if (!result.retry_policy.IsWithinHardBounds() || !result.rate_limit_policy.IsWithinHardBounds() ||
	    !result.resilience_policy.IsWithinHardBounds() ||
	    (result.retry == FeatureState::ENABLED) != result.retry_policy.Enabled() ||
	    (result.rate_limit == FeatureState::ENABLED) != result.rate_limit_policy.Declared() ||
	    result.replay_class != PlannedOperationReplayClass::REPLAYABLE_READ ||
	    result.resilience_policy.max_attempts_per_step != result.Budgets().request_attempts ||
	    (result.pagination.Strategy() != PlannedPaginationStrategy::DISABLED &&
	     result.resilience_policy.max_attempts_per_scan != result.pagination.ScanBudgets().request_attempts)) {
		throw std::logic_error("selected operation produced an incoherent resilience plan");
	}
	result.ValidatePredicateMaterialization();
	result.freshness_policy = request.freshness_policy;
	const auto canonical = BuildCanonicalRepresentation(result, request);
	const auto hash_val = HashCanonical(canonical);
	if (generation_handle.IsValid()) {
		result.cache_identity = std::shared_ptr<const CacheSemanticIdentity>(
		    new CacheSemanticIdentity(CacheSemanticIdentity::ForPackage(canonical, hash_val, generation_handle)));
	} else {
		result.cache_identity = std::shared_ptr<const CacheSemanticIdentity>(
		    new CacheSemanticIdentity(CacheSemanticIdentity::ForBuiltIn(canonical, hash_val)));
	}
	return result;
}

ScanPlan ScanPlanBuilder::Build(const CompiledConnector &connector, const ScanRequest &request) {
	return Build(connector, request, CompiledGenerationHandle());
}

ScanPlan BuildConservativeScanPlan(const CompiledConnector &connector, const ScanRequest &request) {
	try {
		return ScanPlanBuilder::Build(connector, request);
	} catch (const PlanningError &) {
		throw;
	} catch (const std::logic_error &error) {
		throw PlanningError(PlanningErrorCode::INVALID_CONTRACT, error.what());
	}
}

ScanPlan BuildConservativeScanPlan(const CompiledConnector &connector, const ScanRequest &request,
                                   const CompiledGenerationHandle &generation_handle) {
	try {
		return ScanPlanBuilder::Build(connector, request, generation_handle);
	} catch (const PlanningError &) {
		throw;
	} catch (const std::logic_error &error) {
		throw PlanningError(PlanningErrorCode::INVALID_CONTRACT, error.what());
	}
}

} // namespace cuac
