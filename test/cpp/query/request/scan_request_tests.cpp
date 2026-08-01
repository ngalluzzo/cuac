#include "connector/support/package_compiler_test_fixtures.hpp"
#include "cuac/connector/api.hpp"
#include "cuac/query/scan_request.hpp"
#include "connector/support/connector_catalog_test_fixtures.hpp"
#include "query/support/scan_request_test_support.hpp"
#include "query/support/live_scan_request.hpp"
#include "support/require.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using cuac_test::Require;
using cuac_test::query_scan_request::ReadText;
using cuac_test::query_scan_request::RequireCanaryAbsent;
using cuac_test::query_scan_request::RequireFullSelectedSchema;
using cuac_test::query_scan_request::RequireThrows;
using cuac_test::query_scan_request::RuntimeCredentialCanary;
using cuac_test::query_scan_request::ScopedEnvironment;

const char ANONYMOUS_RELATION[] = "duckdb_login_search_page";
const char AUTHENTICATED_RELATION[] = "authenticated_user";
const char REPOSITORY_RELATION[] = "authenticated_repositories";
const char GRAPHQL_RELATION[] = "viewer_repository_metrics";

void TestExplicitInputValues() {
	const auto boolean = cuac::ExplicitInput::Boolean("enabled", true);
	const auto bigint = cuac::ExplicitInput::BigInt("count", -42);
	std::string identifier = "label;\n";
	identifier.push_back(static_cast<char>(0xff));
	std::string exact_varchar = "private;\n";
	exact_varchar.push_back(static_cast<char>(0x01));
	const auto varchar_value = cuac::ExplicitInput::Varchar(identifier, exact_varchar);
	const auto null_boolean = cuac::ExplicitInput::Null("optional_enabled", cuac::ExplicitInputValueKind::BOOLEAN);
	const auto null_bigint = cuac::ExplicitInput::Null("optional_count", cuac::ExplicitInputValueKind::BIGINT);
	const auto null_varchar = cuac::ExplicitInput::Null("optional_label", cuac::ExplicitInputValueKind::VARCHAR);
	const auto bigint_copy = bigint;
	auto varchar_move_source = varchar_value;
	const auto varchar_moved = std::move(varchar_move_source);

	Require(boolean.Identifier() == "enabled" && boolean.Kind() == cuac::ExplicitInputValueKind::BOOLEAN &&
	            !boolean.IsNull() && boolean.BooleanValue() &&
	            boolean.Snapshot() == "input[id=hex:656e61626c6564,kind=boolean,value=true]",
	        "BOOLEAN explicit input lost its exact structural value");
	Require(bigint.Identifier() == "count" && bigint.Kind() == cuac::ExplicitInputValueKind::BIGINT &&
	            !bigint.IsNull() && bigint.BigIntValue() == -42 &&
	            bigint.Snapshot() == "input[id=hex:636f756e74,kind=bigint,value=-42]",
	        "BIGINT explicit input lost its exact structural value");
	Require(varchar_value.Identifier() == identifier && varchar_value.Kind() == cuac::ExplicitInputValueKind::VARCHAR &&
	            !varchar_value.IsNull() && varchar_value.VarcharValue() == exact_varchar &&
	            varchar_value.Snapshot() ==
	                "input[id=hex:6c6162656c3b0aff,kind=varchar,value=hex:707269766174653b0a01]",
	        "VARCHAR explicit input was not byte exact or safely escaped");
	Require(bigint_copy == bigint && bigint_copy.Snapshot() == bigint.Snapshot() && varchar_moved == varchar_value,
	        "explicit input copy or move changed its structural value");
	Require(null_boolean.IsNull() && null_boolean.Kind() == cuac::ExplicitInputValueKind::BOOLEAN &&
	            null_boolean.Snapshot() == "input[id=hex:6f7074696f6e616c5f656e61626c6564,kind=boolean,value=null]" &&
	            null_bigint.IsNull() && null_bigint.Kind() == cuac::ExplicitInputValueKind::BIGINT &&
	            null_varchar.IsNull() && null_varchar.Kind() == cuac::ExplicitInputValueKind::VARCHAR,
	        "explicit NULL lost its present typed identity");

	RequireThrows<std::logic_error>([&]() { (void)boolean.BigIntValue(); },
	                                "BOOLEAN explicit input exposed a BIGINT payload");
	RequireThrows<std::logic_error>([&]() { (void)null_boolean.BooleanValue(); },
	                                "explicit NULL exposed a BOOLEAN payload");
	RequireThrows<std::invalid_argument>([]() { (void)cuac::ExplicitInput::Varchar("", "value"); },
	                                     "empty explicit input identifier was accepted");
	RequireThrows<std::logic_error>(
	    []() { (void)cuac::ExplicitInput::Null("invalid_kind", static_cast<cuac::ExplicitInputValueKind>(99)); },
	    "unknown explicit input kind was accepted");
	for (const auto &unsafe : {"label;", "private;", "\n"}) {
		Require(varchar_value.Snapshot().find(unsafe) == std::string::npos,
		        "explicit input snapshot exposed unescaped structural bytes");
	}
}

