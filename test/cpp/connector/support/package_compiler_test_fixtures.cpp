#include "connector/support/package_compiler_test_fixtures.hpp"

#include "connector/support/catalog_test_access.hpp"

#include "cuac/connector/content_digest.hpp"
#include "cuac/connector/local_package_compiler.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <ftw.h>
#include <mutex>
#include <sstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace cuac_test {

namespace {

class NeverCancel final : public cuac::connector::PackageCancellation {
public:
	bool IsCancellationRequested() const noexcept override {
		return false;
	}
};

int RemoveEntry(const char *path, const struct stat *, int, struct FTW *) {
	return ::remove(path);
}

class FixtureRootCustody {
public:
	~FixtureRootCustody() noexcept {
		for (const auto &root : roots) {
			(void)::nftw(root.c_str(), RemoveEntry, 32, FTW_DEPTH | FTW_PHYS);
		}
	}

	void Retain(std::string root) {
		std::lock_guard<std::mutex> guard(lock);
		roots.push_back(std::move(root));
	}

private:
	std::mutex lock;
	std::vector<std::string> roots;
};

FixtureRootCustody &RetainedFixtureRoots() {
	static FixtureRootCustody custody;
	return custody;
}

std::string ReadFile(const std::string &path) {
	std::ifstream input(path.c_str(), std::ios::binary);
	if (!input) {
		throw std::runtime_error("could not read repository local-package fixture source");
	}
	std::ostringstream result;
	result << input.rdbuf();
	return result.str();
}

void WriteFile(const std::string &path, const std::string &bytes) {
	const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) {
		throw std::runtime_error("could not open repository local-package fixture output");
	}
	std::size_t offset = 0;
	while (offset < bytes.size()) {
		const auto written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
		if (written < 0) {
			const int saved = errno;
			::close(fd);
			errno = saved;
			throw std::runtime_error("could not write repository local-package fixture output");
		}
		offset += static_cast<std::size_t>(written);
	}
	if (::close(fd) != 0) {
		throw std::runtime_error("could not close repository local-package fixture output");
	}
}

std::string WithDistinctConnectorId(std::string manifest) {
	const std::string anchor = "\nid: github\n";
	const auto offset = manifest.find(anchor);
	if (offset == std::string::npos) {
		throw std::runtime_error("repository package connector-id anchor is missing");
	}
	manifest.replace(offset, anchor.size(), "\nid: github_distinct\n");
	return manifest;
}

void FillHeadersToCombinedBytes(std::vector<cuac::CompiledHttpHeader> &headers, std::size_t target) {
	std::size_t bytes = 0;
	for (const auto &header : headers) {
		bytes += header.name.size() + header.value.size();
	}
	for (std::size_t index = 0; bytes < target; index++) {
		const auto name = "X-Budget-" + std::to_string(index);
		const auto remaining = target - bytes;
		if (headers.size() == 32 || remaining < name.size()) {
			throw std::runtime_error("header-byte fixture cannot reach its exact target");
		}
		const auto value_size = std::min<std::size_t>(1024, remaining - name.size());
		headers.push_back({name, std::string(value_size, 'a')});
		bytes += name.size() + value_size;
	}
}

} // namespace

cuac::CompiledQueryRegistrationView
CompileRepositoryGithubRegistrationFixture(const std::string &absolute_repository_root) {
	return CompileRepositoryGithubLocalPackageFixture(absolute_repository_root).Generation().QueryRegistration();
}

cuac::CompiledLocalPackage CompileRepositoryGithubLocalPackageFixture(const std::string &absolute_repository_root) {
	NeverCancel cancellation;
	const auto result =
	    cuac::connector::CompileLocalPackageRoot(absolute_repository_root + "/connectors/github", cancellation);
	if (!result.Succeeded() || result.Package() == nullptr) {
		throw std::runtime_error("repository GitHub connector package fixture did not compile");
	}
	return cuac::CompiledLocalPackage(*result.Package());
}

cuac::CompiledQueryRegistrationView
CompileRepositoryRickAndMortyRegistrationFixture(const std::string &absolute_repository_root) {
	return CompileRepositoryRickAndMortyLocalPackageFixture(absolute_repository_root).Generation().QueryRegistration();
}

