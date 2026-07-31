#include "cuac/connector/catalog.hpp"
#include "connector/support/catalog_contract.hpp"
#include "connector/support/catalog_test_access.hpp"
#include "support/require.hpp"

#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using cuac_test::ConnectorCatalogTestAccess;
using cuac_test::Require;

#define DEFINE_MEMBER_PROBE(PROBE_NAME, MEMBER_NAME)                                                                   \
	template <typename T>                                                                                              \
	class PROBE_NAME {                                                                                                 \
		template <typename U>                                                                                          \
		static char Test(decltype(&U::MEMBER_NAME));                                                                   \
		template <typename U>                                                                                          \
		static long Test(...);                                                                                         \
                                                                                                                       \
	public:                                                                                                            \
		static const bool VALUE = sizeof(Test<T>(0)) == sizeof(char);                                                  \
	}

DEFINE_MEMBER_PROBE(HasBaseUrlMember, base_url);
DEFINE_MEMBER_PROBE(HasPathMember, path);
DEFINE_MEMBER_PROBE(HasQueryMember, query);
DEFINE_MEMBER_PROBE(HasFragmentMember, fragment);
DEFINE_MEMBER_PROBE(HasAuthenticationEnabledMember, authentication_enabled);
DEFINE_MEMBER_PROBE(HasSecretNameMember, secret_name);
DEFINE_MEMBER_PROBE(HasCredentialValueMember, credential_value);
DEFINE_MEMBER_PROBE(HasTokenValueMember, token_value);
DEFINE_MEMBER_PROBE(HasSecretHandleMember, secret_handle);

#undef DEFINE_MEMBER_PROBE

static_assert(!HasBaseUrlMember<cuac::CompiledRestRequest>::VALUE,
              "CompiledRestRequest must not restore a parseable base URL");
static_assert(!HasPathMember<cuac::CompiledHttpOrigin>::VALUE, "CompiledHttpOrigin must not carry a path component");
static_assert(!HasQueryMember<cuac::CompiledHttpOrigin>::VALUE, "CompiledHttpOrigin must not carry a query component");
static_assert(!HasFragmentMember<cuac::CompiledHttpOrigin>::VALUE,
              "CompiledHttpOrigin must not carry a fragment component");
static_assert(!HasAuthenticationEnabledMember<cuac::CompiledOperation>::VALUE,
              "authentication must have one owner in the relation policy");
static_assert(!HasSecretNameMember<cuac::CompiledAuthenticationPolicy>::VALUE,
              "credential policy must not expose a DuckDB secret name");
static_assert(!HasCredentialValueMember<cuac::CompiledAuthenticationPolicy>::VALUE,
              "credential policy must not expose credential bytes");
static_assert(!HasTokenValueMember<cuac::CompiledAuthenticationPolicy>::VALUE,
              "credential policy must not expose token bytes");
static_assert(!HasSecretHandleMember<cuac::CompiledAuthenticationPolicy>::VALUE,
              "credential policy must not expose a provider handle");
static_assert(std::is_default_constructible<cuac::CompiledOperationSelector>::value,
              "installed fallback operations require the closed empty selector");
static_assert(std::is_copy_constructible<cuac::CompiledOperationSelector>::value,
              "immutable selectors must support catalog copies");
static_assert(std::is_move_constructible<cuac::CompiledOperationSelector>::value,
              "immutable selectors must support catalog ownership transfer");
static_assert(!std::is_copy_assignable<cuac::CompiledOperationSelector>::value,
              "selector assignment would permit post-construction replacement");
static_assert(!std::is_move_assignable<cuac::CompiledOperationSelector>::value,
              "selector assignment would permit post-construction replacement");
static_assert(
    !std::is_constructible<cuac::CompiledOperationSelector, std::vector<std::string>,
                           std::vector<std::vector<std::string>>, std::vector<std::string>, std::int32_t>::value,
    "production consumers must not construct arbitrary operation selectors");
