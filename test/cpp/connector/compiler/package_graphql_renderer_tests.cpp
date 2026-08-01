#include "compiler_test_support.hpp"

#include "cuac/connector/content_digest.hpp"
#include "cuac/internal/connector/model/graphql_operation_declaration.hpp"
#include "cuac/internal/connector/model/graphql_query_recipe.hpp"

#include <iostream>
#include <stdexcept>

namespace {

using cuac_test::NeverCancel;
using cuac_test::Require;
using cuac_test::TemporaryPackage;

std::string GraphqlSource() {
	return cuac_test::ReadFile("connectors/github/relations/viewer_repository_metrics.yaml");
}

cuac::CompiledGraphqlOperation CompileGraphql(const std::string &source, TemporaryPackage &package) {
	cuac_test::WriteGithubPackage(package, source);
	NeverCancel cancellation;
	const auto result = cuac_test::CompileRoot(package.Root(), cancellation);
	if (!result.Succeeded()) {
		throw std::runtime_error("structured GraphQL test package did not compile");
	}
	const auto *relation = result.Generation()->Connector().FindRelation("viewer_repository_metrics");
	if (relation == nullptr) {
		throw std::runtime_error("structured GraphQL test relation disappeared");
	}
	return relation->Operation().Graphql();
}

void TestExactGithubGolden() {
	TemporaryPackage package;
	const auto &operation = CompileGraphql(GraphqlSource(), package);
	const auto &recipe = operation.QueryRecipe();
	Require(operation.document.size() == 576 &&
	            operation.document_digest == "9ce36f2aea8bb0c4047d15adebd549c1fb79fe6c75faa011c955f247ebb09dbf" &&
	            recipe.Identity() == cuac::CompiledGraphqlDocumentIdentity::PACKAGE_QUERY_GENERATOR_V1 &&
	            recipe.OperationName() == "CuacViewerRepositoryMetrics" &&
	            recipe.RootPath() == std::vector<std::string>({"viewer", "repositories"}) &&
	            recipe.Variables().size() == 2 && recipe.Variables()[0].Name() == "pageSize" &&
	            recipe.Variables()[0].Role() == cuac::CompiledGraphqlRecipeVariableRole::PAGE_SIZE &&
	            recipe.Variables()[1].Name() == "cursor" &&
	            recipe.Variables()[1].Role() == cuac::CompiledGraphqlRecipeVariableRole::CURSOR &&
	            recipe.FixedArguments().size() == 3 &&
	            recipe.FixedArguments()[0].Value().Kind() == cuac::CompiledGraphqlLiteralKind::LIST &&
	            recipe.FixedArguments()[2].Value().Kind() == cuac::CompiledGraphqlLiteralKind::OBJECT &&
	            recipe.Selections().size() == 8 &&
	            cuac::internal::RenderCompiledGraphqlQueryRecipe(recipe) == operation.document,
	        "renderer did not reproduce the exact 576-byte GitHub query golden");
}

void TestIndependentStructuredQuery() {
	auto source = cuac_test::ReplaceOnce(GraphqlSource(), "CuacViewerRepositoryMetrics", "IndependentAccountProjects");
	source = cuac_test::ReplaceOnce(std::move(source), "root: [viewer, repositories]", "root: [account, projects]");
	source = cuac_test::ReplaceOnce(std::move(source), "path: /graphql", "path: /alternate/graphql");
	TemporaryPackage package;
	const auto &operation = CompileGraphql(source, package);
	Require(operation.document_digest != "9ce36f2aea8bb0c4047d15adebd549c1fb79fe6c75faa011c955f247ebb09dbf" &&
	            operation.document.find("query IndependentAccountProjects") == 0 &&
	            operation.document.find("  account {\n    projects(") != std::string::npos &&
	            operation.response.nodes.segments ==
	                std::vector<std::string>({"data", "account", "projects", "nodes"}) &&
	            operation.endpoint_path == "/alternate/graphql",
	        "renderer was hard-coded to the repository GitHub query identity");
}

void TestCompleteLiteralGrammarAndStringKeywords() {
	auto source = cuac_test::ReplaceOnce(
	    GraphqlSource(), "                - name: direction\n                  value: {enum: DESC}\n",
	    "                - name: direction\n"
	    "                  value: {enum: DESC}\n"
	    "          - name: literalCoverage\n"
	    "            value:\n"
	    "              object:\n"
	    "                - {name: nullValue, value: {null: true}}\n"
	    "                - {name: booleanValue, value: {boolean: true}}\n"
	    "                - {name: integerValue, value: {integer: -42}}\n"
	    "                - {name: stringValue, value: {string: \"fragment mutation subscription __safe\"}}\n"
	    "                - name: listValue\n"
	    "                  value:\n"
	    "                    list:\n"
	    "                      - {boolean: false}\n"
	    "                      - {enum: ACTIVE}\n");
	TemporaryPackage package;
	const auto &operation = CompileGraphql(source, package);
	Require(operation.document.find(
	            "literalCoverage: {nullValue: null, booleanValue: true, integerValue: -42, stringValue: \"fragment "
	            "mutation subscription __safe\", listValue: [false, ACTIVE]}") != std::string::npos,
	        "renderer rejected or changed a value in the closed fixed-literal grammar");
}

void TestDocumentAndDigestCannotReplaceRecipeMembership() {
	TemporaryPackage package;
	auto operation = CompileGraphql(GraphqlSource(), package);
	operation.document += " ";
	operation.document_digest = cuac::ComputeSha256Hex(operation.document);
	bool rejected = false;
	try {
		cuac::internal::ValidateGraphqlOperationValue(operation);
	} catch (const std::invalid_argument &) {
		rejected = true;
	}
	Require(rejected, "changed GraphQL bytes plus their recomputed digest replaced recipe membership");
}

void TestProfileCounterexamples() {
	for (const auto &source :
	     {cuac_test::ReplaceOnce(GraphqlSource(), "field_path: [nameWithOwner]", "field_path: [owner, name]"),
	      cuac_test::ReplaceOnce(GraphqlSource(), "page_size_variable: pageSize", "page_size_variable: cursor"),
	      cuac_test::ReplaceOnce(GraphqlSource(), "max_document_bytes: 4096", "max_document_bytes: 128")}) {
		TemporaryPackage package;
		cuac_test::WriteGithubPackage(package, source);
		NeverCancel cancellation;
		const auto result = cuac_test::CompileRoot(package.Root(), cancellation);
		Require(!result.Succeeded(), "invalid structured GraphQL profile compiled");
		bool found = false;
		for (const auto &diagnostic : result.Diagnostics()) {
			found = found || diagnostic.Code() == cuac::connector::PackageDiagnosticCode::INVALID_GRAPHQL_PROFILE;
		}
		Require(found, "GraphQL profile counterexample used another diagnostic contract");
	}
}

} // namespace

int main() {
	try {
		TestExactGithubGolden();
		TestIndependentStructuredQuery();
		TestCompleteLiteralGrammarAndStringKeywords();
		TestDocumentAndDigestCannotReplaceRecipeMembership();
		TestProfileCounterexamples();
		std::cout << "package GraphQL renderer tests passed" << std::endl;
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << std::endl;
		return 1;
	}
}