cuac::CompiledLocalPackage
CompileRepositoryRickAndMortyLocalPackageFixture(const std::string &absolute_repository_root) {
	NeverCancel cancellation;
	const auto result =
	    cuac::connector::CompileLocalPackageRoot(absolute_repository_root + "/connectors/rickandmorty", cancellation);
	if (!result.Succeeded() || result.Package() == nullptr) {
		throw std::runtime_error("repository Rick and Morty connector package fixture did not compile");
	}
	return cuac::CompiledLocalPackage(*result.Package());
}

cuac::CompiledPackageGeneration CompileRetryGenerationFixture(const std::string &absolute_repository_root) {
	NeverCancel cancellation;
	const auto result = cuac::connector::CompileLocalPackageRoot(
	    absolute_repository_root + "/test/fixtures/package_retry", cancellation);
	if (!result.Succeeded() || result.Package() == nullptr) {
		throw std::runtime_error("repository retry connector package fixture did not compile");
	}
	return result.Package()->Generation();
}

cuac::CompiledPackageGeneration CompileRateLimitGenerationFixture(const std::string &absolute_repository_root) {
	NeverCancel cancellation;
	const auto result = cuac::connector::CompileLocalPackageRoot(
	    absolute_repository_root + "/test/fixtures/package_rate_limit", cancellation);
	if (!result.Succeeded() || result.Package() == nullptr) {
		throw std::runtime_error("repository rate-limit connector package fixture did not compile");
	}
	return result.Package()->Generation();
}

cuac::CompiledPackageGeneration CompileStructuralPathGenerationFixture(const std::string &absolute_repository_root,
                                                                       StructuralPathProvider provider) {
	const char *relative = nullptr;
	switch (provider) {
	case StructuralPathProvider::GITHUB:
		relative = "/test/fixtures/package_rest_structural_path_github";
		break;
	case StructuralPathProvider::GITLAB:
		relative = "/test/fixtures/package_rest_structural_path_gitlab";
		break;
	default:
		throw std::invalid_argument("unknown structural path provider fixture");
	}
	NeverCancel cancellation;
	const auto result = cuac::connector::CompileLocalPackageRoot(absolute_repository_root + relative, cancellation);
	if (!result.Succeeded() || result.Package() == nullptr) {
		throw std::runtime_error("repository structural-path package fixture did not compile");
	}
	return result.Package()->Generation();
}

// The canonical package-independence relation. Every field is identical across
// both package envelopes except the operation origin host, which must track
// each envelope's network policy. It deliberately exercises the v1 mechanisms
// both real packages share (static schema, typed columns with JSONPath
// extractors, a nullable relation input bound into a REST query field with
// omission semantics, anonymous auth, terminal-collection response, disabled
// pagination, full resource ceilings) so equivalence is proven across the
// contract surface, not one accidental field.
const char *const kEquivalenceProbeRelationTemplate = "api_version: cuac/v1\n"
                                                      "kind: relation\n"
                                                      "id: equivalence_probe\n"
                                                      "schema: static\n"
                                                      "\n"
                                                      "columns:\n"
                                                      "  - id: id\n"
                                                      "    type: BIGINT\n"
                                                      "    nullable: false\n"
                                                      "    extract: $.id\n"
                                                      "  - id: name\n"
                                                      "    type: VARCHAR\n"
                                                      "    nullable: false\n"
                                                      "    extract: $.name\n"
                                                      "\n"
                                                      "inputs:\n"
                                                      "  - id: status\n"
                                                      "    type: VARCHAR\n"
                                                      "    nullable: true\n"
                                                      "\n"
                                                      "auth:\n"
                                                      "  mode: anonymous\n"
                                                      "\n"
                                                      "resources:\n"
                                                      "  max_response_bytes_per_page: 65536\n"
                                                      "  max_response_bytes_per_scan: 65536\n"
                                                      "  max_records_per_page: 20\n"
                                                      "  max_records_per_scan: 20\n"
                                                      "  max_extracted_string_bytes: 256\n"
                                                      "\n"
                                                      "operations:\n"
                                                      "  - id: fetch_probe\n"
                                                      "    fallback: true\n"
                                                      "    cardinality: many\n"
                                                      "    replay_safety: safe\n"
                                                      "    request:\n"
                                                      "      protocol: rest\n"
                                                      "      method: GET\n"
                                                      "      origin:\n"
                                                      "        scheme: https\n"
                                                      "        host: %s\n"
                                                      "        port: 443\n"
                                                      "      path: /api/probe\n"
                                                      "      query:\n"
                                                      "        - name: status\n"
                                                      "          input: status\n"
                                                      "          encoding: form_urlencoded\n"
                                                      "          omit_when_unbound: true\n"
                                                      "          omit_when_null: true\n"
                                                      "      headers: []\n"
                                                      "    response:\n"
                                                      "      source: terminal_collection\n"
                                                      "      records: $.results[*]\n"
                                                      "    pagination:\n"
                                                      "      strategy: disabled\n";

