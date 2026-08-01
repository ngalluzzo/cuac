#include "connector/support/package_compiler_test_fixtures.hpp"

#include "cuac/connector/content_digest.hpp"
#include "cuac/connector/local_package_compiler.hpp"
#include "cuac/connector/package_fixture_runner.hpp"
#include "support/require.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

using cuac_test::Require;

class NeverCancel final : public cuac::connector::PackageCancellation {
public:
	bool IsCancellationRequested() const noexcept override {
		return false;
	}
};

class AlwaysCancel final : public cuac::connector::PackageCancellation {
public:
	bool IsCancellationRequested() const noexcept override {
		return true;
	}
};

class FirstCaseProbe final : public cuac::connector::PackageFixtureExecutionService {
public:
	cuac::connector::PackageFixtureObservation
	Execute(const cuac::CompiledPackageGeneration &, const cuac::connector::PackageFixtureCase &,
	        const std::vector<cuac::connector::PackageFixtureCoverageEntry> &,
	        cuac::connector::PackageCancellation &) override {
		calls++;
		throw std::runtime_error("corpus probe stops at the first identity-verified case");
	}

	std::size_t calls = 0;
};

std::string ReadBytes(const std::string &path) {
	std::ifstream input(path.c_str(), std::ios::binary);
	Require(static_cast<bool>(input), "fixture contract asset could not be opened");
	std::ostringstream bytes;
	bytes << input.rdbuf();
	return bytes.str();
}

bool Contains(const cuac::connector::PackageFixtureCoverage &coverage, const std::string &key) {
	return std::find(coverage.RequiredKeys().begin(), coverage.RequiredKeys().end(), key) !=
	       coverage.RequiredKeys().end();
}

void TestGithubCoverageMatchesAcceptedMapping(const std::string &repository_root) {
	const auto generation = cuac_test::CompileRepositoryGithubGenerationFixture(repository_root);
	const auto coverage = cuac::connector::DerivePackageFixtureCoverage(generation);
	Require(coverage.RequiredKeys().size() == 266,
	        "repository GitHub package did not derive all 266 accepted coverage keys (observed=" +
	            std::to_string(coverage.RequiredKeys().size()) + ")");
	Require(coverage.Entries().size() == coverage.RequiredKeys().size(),
	        "typed coverage registry does not align one-for-one with rendered keys");
	Require(coverage.OrderedDigest() == "sha256.555116e3bd3f0dd28164e0e24218a14f536c054ff1cac1ded2b1cb6d33ccdfa5",
	        "repository GitHub coverage ordering drifted from RFC 0013 (observed=" + coverage.OrderedDigest() + ")");
	Require(coverage.RequiredKeys().front() ==
	                "operation_duckdb_login_search_page_github_search_duckdb_login_page_success" &&
	            coverage.RequiredKeys().back() == "diagnostic_cuac_publication_conflict",
	        "coverage rule ordering no longer follows the accepted mapping");
	Require(Contains(coverage, "predicate_authenticated_repositories_private_visibility_unavailable_structure_local") &&
	            Contains(coverage, "resource_viewer_repository_metrics_github_viewer_repository_metrics_"
	                               "max_serialized_body_bytes_per_scan_one_over_rejected") &&
	            Contains(coverage, "diagnostic_cuac_fixture_mismatch"),
	        "coverage derivation lost predicate, GraphQL resource, or fixture-diagnostic scope");
	const auto predicate_entry =
	    std::find_if(coverage.Entries().begin(), coverage.Entries().end(),
	                 [](const cuac::connector::PackageFixtureCoverageEntry &entry) {
		                 return entry.key == "predicate_authenticated_repositories_private_visibility_positive";
	                 });
	const auto diagnostic_entry = std::find_if(coverage.Entries().begin(), coverage.Entries().end(),
	                                           [](const cuac::connector::PackageFixtureCoverageEntry &entry) {
		                                           return entry.key == "diagnostic_cuac_fixture_mismatch";
	                                           });
	Require(predicate_entry != coverage.Entries().end() &&
	            predicate_entry->scope == cuac::connector::PackageFixtureCoverageScope::PREDICATE &&
	            predicate_entry->relation == "authenticated_repositories" &&
	            predicate_entry->predicate == "private_visibility" && predicate_entry->variant == "positive" &&
	            diagnostic_entry != coverage.Entries().end() &&
	            diagnostic_entry->scope == cuac::connector::PackageFixtureCoverageScope::DIAGNOSTIC &&
	            diagnostic_entry->diagnostic == "CUAC_FIXTURE_MISMATCH",
	        "typed coverage registry lost structural predicate or diagnostic bindings");

	const auto *relation = generation.Connector().FindRelation("authenticated_repositories");
	Require(relation != nullptr && relation->PredicateMappings().size() == 1 &&
	            relation->PredicateMappings()[0].Name() == "private_visibility",
	        "compiled predicate facts lost the author identity required for independent coverage derivation");
}

