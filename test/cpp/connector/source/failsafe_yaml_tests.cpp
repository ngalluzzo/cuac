#include "cuac/internal/connector/source/failsafe_yaml.hpp"

#include "connector/compiler/test_support.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

using cuac::connector::FailsafeYamlBudget;
using cuac::connector::FailsafeYamlError;
using cuac::connector::FailsafeYamlErrorCode;
using cuac::connector::FailsafeYamlLimits;
using cuac::connector::FailsafeYamlNode;
using cuac::connector::ParseFailsafeYaml;
using cuac_test::NeverCancel;
using cuac_test::ReadFile;
using cuac_test::Require;

FailsafeYamlNode Parse(const std::string &source, FailsafeYamlLimits limits = FailsafeYamlLimits::V1()) {
	NeverCancel cancellation;
	FailsafeYamlBudget budget(limits);
	return ParseFailsafeYaml("fixture.yaml", source, budget, cancellation);
}

void RequireFailure(const std::string &source, FailsafeYamlErrorCode expected,
                    FailsafeYamlLimits limits = FailsafeYamlLimits::V1()) {
	try {
		(void)Parse(source, limits);
		throw std::runtime_error("invalid YAML was accepted");
	} catch (const FailsafeYamlError &error) {
		Require(error.Code() == expected, "YAML failed with the wrong typed category");
		Require(error.File() == "fixture.yaml", "YAML failure lost its safe relative source");
		Require(error.Span().begin.line > 0 && error.Span().begin.column > 0, "YAML failure lost its source span");
	}
}

void TestAcceptedGithubSemanticSources() {
	const std::vector<std::string> files = {"connectors/github/connector.yaml",
	                                        "connectors/github/relations/authenticated_repositories.yaml",
	                                        "connectors/github/relations/authenticated_user.yaml",
	                                        "connectors/github/relations/duckdb_login_search_page.yaml",
	                                        "connectors/github/relations/viewer_repository_metrics.yaml"};
	NeverCancel cancellation;
	FailsafeYamlBudget budget(FailsafeYamlLimits::V1());
	for (const auto &file : files) {
		FailsafeYamlNode root;
		try {
			root = ParseFailsafeYaml(file.substr(file.find("github/")), ReadFile(file), budget, cancellation);
		} catch (const FailsafeYamlError &error) {
			throw std::runtime_error(file + ":" + std::to_string(error.Span().begin.line) + ":" +
			                         std::to_string(error.Span().begin.column) + ": " + error.what());
		}
		Require(root.Type() == FailsafeYamlNode::Kind::MAPPING, "accepted GitHub source did not parse as a mapping");
		const auto *api_version = root.Find("api_version");
		Require(api_version && api_version->Scalar() == "cuac/v1",
		        "accepted GitHub source lost its exact spec identity");
		Require(api_version->Span().begin.line == 1 && api_version->Span().begin.column == 14,
		        "accepted GitHub source has an incorrect scalar span");
	}
}

void TestFlowAndFailsafeScalarBehavior() {
	const auto root = Parse("values: [true, +443, 01, {kind: null, text: \"A\\n\\uD83D\\uDE00\"}]\n");
	const auto *values = root.Find("values");
	Require(values && values->Type() == FailsafeYamlNode::Kind::SEQUENCE && values->Size() == 4,
	        "flow sequence was not retained structurally");
	Require(values->SequenceValue(0).Scalar() == "true" && values->SequenceValue(1).Scalar() == "+443" &&
	            values->SequenceValue(2).Scalar() == "01",
	        "failsafe plain scalars underwent implicit typing");
	Require(values->SequenceValue(0).Style() == FailsafeYamlNode::ScalarStyle::PLAIN,
	        "failsafe YAML did not retain plain scalar spelling");
	const auto &mapping = values->SequenceValue(3);
	Require(mapping.Find("kind")->Scalar() == "null", "plain null did not remain text");
	Require(mapping.Find("text")->Scalar() == std::string("A\n\xf0\x9f\x98\x80"),
	        "JSON escapes were not decoded as UTF-8");
	Require(mapping.Find("text")->Style() == FailsafeYamlNode::ScalarStyle::DOUBLE_QUOTED,
	        "failsafe YAML did not retain double-quoted scalar spelling");
}