void TestOrderedExplicitInputs() {
	const cuac::ExplicitInputs absent;
	const cuac::ExplicitInputs inputs({cuac::ExplicitInput::Varchar("first", "alpha"),
	                                   cuac::ExplicitInput::Null("second", cuac::ExplicitInputValueKind::BIGINT),
	                                   cuac::ExplicitInput::Boolean("third", false),
	                                   cuac::ExplicitInput::BigInt("fourth", 0)});
	const auto copy = inputs;
	auto move_source = inputs;
	const auto moved = std::move(move_source);

	Require(absent.empty() && absent.size() == 0 && absent.begin() == absent.end() && absent.Snapshot() == "[]",
	        "omission was not represented solely by input absence");
	Require(inputs.size() == 4 && inputs.At(0).Identifier() == "first" && inputs.At(1).Identifier() == "second" &&
	            inputs.At(2).Identifier() == "third" && inputs.At(3).Identifier() == "fourth",
	        "explicit inputs did not preserve exact caller order");
	Require(inputs.Find("second") == &inputs.At(1) && inputs.Find("Second") == nullptr &&
	            inputs.Find("missing") == nullptr,
	        "exact structural input lookup normalized or invented an identifier");
	Require(copy == inputs && copy.Snapshot() == inputs.Snapshot() && moved == inputs,
	        "explicit input copy or move changed value semantics");
	Require(!move_source.Snapshot().empty(), "moved-from explicit inputs did not remain a valid object");
	Require(inputs != cuac::ExplicitInputs({cuac::ExplicitInput::Boolean("third", false),
	                                        cuac::ExplicitInput::Varchar("first", "alpha"),
	                                        cuac::ExplicitInput::Null("second", cuac::ExplicitInputValueKind::BIGINT),
	                                        cuac::ExplicitInput::BigInt("fourth", 0)}),
	        "explicit input equality ignored caller order");
	Require(inputs.Snapshot() == "[input[id=hex:6669727374,kind=varchar,value=hex:616c706861],"
	                             "input[id=hex:7365636f6e64,kind=bigint,value=null],"
	                             "input[id=hex:7468697264,kind=boolean,value=false],"
	                             "input[id=hex:666f75727468,kind=bigint,value=0]]",
	        "ordered explicit input snapshot drifted");
	RequireThrows<std::invalid_argument>(
	    []() {
		    (void)cuac::ExplicitInputs(
		        {cuac::ExplicitInput::Boolean("duplicate", true), cuac::ExplicitInput::BigInt("duplicate", 1)});
	    },
	    "duplicate exact explicit input identifier was accepted");
}