void TestCoverageIsCompiledFactDriven(const std::string &repository_root) {
	const auto generation = cuac_test::CompileNonGithubGraphqlGenerationFixture(repository_root);
	const auto coverage = cuac::connector::DerivePackageFixtureCoverage(generation);
	Require(Contains(coverage, "selection_regional_events_regional_event_graph_selected") &&
	            Contains(coverage, "selection_regional_events_fallback_events_selected") &&
	            Contains(coverage, "selection_regional_events_highest_rank_tie_rejected") &&
	            Contains(coverage, "predicate_regional_events_active_events_positive") &&
	            Contains(coverage, "predicate_regional_events_public_events_positive") &&
	            Contains(coverage, "graphql_regional_events_regional_event_graph_serialized_body_identity") &&
	            !Contains(coverage, "predicate_authenticated_repositories_private_visibility_positive"),
	        "coverage derivation used repository identity instead of compiled feature facts");
	Require(coverage.RequiredKeys().size() == 206,
	        "controlled package did not derive its complete semantic coverage matrix (observed=" +
	            std::to_string(coverage.RequiredKeys().size()) + ")");

	const auto *relation = generation.Connector().FindRelation("regional_events");
	Require(relation != nullptr && relation->Inputs().size() == 7 && relation->Operations().size() == 3 &&
	            relation->PredicateMappings().size() == 2,
	        "controlled package lost its input, operation-selection, or predicate matrix");
	const auto operation = [&](const std::string &name) -> const cuac::CompiledOperation & {
		const auto found =
		    std::find_if(relation->Operations().begin(), relation->Operations().end(),
		                 [&](const cuac::CompiledOperation &candidate) { return candidate.name == name; });
		Require(found != relation->Operations().end(), "controlled package operation is missing");
		return *found;
	};
	const auto input = [&](const std::string &name) -> const cuac::CompiledRelationInput & {
		const auto found =
		    std::find_if(relation->Inputs().begin(), relation->Inputs().end(),
		                 [&](const cuac::CompiledRelationInput &candidate) { return candidate.Name() == name; });
		Require(found != relation->Inputs().end(), "controlled package input is missing");
		return *found;
	};
	Require(input("include_cancelled").Default().HasDefault() &&
	            input("include_cancelled").Default().Value().Type() == cuac::CompiledScalarType::BOOLEAN &&
	            !input("include_cancelled").Default().Value().Boolean() &&
	            input("minimum_attendance").Default().HasDefault() &&
	            input("minimum_attendance").Default().Value().Type() == cuac::CompiledScalarType::BIGINT &&
	            input("minimum_attendance").Default().Value().Bigint() == 25 &&
	            input("audience").Default().HasDefault() &&
	            input("audience").Default().Value().Type() == cuac::CompiledScalarType::VARCHAR &&
	            input("audience").Default().Value().Varchar() == "public" && input("audience").Nullable() &&
	            input("note").Default().HasDefault() && input("note").Default().Value().IsNull() &&
	            input("note").Nullable(),
	        "controlled package lost typed BOOLEAN/BIGINT/VARCHAR or typed NULL defaults");
	const auto &graph = operation("regional_event_graph");
	const auto &rest = operation("regional_event_rest");
	const auto &fallback = operation("fallback_events");
	const auto requires_relation_input = [](const cuac::CompiledOperation &candidate, const std::string &id) {
		return std::find_if(candidate.selector.RequiredInputReferences().begin(),
		                    candidate.selector.RequiredInputReferences().end(),
		                    [&](const cuac::CompiledRequiredInputReference &reference) {
			                    return reference.Kind() == cuac::CompiledRequiredInputKind::RELATION_INPUT &&
			                           reference.Id() == id;
		                    }) != candidate.selector.RequiredInputReferences().end();
	};
	Require(!graph.fallback && graph.selector.RequiredInputReferences().size() == 2 && !rest.fallback &&
	            requires_relation_input(graph, "region") && requires_relation_input(graph, "graph_view") &&
	            rest.selector.RequiredInputReferences().size() == 2 && requires_relation_input(rest, "rest_view") &&
	            requires_relation_input(rest, "include_cancelled") && fallback.fallback &&
	            fallback.selector.RequiredInputReferences().empty(),
	        "controlled package lost its equal-rank unique/tie candidates or unconditional fallback");
	const auto nullable_rest_binding = [&](const std::string &name) {
		const auto &query = rest.Rest().request.query_parameters;
		const auto found = std::find_if(query.begin(), query.end(),
		                                [&](const cuac::CompiledQueryParameter &field) { return field.name == name; });
		return found != query.end() && found->source == cuac::CompiledQueryValueSource::RELATION_INPUT &&
		       found->source_id == name && found->omit_when_null;
	};
	Require(nullable_rest_binding("region") && nullable_rest_binding("audience") && nullable_rest_binding("note"),
	        "controlled REST operation no longer proves nullable inputs are omitted at the request boundary");
	Require(relation->PredicateMappings()[0].Accuracy() == cuac::CompiledPredicateAccuracy::EXACT &&
	            relation->PredicateMappings()[1].Accuracy() == cuac::CompiledPredicateAccuracy::SUPERSET &&
	            graph.Graphql().document_digest == "ca2060e0db0b535bbf4a2b96050127159fb3e953cd52bd17dd8b2ae955464d28",
	        "controlled package lost exact/superset proof facts or its distinct GraphQL recipe identity");
}