static_assert(std::is_same<decltype(cuac::CompiledHttpOrigin::scheme), cuac::CompiledUrlScheme>::value,
              "CompiledHttpOrigin scheme must remain typed");
static_assert(std::is_same<decltype(cuac::CompiledHttpOrigin::host), cuac::CompiledHttpHost>::value,
              "CompiledHttpOrigin host must remain a validated exact host component");
static_assert(std::is_same<decltype(cuac::CompiledHttpOrigin::port), std::uint16_t>::value,
              "CompiledHttpOrigin port must remain an explicit uint16_t");
static_assert(std::is_copy_constructible<cuac::CompiledConnector>::value,
              "immutable catalog must support bind/composition copies");
static_assert(std::is_move_constructible<cuac::CompiledConnector>::value,
              "immutable catalog must support ownership transfer");
static_assert(!std::is_copy_assignable<cuac::CompiledConnector>::value,
              "catalog assignment would permit post-construction replacement");
static_assert(!std::is_move_assignable<cuac::CompiledConnector>::value,
              "catalog assignment would permit post-construction replacement");
static_assert(!std::is_default_constructible<cuac::CompiledConnector>::value,
              "catalog must not admit partial construction");
static_assert(!std::is_constructible<cuac::CompiledConnector, std::string, std::string,
                                     std::vector<cuac::CompiledRelation>, cuac::CompiledNetworkPolicy>::value,
              "production callers must not construct arbitrary catalog provenance or authority");
static_assert(std::is_copy_constructible<cuac::CompiledRelation>::value,
              "immutable relation must support catalog copies");
static_assert(std::is_move_constructible<cuac::CompiledRelation>::value,
              "immutable relation must support catalog ownership transfer");
static_assert(!std::is_copy_assignable<cuac::CompiledRelation>::value,
              "relation assignment would permit post-construction replacement");
static_assert(!std::is_move_assignable<cuac::CompiledRelation>::value,
              "relation assignment would permit post-construction replacement");
static_assert(!std::is_default_constructible<cuac::CompiledRelation>::value,
              "relation must not admit partial construction");
static_assert(!std::is_constructible<cuac::CompiledRelation, std::string, std::vector<cuac::CompiledColumn>,
                                     std::vector<cuac::CompiledPredicateMapping>, cuac::CompiledOperation,
                                     cuac::CompiledAuthenticationPolicy, cuac::CompiledResourceCeilings>::value,
              "production callers must not construct arbitrary relation authority");
static_assert(!std::is_constructible<cuac::CompiledRelation, std::string, std::vector<cuac::CompiledColumn>,
                                     std::vector<cuac::CompiledPredicateMapping>, std::vector<cuac::CompiledOperation>,
                                     cuac::CompiledAuthenticationPolicy, cuac::CompiledResourceCeilings>::value,
              "production callers must not construct arbitrary multi-operation relation authority");

template <typename Callable>
void RequireInvalid(const std::string &message, Callable callback) {
	bool rejected = false;
	try {
		callback();
	} catch (const std::invalid_argument &) {
		rejected = true;
	}
	Require(rejected, message);
}

cuac::CompiledOperation WithSelector(cuac::CompiledOperation operation, bool fallback,
                                     cuac::CompiledOperationSelector selector) {
	const auto rest = operation.Rest();
	return cuac::CompiledOperation {std::move(operation.name),
	                                fallback,
	                                operation.cardinality,
	                                cuac::CompiledProtocol::REST,
	                                rest.method,
	                                rest.replay_safety,
	                                rest.retry_enabled,
	                                rest.pagination,
	                                rest.request,
	                                rest.response_source,
	                                rest.records_extractor,
	                                std::move(selector)};
}