std::string RenderEquivalenceProbeRelation(const std::string &host) {
	char buffer[4096];
	const int written = ::snprintf(buffer, sizeof(buffer), kEquivalenceProbeRelationTemplate, host.c_str());
	if (written < 0 || static_cast<std::size_t>(written) >= sizeof(buffer)) {
		throw std::runtime_error("equivalence probe relation template did not render");
	}
	return std::string(buffer, static_cast<std::size_t>(written));
}

// Derives a package envelope from one real package's connector.yaml by
// re-identifying the connector and replacing its relation list with the single
// canonical probe. Everything else (network policy, credentials, response
// ceiling) is the real package's, so the oracle proves equivalence across the
// actual package profiles rather than a hand-authored approximation.
std::string DeriveEquivalenceManifest(const std::string &real_manifest, const std::string &real_id,
                                      const std::string &package_id) {
	const std::string id_anchor = "\nid: " + real_id + "\n";
	const auto id_offset = real_manifest.find(id_anchor);
	if (id_offset == std::string::npos) {
		throw std::runtime_error("package envelope source connector id anchor is missing");
	}
	const std::string relations_anchor = "\nrelations:\n";
	const auto relations_offset = real_manifest.find(relations_anchor);
	if (relations_offset == std::string::npos) {
		throw std::runtime_error("package envelope source relations block is missing");
	}
	std::string result = real_manifest;
	result.replace(id_offset, id_anchor.size(), "\nid: " + package_id + "\n");
	// Re-locate the relations block after the id replacement shifted the text.
	const auto new_relations_offset = result.find(relations_anchor);
	result.replace(new_relations_offset, result.size() - new_relations_offset, "\nrelations:\n  - equivalence_probe\n");
	return result;
}

std::string ApplyReplacements(std::string text, const std::vector<PackageReplacement> &replacements) {
	for (const auto &replacement : replacements) {
		const auto offset = text.find(replacement.from);
		if (offset == std::string::npos) {
			throw std::runtime_error("package mutation anchor is absent: " + replacement.from);
		}
		text.replace(offset, replacement.from.size(), replacement.to);
	}
	return text;
}

struct PackageProfileSource {
	const char *package;
	const char *real_id;
	const char *package_id;
	const char *host;
};

PackageProfileSource ResolvePackageProfile(PackageProfile profile) {
	switch (profile) {
	case PackageProfile::GITHUB:
		return {"github", "github", "github_equivalence", "api.github.com"};
	case PackageProfile::RICK_AND_MORTY:
		return {"rickandmorty", "rickandmorty", "rickandmorty_equivalence", "rickandmortyapi.com"};
	}
	throw std::invalid_argument("unknown package profile");
}

cuac::connector::PackageCompileResult
CompilePackageEnvelopeWithMutation(const std::string &absolute_repository_root, PackageProfile profile,
                                   const std::vector<PackageReplacement> &manifest_replacements,
                                   const std::vector<PackageReplacement> &relation_replacements) {
	const auto source = ResolvePackageProfile(profile);
	char pattern[] = "/tmp/cuac-equivalence-fixture-XXXXXX";
	const auto *created = ::mkdtemp(pattern);
	if (!created) {
		throw std::runtime_error("could not create package equivalence fixture root");
	}
	const std::string root = created;
	try {
		if (::mkdir((root + "/relations").c_str(), 0700) != 0) {
			throw std::runtime_error("could not create package equivalence fixture relations");
		}
		const std::string evidence = absolute_repository_root + "/connectors/" + source.package + "/";
		WriteFile(root + "/connector.yaml",
		          ApplyReplacements(DeriveEquivalenceManifest(ReadFile(evidence + "connector.yaml"), source.real_id,
		                                                      source.package_id),
		                            manifest_replacements));
		WriteFile(root + "/relations/equivalence_probe.yaml",
		          ApplyReplacements(RenderEquivalenceProbeRelation(source.host), relation_replacements));
		NeverCancel cancellation;
		auto result = cuac::connector::CompileLocalPackageRoot(root, cancellation);
		// A successful compilation retains the root through the returned
		// custody; a failed compilation has no custody, so the provider owns
		// the private root for process lifetime in both cases.
		RetainedFixtureRoots().Retain(root);
		return result;
	} catch (...) {
		(void)::nftw(root.c_str(), RemoveEntry, 32, FTW_DEPTH | FTW_PHYS);
		throw;
	}
}