void TestLogicalSecretReferenceContract() {
	const cuac::LogicalSecretReference absent;
	Require(!absent.IsPresent() && absent.Snapshot() == "none", "default logical reference was not absent");
	RequireThrows<std::logic_error>([&]() { (void)absent.Name(); }, "absent logical reference exposed a name");
	RequireThrows<std::invalid_argument>([]() { (void)cuac::LogicalSecretReference::Named(""); },
	                                     "empty logical reference name was accepted");

	std::string exact_name = "A;\n";
	exact_name.push_back(static_cast<char>(0x01));
	exact_name.push_back(static_cast<char>(0xff));
	const auto named = cuac::LogicalSecretReference::Named(exact_name);
	const auto copy = named;
	Require(named.IsPresent() && named.Name() == exact_name, "named logical reference changed exact bytes");
	Require(named.Snapshot() == "named-hex:413b0a01ff", "logical reference did not use lower-case byte hex");
	Require(copy.Name() == named.Name() && copy.Snapshot() == named.Snapshot(),
	        "logical reference copy changed identity or explanation");
	Require(cuac::LogicalSecretReference::Named(exact_name).Snapshot() == named.Snapshot(),
	        "logical reference snapshot was not deterministic");
	std::string every_byte;
	std::string expected_every_byte = "named-hex:";
	const std::string lower_hex = "0123456789abcdef";
	for (unsigned int value = 0; value <= 0xff; value++) {
		every_byte.push_back(static_cast<char>(value));
		expected_every_byte.push_back(lower_hex[value >> 4]);
		expected_every_byte.push_back(lower_hex[value & 0x0f]);
	}
	Require(cuac::LogicalSecretReference::Named(every_byte).Snapshot() == expected_every_byte,
	        "logical reference did not lower-case and escape every byte");

	auto move_source = cuac::LogicalSecretReference::Named("move_source");
	const auto moved = std::move(move_source);
	Require(moved.IsPresent() && moved.Name() == "move_source", "logical reference move lost destination identity");
	if (move_source.IsPresent()) {
		Require(!move_source.Name().empty() && move_source.Snapshot() != "named-hex:",
		        "moved-from logical reference remained present without a name");
	} else {
		Require(move_source.Snapshot() == "none", "moved-from logical reference did not become valid absence");
		RequireThrows<std::logic_error>([&]() { (void)move_source.Name(); },
		                                "moved-from absent logical reference exposed a name");
	}
}

