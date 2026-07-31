#include "compiler_test_support.hpp"

#include <iostream>
#include <sstream>

namespace {

using cuac_test::NeverCancel;
using cuac_test::Require;
using cuac_test::TemporaryPackage;

std::string PredicateRelation(const std::string &relation, const std::string &type, const std::string &literal) {
	std::ostringstream yaml;
	yaml << "api_version: cuac/v1\n"
	        "kind: relation\n"
	     << "id: " << relation
	     << "\n"
	        "schema: static\n"
	        "columns:\n"
	        "  - {id: occurrence_id, type: BIGINT, nullable: false, extract: $.occurrence_id}\n"
	        "  - {id: value, type: "
	     << type
	     << ", nullable: false, extract: $.value}\n"
	        "auth: {mode: anonymous}\n"
	        "resources:\n"
	        "  max_response_bytes_per_page: 4096\n"
	        "  max_response_bytes_per_scan: 4096\n"
	        "  max_records_per_page: 16\n"
	        "  max_records_per_scan: 16\n"
	        "  max_extracted_string_bytes: 256\n"
	        "operations:\n"
	        "  - id: selected\n"
	        "    when: {required_inputs: [conditional.restriction]}\n"
	        "    cardinality: many\n"
	        "    replay_safety: safe\n"
	        "    request:\n"
	        "      protocol: rest\n"
	        "      method: GET\n"
	        "      origin: {scheme: https, host: predicates.example, port: 443}\n"
	     << "      path: /" << relation
	     << "/restricted\n"
	        "      query:\n"
	        "        - {name: value, conditional_input: restriction, encoding: form_urlencoded, "
	        "omit_when_unbound: true}\n"
	        "      headers: []\n"
	        "    response:\n"
	        "      source: terminal_collection\n"
	        "      records: $.records[*]\n"
	        "    pagination: {strategy: disabled}\n"
	        "  - id: fallback\n"
	        "    fallback: true\n"
	        "    cardinality: many\n"
	        "    replay_safety: safe\n"
	        "    request:\n"
	        "      protocol: rest\n"
	        "      method: GET\n"
	        "      origin: {scheme: https, host: predicates.example, port: 443}\n"
	     << "      path: /" << relation
	     << "/all\n"
	        "      query: []\n"
	        "      headers: []\n"
	        "    response:\n"
	        "      source: terminal_collection\n"
	        "      records: $.records[*]\n"
	        "    pagination: {strategy: disabled}\n"
	        "predicates:\n"
	        "  - id: value_equality\n"
	        "    column: value\n"
	        "    operator: eq\n"
	        "    literal: {type: "
	     << type << ", value: " << literal
	     << "}\n"
	        "    conditional_input: restriction\n"
	        "    operations: [selected]\n"
	        "    accuracy: exact\n"
	        "    occurrence_fixtures:\n"
	        "      matching: match\n"
	        "      false_or_null: false_value\n"
	        "      duplicates: duplicate_values\n";
	return yaml.str();
}

void WriteTypedPredicatePackage(TemporaryPackage &package) {
	package.Write("connector.yaml", R"YAML(api_version: cuac/v1
kind: connector
id: typed_predicates
version: 1.0.0
extractor_dialect: cuac/json_path_v1
network_policy:
  origins: [{scheme: https, host: predicates.example, port: 443}]
  redirects: deny
  private_addresses: deny
  link_local_addresses: deny
  loopback_addresses: deny
  max_response_bytes: 4096
relations: [boolean_values, bigint_values, varchar_values]
)YAML");
	package.Write("relations/boolean_values.yaml", PredicateRelation("boolean_values", "BOOLEAN", "true"));
	package.Write("relations/bigint_values.yaml", PredicateRelation("bigint_values", "BIGINT", "42"));
	package.Write("relations/varchar_values.yaml", PredicateRelation("varchar_values", "VARCHAR", "\"\""));
}

void TestTypedPredicateCompilation() {
	TemporaryPackage package;
	WriteTypedPredicatePackage(package);
	NeverCancel cancellation;
	const auto result = cuac_test::CompileRoot(package.Root(), cancellation);
	Require(result.Succeeded() && result.Generation()->Connector().Relations().size() == 3,
	        "independent typed predicate package did not compile");
	const auto &boolean = result.Generation()->Connector().Relations()[0].PredicateMappings()[0];
	const auto &bigint = result.Generation()->Connector().Relations()[1].PredicateMappings()[0];
	const auto &varchar = result.Generation()->Connector().Relations()[2].PredicateMappings()[0];
	Require(boolean.TypedLiteral().Type() == cuac::CompiledScalarType::BOOLEAN && boolean.TypedLiteral().Boolean() &&
	            boolean.EncodedRemoteValue() == "true" &&
	            bigint.TypedLiteral().Type() == cuac::CompiledScalarType::BIGINT &&
	            bigint.TypedLiteral().Bigint() == 42 && bigint.EncodedRemoteValue() == "42" &&
	            varchar.TypedLiteral().Type() == cuac::CompiledScalarType::VARCHAR &&
	            varchar.TypedLiteral().Varchar().empty() && varchar.EncodedRemoteValue().empty(),
	        "compiler collapsed BOOLEAN, BIGINT, or empty VARCHAR predicate values");
	for (const auto &relation : result.Generation()->Connector().Relations()) {
		const auto &mapping = relation.PredicateMappings()[0];
		Require(mapping.ProofIdentity() == cuac::CompiledPredicateProofIdentity::PACKAGE_DECLARED_V1 &&
		            mapping.BaseDomain() == cuac::CompiledPredicateBaseDomain::PACKAGE_DECLARED_OCCURRENCE_DOMAIN &&
		            mapping.ProofIdentityValue().find("proof.sha256.") == 0 &&
		            mapping.BaseDomainValue().find("domain.sha256.") == 0 &&
		            relation.Operations()[0].Rest().response_source == cuac::CompiledResponseSource::JSON_PATH_MANY &&
		            relation.Operations()[0].Rest().records_extractor_segments == std::vector<std::string>({"records"}),
		        "package predicate lost derived identities or structural collection authority");
	}
}

void TestPredicateRequestBindingFailsClosed() {
	TemporaryPackage package;
	WriteTypedPredicatePackage(package);
	const auto invalid = cuac_test::ReplaceOnce(PredicateRelation("boolean_values", "BOOLEAN", "true"),
	                                            "conditional_input: restriction", "conditional_input: another_input");
	package.Write("relations/boolean_values.yaml", invalid);
	NeverCancel cancellation;
	const auto result = cuac_test::CompileRoot(package.Root(), cancellation);
	Require(!result.Succeeded(), "predicate without its exact request binding compiled");
	bool found = false;
	for (const auto &diagnostic : result.Diagnostics()) {
		found = found || diagnostic.Code() == cuac::connector::PackageDiagnosticCode::INVALID_PREDICATE;
	}
	Require(found, "predicate/request mismatch used another diagnostic contract");
}

} // namespace

int main() {
	try {
		TestTypedPredicateCompilation();
		TestPredicateRequestBindingFailsClosed();
		std::cout << "package predicate compiler tests passed" << std::endl;
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << std::endl;
		return 1;
	}
}