CrossPackageEquivalenceFixture
BuildRepositoryCrossPackageEquivalenceFixture(const std::string &absolute_repository_root) {
	const auto github = CompilePackageEnvelopeWithMutation(absolute_repository_root, PackageProfile::GITHUB, {}, {});
	if (!github.Succeeded() || github.Package() == nullptr) {
		throw std::runtime_error("github-profile package envelope did not compile");
	}
	const auto rickandmorty =
	    CompilePackageEnvelopeWithMutation(absolute_repository_root, PackageProfile::RICK_AND_MORTY, {}, {});
	if (!rickandmorty.Succeeded() || rickandmorty.Package() == nullptr) {
		throw std::runtime_error("rickandmorty-profile package envelope did not compile");
	}
	return CrossPackageEquivalenceFixture {cuac::CompiledLocalPackage(*github.Package()),
	                                       cuac::CompiledLocalPackage(*rickandmorty.Package())};
}

cuac::CompiledLocalPackage CompileRepositoryDistinctLocalPackageFixture(const std::string &absolute_repository_root) {
	char pattern[] = "/tmp/cuac-distinct-fixture-XXXXXX";
	const auto *created = ::mkdtemp(pattern);
	if (!created) {
		throw std::runtime_error("could not create distinct local-package fixture root");
	}
	const std::string root = created;
	try {
		if (::mkdir((root + "/relations").c_str(), 0700) != 0) {
			throw std::runtime_error("could not create distinct local-package fixture relations");
		}
		const std::string evidence = absolute_repository_root + "/connectors/github/";
		WriteFile(root + "/connector.yaml", WithDistinctConnectorId(ReadFile(evidence + "connector.yaml")));
		for (const auto &relation : {"authenticated_repositories", "authenticated_user", "duckdb_login_search_page",
		                             "viewer_repository_metrics"}) {
			WriteFile(root + "/relations/" + std::string(relation) + ".yaml",
			          ReadFile(evidence + "relations/" + std::string(relation) + ".yaml"));
		}
		NeverCancel cancellation;
		const auto result = cuac::connector::CompileLocalPackageRoot(root, cancellation);
		if (!result.Succeeded() || result.Package() == nullptr) {
			throw std::runtime_error("distinct local-package fixture did not compile");
		}
		cuac::CompiledLocalPackage package(*result.Package());
		RetainedFixtureRoots().Retain(root);
		return package;
	} catch (...) {
		(void)::nftw(root.c_str(), RemoveEntry, 32, FTW_DEPTH | FTW_PHYS);
		throw;
	}
}

cuac::CompiledPackageGeneration CompileRepositoryGithubGenerationFixture(const std::string &absolute_repository_root) {
	return CompileRepositoryGithubLocalPackageFixture(absolute_repository_root).Generation();
}

cuac::CompiledConnector CompileRepositoryGithubConnectorFixture(const std::string &absolute_repository_root) {
	return CompileRepositoryGithubGenerationFixture(absolute_repository_root).Connector();
}

cuac::CompiledPackageGeneration CompileNonGithubGraphqlGenerationFixture(const std::string &absolute_repository_root) {
	NeverCancel cancellation;
	const auto result = cuac::connector::CompileLocalPackageRoot(
	    absolute_repository_root + "/test/fixtures/package_graphql_non_github", cancellation);
	if (!result.Succeeded() || result.Package() == nullptr) {
		throw std::runtime_error("non-GitHub GraphQL connector package fixture did not compile");
	}
	return result.Package()->Generation();
}