void TestRetryCoverageUsesTheSameOfflineContract(const std::string &repository_root) {
	const auto generation = cuac_test::CompileRetryGenerationFixture(repository_root);
	const auto coverage = cuac::connector::DerivePackageFixtureCoverage(generation);
	Require(generation.Identity().SpecIdentifier() == "cuac/v1" &&
	            Contains(coverage, "operation_duplicate_events_list_duplicate_events_success") &&
	            Contains(coverage, "operation_duplicate_graphql_events_list_duplicate_graphql_events_success") &&
	            Contains(coverage, "pagination_duplicate_events_list_duplicate_events_single_page_termination") &&
	            Contains(coverage, "graphql_duplicate_graphql_events_list_duplicate_graphql_events_"
	                               "serialized_body_identity"),
	        "cuac/v1 retry operations were rejected or omitted by offline fixture coverage derivation");
	const auto *relation = generation.Connector().FindRelation("duplicate_events");
	Require(relation != nullptr && relation->Snapshot().find("rate_limit=disabled") != std::string::npos,
	        "single v1 policy path omitted disabled rate-limit state");
}

void TestRateLimitCoverageIsTypedAndClosed(const std::string &repository_root) {
	const auto generation = cuac_test::CompileRateLimitGenerationFixture(repository_root);
	const auto coverage = cuac::connector::DerivePackageFixtureCoverage(generation);
	const auto *rest_relation = generation.Connector().FindRelation("duplicate_events");
	const auto *graphql_relation = generation.Connector().FindRelation("duplicate_graphql_events");
	const auto *disabled_relation = generation.Connector().FindRelation("disabled_events");
	Require(rest_relation != nullptr && graphql_relation != nullptr && disabled_relation != nullptr &&
	            disabled_relation->Snapshot().find("rate_limit=disabled") != std::string::npos,
	        "rate-limit fixture lost a relation or its explicit disabled safe explanation");
	const auto &rest = rest_relation->Operation();
	const auto &rest_policy = rest.RateLimitPolicy();
	const auto &graphql = graphql_relation->Operation();
	const auto &graphql_policy = graphql.RateLimitPolicy();
	Require(
	    rest.RetryRecommendation().Enabled() && rest_policy.Declared() && rest_policy.WaitingEnabled() &&
	        rest_policy.mode == cuac::CompiledRateLimitMode::WAIT_IF_DEADLINE_ALLOWS &&
	        rest_policy.statuses == std::vector<std::uint16_t>({429, 503}) &&
	        rest_policy.operation_family == "core_requests" &&
	        rest_policy.scope == cuac::CompiledRateLimitPrincipalScope::CREDENTIAL_AUTHORITY &&
	        rest_policy.guidance.size() == 2 && rest_policy.guidance[0].header_name == "retry-after" &&
	        rest_policy.guidance[0].format == cuac::CompiledRateLimitGuidanceFormat::RETRY_AFTER &&
	        rest_policy.guidance[1].header_name == "x-ratelimit-reset" &&
	        rest_policy.guidance[1].format == cuac::CompiledRateLimitGuidanceFormat::UNIX_SECONDS &&
	        rest_policy.remaining_quota_header == "x-ratelimit-remaining" &&
	        rest_policy.remote_bucket_header == "x-ratelimit-resource" && rest_policy.max_attempts_per_step == 3 &&
	        rest_policy.max_delay_milliseconds == 30000 &&
	        rest_policy.max_cumulative_waiting_milliseconds_per_scan == 30000 &&
	        graphql.RetryRecommendation().Enabled() && graphql_policy.Declared() &&
	        graphql_policy.mode == cuac::CompiledRateLimitMode::WAIT &&
	        graphql_policy.scope == cuac::CompiledRateLimitPrincipalScope::SHARED &&
	        graphql_policy.guidance.size() == 1 &&
	        graphql_policy.guidance[0].format == cuac::CompiledRateLimitGuidanceFormat::DELTA_SECONDS,
	    "v1 IR lost sorted statuses, ordered guidance, policy identity, optional roles, maxima, or retry coexistence");
	const auto snapshot = rest_relation->Snapshot();
	Require(
	    snapshot.find("rate_limit=mode:wait_if_deadline_allows,statuses:[429,503],operation_family:core_requests") !=
	            std::string::npos &&
	        snapshot.find("retry-after:retry_after,x-ratelimit-reset:unix_seconds") != std::string::npos &&
	        snapshot.find("received") == std::string::npos,
	    "Connector safe explanation omitted normalized policy facts or exposed response values");
	Require(generation.Identity().SpecIdentifier() == "cuac/v1" &&
	            Contains(coverage, "rate_limit_disabled_events_list_disabled_events_mode_disabled") &&
	            Contains(coverage, "rate_limit_fail_events_list_fail_events_mode_fail") &&
	            Contains(coverage, "rate_limit_duplicate_events_list_duplicate_events_mode_wait_if_deadline_allows") &&
	            Contains(coverage, "rate_limit_duplicate_graphql_events_list_duplicate_graphql_events_mode_wait") &&
	            Contains(coverage, "rate_limit_duplicate_events_list_duplicate_events_status_429") &&
	            Contains(coverage, "rate_limit_duplicate_events_list_duplicate_events_status_503") &&
	            Contains(coverage, "rate_limit_duplicate_events_list_duplicate_events_guidance_0_retry_after") &&
	            Contains(coverage, "rate_limit_duplicate_events_list_duplicate_events_guidance_1_unix_seconds") &&
	            Contains(coverage, "rate_limit_duplicate_events_list_duplicate_events_remaining_present") &&
	            Contains(coverage, "rate_limit_duplicate_events_list_duplicate_events_remote_bucket_present") &&
	            Contains(coverage, "rate_limit_duplicate_events_list_duplicate_events_safe_explanation"),
	        "v1 coverage omitted a disabled, fail, wait, status, guidance, optional-role, or explanation variant");
	const auto status =
	    std::find_if(coverage.Entries().begin(), coverage.Entries().end(),
	                 [](const cuac::connector::PackageFixtureCoverageEntry &entry) {
		                 return entry.key == "rate_limit_duplicate_events_list_duplicate_events_status_503";
	                 });
	const auto guidance = std::find_if(
	    coverage.Entries().begin(), coverage.Entries().end(),
	    [](const cuac::connector::PackageFixtureCoverageEntry &entry) {
		    return entry.key == "rate_limit_duplicate_events_list_duplicate_events_guidance_1_unix_seconds";
	    });
	Require(status != coverage.Entries().end() && guidance != coverage.Entries().end() &&
	            status->scope == cuac::connector::PackageFixtureCoverageScope::RATE_LIMIT &&
	            status->relation == "duplicate_events" && status->operation == "list_duplicate_events" &&
	            status->rate_limit_status == 503 && guidance->rate_limit_header == "x-ratelimit-reset" &&
	            guidance->rate_limit_format == cuac::CompiledRateLimitGuidanceFormat::UNIX_SECONDS,
	        "v1 coverage recovered rate-limit bindings from rendered keys instead of typed compiled facts");
}

