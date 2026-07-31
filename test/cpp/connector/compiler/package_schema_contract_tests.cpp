#include "compiler_test_support.hpp"

#include <iostream>

namespace {

using cuac::connector::PackageDiagnosticCode;
using cuac::connector::PackageDiagnosticPhase;
using cuac_test::NeverCancel;
using cuac_test::Require;
using cuac_test::TemporaryPackage;

std::string GithubRelation(const std::string &name) {
	return cuac_test::ReadFile("connectors/github/relations/" + name + ".yaml");
}

void WriteArrayPackage(TemporaryPackage &package, const std::string &columns) {
	package.Write("connector.yaml", R"YAML(api_version: cuac/v1
kind: connector
id: array_fixture
version: 1.0.0
extractor_dialect: cuac/json_path_v1
network_policy:
  origins: [{scheme: https, host: arrays.example, port: 443}]
  redirects: deny
  private_addresses: deny
  link_local_addresses: deny
  loopback_addresses: deny
  max_response_bytes: 4096
relations: [records]
)YAML");
	package.Write("relations/records.yaml", "api_version: cuac/v1\nkind: relation\nid: records\n"
	                                        "schema: static\ncolumns:\n" +
	                                            columns +
	                                            R"YAML(auth: {mode: anonymous}
resources:
  max_response_bytes_per_page: 4096
  max_response_bytes_per_scan: 4096
  max_records_per_page: 16
  max_records_per_scan: 16
  max_extracted_string_bytes: 256
operations:
  - id: all_records
    fallback: true
    cardinality: many
    replay_safety: safe
    request:
      protocol: rest
      method: GET
      origin: {scheme: https, host: arrays.example, port: 443}
      path: /records
      query: []
      headers: []
    response: {source: terminal_collection, records: "$.records[*]"}
    pagination: {strategy: disabled}
)YAML");
}

void WriteRateLimitPackage(TemporaryPackage &package, const std::string &rest_relation = std::string()) {
	package.Write("connector.yaml", cuac_test::ReadFile("test/fixtures/package_rate_limit/connector.yaml"));
	package.Write("relations/disabled_events.yaml",
	              cuac_test::ReadFile("test/fixtures/package_rate_limit/relations/disabled_events.yaml"));
	package.Write("relations/fail_events.yaml",
	              cuac_test::ReadFile("test/fixtures/package_rate_limit/relations/fail_events.yaml"));
	package.Write("relations/duplicate_events.yaml",
	              rest_relation.empty()
	                  ? cuac_test::ReadFile("test/fixtures/package_rate_limit/relations/duplicate_events.yaml")
	                  : rest_relation);
	package.Write("relations/duplicate_graphql_events.yaml",
	              cuac_test::ReadFile("test/fixtures/package_rate_limit/relations/duplicate_graphql_events.yaml"));
}

void RequireFirstDiagnostic(const cuac::connector::PackageCompileResult &result, PackageDiagnosticCode code,
                            PackageDiagnosticPhase phase, const std::string &message) {
	Require(!result.Succeeded() && result.Generation() == nullptr && !result.Diagnostics().empty() &&
	            result.Diagnostics()[0].Code() == code && result.Diagnostics()[0].Phase() == phase,
	        message);
}

void TestClosedSchemaAndAllOrNothing() {
	TemporaryPackage package;
	cuac_test::WriteGithubPackage(package);
	package.Write("relations/authenticated_user.yaml",
	              cuac_test::ReplaceOnce(GithubRelation("authenticated_user"), "schema: static\n",
	                                     "schema: static\nfuture_capability: true\n"));
	NeverCancel cancellation;
	const auto result = cuac_test::CompileRoot(package.Root(), cancellation);
	RequireFirstDiagnostic(result, PackageDiagnosticCode::UNKNOWN_FIELD, PackageDiagnosticPhase::SCHEMA,
	                       "unknown stable source field did not fail the complete candidate");
	Require(result.Diagnostics()[0].Coordinate().file == "relations/authenticated_user.yaml" &&
	            result.Diagnostics()[0].Coordinate().yaml_path == "$.future_capability",
	        "closed-schema diagnostic lost its exact safe source coordinate");
}

void TestV1SupportsRetryDeclaration() {
	TemporaryPackage package;
	cuac_test::WriteGithubPackage(package);
	package.Write("relations/authenticated_user.yaml",
	              cuac_test::ReplaceOnce(GithubRelation("authenticated_user"), "    replay_safety: safe\n",
	                                     "    replay_safety: safe\n"
	                                     "    retry:\n"
	                                     "      max_attempts_per_step: 3\n"
	                                     "      max_delay_milliseconds: 10\n"
	                                     "      max_cumulative_waiting_milliseconds_per_scan: 25\n"));
	NeverCancel cancellation;
	const auto result = cuac_test::CompileRoot(package.Root(), cancellation);
	Require(result.Succeeded() && result.Generation() != nullptr, "cuac/v1 rejected its bounded retry declaration");
	const auto *relation = result.Generation()->Connector().FindRelation("authenticated_user");
	Require(relation != nullptr && relation->Operation().RetryRecommendation().Enabled(),
	        "cuac/v1 retry declaration did not reach compiled operation policy");
}

void TestRejectsUnknownSpecIdentifier() {
	TemporaryPackage package;
	cuac_test::WriteGithubPackage(package);
	package.Write("connector.yaml", cuac_test::ReplaceOnce(cuac_test::ReadFile("connectors/github/connector.yaml"),
	                                                       "api_version: cuac/v1", "api_version: cuac/unsupported"));
	NeverCancel cancellation;
	RequireFirstDiagnostic(cuac_test::CompileRoot(package.Root(), cancellation),
	                       PackageDiagnosticCode::UNSUPPORTED_SPEC, PackageDiagnosticPhase::SCHEMA,
	                       "unknown package specification did not fail closed");
}

void TestRateLimitSchemaContract() {
	const auto source = cuac_test::ReadFile("test/fixtures/package_rate_limit/relations/duplicate_events.yaml");
	{
		TemporaryPackage package;
		WriteRateLimitPackage(package);
		NeverCancel cancellation;
		const auto result = cuac_test::CompileRoot(package.Root(), cancellation);
		Require(result.Succeeded() && result.Generation() != nullptr,
		        "accepted cuac/v1 rate-limit fixture did not compile");
	}
	{
		TemporaryPackage package;
		WriteRateLimitPackage(package, cuac_test::ReplaceOnce(source, "operation_family: core_requests",
		                                                      "operation_family: " + std::string(64, 'a')));
		NeverCancel cancellation;
		Require(cuac_test::CompileRoot(package.Root(), cancellation).Succeeded(),
		        "v1 rejected the exact 64-byte operation-family boundary");
	}
	const std::vector<std::pair<std::string, std::string>> mutations = {
	    {"statuses: [503, 429]", "statuses: [503, 401]"},
	    {"statuses: [503, 429]", "statuses: [503, \"429\"]"},
	    {"operation_family: core_requests", "operation_family: " + std::string(65, 'a')},
	    {"header: retry-after", "header: Retry-After"},
	    {"header: retry-after", "header: date"},
	    {"header: x-ratelimit-remaining", "header: retry-after"},
	    {"mode: wait_if_deadline_allows", "mode: fail"},
	    {"max_attempts_per_step: 3", "max_attempts_per_step: 4"},
	    {"max_delay_milliseconds: 30000", "max_delay_milliseconds: 30001"},
	    {"principal_scope: credential_authority", "principal_scope: Credential_Authority"}};
	for (const auto &mutation : mutations) {
		TemporaryPackage package;
		WriteRateLimitPackage(package, cuac_test::ReplaceOnce(source, mutation.first, mutation.second));
		NeverCancel cancellation;
		const auto result = cuac_test::CompileRoot(package.Root(), cancellation);
		Require(!result.Succeeded() && !result.Diagnostics().empty(),
		        "v1 accepted a malformed, contradictory, or differently cased rate-limit fact");
	}
}

void TestRateLimitExampleCompiles() {
	TemporaryPackage package;
	package.Write("connector.yaml", cuac_test::ReadFile("examples/rate-limit/connector.yaml"));
	package.Write("relations/events.yaml", cuac_test::ReadFile("examples/rate-limit/relations/events.yaml"));
	NeverCancel cancellation;
	const auto result = cuac_test::CompileRoot(package.Root(), cancellation);
	Require(result.Succeeded() && result.Generation() != nullptr &&
	            result.Generation()->Identity().SpecIdentifier() == "cuac/v1" &&
	            result.Generation()->Connector().Relations()[0].Operation().RateLimitPolicy().WaitingEnabled(),
	        "documented rate-limit example drifted from the production compiler contract");
}

void TestCrossFileAndPolicyReferences() {
	{
		TemporaryPackage package;
		cuac_test::WriteGithubPackage(package);
		package.Write(
		    "relations/authenticated_user.yaml",
		    cuac_test::ReplaceOnce(GithubRelation("authenticated_user"), "id: authenticated_user", "id: another_user"));
		NeverCancel cancellation;
		RequireFirstDiagnostic(cuac_test::CompileRoot(package.Root(), cancellation),
		                       PackageDiagnosticCode::INVALID_REFERENCE, PackageDiagnosticPhase::REFERENCE,
		                       "relation file identity mismatch escaped cross-file validation");
	}
	{
		TemporaryPackage package;
		cuac_test::WriteGithubPackage(package);
		package.Write("relations/authenticated_user.yaml",
		              cuac_test::ReplaceOnce(GithubRelation("authenticated_user"), "host: api.github.com",
		                                     "host: outside.example"));
		NeverCancel cancellation;
		const auto result = cuac_test::CompileRoot(package.Root(), cancellation);
		Require(!result.Succeeded(), "operation origin widened manifest network authority");
		bool found = false;
		for (const auto &diagnostic : result.Diagnostics()) {
			found = found || (diagnostic.Code() == PackageDiagnosticCode::POLICY_WIDENING &&
			                  diagnostic.Phase() == PackageDiagnosticPhase::COMPILE);
		}
		Require(found, "network widening did not use the stable policy diagnostic");
	}
}

void TestTypedDefaults() {
	TemporaryPackage package;
	package.Write("connector.yaml", R"YAML(api_version: cuac/v1
kind: connector
id: typed_defaults
version: 1.0.0
extractor_dialect: cuac/json_path_v1
network_policy:
  origins:
    - {scheme: https, host: defaults.example, port: 443}
  redirects: deny
  private_addresses: deny
  link_local_addresses: deny
  loopback_addresses: deny
  max_response_bytes: 4096
relations: [records]
)YAML");
	package.Write("relations/records.yaml", R"YAML(api_version: cuac/v1
kind: relation
id: records
schema: static
columns:
  - {id: value, type: VARCHAR, nullable: false, extract: $.value}
inputs:
  - {id: enabled, type: BOOLEAN, nullable: false, default: {kind: value, value: false}}
  - {id: count, type: BIGINT, nullable: false, default: {kind: value, value: -42}}
  - {id: label, type: VARCHAR, nullable: false, default: {kind: value, value: "private"}}
  - {id: cursor, type: VARCHAR, nullable: true, default: {kind: null}}
  - {id: rating, type: DOUBLE, nullable: false, default: {kind: value, value: -0.0}}
auth: {mode: anonymous}
resources:
  max_response_bytes_per_page: 4096
  max_response_bytes_per_scan: 4096
  max_records_per_page: 16
  max_records_per_scan: 16
  max_extracted_string_bytes: 256
operations:
  - id: all_records
    fallback: true
    cardinality: many
    replay_safety: safe
    request:
      protocol: rest
      method: GET
      origin: {scheme: https, host: defaults.example, port: 443}
      path: /records
      query: []
      headers: []
    response: {source: root_array}
    pagination: {strategy: disabled}
)YAML");
	NeverCancel cancellation;
	const auto result = cuac_test::CompileRoot(package.Root(), cancellation);
	Require(result.Succeeded(), "valid typed defaults package did not compile");
	const auto &inputs = result.Generation()->Connector().Relations()[0].Inputs();
	Require(inputs.size() == 5 && !inputs[0].Default().Value().Boolean() &&
	            inputs[1].Default().Value().Bigint() == -42 && inputs[2].Default().Value().Varchar() == "private" &&
	            inputs[3].Nullable() && inputs[3].Default().Value().IsNull() &&
	            inputs[3].Default().Value().Type() == cuac::CompiledScalarType::VARCHAR &&
	            inputs[4].Default().Value().Double() == 0.0,
	        "compiler collapsed typed defaults, typed NULL, or source order");
}

void TestTypedDoubleDefaultsRoundTrip() {
	// RFC 0020: round-trip boundary values a naive fixed-precision encoder could
	// lose: the largest finite magnitude and a value requiring all 17
	// significant digits to reconstruct bit-for-bit.
	TemporaryPackage package;
	package.Write("connector.yaml", R"YAML(api_version: cuac/v1
kind: connector
id: typed_double_defaults
version: 1.0.0
extractor_dialect: cuac/json_path_v1
network_policy:
  origins:
    - {scheme: https, host: defaults.example, port: 443}
  redirects: deny
  private_addresses: deny
  link_local_addresses: deny
  loopback_addresses: deny
  max_response_bytes: 4096
relations: [records]
)YAML");
	package.Write("relations/records.yaml", R"YAML(api_version: cuac/v1
kind: relation
id: records
schema: static
columns:
  - {id: value, type: VARCHAR, nullable: false, extract: $.value}
inputs:
  - {id: maximum, type: DOUBLE, nullable: false, default: {kind: value, value: 1.7976931348623157e+308}}
  - {id: precise, type: DOUBLE, nullable: false, default: {kind: value, value: 0.12345678901234567}}
  - {id: subnormal, type: DOUBLE, nullable: false, default: {kind: value, value: 4.9e-324}}
auth: {mode: anonymous}
resources:
  max_response_bytes_per_page: 4096
  max_response_bytes_per_scan: 4096
  max_records_per_page: 16
  max_records_per_scan: 16
  max_extracted_string_bytes: 256
operations:
  - id: all_records
    fallback: true
    cardinality: many
    replay_safety: safe
    request:
      protocol: rest
      method: GET
      origin: {scheme: https, host: defaults.example, port: 443}
      path: /records
      query: []
      headers: []
    response: {source: root_array}
    pagination: {strategy: disabled}
)YAML");
	NeverCancel cancellation;
	const auto result = cuac_test::CompileRoot(package.Root(), cancellation);
	Require(result.Succeeded(), "valid DOUBLE boundary defaults did not compile");
	const auto &inputs = result.Generation()->Connector().Relations()[0].Inputs();
	Require(inputs.size() == 3 && inputs[0].Default().Value().Double() == 1.7976931348623157e+308 &&
	            inputs[1].Default().Value().Double() == 0.12345678901234567 &&
	            inputs[2].Default().Value().Double() == 4.9e-324,
	        "compiler lost a DOUBLE boundary value's exact bits");
}

void TestDoubleMagnitudeOverflowRejected() {
	TemporaryPackage package;
	package.Write("connector.yaml", R"YAML(api_version: cuac/v1
kind: connector
id: typed_double_overflow
version: 1.0.0
extractor_dialect: cuac/json_path_v1
network_policy:
  origins:
    - {scheme: https, host: defaults.example, port: 443}
  redirects: deny
  private_addresses: deny
  link_local_addresses: deny
  loopback_addresses: deny
  max_response_bytes: 4096
relations: [records]
)YAML");
	package.Write("relations/records.yaml", R"YAML(api_version: cuac/v1
kind: relation
id: records
schema: static
columns:
  - {id: value, type: VARCHAR, nullable: false, extract: $.value}
inputs:
  - {id: overflowed, type: DOUBLE, nullable: false, default: {kind: value, value: 1e400}}
auth: {mode: anonymous}
resources:
  max_response_bytes_per_page: 4096
  max_response_bytes_per_scan: 4096
  max_records_per_page: 16
  max_records_per_scan: 16
  max_extracted_string_bytes: 256
operations:
  - id: all_records
    fallback: true
    cardinality: many
    replay_safety: safe
    request:
      protocol: rest
      method: GET
      origin: {scheme: https, host: defaults.example, port: 443}
      path: /records
      query: []
      headers: []
    response: {source: root_array}
    pagination: {strategy: disabled}
)YAML");
	NeverCancel cancellation;
	const auto result = cuac_test::CompileRoot(package.Root(), cancellation);
	Require(!result.Succeeded() && result.Generation() == nullptr && !result.Diagnostics().empty(),
	        "a JSON-number-shaped DOUBLE literal exceeding any finite magnitude compiled successfully");
}

void TestDiagnosticBudgetAndCancellation() {
	TemporaryPackage package;
	cuac_test::WriteGithubPackage(package);
	package.Write("relations/authenticated_user.yaml",
	              cuac_test::ReplaceOnce(GithubRelation("authenticated_user"), "schema: static\n",
	                                     "schema: static\nunknown_a: true\nunknown_b: true\nunknown_c: true\n"));
	NeverCancel cancellation;
	const auto snapshot =
	    cuac::connector::AcquirePackageSource(package.Root(), cuac::connector::PackageSourceLimits::V1(), cancellation);
	auto limits = cuac::connector::PackageCompilerLimits::V1();
	limits.max_diagnostics = 2;
	const auto result = cuac::connector::CompilePackage(snapshot, limits, cancellation);
	Require(!result.Succeeded() && result.Diagnostics().size() == 2 &&
	            result.Diagnostics()[1].Code() == PackageDiagnosticCode::RESOURCE_EXHAUSTED,
	        "diagnostic budget did not retain one detail plus its terminal resource record");
	cuac_test::AlwaysCancel cancelled;
	try {
		(void)cuac::connector::CompilePackage(snapshot, cuac::connector::PackageCompilerLimits::V1(), cancelled);
	} catch (const cuac::connector::FailsafeYamlError &error) {
		Require(error.Code() == cuac::connector::FailsafeYamlErrorCode::CANCELLED,
		        "compiler cancellation used another error boundary");
		return;
	}
	throw std::runtime_error("compiler ignored call-scoped cancellation");
}

void TestCompiledModelCounterexamplesStayDiagnostics() {
	for (const auto &source :
	     {cuac_test::ReplaceOnce(GithubRelation("authenticated_repositories"), "literal: \"100\"", "literal: \"99\""),
	      cuac_test::ReplaceOnce(GithubRelation("authenticated_repositories"), "first_page: 1",
	                             "first_page: 9223372036854775807"),
	      cuac_test::ReplaceOnce(GithubRelation("authenticated_user"), "max_response_bytes_per_scan: 65536",
	                             "max_response_bytes_per_scan: 65537")}) {
		TemporaryPackage package;
		cuac_test::WriteGithubPackage(package);
		const bool user_relation = source.find("id: authenticated_user") != std::string::npos;
		package.Write(user_relation ? "relations/authenticated_user.yaml" : "relations/authenticated_repositories.yaml",
		              source);
		NeverCancel cancellation;
		const auto result = cuac_test::CompileRoot(package.Root(), cancellation);
		Require(!result.Succeeded() && result.Generation() == nullptr && !result.Diagnostics().empty(),
		        "invalid compiled-model counterexample escaped as a generation or exception");
	}
}

void TestHeaderValueProfile() {
	{
		TemporaryPackage package;
		cuac_test::WriteGithubPackage(package);
		package.Write("relations/authenticated_user.yaml",
		              cuac_test::ReplaceOnce(GithubRelation("authenticated_user"), "value: application/vnd.github+json",
		                                     "value: \"\""));
		NeverCancel cancellation;
		const auto result = cuac_test::CompileRoot(package.Root(), cancellation);
		Require(
		    result.Succeeded() &&
		        result.Generation()->Connector().Relations()[1].Operations()[0].Rest().request.headers[0].value.empty(),
		    "the accepted empty HTTP field-value was rejected or changed");
	}
	{
		TemporaryPackage package;
		cuac_test::WriteGithubPackage(package);
		package.Write("relations/authenticated_user.yaml",
		              cuac_test::ReplaceOnce(GithubRelation("authenticated_user"), "value: application/vnd.github+json",
		                                     "value: \" leading\""));
		NeverCancel cancellation;
		RequireFirstDiagnostic(cuac_test::CompileRoot(package.Root(), cancellation),
		                       PackageDiagnosticCode::UNSUPPORTED_DECLARATION, PackageDiagnosticPhase::SCHEMA,
		                       "leading optional whitespace escaped the HTTP field-value profile");
	}
}

void TestExtractorByteLimit() {
	const auto at_limit = "$." + std::string(1022, 'a');
	const auto one_over = at_limit + "a";
	{
		TemporaryPackage package;
		cuac_test::WriteGithubPackage(package);
		package.Write(
		    "relations/authenticated_user.yaml",
		    cuac_test::ReplaceOnce(GithubRelation("authenticated_user"), "extract: $.id", "extract: " + at_limit));
		NeverCancel cancellation;
		Require(cuac_test::CompileRoot(package.Root(), cancellation).Succeeded(),
		        "the maximum 1024-byte field extractor was rejected");
	}
	{
		TemporaryPackage package;
		cuac_test::WriteGithubPackage(package);
		package.Write(
		    "relations/authenticated_user.yaml",
		    cuac_test::ReplaceOnce(GithubRelation("authenticated_user"), "extract: $.id", "extract: " + one_over));
		NeverCancel cancellation;
		RequireFirstDiagnostic(cuac_test::CompileRoot(package.Root(), cancellation),
		                       PackageDiagnosticCode::INVALID_EXTRACTOR, PackageDiagnosticPhase::SCHEMA,
		                       "a 1025-byte field extractor escaped the v1 byte limit");
	}
}

void TestInvalidIdentifiersStayDiagnosticOnly() {
	TemporaryPackage package;
	cuac_test::WriteGithubPackage(package);
	package.Write("connector.yaml", cuac_test::ReplaceOnce(cuac_test::ReadFile("connectors/github/connector.yaml"),
	                                                       "id: github", "id: \"NOT SAFE!\""));
	package.Write(
	    "relations/authenticated_user.yaml",
	    cuac_test::ReplaceOnce(GithubRelation("authenticated_user"), "id: authenticated_user", "id: another_user"));
	NeverCancel cancellation;
	const auto result = cuac_test::CompileRoot(package.Root(), cancellation);
	Require(!result.Succeeded() && !result.Diagnostics().empty(),
	        "invalid source identifiers escaped as a generation or exception");
	for (const auto &diagnostic : result.Diagnostics()) {
		Require(diagnostic.Connector().empty() || diagnostic.Connector() == "github",
		        "an unvalidated author value entered the safe diagnostic identity");
	}
}

void TestUnsupportedPaginationStrategyRejected() {
	TemporaryPackage package;
	cuac_test::WriteGithubPackage(package);
	package.Write("relations/authenticated_repositories.yaml",
	              cuac_test::ReplaceOnce(GithubRelation("authenticated_repositories"), "strategy: link_next",
	                                     "strategy: body_url_next"));
	NeverCancel cancellation;
	RequireFirstDiagnostic(cuac_test::CompileRoot(package.Root(), cancellation),
	                       PackageDiagnosticCode::UNSUPPORTED_DECLARATION, PackageDiagnosticPhase::SCHEMA,
	                       "a response-body URL pagination strategy escaped the closed v1 pagination set");
}

// RFC 0019: authenticated_repositories.yaml already declares page_size_parameter/
// page_size (link_next's RFC 0017 optionality does not remove them here), so a
// short_page relation reusing this exact shape needs only its strategy changed.
void TestShortPagePaginationCompiles() {
	TemporaryPackage package;
	cuac_test::WriteGithubPackage(package);
	package.Write("relations/authenticated_repositories.yaml",
	              cuac_test::ReplaceOnce(GithubRelation("authenticated_repositories"), "strategy: link_next",
	                                     "strategy: short_page"));
	NeverCancel cancellation;
	const auto result = cuac_test::CompileRoot(package.Root(), cancellation);
	Require(result.Succeeded() && result.Generation() != nullptr && result.Diagnostics().empty(),
	        "a short_page relation with a complete link_next-shaped declaration was rejected");
}

void TestShortPageMissingPageSizeRejected() {
	TemporaryPackage package;
	cuac_test::WriteGithubPackage(package);
	auto source = cuac_test::ReplaceOnce(GithubRelation("authenticated_repositories"), "strategy: link_next",
	                                     "strategy: short_page");
	source = cuac_test::ReplaceOnce(source, "      page_size_parameter: per_page\n      page_size: 100\n", "");
	package.Write("relations/authenticated_repositories.yaml", source);
	NeverCancel cancellation;
	RequireFirstDiagnostic(cuac_test::CompileRoot(package.Root(), cancellation), PackageDiagnosticCode::MISSING_FIELD,
	                       PackageDiagnosticPhase::SCHEMA,
	                       "short_page compiled without a declared page size, unlike link_next's RFC 0017 optionality");
}

void TestShortPagePageSizeNameCollisionRejected() {
	TemporaryPackage package;
	cuac_test::WriteGithubPackage(package);
	auto source = cuac_test::ReplaceOnce(GithubRelation("authenticated_repositories"), "strategy: link_next",
	                                     "strategy: short_page");
	source = cuac_test::ReplaceOnce(source, "page_size_parameter: per_page", "page_size_parameter: page");
	package.Write("relations/authenticated_repositories.yaml", source);
	NeverCancel cancellation;
	const auto result = cuac_test::CompileRoot(package.Root(), cancellation);
	Require(!result.Succeeded() && !result.Diagnostics().empty(),
	        "short_page accepted a page-size parameter colliding with its page-number parameter");
}

void TestShortPageNonPositiveIncrementRejected() {
	TemporaryPackage package;
	cuac_test::WriteGithubPackage(package);
	auto source = cuac_test::ReplaceOnce(GithubRelation("authenticated_repositories"), "strategy: link_next",
	                                     "strategy: short_page");
	source = cuac_test::ReplaceOnce(source, "page_increment: 1", "page_increment: 0");
	package.Write("relations/authenticated_repositories.yaml", source);
	NeverCancel cancellation;
	const auto result = cuac_test::CompileRoot(package.Root(), cancellation);
	Require(!result.Succeeded() && !result.Diagnostics().empty(), "short_page accepted a non-positive page_increment");
}

void TestDiagnosticCodePhaseContract() {
	{
		TemporaryPackage package;
		cuac_test::WriteGithubPackage(package);
		package.Write("relations/authenticated_user.yaml",
		              cuac_test::ReplaceOnce(GithubRelation("authenticated_user"), "port: 443", "port: 0"));
		NeverCancel cancellation;
		RequireFirstDiagnostic(cuac_test::CompileRoot(package.Root(), cancellation),
		                       PackageDiagnosticCode::INVALID_TYPE, PackageDiagnosticPhase::SCHEMA,
		                       "an invalid HTTP port used a policy code or non-schema phase");
	}
	{
		TemporaryPackage package;
		cuac_test::WriteGithubPackage(package);
		package.Write("connector.yaml", cuac_test::ReplaceOnce(cuac_test::ReadFile("connectors/github/connector.yaml"),
		                                                       "redirects: deny", "redirects: allow"));
		NeverCancel cancellation;
		RequireFirstDiagnostic(cuac_test::CompileRoot(package.Root(), cancellation),
		                       PackageDiagnosticCode::POLICY_WIDENING, PackageDiagnosticPhase::COMPILE,
		                       "network widening used a schema phase outside the closed phase vocabulary");
	}
}

void TestArrayColumnSchemaAndGraphqlDoubleGap() {
	const std::string valid_columns =
	    "  - {id: flags, type: ARRAY, element_type: BOOLEAN, element_nullable: true, nullable: false, extract: "
	    "$.flags}\n"
	    "  - {id: ids, type: ARRAY, element_type: BIGINT, element_nullable: false, nullable: false, extract: "
	    "$.ids}\n"
	    "  - {id: names, type: ARRAY, element_type: VARCHAR, element_nullable: false, nullable: true, extract: "
	    "$.names}\n"
	    "  - {id: scores, type: ARRAY, element_type: DOUBLE, element_nullable: false, nullable: false, extract: "
	    "$.scores}\n";
	{
		TemporaryPackage package;
		WriteArrayPackage(package, valid_columns);
		NeverCancel cancellation;
		const auto result = cuac_test::CompileRoot(package.Root(), cancellation);
		Require(result.Succeeded() && result.Generation() &&
		            result.Generation()->Connector().Relations()[0].Columns().size() == 4 &&
		            result.Generation()->Connector().Relations()[0].Columns()[0].Shape() ==
		                cuac::CompiledColumnShape::ARRAY &&
		            result.Generation()->Connector().Relations()[0].Columns()[0].ElementNullable() &&
		            result.Generation()->Connector().Relations()[0].Columns()[3].ElementType() ==
		                cuac::CompiledScalarType::DOUBLE,
		        "REST ARRAY columns did not compile across the four scalar element kinds");
	}
	for (const auto &invalid :
	     {cuac_test::ReplaceOnce(valid_columns, "element_type: BOOLEAN, ", ""),
	      cuac_test::ReplaceOnce(valid_columns, "element_nullable: true, ", ""),
	      cuac_test::ReplaceOnce(valid_columns, "element_type: BOOLEAN", "element_type: ARRAY"),
	      std::string("  - {id: value, type: VARCHAR, element_type: VARCHAR, nullable: false, extract: $.value}\n")}) {
		TemporaryPackage package;
		WriteArrayPackage(package, invalid);
		NeverCancel cancellation;
		RequireFirstDiagnostic(
		    cuac_test::CompileRoot(package.Root(), cancellation),
		    invalid.find("element_type: VARCHAR, nullable") != std::string::npos
		        ? PackageDiagnosticCode::UNKNOWN_FIELD
		        : (invalid.find("element_type: ARRAY") != std::string::npos ? PackageDiagnosticCode::INVALID_TYPE
		                                                                    : PackageDiagnosticCode::MISSING_FIELD),
		    PackageDiagnosticPhase::SCHEMA, "invalid ARRAY structural declaration did not fail at schema validation");
	}
	{
		TemporaryPackage package;
		auto graphql = GithubRelation("viewer_repository_metrics");
		graphql = cuac_test::ReplaceOnce(
		    graphql, "  - id: id\n    type: VARCHAR\n    nullable: false",
		    "  - id: id\n    type: ARRAY\n    element_type: DOUBLE\n    element_nullable: false\n    nullable: false");
		cuac_test::WriteGithubPackage(package, graphql);
		NeverCancel cancellation;
		RequireFirstDiagnostic(cuac_test::CompileRoot(package.Root(), cancellation),
		                       PackageDiagnosticCode::INVALID_GRAPHQL_PROFILE, PackageDiagnosticPhase::COMPILE,
		                       "GraphQL ARRAY<DOUBLE> did not preserve the accepted scalar DOUBLE profile gap");
	}
}

} // namespace

int main() {
	try {
		TestClosedSchemaAndAllOrNothing();
		TestV1SupportsRetryDeclaration();
		TestRejectsUnknownSpecIdentifier();
		TestRateLimitSchemaContract();
		TestRateLimitExampleCompiles();
		TestCrossFileAndPolicyReferences();
		TestTypedDefaults();
		TestTypedDoubleDefaultsRoundTrip();
		TestDoubleMagnitudeOverflowRejected();
		TestDiagnosticBudgetAndCancellation();
		TestCompiledModelCounterexamplesStayDiagnostics();
		TestHeaderValueProfile();
		TestExtractorByteLimit();
		TestInvalidIdentifiersStayDiagnosticOnly();
		TestDiagnosticCodePhaseContract();
		TestArrayColumnSchemaAndGraphqlDoubleGap();
		TestUnsupportedPaginationStrategyRejected();
		TestShortPagePaginationCompiles();
		TestShortPageMissingPageSizeRejected();
		TestShortPagePageSizeNameCollisionRejected();
		TestShortPageNonPositiveIncrementRejected();
		std::cout << "package schema contract tests passed" << std::endl;
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << std::endl;
		return 1;
	}
}