void TestAcceptedAnonymousAndAuthenticatedRequests() {
	const auto connector = cuac_test::CompileRepositoryGithubConnectorFixture(CUAC_SOURCE_ROOT);
	const auto anonymous =
	    cuac_test::BuildPackageScanRequest(connector, ANONYMOUS_RELATION, cuac::LogicalSecretReference());
	const auto authenticated = cuac_test::BuildPackageScanRequest(
	    connector, AUTHENTICATED_RELATION, cuac::LogicalSecretReference::Named("github_default"));
	const auto *anonymous_relation = connector.FindRelation(ANONYMOUS_RELATION);
	const auto *authenticated_relation = connector.FindRelation(AUTHENTICATED_RELATION);

	Require(anonymous_relation && authenticated_relation, "package relations disappeared");
	Require(anonymous.connector_name == "github" && anonymous.relation_name == ANONYMOUS_RELATION,
	        "anonymous request did not copy exact selected identity");
	Require(authenticated.connector_name == "github" && authenticated.relation_name == AUTHENTICATED_RELATION,
	        "authenticated request did not copy exact selected identity");
	RequireFullSelectedSchema(anonymous, *anonymous_relation);
	RequireFullSelectedSchema(authenticated, *authenticated_relation);
	Require(anonymous.explicit_inputs.empty() && authenticated.explicit_inputs.empty(),
	        "logical selector entered explicit relation inputs");
	Require(anonymous.requested_predicate == cuac::RequestedPredicate::Unrestricted() &&
	            authenticated.requested_predicate == cuac::RequestedPredicate::Unrestricted() &&
	            anonymous.retained_predicate_scope == cuac::RetainedPredicateScope::UNRESTRICTED &&
	            authenticated.retained_predicate_scope == cuac::RetainedPredicateScope::UNRESTRICTED &&
	            anonymous.orderings.empty() && authenticated.orderings.empty() && !anonymous.has_limit &&
	            !authenticated.has_limit && !anonymous.has_offset && !authenticated.has_offset,
	        "request builder left the conservative relational profile");
	Require(anonymous.capabilities.HasConservativeRelationalProfile() &&
	            authenticated.capabilities.HasConservativeRelationalProfile() &&
	            anonymous.capabilities.secret_manager && authenticated.capabilities.secret_manager,
	        "request builder did not publish the supported adapter capability profile");
	Require(!anonymous.secret_reference.IsPresent() && authenticated.secret_reference.Name() == "github_default",
	        "request builder changed logical-reference presence or identity");
	Require(anonymous.Snapshot() ==
	            "connector=github;relation=duckdb_login_search_page;inputs=[];projection=id,login,site_admin;"
	            "requested-predicate=true;retained-predicate-scope=unrestricted;ordering=[];limit=unset;"
	            "offset=unset;capabilities=projection:"
	            "unavailable,filter:unavailable,selective-predicate:unavailable,retains-predicate:unavailable,"
	            "ordering:unavailable,limit:unavailable,offset:unavailable,progress:unavailable,"
	            "cancellation:verified,secret-manager:available;secret-reference=none;generation=package,"
	            "spec_identifier=hex:637561632f7631,connector_id=hex:676974687562,version=hex:312e302e30,"
	            "digest=hex:7368613235362e666666666666666666666666666666666666666666666666666666666666666666666666"
	            "66666666666666666666666666666666666666666666666666666666;"
	            "cache_mode=off;fresh_ms=0;stale_ms=0",
	        "anonymous request snapshot changed (observed=" + anonymous.Snapshot() + ")");
	Require(authenticated.Snapshot().find(";relation=authenticated_user;") != std::string::npos &&
	            authenticated.Snapshot().find("secret-reference=named-hex:6769746875625f64656661756c74") !=
	                std::string::npos,
	        "authenticated request snapshot lost exact relation or escaped selector identity");
}

void TestAuthenticatedRepositoryRequest() {
	const auto connector = cuac_test::CompileRepositoryGithubConnectorFixture(CUAC_SOURCE_ROOT);
	const auto request = cuac_test::BuildPackageScanRequest(connector, REPOSITORY_RELATION,
	                                                        cuac::LogicalSecretReference::Named("github_default"));
	const auto copy = request;
	const auto *relation = connector.FindRelation(REPOSITORY_RELATION);

	Require(relation, "package repository relation disappeared");
	Require(request.connector_name == "github" && request.relation_name == REPOSITORY_RELATION,
	        "repository request did not copy exact selected identity");
	RequireFullSelectedSchema(request, *relation);
	Require(request.projected_columns ==
	                std::vector<std::string>({"id", "full_name", "private", "fork", "archived", "visibility"}) &&
	            request.explicit_inputs.empty(),
	        "repository request did not preserve its exact full-projection closure");
	Require(request.requested_predicate == cuac::RequestedPredicate::Unrestricted() &&
	            request.retained_predicate_scope == cuac::RetainedPredicateScope::UNRESTRICTED &&
	            request.orderings.empty() && !request.has_limit && !request.has_offset &&
	            request.capabilities.HasConservativeRelationalProfile() && request.capabilities.secret_manager,
	        "repository request changed Query's conservative capability profile");
	Require(request.secret_reference.Name() == "github_default",
	        "repository request changed the exact logical secret reference");
	const std::string expected =
	    "connector=github;relation=authenticated_repositories;inputs=[];projection=id,full_name,private,fork,"
	    "archived,visibility;requested-predicate=true;retained-predicate-scope=unrestricted;ordering=[];"
	    "limit=unset;offset=unset;capabilities="
	    "projection:unavailable,filter:unavailable,selective-predicate:unavailable,retains-predicate:unavailable,"
	    "ordering:unavailable,limit:unavailable,offset:unavailable,progress:unavailable,cancellation:"
	    "verified,secret-manager:available;secret-reference=named-hex:6769746875625f64656661756c74;"
	    "generation=package,spec_identifier=hex:637561632f7631,connector_id=hex:676974687562,version=hex:312e302e30,"
	    "digest=hex:7368613235362e666666666666666666666666666666666666666666666666666666666666666666666666"
	    "66666666666666666666666666666666666666666666666666666666;cache_mode=off;fresh_ms=0;stale_ms=0";
	Require(request.Snapshot() == expected && copy.Snapshot() == expected,
	        "repository request copy or exact snapshot drifted");
	for (const auto &forbidden : {"api.github.com", "/user/repos", "per_page", "page=", "Link", "Bearer "}) {
		Require(request.Snapshot().find(forbidden) == std::string::npos,
		        "repository request acquired provider pagination or credential state");
	}
}