cuac::CompiledConnector BuildValidCatalogFixture() {
	const cuac::CompiledHttpOrigin origin = {cuac::CompiledUrlScheme::HTTPS, cuac::CompiledHttpHost("api.github.com"),
	                                         443};
	const std::vector<cuac::CompiledColumn> columns = {{"id", "BIGINT", false, "$.id"}};
	const std::vector<cuac::CompiledHttpHeader> headers = {{"X-Fixture", "safe"}};

	std::vector<cuac::CompiledRelation> relations;
	relations.push_back(ConnectorCatalogTestAccess::Relation(
	    "anonymous_rows", columns,
	    cuac::CompiledOperation {"fixture_anonymous_rows",
	                             true,
	                             cuac::CompiledOperationCardinality::ZERO_TO_MANY,
	                             cuac::CompiledProtocol::REST,
	                             cuac::CompiledHttpMethod::GET,
	                             cuac::CompiledReplaySafety::SAFE,
	                             false,
	                             ConnectorCatalogTestAccess::DisabledPagination(),
	                             {origin, "/rows", {}, headers},
	                             cuac::CompiledResponseSource::JSON_PATH_MANY,
	                             "$.items[*]",
	                             cuac::CompiledOperationSelector()},
	    ConnectorCatalogTestAccess::Anonymous(), ConnectorCatalogTestAccess::UnpaginatedResources(2, 64)));
	relations.push_back(ConnectorCatalogTestAccess::Relation(
	    "current_row", columns,
	    cuac::CompiledOperation {"fixture_current_row",
	                             true,
	                             cuac::CompiledOperationCardinality::EXACTLY_ONE_ON_SUCCESS,
	                             cuac::CompiledProtocol::REST,
	                             cuac::CompiledHttpMethod::GET,
	                             cuac::CompiledReplaySafety::SAFE,
	                             false,
	                             ConnectorCatalogTestAccess::DisabledPagination(),
	                             {origin, "/row", {}, headers},
	                             cuac::CompiledResponseSource::ROOT_OBJECT,
	                             "$",
	                             cuac::CompiledOperationSelector()},
	    ConnectorCatalogTestAccess::RequiredBearer(), ConnectorCatalogTestAccess::UnpaginatedResources(1, 64)));
	return ConnectorCatalogTestAccess::Catalog(
	    "fixture", "1.0.0", std::move(relations),
	    cuac::CompiledNetworkPolicy {{"https"}, {"api.github.com"}, false, false, false, false, 4096});
}

void TestSafeImmutableService() {
	const auto catalog = BuildValidCatalogFixture();
	Require(catalog.Relations().size() == 2, "validated fixture catalog lost relations");
	Require(catalog.FindRelation("anonymous_rows") == &catalog.Relations()[0],
	        "exact lookup did not return the owned anonymous relation");
	Require(catalog.FindRelation("current_row") == &catalog.Relations()[1],
	        "exact lookup did not return the owned authenticated relation");
	Require(catalog.FindRelation("Current_Row") == nullptr, "exact lookup folded identifier case");
	Require(catalog.FindRelation("missing") == nullptr, "exact lookup fabricated a relation");
	Require(catalog.Relations()[0].HasSingleOperation() && catalog.Relations()[0].Operations().size() == 1 &&
	            &catalog.Relations()[0].Operation() == &catalog.Relations()[0].Operations()[0],
	        "single-operation access diverged from the immutable operation collection");
	const auto &selector = catalog.Relations()[0].Operation().selector;
	Require(selector.RequiredInputReferences().empty(),
	        "installed-compatible operation did not receive the closed empty selector");

	const auto copy = catalog;
	Require(copy.Snapshot() == catalog.Snapshot(), "copy construction changed immutable catalog metadata");
	Require(&copy.Relations()[0] != &catalog.Relations()[0], "copy did not own its immutable relation storage");
	Require(catalog.Relations()[0].Authentication().Destination() == nullptr,
	        "anonymous policy retained credential destination authority");
	Require(catalog.Relations()[1].Authentication().LogicalCredential() == "token",
	        "required policy lost its safe logical identifier");
	Require(catalog.Relations()[1].Authentication().Destination() != nullptr,
	        "required policy lost its exact destination");
	Require(catalog.Snapshot().find("Authorization=") == std::string::npos,
	        "safe snapshot rendered credential placement as a fixed header");
	Require(catalog.Snapshot().find("secret_name=") == std::string::npos,
	        "safe snapshot rendered a secret-binding identifier");
}

