#include "semantics/support/scan_plan_test_fixture_test_support.hpp"

#include "support/require.hpp"

#include <fstream>
#include <string>
#include <vector>

#ifndef CUAC_SOURCE_ROOT
#define CUAC_SOURCE_ROOT "."
#endif

namespace cuac_test {
namespace scan_plan_fixture_contract {

namespace {

std::string ReadText(const std::string &path) {
	std::ifstream input(path.c_str(), std::ios::binary);
	Require(input.good(), "could not read fixture boundary source: " + path);
	return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

} // namespace

void RequireCanaryAbsent(const cuac::ScanPlan &plan, const std::string &canary) {
	Require(plan.ConnectorName().find(canary) == std::string::npos &&
	            plan.ConnectorVersion().find(canary) == std::string::npos &&
	            plan.RelationName().find(canary) == std::string::npos &&
	            plan.SourceSnapshot().find(canary) == std::string::npos &&
	            plan.ClassificationReason().find(canary) == std::string::npos &&
	            plan.Operation().Rest().operation_name.find(canary) == std::string::npos &&
	            plan.Operation().Rest().origin.host.find(canary) == std::string::npos &&
	            plan.Operation().Rest().path.find(canary) == std::string::npos &&
	            plan.Operation().Rest().records_extractor.find(canary) == std::string::npos &&
	            plan.AuthenticationObligation().LogicalCredential().find(canary) == std::string::npos,
	        "runtime-built credential canary entered scalar fixture plan state");
	if (plan.SecretReference().IsPresent()) {
		Require(plan.SecretReference().Name().find(canary) == std::string::npos,
		        "runtime-built credential canary entered fixture logical reference");
	}
	if (plan.AuthenticationObligation().Destination() != nullptr) {
		Require(plan.AuthenticationObligation().Destination()->host.find(canary) == std::string::npos,
		        "runtime-built credential canary entered fixture authorization destination");
	}
	for (const auto &query : plan.Operation().Rest().query_parameters) {
		Require(query.name.find(canary) == std::string::npos && query.encoded_value.find(canary) == std::string::npos,
		        "runtime-built credential canary entered fixture query fields");
	}
	for (const auto &binding : plan.Operation().Rest().query_bindings) {
		Require(binding.Name().find(canary) == std::string::npos &&
		            binding.SourceId().find(canary) == std::string::npos &&
		            binding.EncodedValue().find(canary) == std::string::npos,
		        "runtime-built credential canary entered typed REST query fields");
		if (binding.Kind() == cuac::PlannedRestScalarKind::VARCHAR) {
			Require(binding.VarcharValue().find(canary) == std::string::npos,
			        "runtime-built credential canary entered decoded REST query payload");
		}
	}
	for (const auto &segment : plan.Operation().Rest().records_path.segments) {
		Require(segment.find(canary) == std::string::npos,
		        "runtime-built credential canary entered structural REST records path");
	}
	for (const auto &column : plan.Operation().Rest().result_columns) {
		Require(column.name.find(canary) == std::string::npos,
		        "runtime-built credential canary entered structural REST result schema");
		for (const auto &segment : column.response_path.segments) {
			Require(segment.find(canary) == std::string::npos,
			        "runtime-built credential canary entered structural REST result path");
		}
	}
	for (const auto &header : plan.Operation().Rest().headers) {
		Require(header.name.find(canary) == std::string::npos && header.value.find(canary) == std::string::npos,
		        "runtime-built credential canary entered fixture fixed headers");
	}
	for (const auto &column : plan.OutputColumns()) {
		Require(column.name.find(canary) == std::string::npos &&
		            column.logical_type.find(canary) == std::string::npos &&
		            column.extractor.find(canary) == std::string::npos,
		        "runtime-built credential canary entered fixture schema");
	}
	for (const auto &scheme : plan.Network().allowed_schemes) {
		Require(scheme.find(canary) == std::string::npos,
		        "runtime-built credential canary entered fixture network schemes");
	}
	for (const auto &host : plan.Network().allowed_hosts) {
		Require(host.find(canary) == std::string::npos,
		        "runtime-built credential canary entered fixture network hosts");
	}
}

void TestSafeConsumerHeaderBoundary() {
	const std::string root = CUAC_SOURCE_ROOT;
	const auto header = ReadText(root + "/test/cpp/semantics/support/scan_plan_test_fixtures.hpp");
	const auto consumer = ReadText(root + "/test/cpp/semantics/support/scan_plan_fixture_consumer_probe.cpp");
	const auto plan_header = ReadText(root + "/src/include/cuac/semantics/scan_plan.hpp");
	const auto runtime_targets = ReadText(root + "/src/runtime/targets.cmake");
	const auto runtime_test_targets = ReadText(root + "/test/cpp/runtime/targets.cmake");
	const auto semantics_targets = ReadText(root + "/test/cpp/semantics/targets.cmake");
	const std::vector<std::string> forbidden = {"scan_plan_test_access",
	                                            "connector_catalog_test_access",
	                                            "duckdb/main",
	                                            "duckdb_secret",
	                                            "SecretManager",
	                                            "ClientContext",
	                                            "authorized_secret",
	                                            "http_scan_executor",
	                                            "curl"};
	for (const auto &value : forbidden) {
		Require(header.find(value) == std::string::npos,
		        "safe fixture consumer header leaked a forbidden dependency: " + value);
		Require(consumer.find(value) == std::string::npos,
		        "safe fixture consumer probe leaked a forbidden dependency: " + value);
	}
	Require(header.find("friend ") == std::string::npos &&
	            header.find("#include \"cuac/semantics/scan_plan.hpp\"") != std::string::npos,
	        "safe fixture header exposed construction authority or omitted the public plan API");
	for (const auto &value : {"cuac/connector", "cuac/query", "LogicalSecretReference", "BuildConservativeScanPlan"}) {
		Require(plan_header.find(value) == std::string::npos,
		        "ScanPlan consumer header leaked a planner input dependency: " + std::string(value));
	}
	for (const auto &value : {"cuac_connector", "cuac_query_request", "cuac_relational_planning_service"}) {
		Require(runtime_targets.find(value) == std::string::npos,
		        "Runtime production target linked a planner input or construction service: " + std::string(value));
		Require(runtime_test_targets.find(value) == std::string::npos,
		        "Runtime test target linked a planner input or construction service: " + std::string(value));
	}
	const auto fixture_target = semantics_targets.find("cuac_semantics_fixture_service STATIC");
	const auto fixture_target_end = semantics_targets.find("add_executable", fixture_target);
	Require(fixture_target != std::string::npos && fixture_target_end != std::string::npos,
	        "Semantics fixture provider target was not inspectable");
	const auto fixture_block = semantics_targets.substr(fixture_target, fixture_target_end - fixture_target);
	for (const auto &value : {"cuac_connector", "cuac_query_request", "cuac_relational_planning_service"}) {
		Require(fixture_block.find(value) == std::string::npos,
		        "Semantics plan-only fixture provider imported a construction service: " + std::string(value));
	}
	Require(fixture_block.find("cuac_scan_plan_service") != std::string::npos,
	        "Semantics plan-only fixture provider omitted the immutable ScanPlan service");
	const auto first_include = consumer.find("#include");
	Require(first_include != std::string::npos &&
	            consumer.find("#include", first_include + std::string("#include").size()) == std::string::npos &&
	            consumer.find("semantics/support/scan_plan_test_fixtures.hpp") != std::string::npos,
	        "consumer probe included more than the safe fixture header");
}

} // namespace scan_plan_fixture_contract
} // namespace cuac_test