void TestFixtureDiagnosticVocabulary() {
	const cuac::connector::PackageDiagnostic diagnostic(
	    cuac::connector::PackageDiagnosticCode::FIXTURE_MISMATCH, cuac::connector::PackageDiagnosticPhase::FIXTURE,
	    {"fixtures/index.yaml", 7, 5, "$.cases[1]"}, "github", "authenticated_repositories",
	    "github_authenticated_repositories", nullptr, "private_visibility_matching");
	Require(std::string(cuac::connector::PackageDiagnosticCodeName(diagnostic.Code())) == "CUAC_FIXTURE_MISMATCH" &&
	            std::string(cuac::connector::PackageDiagnosticPhaseName(diagnostic.Phase())) == "fixture" &&
	            diagnostic.FixtureCase() == "private_visibility_matching",
	        "fixture mismatch diagnostic lost its closed name, phase, or safe case identity");
}

void TestFixtureContractAssetsAreByteLocked(const std::string &repository_root) {
	const auto schema = ReadBytes(repository_root + "/src/connector/fixtures/assets/fixture-index-v1.schema.json");
	const auto mapping = ReadBytes(repository_root + "/src/connector/fixtures/assets/fixture-coverage-v1.json");
	Require(cuac::connector::VerifyPackageFixtureContractAssets(),
	        "embedded fixture contract assets failed their content identities");
	Require("sha256." + cuac::ComputeSha256Hex(schema) == cuac::connector::PackageFixtureIndexV1SchemaDigest() &&
	            "sha256." + cuac::ComputeSha256Hex(mapping) == cuac::connector::PackageFixtureCoverageV1MappingDigest(),
	        "production fixture contract asset bytes drifted from their accepted identities");
	Require(schema == ReadBytes(repository_root + "/test/fixtures/contract_authority/fixture-index-v1.schema.json") &&
	            mapping == ReadBytes(repository_root + "/test/fixtures/contract_authority/fixture-coverage-v1.json"),
	        "production fixture assets are not exact copies of checked-in contract authority");
}