cuac::CompiledConnector
CompileRepositoryGithubGraphqlCounterexample(const std::string &absolute_repository_root,
                                             RepositoryGithubGraphqlCounterexample counterexample) {
	const auto generation = CompileRepositoryGithubGenerationFixture(absolute_repository_root);
	auto connector = generation.Connector();
	const auto *relation = connector.FindRelation("viewer_repository_metrics");
	if (relation == nullptr || relation->Operations().size() != 1) {
		throw std::runtime_error("repository GitHub GraphQL fixture lost its exact relation");
	}
	auto graphql = relation->Operations()[0].Graphql();
	switch (counterexample) {
	case RepositoryGithubGraphqlCounterexample::DOCUMENT_MISMATCH:
		graphql.document += " ";
		graphql.document_digest = cuac::ComputeSha256Hex(graphql.document);
		break;
	case RepositoryGithubGraphqlCounterexample::DIGEST_MISMATCH:
		graphql.document_digest[0] = graphql.document_digest[0] == '0' ? '1' : '0';
		break;
	case RepositoryGithubGraphqlCounterexample::VARIABLE_MISMATCH:
		graphql.variables[0].name = "otherPageSize";
		break;
	case RepositoryGithubGraphqlCounterexample::RESPONSE_PATH_MISMATCH:
		graphql.response.nodes.segments.back() = "edges";
		break;
	case RepositoryGithubGraphqlCounterexample::COLUMN_MISMATCH:
		graphql.result_columns[0].response_path.segments[0] = "nodeId";
		break;
	case RepositoryGithubGraphqlCounterexample::CURSOR_MISMATCH:
		graphql.cursor.cursor_variable = "otherCursor";
		break;
	case RepositoryGithubGraphqlCounterexample::UNKNOWN_RECIPE_IDENTITY:
		graphql = ConnectorCatalogTestAccess::WithUnknownGraphqlRecipeIdentity(std::move(graphql));
		break;
	case RepositoryGithubGraphqlCounterexample::MIXED_CASE_AUTHORIZATION_HEADER:
		graphql.headers.push_back({"AUTHORIZATION", "public-test-value"});
		break;
	case RepositoryGithubGraphqlCounterexample::MIXED_CASE_HOST_HEADER:
		graphql.headers.push_back({"hOsT", "api.github.com"});
		break;
	case RepositoryGithubGraphqlCounterexample::MIXED_CASE_CONTENT_LENGTH_HEADER:
		graphql.headers.push_back({"cOnTeNt-LeNgTh", "1"});
		break;
	case RepositoryGithubGraphqlCounterexample::CASE_INSENSITIVE_DUPLICATE_HEADER:
		graphql.headers.push_back({"aCcEpT", "application/json"});
		break;
	case RepositoryGithubGraphqlCounterexample::MIXED_CASE_CONTENT_TYPE_MISMATCH:
		for (auto &header : graphql.headers) {
			if (header.name == "Content-Type") {
				header.name = "cOnTeNt-TyPe";
				header.value = "text/plain";
			}
		}
		break;
	case RepositoryGithubGraphqlCounterexample::INVALID_HEADER_NAME:
		graphql.headers.push_back({"Bad Header", "public-test-value"});
		break;
	case RepositoryGithubGraphqlCounterexample::INVALID_HEADER_VALUE:
		graphql.headers.push_back({"X-Test-Value", "safe\r\nInjected: value"});
		break;
	case RepositoryGithubGraphqlCounterexample::INVALID_ENDPOINT_PATH_GRAMMAR:
		graphql.endpoint_path = "/graphql?debug=true";
		break;
	case RepositoryGithubGraphqlCounterexample::TRAILING_ENDPOINT_PATH_SEPARATOR:
		graphql.endpoint_path = "/graphql/";
		break;
	case RepositoryGithubGraphqlCounterexample::ENDPOINT_PATH_TOO_LONG:
		graphql.endpoint_path = "/" + std::string(2048, 'a');
		break;
	case RepositoryGithubGraphqlCounterexample::ENDPOINT_PORT_OUTSIDE_POLICY:
		graphql.endpoint_origin.port = 8443;
		break;
	case RepositoryGithubGraphqlCounterexample::TOO_MANY_HEADERS:
		while (graphql.headers.size() <= 32) {
			graphql.headers.push_back({"X-Count-" + std::to_string(graphql.headers.size()), "public-test-value"});
		}
		break;
	case RepositoryGithubGraphqlCounterexample::HEADER_BYTES_EXCEEDED:
		FillHeadersToCombinedBytes(graphql.headers, 16ULL * 1024ULL + 1);
		break;
	case RepositoryGithubGraphqlCounterexample::RESPONSE_SCAN_SCOPE_EXCEEDED:
		return ConnectorCatalogTestAccess::WithInvalidRelationResources(
		    ConnectorCatalogTestAccess::WithInvalidGraphqlOperation(std::move(connector), "viewer_repository_metrics",
		                                                            "github_viewer_repository_metrics",
		                                                            std::move(graphql)),
		    "viewer_repository_metrics", 8ULL * 1024ULL * 1024ULL, 256ULL * 1024ULL * 1024ULL + 1, 100, 3200, 512);
	case RepositoryGithubGraphqlCounterexample::RECORD_SCAN_SCOPE_EXCEEDED:
		return ConnectorCatalogTestAccess::WithInvalidRelationResources(
		    ConnectorCatalogTestAccess::WithInvalidGraphqlOperation(std::move(connector), "viewer_repository_metrics",
		                                                            "github_viewer_repository_metrics",
		                                                            std::move(graphql)),
		    "viewer_repository_metrics", 8ULL * 1024ULL * 1024ULL, 64ULL * 1024ULL * 1024ULL, 100, 3201, 512);
	case RepositoryGithubGraphqlCounterexample::RESOURCE_PRODUCT_OVERFLOW:
		return ConnectorCatalogTestAccess::WithInvalidRelationResources(
		    ConnectorCatalogTestAccess::WithInvalidGraphqlOperation(std::move(connector), "viewer_repository_metrics",
		                                                            "github_viewer_repository_metrics",
		                                                            std::move(graphql)),
		    "viewer_repository_metrics", 8ULL * 1024ULL * 1024ULL, 64ULL * 1024ULL * 1024ULL,
		    std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<std::uint64_t>::max(), 512);
	case RepositoryGithubGraphqlCounterexample::COUNT:
		throw std::invalid_argument("unknown repository GitHub GraphQL counterexample");
	}
	return ConnectorCatalogTestAccess::WithInvalidGraphqlOperation(
	    std::move(connector), "viewer_repository_metrics", "github_viewer_repository_metrics", std::move(graphql));
}