void TestGraphqlRepositoryMetricsRequestStaysProtocolNeutral() {
	const auto connector = cuac_test::CompileRepositoryGithubConnectorFixture(CUAC_SOURCE_ROOT);
	const auto request = cuac_test::BuildPackageScanRequest(connector, GRAPHQL_RELATION,
	                                                        cuac::LogicalSecretReference::Named("github_default"));
	const auto copy = request;
	const auto *relation = connector.FindRelation(GRAPHQL_RELATION);

	Require(relation, "package GraphQL relation disappeared");
	Require(request.connector_name == "github" && request.relation_name == GRAPHQL_RELATION,
	        "GraphQL request did not copy exact selected identity");
	RequireFullSelectedSchema(request, *relation);
	Require(request.projected_columns ==
	                std::vector<std::string>({"id", "full_name", "owner_login", "stars", "primary_language", "private",
	                                          "archived", "updated_at"}) &&
	            request.explicit_inputs.empty(),
	        "GraphQL request did not preserve its exact full-schema closure");
	Require(request.requested_predicate == cuac::RequestedPredicate::Unrestricted() &&
	            request.retained_predicate_scope == cuac::RetainedPredicateScope::UNRESTRICTED &&
	            request.orderings.empty() && !request.has_limit && !request.has_offset &&
	            request.capabilities.HasConservativeRelationalProfile() && request.capabilities.secret_manager &&
	            request.secret_reference.Name() == "github_default",
	        "GraphQL request changed Query's conservative capability or secret-selector profile");
	Require(copy.Snapshot() == request.Snapshot(), "GraphQL request copy changed its immutable snapshot");
	for (const auto &forbidden : {"api.github.com", "viewer {", "repositories(", "query Cuac", "$cursor", "pageSize",
	                              "Authorization", "Bearer ", "SHA256"}) {
		Require(request.Snapshot().find(forbidden) == std::string::npos,
		        "protocol-specific operation authority entered Query's ScanRequest");
	}
}