cuac::CompiledLocalPackage CompileControlledPackage(const std::string &repository_root) {
	NeverCancel cancellation;
	const auto result = cuac::connector::CompileLocalPackageRoot(
	    repository_root + "/test/fixtures/package_graphql_non_github", cancellation);
	Require(result.Succeeded() && result.Package() != nullptr, "controlled fixture package did not compile");
	return *result.Package();
}

void RequireCompleteCorpusReachesProvider(const cuac::CompiledLocalPackage &package, const std::string &first_case) {
	NeverCancel cancellation;
	FirstCaseProbe execution;
	const auto report = cuac::connector::RunPackageFixtures(package, execution,
	                                                        cuac::connector::PackageFixtureLimits::V1(), cancellation);
	Require(!report.Succeeded() && report.ExecutedCases() == 0 && report.RequiredCoverageKeys().empty() &&
	            report.Diagnostics().size() == 1 &&
	            report.Diagnostics()[0].Code() == cuac::connector::PackageDiagnosticCode::FIXTURE_MISMATCH &&
	            report.Diagnostics()[0].Phase() == cuac::connector::PackageDiagnosticPhase::FIXTURE &&
	            report.Diagnostics()[0].FixtureCase() == first_case && execution.calls == 1,
	        "fixture corpus did not establish schema, exact claims, and exact payload identity before provider entry");
}