cuac::CompiledConnector CompileRepositoryGithubGraphqlBoundary(const std::string &absolute_repository_root,
                                                               RepositoryGithubGraphqlBoundary boundary) {
	const auto generation = CompileRepositoryGithubGenerationFixture(absolute_repository_root);
	auto connector = generation.Connector();
	const auto *relation = connector.FindRelation("viewer_repository_metrics");
	if (relation == nullptr || relation->Operations().size() != 1) {
		throw std::runtime_error("repository GitHub GraphQL fixture lost its exact relation");
	}
	auto graphql = relation->Operations()[0].Graphql();
	switch (boundary) {
	case RepositoryGithubGraphqlBoundary::ENDPOINT_PATH_BYTES:
		graphql.endpoint_path = "/" + std::string(2047, 'a');
		break;
	case RepositoryGithubGraphqlBoundary::FIXED_HEADER_COUNT:
		while (graphql.headers.size() < 32) {
			graphql.headers.push_back({"X-Count-" + std::to_string(graphql.headers.size()), "v"});
		}
		break;
	case RepositoryGithubGraphqlBoundary::FIXED_HEADER_BYTES: {
		FillHeadersToCombinedBytes(graphql.headers, 16ULL * 1024ULL);
		break;
	}
	case RepositoryGithubGraphqlBoundary::RESPONSE_SCAN_PRODUCT:
		return ConnectorCatalogTestAccess::WithInvalidRelationResources(
		    ConnectorCatalogTestAccess::WithInvalidGraphqlOperation(std::move(connector), "viewer_repository_metrics",
		                                                            "github_viewer_repository_metrics",
		                                                            std::move(graphql)),
		    "viewer_repository_metrics", 8ULL * 1024ULL * 1024ULL, 256ULL * 1024ULL * 1024ULL, 100, 3200, 512);
	case RepositoryGithubGraphqlBoundary::COUNT:
		throw std::invalid_argument("unknown repository GitHub GraphQL boundary");
	}
	return ConnectorCatalogTestAccess::WithInvalidGraphqlOperation(
	    std::move(connector), "viewer_repository_metrics", "github_viewer_repository_metrics", std::move(graphql));
}

