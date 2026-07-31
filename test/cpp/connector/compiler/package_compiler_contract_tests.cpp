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
	                "sha256.42f95003eb789235c7c911e3f2ed2a8395373a742c477ab016a0d1e77231920c",
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
	            graphql->Operation().Graphql().document.size() == 581 &&
	            graphql->Operation().Graphql().document_digest ==
	                "9d3d78e2214669f11b9caabc2a7f062e2985f9da9628485f124e1f24e3a50c85",
	        "structured GitHub package did not reproduce the exact GraphQL golden");
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
	                "sha256.852d3635b590b0f8bf610b34e9dd3b324c70f4c500ea38886a44383bfa0c2aaf" &&
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