void TestProviderOwnedDistinctSchemaRequests() {
	const auto connector = cuac_test::BuildDistinctSchemaConnectorCatalogFixture();
	const auto anonymous = cuac_test::BuildPackageScanRequest(connector, cuac_test::DISTINCT_SCHEMA_ANONYMOUS_RELATION,
	                                                          cuac::LogicalSecretReference());
	const auto authenticated =
	    cuac_test::BuildPackageScanRequest(connector, cuac_test::DISTINCT_SCHEMA_AUTHENTICATED_RELATION,
	                                       cuac::LogicalSecretReference::Named("fixture_secret_name"));
	const auto *anonymous_relation = connector.FindRelation(cuac_test::DISTINCT_SCHEMA_ANONYMOUS_RELATION);
	const auto *authenticated_relation = connector.FindRelation(cuac_test::DISTINCT_SCHEMA_AUTHENTICATED_RELATION);

	Require(anonymous_relation && authenticated_relation, "Connector-owned distinct relations disappeared");
	Require(anonymous.connector_name == "fixture_distinct_catalog" &&
	            anonymous.relation_name == cuac_test::DISTINCT_SCHEMA_ANONYMOUS_RELATION &&
	            anonymous.projected_columns == std::vector<std::string>({"public_id", "public_label"}) &&
	            !anonymous.secret_reference.IsPresent(),
	        "anonymous request was hard-coded to the package catalog shape");
	Require(authenticated.connector_name == "fixture_distinct_catalog" &&
	            authenticated.relation_name == cuac_test::DISTINCT_SCHEMA_AUTHENTICATED_RELATION &&
	            authenticated.projected_columns ==
	                std::vector<std::string>({"profile_login", "profile_verified", "profile_generation"}) &&
	            authenticated.secret_reference.Name() == "fixture_secret_name",
	        "authenticated request was hard-coded to the package catalog shape");
	RequireFullSelectedSchema(anonymous, *anonymous_relation);
	RequireFullSelectedSchema(authenticated, *authenticated_relation);
}

void TestPresenceRulesAndExactSelection() {
	const auto connector = cuac_test::CompileRepositoryGithubConnectorFixture(CUAC_SOURCE_ROOT);
	RequireThrows<std::invalid_argument>(
	    [&]() {
		    (void)cuac_test::BuildPackageScanRequest(connector, ANONYMOUS_RELATION,
		                                             cuac::LogicalSecretReference::Named("github_default"));
	    },
	    "anonymous relation accepted a logical reference");
	RequireThrows<std::invalid_argument>(
	    [&]() {
		    (void)cuac_test::BuildPackageScanRequest(connector, AUTHENTICATED_RELATION, cuac::LogicalSecretReference());
	    },
	    "authenticated relation accepted an absent logical reference");
	RequireThrows<std::invalid_argument>(
	    [&]() {
		    (void)cuac_test::BuildPackageScanRequest(connector, REPOSITORY_RELATION, cuac::LogicalSecretReference());
	    },
	    "repository relation accepted an absent logical reference");
	RequireThrows<std::invalid_argument>(
	    [&]() {
		    (void)cuac_test::BuildPackageScanRequest(connector, GRAPHQL_RELATION, cuac::LogicalSecretReference());
	    },
	    "GraphQL relation accepted an absent logical reference");
	for (const auto &relation_name : {"Authenticated_User", "Authenticated_Repositories", "missing"}) {
		RequireThrows<std::invalid_argument>(
		    [&]() {
			    (void)cuac_test::BuildPackageScanRequest(connector, relation_name, cuac::LogicalSecretReference());
		    },
		    "unknown or wrong-case relation fell back under an absent reference");
		RequireThrows<std::invalid_argument>(
		    [&]() {
			    (void)cuac_test::BuildPackageScanRequest(connector, relation_name,
			                                             cuac::LogicalSecretReference::Named("github_default"));
		    },
		    "unknown or wrong-case relation fell back under a named reference");
	}
}