void TestCompleteCorporaAndPreProviderBoundaries(const std::string &repository_root) {
	const auto package = cuac_test::CompileRepositoryGithubLocalPackageFixture(repository_root);
	RequireCompleteCorpusReachesProvider(package, "github_search_base");
	RequireCompleteCorpusReachesProvider(CompileControlledPackage(repository_root), "announcements_base");

	auto one_byte = cuac::connector::PackageFixtureLimits::V1();
	one_byte.max_index_bytes = 1;
	NeverCancel cancellation;
	FirstCaseProbe execution;
	const auto exhausted = cuac::connector::RunPackageFixtures(package, execution, one_byte, cancellation);
	Require(!exhausted.Succeeded() && exhausted.Diagnostics().size() == 1 &&
	            exhausted.Diagnostics()[0].Code() == cuac::connector::PackageDiagnosticCode::RESOURCE_EXHAUSTED &&
	            exhausted.Diagnostics()[0].Coordinate().file == "fixtures/index.yaml" && execution.calls == 0,
	        "fixture index boundary did not fail closed before provider execution");

	AlwaysCancel cancelled;
	try {
		(void)cuac::connector::RunPackageFixtures(package, execution, cuac::connector::PackageFixtureLimits::V1(),
		                                          cancelled);
	} catch (const cuac::connector::PackageCompilationCancelled &) {
		Require(execution.calls == 0, "cancelled fixture work reached its provider");
		return;
	}
	throw std::runtime_error("fixture cancellation did not preserve the public cancellation boundary");
}

} // namespace

int main(int argc, char **argv) {
	try {
		Require(argc == 2, "usage: package_fixture_coverage_tests ABSOLUTE_REPOSITORY_ROOT");
		TestFixtureDiagnosticVocabulary();
		TestFixtureContractAssetsAreByteLocked(argv[1]);
		TestGithubCoverageMatchesAcceptedMapping(argv[1]);
		TestCoverageIsCompiledFactDriven(argv[1]);
		TestRetryCoverageUsesTheSameOfflineContract(argv[1]);
		TestRateLimitCoverageIsTypedAndClosed(argv[1]);
		TestCompleteCorporaAndPreProviderBoundaries(argv[1]);
		std::cout << "package fixture coverage tests passed" << std::endl;
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << std::endl;
		return 1;
	}
}
