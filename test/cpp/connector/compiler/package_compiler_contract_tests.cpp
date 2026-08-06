#include "compiler_test_support.hpp"

#include <iostream>

namespace {

using cuac_test::NeverCancel;
using cuac_test::Require;
using cuac_test::TemporaryPackage;

void TestGithubPackageCompilesAsOneGeneration() {
	TemporaryPackage package;
	cuac_test::WriteGithubPackage(package);
	NeverCancel cancellation;
	const auto result = cuac_test::CompileRoot(package.Root(), cancellation);
	Require(result.Succeeded() && result.Diagnostics().empty(), "accepted GitHub package did not compile");
	const auto *generation = result.Generation();
	Require(generation != nullptr && generation->Identity().SpecIdentifier() == "cuac/v1" &&
	            generation->Identity().ConnectorId() == "github" &&
	            generation->Identity().PackageVersion() == "1.0.0" &&
	            generation->Identity().PackageDigest() ==
	                "sha256.98925899f3e110a5ed5903dae93f633061ac9800b5b613db3884b02cee65034c",
	        "compiled generation lost stable package identity");
	const auto &connector = generation->Connector();
	Require(connector.Relations().size() == 4 && connector.Relations()[0].Name() == "duckdb_login_search_page" &&
	            connector.Relations()[1].Name() == "authenticated_user" &&
	            connector.Relations()[2].Name() == "authenticated_repositories" &&
	            connector.Relations()[3].Name() == "viewer_repository_metrics",
	        "compiled generation lost manifest relation order or package provenance");
	const auto *predicate = connector.FindRelation("authenticated_repositories");
	Require(predicate != nullptr && predicate->PredicateMappings().size() == 1 &&
	            predicate->PredicateMappings()[0].ProofIdentity() ==
	                cuac::CompiledPredicateProofIdentity::PACKAGE_DECLARED_V1 &&
	            predicate->PredicateMappings()[0].EncodedRemoteValue() == "private",
	        "compiled GitHub package lost its declared predicate binding");
	const auto *graphql = connector.FindRelation("viewer_repository_metrics");
	Require(graphql != nullptr && graphql->Operations().size() == 1 &&
	            graphql->Operation().Graphql().document.size() == 576 &&
	            graphql->Operation().Graphql().document_digest ==
	                "9ce36f2aea8bb0c4047d15adebd549c1fb79fe6c75faa011c955f247ebb09dbf",
	        "structured GitHub package did not reproduce the exact GraphQL golden (bytes=" +
	            std::to_string(graphql == nullptr ? 0 : graphql->Operation().Graphql().document.size()) + ", digest=" +
	            (graphql == nullptr ? std::string("absent") : graphql->Operation().Graphql().document_digest) + ")");
	const auto query = generation->QueryRegistration();
	Require(query.Relations().size() == 4 && query.GenerationHandle().IsValid() &&
	            query.Identity().PackageDigest() == generation->Identity().PackageDigest(),
	        "compiler did not provide the bounded Query registration projection");
}

void TestLocalRootSourceFailuresStayDiagnosticOnly() {
	TemporaryPackage package;
	NeverCancel cancellation;
	const auto result = cuac::connector::CompileLocalPackageRoot(package.Root() + "/missing", cancellation);
	Require(!result.Succeeded() && result.Generation() == nullptr && result.Diagnostics().size() == 1 &&
	            result.Diagnostics()[0].Code() == cuac::connector::PackageDiagnosticCode::PACKAGE_IDENTITY &&
	            result.Diagnostics()[0].Phase() == cuac::connector::PackageDiagnosticPhase::SOURCE &&
	            result.Diagnostics()[0].Coordinate().file.empty(),
	        "production local-root compiler leaked or threw a source-custody failure");
}

void TestSchemaAssetIdentity() {
	Require(std::string(cuac::connector::ConnectorPackageV1SchemaDigest()) ==
	                "sha256.ee4a724ea1d98654f569f20c12c619ab4cd439559166474eadd477bcdccd60a0" &&
	            cuac::connector::VerifyConnectorPackageV1SchemaAsset(),
	        "permanent connector schema asset drifted");
}

} // namespace

int main() {
	try {
		TestSchemaAssetIdentity();
		TestGithubPackageCompilesAsOneGeneration();
		TestLocalRootSourceFailuresStayDiagnosticOnly();
		std::cout << "package compiler contract tests passed" << std::endl;
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << std::endl;
		return 1;
	}
}