void RequireRelationalMutationsRejected(bool secret_manager) {
	cuac::AdapterCapabilities capabilities = {false, false, false, false, false,
	                                          false, false, false, true,  secret_manager};
	Require(capabilities.HasConservativeRelationalProfile(), "baseline relational profile was not conservative");
	capabilities.projection = true;
	Require(!capabilities.HasConservativeRelationalProfile(), "available projection was classified as conservative");
	capabilities.projection = false;
	capabilities.filter = true;
	Require(!capabilities.HasConservativeRelationalProfile(),
	        "available filter metadata was classified as conservative");
	capabilities.filter = false;
	capabilities.selective_predicate = true;
	Require(!capabilities.HasConservativeRelationalProfile(),
	        "available selective predicate metadata was classified as baseline conservative");
	capabilities.selective_predicate = false;
	capabilities.retains_predicate = true;
	Require(!capabilities.HasConservativeRelationalProfile(),
	        "retained-predicate verification was classified as baseline conservative");
	capabilities.retains_predicate = false;
	capabilities.ordering = true;
	Require(!capabilities.HasConservativeRelationalProfile(), "available ordering was classified as conservative");
	capabilities.ordering = false;
	capabilities.limit = true;
	Require(!capabilities.HasConservativeRelationalProfile(), "available limit was classified as conservative");
	capabilities.limit = false;
	capabilities.offset = true;
	Require(!capabilities.HasConservativeRelationalProfile(), "available offset was classified as conservative");
	capabilities.offset = false;
	capabilities.progress = true;
	Require(!capabilities.HasConservativeRelationalProfile(), "available progress was classified as conservative");
	capabilities.progress = false;
	capabilities.cancellation = false;
	Require(!capabilities.HasConservativeRelationalProfile(), "unverified cancellation was classified as conservative");
}

void TestProtocolNeutralSelectiveCandidateCopy() {
	const auto connector = cuac_test::CompileRepositoryGithubConnectorFixture(CUAC_SOURCE_ROOT);
	const auto baseline = cuac_test::BuildPackageScanRequest(connector, REPOSITORY_RELATION,
	                                                         cuac::LogicalSecretReference::Named("github_default"));
	auto candidate = baseline;
	candidate.requested_predicate = cuac::RequestedPredicate::Comparison(
	    5, cuac::RequestedPredicateValueKind::VARCHAR, cuac::RequestedPredicateComparisonOperator::EQUALS,
	    cuac::RequestedPredicateValue::Varchar("private"));
	candidate.retained_predicate_scope = cuac::RetainedPredicateScope::REQUESTED_PREDICATE;
	candidate.capabilities.selective_predicate = true;
	candidate.capabilities.retains_predicate = true;

	Require(baseline.requested_predicate == cuac::RequestedPredicate::Unrestricted() &&
	            baseline.capabilities.HasConservativeRelationalProfile(),
	        "selective candidate mutation changed the retained baseline request");
	Require(candidate.requested_predicate ==
	                cuac::RequestedPredicate::Comparison(5, cuac::RequestedPredicateValueKind::VARCHAR,
	                                                     cuac::RequestedPredicateComparisonOperator::EQUALS,
	                                                     cuac::RequestedPredicateValue::Varchar("private")) &&
	            candidate.capabilities.selective_predicate && candidate.capabilities.retains_predicate &&
	            !candidate.capabilities.filter,
	        "selective candidate did not distinguish advisory recognition from generic filter execution");
	Require(candidate.Snapshot().find("requested-predicate=comparison[column:5,type:varchar,operator:equals,"
	                                  "literal:varchar:hex:70726976617465]") != std::string::npos &&
	            candidate.Snapshot().find("retained-predicate-scope=requested_predicate") != std::string::npos &&
	            candidate.Snapshot().find("selective-predicate:available,retains-predicate:verified") !=
	                std::string::npos,
	        "selective candidate snapshot lost its closed capability facts");
	for (const auto &forbidden : {"visibility=private", "/user/repos", "REST_QUERY_PARAMETER", "api.github.com"}) {
		Require(candidate.Snapshot().find(forbidden) == std::string::npos,
		        "Query candidate selected a provider request field, value encoding, or operation");
	}
}

void TestCapabilityClassificationAndSemanticsMutation() {
	RequireRelationalMutationsRejected(false);
	RequireRelationalMutationsRejected(true);

	const auto connector = cuac_test::CompileRepositoryGithubConnectorFixture(CUAC_SOURCE_ROOT);
	auto request = cuac_test::BuildPackageScanRequest(connector, AUTHENTICATED_RELATION,
	                                                  cuac::LogicalSecretReference::Named("github_default"));
	request.capabilities.secret_manager = false;
	Require(request.capabilities.HasConservativeRelationalProfile() && request.secret_reference.IsPresent(),
	        "request could not represent missing required capability for defensive Semantics validation");
}

