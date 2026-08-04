#pragma once

#include "cuac/semantics/scan_planner.hpp"
#include "cuac/internal/semantics/planner/input_resolution.hpp"

#include <cstdint>
#include <stdexcept>

namespace cuac {
namespace scan_planner_internal {

inline const char *UrlSchemeName(CompiledUrlScheme scheme) {
	switch (scheme) {
	case CompiledUrlScheme::HTTP:
		return "http";
	case CompiledUrlScheme::HTTPS:
		return "https";
	}
	throw std::logic_error("compiled connector contains an unknown URL scheme");
}

inline PlannedUrlScheme PlanUrlScheme(CompiledUrlScheme scheme) {
	switch (scheme) {
	case CompiledUrlScheme::HTTP:
		return PlannedUrlScheme::HTTP;
	case CompiledUrlScheme::HTTPS:
		return PlannedUrlScheme::HTTPS;
	}
	throw std::logic_error("compiled connector contains an unknown URL scheme");
}

inline PlannedAuthenticator PlanAuthenticator(CompiledAuthenticator authenticator) {
	switch (authenticator) {
	case CompiledAuthenticator::NONE:
		return PlannedAuthenticator::NONE;
	case CompiledAuthenticator::BEARER:
		return PlannedAuthenticator::BEARER;
	case CompiledAuthenticator::API_KEY:
		return PlannedAuthenticator::API_KEY;
	}
	throw std::logic_error("compiled connector contains an unknown authenticator");
}

inline PlannedCredentialPlacement PlanCredentialPlacement(CompiledCredentialPlacement placement) {
	switch (placement) {
	case CompiledCredentialPlacement::NONE:
		return PlannedCredentialPlacement::NONE;
	case CompiledCredentialPlacement::AUTHORIZATION_HEADER:
		return PlannedCredentialPlacement::AUTHORIZATION_HEADER;
	case CompiledCredentialPlacement::HEADER_NAMED:
		return PlannedCredentialPlacement::HEADER_NAMED;
	case CompiledCredentialPlacement::QUERY_NAMED:
		return PlannedCredentialPlacement::QUERY_NAMED;
	}
	throw std::logic_error("compiled connector contains an unknown credential placement");
}

inline PlannedRateLimitMode PlanRateLimitMode(CompiledRateLimitMode mode) {
	switch (mode) {
	case CompiledRateLimitMode::FAIL:
		return PlannedRateLimitMode::FAIL;
	case CompiledRateLimitMode::WAIT:
		return PlannedRateLimitMode::WAIT;
	case CompiledRateLimitMode::WAIT_IF_DEADLINE_ALLOWS:
		return PlannedRateLimitMode::WAIT_IF_DEADLINE_ALLOWS;
	}
	throw std::logic_error("compiled operation contains an unknown rate-limit mode");
}

inline PlannedRateLimitPrincipalScope PlanRateLimitPrincipalScope(CompiledRateLimitPrincipalScope scope) {
	switch (scope) {
	case CompiledRateLimitPrincipalScope::CREDENTIAL_AUTHORITY:
		return PlannedRateLimitPrincipalScope::CREDENTIAL_AUTHORITY;
	case CompiledRateLimitPrincipalScope::SHARED:
		return PlannedRateLimitPrincipalScope::SHARED;
	}
	throw std::logic_error("compiled operation contains an unknown rate-limit principal scope");
}

inline PlannedRateLimitGuidanceFormat PlanRateLimitGuidanceFormat(CompiledRateLimitGuidanceFormat format) {
	switch (format) {
	case CompiledRateLimitGuidanceFormat::RETRY_AFTER:
		return PlannedRateLimitGuidanceFormat::RETRY_AFTER;
	case CompiledRateLimitGuidanceFormat::DELTA_SECONDS:
		return PlannedRateLimitGuidanceFormat::DELTA_SECONDS;
	case CompiledRateLimitGuidanceFormat::UNIX_SECONDS:
		return PlannedRateLimitGuidanceFormat::UNIX_SECONDS;
	}
	throw std::logic_error("compiled operation contains an unknown rate-limit guidance format");
}

inline PlannedProtocol PlanProtocol(CompiledProtocol protocol) {
	switch (protocol) {
	case CompiledProtocol::REST:
		return PlannedProtocol::REST;
	case CompiledProtocol::GRAPHQL:
		return PlannedProtocol::GRAPHQL;
	}
	throw std::logic_error("compiled relation contains an unsupported protocol");
}

inline PlannedHttpMethod PlanMethod(CompiledHttpMethod method) {
	switch (method) {
	case CompiledHttpMethod::GET:
		return PlannedHttpMethod::GET;
	}
	throw std::logic_error("compiled relation contains an unsupported HTTP method");
}

inline PlannedReplaySafety PlanReplaySafety(CompiledReplaySafety replay_safety) {
	switch (replay_safety) {
	case CompiledReplaySafety::SAFE:
		return PlannedReplaySafety::SAFE;
	}
	throw std::logic_error("compiled relation contains an unsupported replay-safety declaration");
}

inline PlannedCardinality PlanCardinality(CompiledOperationCardinality cardinality) {
	switch (cardinality) {
	case CompiledOperationCardinality::ZERO_TO_MANY:
		return PlannedCardinality::ZERO_TO_MANY;
	case CompiledOperationCardinality::EXACTLY_ONE_ON_SUCCESS:
		return PlannedCardinality::EXACTLY_ONE_ON_SUCCESS;
	}
	throw std::logic_error("compiled relation contains an unsupported source cardinality");
}

inline PlannedResponseSource PlanResponseSource(CompiledResponseSource source) {
	switch (source) {
	case CompiledResponseSource::JSON_PATH_MANY:
		return PlannedResponseSource::JSON_PATH_MANY;
	case CompiledResponseSource::ROOT_ARRAY:
		return PlannedResponseSource::ROOT_ARRAY;
	case CompiledResponseSource::ROOT_OBJECT:
		return PlannedResponseSource::ROOT_OBJECT;
	}
	throw std::logic_error("compiled relation contains an unsupported response source");
}

inline BaseDomain PlanBaseDomain(CompiledResponseSource source, CompiledPaginationStrategy pagination) {
	// SHORT_PAGE (RFC 0019) shares this branch with LINK_HEADER/RESPONSE_NEXT_URL:
	// the domain classification depends only on the response source's
	// duplicate-preserving-bag shape, never on which mechanism (header, body
	// URL, or decoded row count) signals continuation.
	// RFC 0029: a cursor traversal produces the identical base domain — the
	// duplicate-preserving bag from every accepted page, with no ordering or
	// snapshot guarantee. Only how the next page is addressed differs.
	if (pagination == CompiledPaginationStrategy::LINK_HEADER ||
	    pagination == CompiledPaginationStrategy::RESPONSE_NEXT_URL ||
	    pagination == CompiledPaginationStrategy::SHORT_PAGE ||
	    pagination == CompiledPaginationStrategy::RESPONSE_CURSOR) {
		switch (source) {
		case CompiledResponseSource::JSON_PATH_MANY:
			return BaseDomain::PAGINATED_JSON_PATH_RECORDS;
		case CompiledResponseSource::ROOT_ARRAY:
			return BaseDomain::PAGINATED_ROOT_ARRAY_RECORDS;
		case CompiledResponseSource::ROOT_OBJECT:
			break;
		}
		throw std::logic_error("paginated operation does not expose a many-row response source");
	}
	if (pagination != CompiledPaginationStrategy::DISABLED) {
		throw std::logic_error("compiled relation contains an unsupported pagination strategy");
	}
	switch (source) {
	case CompiledResponseSource::JSON_PATH_MANY:
		return BaseDomain::JSON_PATH_RECORDS;
	case CompiledResponseSource::ROOT_ARRAY:
		return BaseDomain::ROOT_ARRAY_RECORDS;
	case CompiledResponseSource::ROOT_OBJECT:
		return BaseDomain::SUCCESSFUL_ROOT_OBJECT;
	}
	throw std::logic_error("compiled relation contains an unsupported response source");
}

inline PlannedPageDependency PlanPageDependency(CompiledPageDependency dependency) {
	switch (dependency) {
	case CompiledPageDependency::SEQUENTIAL:
		return PlannedPageDependency::SEQUENTIAL;
	}
	throw std::logic_error("compiled relation contains an unsupported pagination dependency");
}

inline PlannedPageConsistency PlanPageConsistency(CompiledPageConsistency consistency) {
	switch (consistency) {
	case CompiledPageConsistency::MUTABLE:
		return PlannedPageConsistency::MUTABLE;
	}
	throw std::logic_error("compiled relation contains an unsupported pagination consistency");
}

inline PlannedLinkRelation PlanLinkRelation(CompiledLinkRelation relation) {
	switch (relation) {
	case CompiledLinkRelation::NEXT:
		return PlannedLinkRelation::NEXT;
	}
	throw std::logic_error("compiled relation contains an unsupported Link relation");
}

inline PlannedContinuationTargetScope PlanTargetScope(CompiledContinuationTargetScope scope) {
	switch (scope) {
	case CompiledContinuationTargetScope::EXACT_OPERATION_ORIGIN_AND_PATH:
		return PlannedContinuationTargetScope::EXACT_OPERATION_ORIGIN_AND_PATH;
	}
	throw std::logic_error("compiled relation contains an unsupported pagination target scope");
}

// Maps a compiled REST pagination strategy to its planned counterpart.
// GRAPHQL_CURSOR is a Planned-only value assigned directly by GraphQL
// planning and is never produced from a compiled REST strategy, so it is
// deliberately absent from this switch's domain.
inline PlannedPaginationStrategy PlanPaginationStrategy(CompiledPaginationStrategy strategy) {
	switch (strategy) {
	case CompiledPaginationStrategy::DISABLED:
		return PlannedPaginationStrategy::DISABLED;
	case CompiledPaginationStrategy::LINK_HEADER:
		return PlannedPaginationStrategy::LINK_HEADER;
	case CompiledPaginationStrategy::RESPONSE_NEXT_URL:
		return PlannedPaginationStrategy::RESPONSE_NEXT_URL;
	case CompiledPaginationStrategy::SHORT_PAGE:
		return PlannedPaginationStrategy::SHORT_PAGE;
	case CompiledPaginationStrategy::RESPONSE_CURSOR:
		return PlannedPaginationStrategy::RESPONSE_CURSOR;
	}
	throw std::logic_error("compiled relation contains an unsupported pagination strategy");
}

std::uint64_t BoundedProduct(std::uint64_t left, std::uint64_t right, std::uint64_t ceiling, const char *field);
std::uint64_t BoundedSum(std::uint64_t left, std::uint64_t right, std::uint64_t ceiling, const char *field);

// Complete, side-effect-free selection result. The operation pointer is always
// one member of relation->Operations(); plural relations never reach plan
// construction unless one operation has been selected unambiguously.
struct SelectedRelationOperation {
	const CompiledRelation *relation;
	const CompiledOperation *operation;
	input_resolution::ResolvedRelationInputs resolved_inputs;
};

SelectedRelationOperation ValidateAndSelectOperation(const CompiledConnector &connector, const ScanRequest &request);

// GraphQL planning is a closed profile match, not a parser. These helpers
// validate Connector's public typed handoff and derive the replay-safe planned
// alternative only when every accepted fact agrees.
void ValidateGraphqlOperationProfile(const CompiledRelation &relation, const CompiledOperation &operation,
                                     const CompiledNetworkPolicy &network_policy);
PlannedGraphqlOperation PlanGraphqlOperation(const CompiledOperation &operation);

} // namespace scan_planner_internal
} // namespace cuac