cuac::CompiledGraphqlQueryRecipe
CompileRepositoryGithubGraphqlRecipeFixture(const std::string &absolute_repository_root,
                                            RepositoryGithubGraphqlRecipeFixture fixture) {
	const auto generation = CompileRepositoryGithubGenerationFixture(absolute_repository_root);
	const auto *relation = generation.Connector().FindRelation("viewer_repository_metrics");
	if (relation == nullptr || relation->Operations().size() != 1) {
		throw std::runtime_error("repository GitHub GraphQL recipe fixture lost its exact relation");
	}
	const auto &recipe = relation->Operations()[0].Graphql().QueryRecipe();
	switch (fixture) {
	case RepositoryGithubGraphqlRecipeFixture::EXACT_LITERAL_DEPTH:
		return ConnectorCatalogTestAccess::WithFirstGraphqlFixedArgument(
		    recipe, ConnectorCatalogTestAccess::NestedGraphqlList(31));
	case RepositoryGithubGraphqlRecipeFixture::EXCESSIVE_LITERAL_DEPTH:
		return ConnectorCatalogTestAccess::WithFirstGraphqlFixedArgument(
		    recipe, ConnectorCatalogTestAccess::NestedGraphqlList(32));
	case RepositoryGithubGraphqlRecipeFixture::EXACT_LIST_ITEMS:
		return ConnectorCatalogTestAccess::WithFirstGraphqlFixedArgument(
		    recipe, ConnectorCatalogTestAccess::FlatGraphqlNullList(4096));
	case RepositoryGithubGraphqlRecipeFixture::EXCESSIVE_LIST_ITEMS:
		return ConnectorCatalogTestAccess::WithFirstGraphqlFixedArgument(
		    recipe, ConnectorCatalogTestAccess::FlatGraphqlNullList(4097));
	case RepositoryGithubGraphqlRecipeFixture::MINIMUM_SIGNED_INTEGER:
		return ConnectorCatalogTestAccess::WithFirstGraphqlFixedArgument(
		    recipe, ConnectorCatalogTestAccess::RawGraphqlInteger("-9223372036854775808"));
	case RepositoryGithubGraphqlRecipeFixture::MAXIMUM_SIGNED_INTEGER:
		return ConnectorCatalogTestAccess::WithFirstGraphqlFixedArgument(
		    recipe, ConnectorCatalogTestAccess::RawGraphqlInteger("9223372036854775807"));
	case RepositoryGithubGraphqlRecipeFixture::BELOW_MINIMUM_SIGNED_INTEGER:
		return ConnectorCatalogTestAccess::WithFirstGraphqlFixedArgument(
		    recipe, ConnectorCatalogTestAccess::RawGraphqlInteger("-9223372036854775809"));
	case RepositoryGithubGraphqlRecipeFixture::ABOVE_MAXIMUM_SIGNED_INTEGER:
		return ConnectorCatalogTestAccess::WithFirstGraphqlFixedArgument(
		    recipe, ConnectorCatalogTestAccess::RawGraphqlInteger("9223372036854775808"));
	case RepositoryGithubGraphqlRecipeFixture::COUNT:
		break;
	}
	throw std::invalid_argument("unknown repository GitHub GraphQL recipe fixture");
}

cuac::CompiledGraphqlLiteral BuildGraphqlLiteralNodeBudgetFixture(GraphqlLiteralNodeBudgetFixture fixture) {
	switch (fixture) {
	case GraphqlLiteralNodeBudgetFixture::EXACT:
		return ConnectorCatalogTestAccess::GraphqlLiteralNodeTree(100000);
	case GraphqlLiteralNodeBudgetFixture::EXCESSIVE:
		return ConnectorCatalogTestAccess::GraphqlLiteralNodeTree(100001);
	case GraphqlLiteralNodeBudgetFixture::COUNT:
		break;
	}
	throw std::invalid_argument("unknown GraphQL literal node budget fixture");
}

} // namespace cuac_test