void TestOperationSelectorValidation() {
	const auto selector =
	    ConnectorCatalogTestAccess::OperationSelector({ConnectorCatalogTestAccess::ConditionalInputReference("zeta"),
	                                                   ConnectorCatalogTestAccess::RelationInputReference("alpha")});
	Require(selector.RequiredInputReferences().size() == 2 &&
	            selector.RequiredInputReferences()[0].Kind() == cuac::CompiledRequiredInputKind::RELATION_INPUT &&
	            selector.RequiredInputReferences()[0].Id() == "alpha" &&
	            selector.RequiredInputReferences()[1].Kind() == cuac::CompiledRequiredInputKind::CONDITIONAL_INPUT &&
	            selector.RequiredInputReferences()[1].Id() == "zeta",
	        "v1 selector did not preserve and canonicalize tagged required inputs");

	RequireInvalid("selector accepted an invalid required-input identifier",
	               []() { (void)ConnectorCatalogTestAccess::ConditionalInputReference("Bad-Input"); });
	RequireInvalid("selector accepted duplicate tagged required inputs", []() {
		(void)ConnectorCatalogTestAccess::OperationSelector(
		    {ConnectorCatalogTestAccess::ConditionalInputReference("visibility"),
		     ConnectorCatalogTestAccess::ConditionalInputReference("visibility")});
	});

	const auto catalog = BuildValidCatalogFixture();
	const auto &anonymous = catalog.Relations()[0];
	RequireInvalid("relation accepted a selector input absent from operation-scoped declarations", [&anonymous]() {
		auto operation = WithSelector(anonymous.Operation(), false,
		                              ConnectorCatalogTestAccess::OperationSelector(
		                                  {ConnectorCatalogTestAccess::ConditionalInputReference("phantom_input")}));
		ConnectorCatalogTestAccess::Relation(anonymous.Name(), anonymous.Columns(), std::move(operation),
		                                     anonymous.Authentication(), anonymous.ResourceCeilings());
	});
}