void TestScanGenerationIdentityContract() {
	cuac::ScanGenerationIdentity package_a {"spec_a", "conn_a", "1.0.0", "sha256.aaa"};
	cuac::ScanGenerationIdentity package_b {"spec_a", "conn_a", "1.0.0", "sha256.aaa"};
	cuac::ScanGenerationIdentity package_c {"spec_a", "conn_a", "1.0.0", "sha256.bbb"};
	Require(package_a == package_b, "identical package identities were not equal");
	Require(package_a != package_c, "package identities differing in digest compared equal");

	const std::string pkg_snapshot = package_a.Snapshot();
	for (const auto &field : {"spec_a", "conn_a", "1.0.0", "sha256.aaa"}) {
		Require(pkg_snapshot.find(field) == std::string::npos,
		        "package identity snapshot leaked unescaped structural bytes");
	}
	Require(pkg_snapshot.find("hex:") != std::string::npos,
	        "package identity snapshot did not hex-encode structural fields");
	Require(pkg_snapshot.find("package") != std::string::npos,
	        "package identity snapshot did not identify itself as package kind");
}

void TestEnvironmentIndependenceAndCredentialAbsence() {
	const auto connector = cuac_test::CompileRepositoryGithubConnectorFixture(CUAC_SOURCE_ROOT);
	const auto baseline = cuac_test::BuildPackageScanRequest(connector, AUTHENTICATED_RELATION,
	                                                         cuac::LogicalSecretReference::Named("github_default"));
	const auto canary = RuntimeCredentialCanary();
	for (const auto &path :
	     {"src/include/cuac/query/scan_request.hpp", "src/query/request/scan_request.cpp",
	      "test/cpp/query/request/scan_request_tests.cpp", "test/cpp/query/support/scan_request_test_support.hpp"}) {
		Require(ReadText(path).find(canary) == std::string::npos, "credential canary pre-existed in request source");
	}
	const auto production_source =
	    ReadText("src/include/cuac/query/scan_request.hpp") + ReadText("src/query/request/scan_request.cpp");
	for (const auto &ambient_api :
	     {"getenv(", "std::getenv", "::getenv", "setenv(", "unsetenv(", "**environ", "*environ["}) {
		Require(production_source.find(ambient_api) == std::string::npos,
		        "request production source gained ambient environment access");
	}
	RequireCanaryAbsent(baseline, canary);

	ScopedEnvironment environment;
	for (const auto &name :
	     {"CUAC_TOKEN", "cuac_token", "GITHUB_TOKEN", "github_token", "HOME", "CURL_HOME", "HTTP_PROXY", "http_proxy",
	      "HTTPS_PROXY", "https_proxy", "ALL_PROXY", "all_proxy", "NO_PROXY", "no_proxy"}) {
		environment.Set(name, canary);
	}
	const auto hostile = cuac_test::BuildPackageScanRequest(connector, AUTHENTICATED_RELATION,
	                                                        cuac::LogicalSecretReference::Named("github_default"));
	Require(hostile.Snapshot() == baseline.Snapshot(), "request construction depended on ambient environment state");
	RequireCanaryAbsent(hostile, canary);
}

} // namespace

int main() {
	try {
		TestExplicitInputValues();
		TestOrderedExplicitInputs();
		TestLogicalSecretReferenceContract();
		TestAcceptedAnonymousAndAuthenticatedRequests();
		TestAuthenticatedRepositoryRequest();
		TestGraphqlRepositoryMetricsRequestStaysProtocolNeutral();
		TestProviderOwnedDistinctSchemaRequests();
		TestPresenceRulesAndExactSelection();
		TestCapabilityClassificationAndSemanticsMutation();
		TestProtocolNeutralSelectiveCandidateCopy();
		TestScanGenerationIdentityContract();
		TestEnvironmentIndependenceAndCredentialAbsence();
		std::cout << "scan request tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "scan request tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