void TestForbiddenSyntaxAndEncoding() {
	RequireFailure(std::string("\xef\xbb\xbf") + "a: b\n", FailsafeYamlErrorCode::INVALID_ENCODING);
	RequireFailure("a: b\r\n", FailsafeYamlErrorCode::INVALID_ENCODING);
	RequireFailure("a:\n\tb: c\n", FailsafeYamlErrorCode::FORBIDDEN_SYNTAX);
	RequireFailure("a: !tag b\n", FailsafeYamlErrorCode::FORBIDDEN_SYNTAX);
	RequireFailure("a: 'b'\n", FailsafeYamlErrorCode::FORBIDDEN_SYNTAX);
	RequireFailure("a: |\n  b\n", FailsafeYamlErrorCode::FORBIDDEN_SYNTAX);
	RequireFailure("a: >\n  b\n", FailsafeYamlErrorCode::FORBIDDEN_SYNTAX);
	RequireFailure("a: &anchor b\n", FailsafeYamlErrorCode::FORBIDDEN_SYNTAX);
	RequireFailure("a: *anchor\n", FailsafeYamlErrorCode::FORBIDDEN_SYNTAX);
	RequireFailure("base: x\n<<: y\n", FailsafeYamlErrorCode::FORBIDDEN_SYNTAX);
	RequireFailure("---\na: b\n", FailsafeYamlErrorCode::FORBIDDEN_SYNTAX);
	RequireFailure("a: b\n---\nc: d\n", FailsafeYamlErrorCode::FORBIDDEN_SYNTAX);
	RequireFailure("a: b\na: c\n", FailsafeYamlErrorCode::DUPLICATE_KEY);
	RequireFailure("a: {b: c, b: d}\n", FailsafeYamlErrorCode::DUPLICATE_KEY);
	RequireFailure("a: \"\\x41\"\n", FailsafeYamlErrorCode::FORBIDDEN_SYNTAX);
	RequireFailure(std::string("a: \xc0\xaf\n", 6), FailsafeYamlErrorCode::INVALID_ENCODING);
}

void TestBoundaryAndOneOverBudgets() {
	FailsafeYamlLimits node_limit = {32, 3, 1024, 4096};
	(void)Parse("a: b\n", node_limit);
	node_limit.max_nodes = 2;
	RequireFailure("a: b\n", FailsafeYamlErrorCode::RESOURCE_EXHAUSTED, node_limit);

	FailsafeYamlLimits scalar_limit = {32, 100, 3, 4096};
	(void)Parse("a: abc\n", scalar_limit);
	scalar_limit.max_scalar_bytes = 2;
	RequireFailure("a: abc\n", FailsafeYamlErrorCode::RESOURCE_EXHAUSTED, scalar_limit);

	FailsafeYamlLimits entry_limit = {32, 100, 1024, 2};
	(void)Parse("a: b\nc: d\n", entry_limit);
	RequireFailure("a: b\nc: d\ne: f\n", FailsafeYamlErrorCode::RESOURCE_EXHAUSTED, entry_limit);

	FailsafeYamlLimits depth_limit = {3, 100, 1024, 4096};
	(void)Parse("a: [[b]]\n", depth_limit);
	depth_limit.max_depth = 2;
	RequireFailure("a: [[b]]\n", FailsafeYamlErrorCode::RESOURCE_EXHAUSTED, depth_limit);

	NeverCancel cancellation;
	FailsafeYamlLimits aggregate_limits = {32, 6, 1024, 4096};
	FailsafeYamlBudget aggregate_budget(aggregate_limits);
	(void)ParseFailsafeYaml("first.yaml", "a: b\n", aggregate_budget, cancellation);
	(void)ParseFailsafeYaml("second.yaml", "c: d\n", aggregate_budget, cancellation);
	Require(aggregate_budget.NodesConsumed() == 6, "YAML node budget was not shared across semantic documents");
	try {
		(void)ParseFailsafeYaml("third.yaml", "e: f\n", aggregate_budget, cancellation);
		throw std::runtime_error("aggregate YAML node one-over was accepted");
	} catch (const FailsafeYamlError &error) {
		Require(error.Code() == FailsafeYamlErrorCode::RESOURCE_EXHAUSTED,
		        "aggregate YAML node one-over used the wrong failure category");
	}
}

void TestCancellation() {
	cuac_test::AlwaysCancel cancellation;
	FailsafeYamlBudget budget(FailsafeYamlLimits::V1());
	try {
		(void)ParseFailsafeYaml("fixture.yaml", "a: b\n", budget, cancellation);
		throw std::runtime_error("cancelled YAML parse succeeded");
	} catch (const FailsafeYamlError &error) {
		Require(error.Code() == FailsafeYamlErrorCode::CANCELLED, "YAML cancellation used the wrong error category");
	}
}

} // namespace

int main() {
	try {
		TestAcceptedGithubSemanticSources();
		TestFlowAndFailsafeScalarBehavior();
		TestForbiddenSyntaxAndEncoding();
		TestBoundaryAndOneOverBudgets();
		TestCancellation();
		std::cout << "failsafe YAML tests passed" << std::endl;
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "failsafe YAML tests failed: " << error.what() << std::endl;
		return 1;
	}
}