void TestClosedValidation() {
	const auto catalog = BuildValidCatalogFixture();
	const auto &anonymous = catalog.Relations()[0];
	const auto &authenticated = catalog.Relations()[1];

	const std::vector<std::string> invalid_hosts = {
	    "service.example:444",      "service.example/root", "service.example?pre=1",
	    "service.example#fragment", "user@service.example", "https://service.example:444/root?pre=1#fragment",
	    "Service.example",          ".service.example"};
	for (const auto &value : invalid_hosts) {
		RequireInvalid("CompiledHttpHost accepted URL structure: " + value,
		               [value]() { cuac::CompiledHttpHost host(value); });
	}

	const cuac::CompiledHttpOrigin origin = {cuac::CompiledUrlScheme::HTTPS, cuac::CompiledHttpHost("api.github.com"),
	                                         443};
	RequireInvalid("required bearer policy accepted an empty logical credential",
	               [origin]() { ConnectorCatalogTestAccess::ValidateRequiredBearer("", origin); });
	RequireInvalid("required bearer policy accepted an open-ended logical credential",
	               [origin]() { ConnectorCatalogTestAccess::ValidateRequiredBearer("password", origin); });
	RequireInvalid("required bearer policy accepted a cleartext destination", []() {
		const cuac::CompiledHttpOrigin destination = {cuac::CompiledUrlScheme::HTTP,
		                                              cuac::CompiledHttpHost("api.github.com"), 80};
		ConnectorCatalogTestAccess::ValidateRequiredBearer("token", destination);
	});
	const auto alternate_port = []() {
		const cuac::CompiledHttpOrigin destination = {cuac::CompiledUrlScheme::HTTPS,
		                                              cuac::CompiledHttpHost("api.github.com"), 444};
		return ConnectorCatalogTestAccess::ValidateRequiredBearer("token", destination);
	}();
	const auto alternate_host = []() {
		const cuac::CompiledHttpOrigin destination = {cuac::CompiledUrlScheme::HTTPS,
		                                              cuac::CompiledHttpHost("other.example"), 443};
		return ConnectorCatalogTestAccess::ValidateRequiredBearer("token", destination);
	}();
	Require(alternate_port.Destinations().size() == 1 && alternate_port.Destinations()[0].port == 444 &&
	            alternate_host.Destinations().size() == 1 &&
	            alternate_host.Destinations()[0].host.Value() == "other.example",
	        "required bearer policy lost exact package destination authority");

	const auto api_key_header = ConnectorCatalogTestAccess::ValidateRequiredApiKey(
	    "token", cuac::CompiledCredentialPlacement::HEADER_NAMED, "X-Api-Key", origin);
	Require(api_key_header.Authenticator() == cuac::CompiledAuthenticator::API_KEY &&
	            api_key_header.Placement() == cuac::CompiledCredentialPlacement::HEADER_NAMED &&
	            api_key_header.PlacementName() == "X-Api-Key",
	        "required api_key header policy lost its declared placement name");
	const auto api_key_query = ConnectorCatalogTestAccess::ValidateRequiredApiKey(
	    "token", cuac::CompiledCredentialPlacement::QUERY_NAMED, "api_key", origin);
	Require(api_key_query.Authenticator() == cuac::CompiledAuthenticator::API_KEY &&
	            api_key_query.Placement() == cuac::CompiledCredentialPlacement::QUERY_NAMED &&
	            api_key_query.PlacementName() == "api_key",
	        "required api_key query policy lost its declared placement name");
	RequireInvalid("required api_key header policy accepted an empty placement name", [origin]() {
		ConnectorCatalogTestAccess::ValidateRequiredApiKey("token", cuac::CompiledCredentialPlacement::HEADER_NAMED, "",
		                                                   origin);
	});
	RequireInvalid("required api_key query policy accepted an empty placement name", [origin]() {
		ConnectorCatalogTestAccess::ValidateRequiredApiKey("token", cuac::CompiledCredentialPlacement::QUERY_NAMED, "",
		                                                   origin);
	});
	RequireInvalid("required api_key policy accepted the bearer placement", [origin]() {
		ConnectorCatalogTestAccess::ValidateRequiredApiKey(
		    "token", cuac::CompiledCredentialPlacement::AUTHORIZATION_HEADER, "X-Api-Key", origin);
	});

	RequireInvalid("relation accepted a fixed Authorization header", [&authenticated]() {
		auto operation = authenticated.Operation();
		auto rest = operation.Rest();
		rest.request.headers.push_back({"authorization", "x"});
		auto changed = ConnectorCatalogTestAccess::RestOperation(operation, std::move(rest), operation.selector);
		ConnectorCatalogTestAccess::Relation(authenticated.Name(), authenticated.Columns(), std::move(changed),
		                                     authenticated.Authentication(), authenticated.ResourceCeilings());
	});
	RequireInvalid("relation accepted root-object shape with zero-to-many cardinality", [&anonymous]() {
		auto operation = anonymous.Operation();
		auto rest = operation.Rest();
		rest.response_source = cuac::CompiledResponseSource::ROOT_OBJECT;
		rest.records_extractor = "$";
		auto changed = ConnectorCatalogTestAccess::RestOperation(operation, std::move(rest), operation.selector);
		ConnectorCatalogTestAccess::Relation(anonymous.Name(), anonymous.Columns(), std::move(changed),
		                                     anonymous.Authentication(), anonymous.ResourceCeilings());
	});
	RequireInvalid("relation accepted multi-record shape with exactly-one cardinality", [&authenticated]() {
		auto operation = authenticated.Operation();
		auto rest = operation.Rest();
		rest.response_source = cuac::CompiledResponseSource::JSON_PATH_MANY;
		rest.records_extractor = "$.items[*]";
		auto changed = ConnectorCatalogTestAccess::RestOperation(operation, std::move(rest), operation.selector);
		ConnectorCatalogTestAccess::Relation(authenticated.Name(), authenticated.Columns(), std::move(changed),
		                                     authenticated.Authentication(), authenticated.ResourceCeilings());
	});
	RequireInvalid("relation accepted root-array shape with exactly-one cardinality", [&authenticated]() {
		auto operation = authenticated.Operation();
		auto rest = operation.Rest();
		rest.response_source = cuac::CompiledResponseSource::ROOT_ARRAY;
		rest.records_extractor = "$";
		auto changed = ConnectorCatalogTestAccess::RestOperation(operation, std::move(rest), operation.selector);
		ConnectorCatalogTestAccess::Relation(authenticated.Name(), authenticated.Columns(), std::move(changed),
		                                     authenticated.Authentication(), authenticated.ResourceCeilings());
	});
	RequireInvalid("relation inferred a root array from an extractor", [&anonymous]() {
		auto operation = anonymous.Operation();
		auto rest = operation.Rest();
		rest.response_source = cuac::CompiledResponseSource::ROOT_ARRAY;
		rest.records_extractor = "$[*]";
		auto changed = ConnectorCatalogTestAccess::RestOperation(operation, std::move(rest), operation.selector);
		ConnectorCatalogTestAccess::Relation(anonymous.Name(), anonymous.Columns(), std::move(changed),
		                                     anonymous.Authentication(), anonymous.ResourceCeilings());
	});
	RequireInvalid("exactly-one relation accepted a wider record ceiling", [&authenticated]() {
		ConnectorCatalogTestAccess::Relation(authenticated.Name(), authenticated.Columns(), authenticated.Operation(),
		                                     authenticated.Authentication(),
		                                     ConnectorCatalogTestAccess::UnpaginatedResources(2, 64));
	});
	RequireInvalid("authenticated relation accepted a query-bearing request", [&authenticated]() {
		auto operation = authenticated.Operation();
		auto rest = operation.Rest();
		rest.request.query_parameters.push_back(ConnectorCatalogTestAccess::FixedQuery("page", "1"));
		auto changed = ConnectorCatalogTestAccess::RestOperation(operation, std::move(rest), operation.selector);
		ConnectorCatalogTestAccess::Relation(authenticated.Name(), authenticated.Columns(), std::move(changed),
		                                     authenticated.Authentication(), authenticated.ResourceCeilings());
	});
	RequireInvalid("authenticated relation accepted a mismatched credential destination", [&authenticated]() {
		auto operation = authenticated.Operation();
		auto rest = operation.Rest();
		rest.request.origin.host = cuac::CompiledHttpHost("other.example");
		auto changed = ConnectorCatalogTestAccess::RestOperation(operation, std::move(rest), operation.selector);
		ConnectorCatalogTestAccess::Relation(authenticated.Name(), authenticated.Columns(), std::move(changed),
		                                     ConnectorCatalogTestAccess::RequiredBearer(),
		                                     authenticated.ResourceCeilings());
	});
	RequireInvalid("relation accepted duplicate output columns", [&anonymous]() {
		auto columns = anonymous.Columns();
		columns.push_back(columns[0]);
		ConnectorCatalogTestAccess::Relation(anonymous.Name(), std::move(columns), anonymous.Operation(),
		                                     anonymous.Authentication(), anonymous.ResourceCeilings());
	});
	RequireInvalid("relation accepted an invalid request path", [&anonymous]() {
		auto operation = anonymous.Operation();
		auto rest = operation.Rest();
		rest.request.path = "/rows?escape=1";
		auto changed = ConnectorCatalogTestAccess::RestOperation(operation, std::move(rest), operation.selector);
		ConnectorCatalogTestAccess::Relation(anonymous.Name(), anonymous.Columns(), std::move(changed),
		                                     anonymous.Authentication(), anonymous.ResourceCeilings());
	});
	RequireInvalid("relation accepted header injection", [&anonymous]() {
		auto operation = anonymous.Operation();
		auto rest = operation.Rest();
		rest.request.headers[0].value = "value\r\ninjected";
		auto changed = ConnectorCatalogTestAccess::RestOperation(operation, std::move(rest), operation.selector);
		ConnectorCatalogTestAccess::Relation(anonymous.Name(), anonymous.Columns(), std::move(changed),
		                                     anonymous.Authentication(), anonymous.ResourceCeilings());
	});
	RequireInvalid("relation accepted query injection", [&anonymous]() {
		auto operation = anonymous.Operation();
		auto rest = operation.Rest();
		auto query = ConnectorCatalogTestAccess::FixedQuery("page", "value");
		query.encoded_value = "value&injected=1";
		rest.request.query_parameters.push_back(std::move(query));
		auto changed = ConnectorCatalogTestAccess::RestOperation(operation, std::move(rest), operation.selector);
		ConnectorCatalogTestAccess::Relation(anonymous.Name(), anonymous.Columns(), std::move(changed),
		                                     anonymous.Authentication(), anonymous.ResourceCeilings());
	});
	RequireInvalid("catalog accepted duplicate relation identifiers", [&catalog]() {
		std::vector<cuac::CompiledRelation> relations = {catalog.Relations()[0], catalog.Relations()[0]};
		ConnectorCatalogTestAccess::Catalog(catalog.ConnectorName(), catalog.Version(), std::move(relations),
		                                    catalog.NetworkPolicy());
	});
	RequireInvalid("catalog accepted a destination outside its network policy", [&catalog]() {
		auto policy = catalog.NetworkPolicy();
		policy.allowed_hosts = {"other.example"};
		ConnectorCatalogTestAccess::Catalog(catalog.ConnectorName(), catalog.Version(), catalog.Relations(),
		                                    std::move(policy));
	});
	RequireInvalid("relation accepted an empty operation collection", [&anonymous]() {
		ConnectorCatalogTestAccess::Relation(anonymous.Name(), anonymous.Columns(),
		                                     std::vector<cuac::CompiledOperation> {}, anonymous.Authentication(),
		                                     anonymous.ResourceCeilings());
	});
	RequireInvalid("relation accepted duplicate operation identifiers", [&anonymous]() {
		auto second = anonymous.Operation();
		auto rest = second.Rest();
		rest.request.path = "/other-rows";
		auto changed = ConnectorCatalogTestAccess::RestOperation(second, std::move(rest), second.selector);
		std::vector<cuac::CompiledOperation> operations = {anonymous.Operation(), std::move(changed)};
		ConnectorCatalogTestAccess::Relation(anonymous.Name(), anonymous.Columns(), std::move(operations),
		                                     anonymous.Authentication(), anonymous.ResourceCeilings());
	});
	RequireInvalid("catalog ignored a later operation destination outside network policy", [&catalog, &anonymous]() {
		auto second = anonymous.Operation();
		second.name = "fixture_other_rows";
		second.fallback = false;
		auto rest = second.Rest();
		rest.request.origin.host = cuac::CompiledHttpHost("other.example");
		auto changed = ConnectorCatalogTestAccess::RestOperation(second, std::move(rest), second.selector);
		std::vector<cuac::CompiledOperation> operations = {anonymous.Operation(), std::move(changed)};
		std::vector<cuac::CompiledRelation> relations;
		relations.push_back(ConnectorCatalogTestAccess::Relation(anonymous.Name(), anonymous.Columns(),
		                                                         std::move(operations), anonymous.Authentication(),
		                                                         anonymous.ResourceCeilings()));
		ConnectorCatalogTestAccess::Catalog(catalog.ConnectorName(), catalog.Version(), std::move(relations),
		                                    catalog.NetworkPolicy());
	});
}

} // namespace

namespace cuac_test {

void RunConnectorCatalogContractTests() {
	TestSafeImmutableService();
	TestOperationSelectorValidation();
	TestClosedValidation();
}

} // namespace cuac_test
