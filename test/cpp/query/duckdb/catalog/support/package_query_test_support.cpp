#include "query/duckdb/catalog/support/package_query_test_support.hpp"
#include "query/support/isolated_credential_root.hpp"

#include "cuac/internal/query/catalog/catalog_generation_coordinator.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "cuac/query/duckdb_secret.hpp"
#include "cuac/runtime/execution.hpp"
#include "cuac/semantics/scan_planner.hpp"
#include "cuac/internal/query/catalog/package_lifecycle_sentry.hpp"
#include "connector/support/package_generation_test_fixtures.hpp"
#include "connector/support/package_compiler_test_fixtures.hpp"
#include "support/require.hpp"

#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cuac_test {
namespace {

cuac::ValueKind ValueKindFor(cuac::PlannedColumnScalarKind kind) {
	switch (kind) {
	case cuac::PlannedColumnScalarKind::BIGINT:
		return cuac::ValueKind::BIGINT;
	case cuac::PlannedColumnScalarKind::VARCHAR:
		return cuac::ValueKind::VARCHAR;
	case cuac::PlannedColumnScalarKind::BOOLEAN:
		return cuac::ValueKind::BOOLEAN;
	case cuac::PlannedColumnScalarKind::DOUBLE:
		return cuac::ValueKind::DOUBLE;
	case cuac::PlannedColumnScalarKind::TIMESTAMPTZ:
		return cuac::ValueKind::TIMESTAMPTZ;
	}
	throw std::logic_error("Query package test executor received an unsupported planned scalar type");
}

cuac::TypedScalarValue MarkerElement(cuac::ValueKind kind, const std::string &marker, const std::string &column) {
	switch (kind) {
	case cuac::ValueKind::BIGINT:
		return cuac::TypedScalarValue::BigInt(marker == "old" ? 1 : 2);
	case cuac::ValueKind::VARCHAR:
		return cuac::TypedScalarValue::Varchar(marker + ":" + column);
	case cuac::ValueKind::BOOLEAN:
		return cuac::TypedScalarValue::Boolean(marker != "old");
	case cuac::ValueKind::DOUBLE:
		return cuac::TypedScalarValue::Double(marker == "old" ? 1.5 : 2.5);
	case cuac::ValueKind::TIMESTAMPTZ:
		return cuac::TypedScalarValue::Timestamptz(marker == "old" ? 0 : INT64_C(1000000));
	}
	throw std::logic_error("Query package test executor received an unsupported ARRAY element type");
}

class PackagePlanningService final : public cuac::QueryScanPlanningService {
public:
	PackagePlanningService(cuac::CompiledGenerationHandle handle_p,
	                       std::shared_ptr<const cuac::CompiledConnector> connector_p,
	                       std::shared_ptr<const cuac::CompiledConnector> selective_connector_p,
	                       std::shared_ptr<PackageQueryProbe> probe_p)
	    : handle(std::move(handle_p)), connector(std::move(connector_p)),
	      selective_connector(std::move(selective_connector_p)), probe(std::move(probe_p)) {
	}

	cuac::ScanPlan BuildPlan(const cuac::CompiledGenerationHandle &candidate_handle,
	                         const cuac::ScanRequest &request) const override {
		if (!candidate_handle.IsSameGeneration(handle)) {
			throw std::logic_error("Query package test planner received the wrong immutable generation");
		}
		probe->plans.fetch_add(1, std::memory_order_relaxed);
		const auto &selected =
		    request.capabilities.selective_predicate && selective_connector ? selective_connector : connector;
		if (!selected) {
			throw std::logic_error("Query registration-only fixture has no Semantics planning provider");
		}
		return cuac::BuildConservativeScanPlan(*selected, request);
	}

private:
	const cuac::CompiledGenerationHandle handle;
	const std::shared_ptr<const cuac::CompiledConnector> connector;
	const std::shared_ptr<const cuac::CompiledConnector> selective_connector;
	const std::shared_ptr<PackageQueryProbe> probe;
};

class PackageGenerationOwner final : public cuac::QueryGenerationOwner {
public:
	PackageGenerationOwner(std::shared_ptr<const cuac::CompiledQueryRegistrationView> registration_p,
	                       std::shared_ptr<PackageQueryProbe> probe_p)
	    : registration(std::move(registration_p)), probe(std::move(probe_p)) {
	}

	~PackageGenerationOwner() noexcept override {
		probe->generation_owners_destroyed.fetch_add(1, std::memory_order_relaxed);
	}

private:
	const std::shared_ptr<const cuac::CompiledQueryRegistrationView> registration;
	const std::shared_ptr<PackageQueryProbe> probe;
};

// Runtime owns this concrete capability in production. The test lease proves
// that Query publishes a changed candidate only from DuckDB's commit callback
// and contains every rejected or rolled-back candidate as an exact-once
// discard.
class PackagePublicationLease final : public cuac::QueryPublicationLease {
public:
	explicit PackagePublicationLease(std::shared_ptr<PackageQueryProbe> probe_p)
	    : probe(std::move(probe_p)), terminal(false) {
	}

	~PackagePublicationLease() noexcept override {
		Discard();
	}

	void Commit() noexcept override {
		if (!terminal.exchange(true, std::memory_order_acq_rel)) {
			probe->publication_commits.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void Discard() noexcept override {
		if (!terminal.exchange(true, std::memory_order_acq_rel)) {
			probe->publication_discards.fetch_add(1, std::memory_order_relaxed);
		}
	}

private:
	const std::shared_ptr<PackageQueryProbe> probe;
	std::atomic<bool> terminal;
};

class PackageRowStream final : public cuac::BatchStream {
public:
	PackageRowStream(cuac::ScanPlan plan_p, std::string marker_p, std::shared_ptr<PackageQueryProbe> probe_p)
	    : plan(std::move(plan_p)), marker(std::move(marker_p)), probe(std::move(probe_p)), emitted(false),
	      closed(false) {
	}

	~PackageRowStream() noexcept override {
		Close();
	}

	bool Next(cuac::ExecutionControl &control, cuac::TypedBatch &batch) override {
		batch.Clear();
		if (control.IsCancellationRequested()) {
			throw cuac::ExecutionCancelled();
		}
		if (emitted || closed) {
			return false;
		}
		cuac::TypedRow row;
		for (const auto &column : plan.OutputColumns()) {
			const auto kind = ValueKindFor(column.ElementKind());
			if (column.shape == cuac::PlannedColumnShape::ARRAY) {
				batch.column_types.push_back(cuac::OutputValueType::Array(kind, column.element_nullable));
				std::vector<cuac::TypedScalarValue> elements;
				elements.push_back(MarkerElement(kind, marker, column.name));
				row.values.push_back(cuac::TypedValue::Array(kind, column.element_nullable, std::move(elements)));
				continue;
			}
			batch.column_types.push_back(kind);
			switch (kind) {
			case cuac::ValueKind::BIGINT:
				row.values.push_back(cuac::TypedValue::BigInt(marker == "old" ? 1 : 2));
				break;
			case cuac::ValueKind::VARCHAR:
				row.values.push_back(cuac::TypedValue::Varchar(marker + ":" + column.name));
				break;
			case cuac::ValueKind::BOOLEAN:
				row.values.push_back(cuac::TypedValue::Boolean(marker != "old"));
				break;
			case cuac::ValueKind::DOUBLE:
				row.values.push_back(cuac::TypedValue::Double(marker == "old" ? 1.5 : 2.5));
				break;
			case cuac::ValueKind::TIMESTAMPTZ:
				row.values.push_back(cuac::TypedValue::Timestamptz(marker == "old" ? 0 : INT64_C(1000000)));
				break;
			}
		}
		batch.rows.push_back(std::move(row));
		emitted = true;
		probe->rows.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	void Cancel() noexcept override {
	}

	void Close() noexcept override {
		if (!closed) {
			closed = true;
			probe->streams_closed.fetch_add(1, std::memory_order_relaxed);
		}
	}

	cuac::ExecutionSnapshot Diagnostics() const noexcept override {
		return BatchStream::Diagnostics();
	}

private:
	const cuac::ScanPlan plan;
	const std::string marker;
	const std::shared_ptr<PackageQueryProbe> probe;
	bool emitted;
	bool closed;
};

class PackageExecutor final : public cuac::ScanExecutor {
public:
	PackageExecutor(std::string marker_p, std::shared_ptr<PackageQueryProbe> probe_p)
	    : marker(std::move(marker_p)), probe(std::move(probe_p)) {
	}

	std::unique_ptr<cuac::BatchStream> Open(const cuac::ScanPlan &plan,
	                                        cuac::ExecutionControl &control) const override {
		return OpenAuthorizationEnvelope(plan, cuac::ScanAuthorization::Anonymous(), control);
	}

	void Close() const noexcept override {
	}

protected:
	std::unique_ptr<cuac::BatchStream> OpenCredentialProviderEnvelope(const cuac::ScanPlan &plan,
	                                                                  const cuac::CredentialProvider &provider,
	                                                                  cuac::ExecutionControl &control) const override {
		if (plan.Authentication() != cuac::FeatureState::ENABLED) {
			throw std::logic_error("Query package test provider received an anonymous plan");
		}
		auto resolved = ResolveCredentialWithAuthorityAfterAdmission(plan, provider, control);
		return OpenAuthorizationEnvelope(plan, std::move(resolved.authorization), control);
	}

	std::unique_ptr<cuac::BatchStream> OpenAuthorizationEnvelope(const cuac::ScanPlan &plan,
	                                                             cuac::ScanAuthorization authorization,
	                                                             cuac::ExecutionControl &control) const override {
		if (control.IsCancellationRequested()) {
			throw cuac::ExecutionCancelled();
		}
		const auto alternative = AlternativeOf(authorization);
		// Query's credential provider supplies the kind-neutral CREDENTIAL
		// alternative for every authenticated relation, not BEARER/
		// a provider-specific bearer alternative, so any non-anonymous alternative
		// is valid here.
		if ((plan.Authentication() == cuac::FeatureState::ENABLED) !=
		    (alternative != AuthorizationAlternative::ANONYMOUS)) {
			throw std::logic_error("Query package test executor received the wrong authorization alternative");
		}
		probe->streams_opened.fetch_add(1, std::memory_order_relaxed);
		return std::unique_ptr<cuac::BatchStream>(new PackageRowStream(plan, marker, probe));
	}

private:
	const std::string marker;
	const std::shared_ptr<PackageQueryProbe> probe;
};

} // namespace

PackageQueryProbe::PackageQueryProbe()
    : load_stages(0), reload_stages(0), plans(0), streams_opened(0), streams_closed(0), rows(0),
      generation_owners_destroyed(0), publication_commits(0), publication_discards(0), closes(0),
      query_was_closing_at_close(false) {
}

void PackageQueryStagingService::Close() const noexcept {
	probe->closes.fetch_add(1, std::memory_order_relaxed);
	auto coordinator = close_observer.lock();
	probe->query_was_closing_at_close.store(coordinator && coordinator->IsClosing(), std::memory_order_release);
}

PackageQueryStagingService::PackageQueryStagingService(cuac::CompiledQueryRegistrationView initial_registration_p,
                                                       cuac::CompiledConnector initial_connector_p,
                                                       cuac::CompiledQueryRegistrationView replacement_registration_p,
                                                       cuac::CompiledConnector replacement_connector_p,
                                                       std::string accepted_root_p,
                                                       std::shared_ptr<PackageQueryProbe> probe_p)
    : initial_registration(new cuac::CompiledQueryRegistrationView(std::move(initial_registration_p))),
      initial_connector(new cuac::CompiledConnector(std::move(initial_connector_p))), initial_selective_connector(),
      replacement_registration(new cuac::CompiledQueryRegistrationView(std::move(replacement_registration_p))),
      replacement_connector(new cuac::CompiledConnector(std::move(replacement_connector_p))),
      replacement_selective_connector(), accepted_root(std::move(accepted_root_p)), probe(std::move(probe_p)),
      reload_changed(false) {
	if (accepted_root.empty() || accepted_root[0] != '/' || !probe) {
		throw std::invalid_argument("Query package test staging requires an absolute root and probe");
	}
}

PackageQueryStagingService::PackageQueryStagingService(cuac::CompiledQueryRegistrationView initial_registration_p,
                                                       cuac::CompiledConnector initial_connector_p,
                                                       cuac::CompiledConnector initial_selective_connector_p,
                                                       cuac::CompiledQueryRegistrationView replacement_registration_p,
                                                       cuac::CompiledConnector replacement_connector_p,
                                                       cuac::CompiledConnector replacement_selective_connector_p,
                                                       std::string accepted_root_p,
                                                       std::shared_ptr<PackageQueryProbe> probe_p)
    : initial_registration(new cuac::CompiledQueryRegistrationView(std::move(initial_registration_p))),
      initial_connector(new cuac::CompiledConnector(std::move(initial_connector_p))),
      initial_selective_connector(new cuac::CompiledConnector(std::move(initial_selective_connector_p))),
      replacement_registration(new cuac::CompiledQueryRegistrationView(std::move(replacement_registration_p))),
      replacement_connector(new cuac::CompiledConnector(std::move(replacement_connector_p))),
      replacement_selective_connector(new cuac::CompiledConnector(std::move(replacement_selective_connector_p))),
      accepted_root(std::move(accepted_root_p)), probe(std::move(probe_p)), reload_changed(false) {
	if (accepted_root.empty() || accepted_root[0] != '/' || !probe) {
		throw std::invalid_argument("Query package test staging requires an absolute root and probe");
	}
}

PackageQueryStagingService::PackageQueryStagingService(cuac::CompiledQueryRegistrationView initial_registration_p,
                                                       cuac::CompiledQueryRegistrationView replacement_registration_p,
                                                       std::string accepted_root_p,
                                                       std::shared_ptr<PackageQueryProbe> probe_p)
    : initial_registration(new cuac::CompiledQueryRegistrationView(std::move(initial_registration_p))),
      initial_connector(), initial_selective_connector(),
      replacement_registration(new cuac::CompiledQueryRegistrationView(std::move(replacement_registration_p))),
      replacement_connector(), replacement_selective_connector(), accepted_root(std::move(accepted_root_p)),
      probe(std::move(probe_p)), reload_changed(false) {
	if (accepted_root.empty() || accepted_root[0] != '/' || !probe) {
		throw std::invalid_argument("Query package test staging requires an absolute root and probe");
	}
}

std::shared_ptr<const cuac::QueryPublishedGeneration> PackageQueryStagingService::BuildPublished(
    const std::shared_ptr<const cuac::CompiledQueryRegistrationView> &registration,
    const std::shared_ptr<const cuac::CompiledConnector> &connector,
    const std::shared_ptr<const cuac::CompiledConnector> &selective_connector, const std::string &marker) const {
	auto published = std::shared_ptr<const cuac::QueryPublishedGeneration>(new cuac::QueryPublishedGeneration(
	    *registration,
	    std::shared_ptr<const cuac::QueryScanPlanningService>(
	        new PackagePlanningService(registration->GenerationHandle(), connector, selective_connector, probe)),
	    std::shared_ptr<const cuac::ScanExecutor>(new PackageExecutor(marker, probe)),
	    std::shared_ptr<const cuac::QueryGenerationOwner>(new PackageGenerationOwner(registration, probe))));
	{
		std::lock_guard<std::mutex> guard(candidate_mutex);
		last_candidate = published;
	}
	return published;
}

cuac::QueryStagedGeneration PackageQueryStagingService::StageLoad(const std::string &absolute_root,
                                                                  cuac::ExecutionControl &control) const {
	probe->load_stages.fetch_add(1, std::memory_order_relaxed);
	if (control.IsCancellationRequested()) {
		throw cuac::ExecutionCancelled();
	}
	if (absolute_root != accepted_root) {
		throw cuac::QueryStagingError("package_root", "compile", "connector.yaml", 1, 1, "$.package_root",
		                              "package root is not the controlled fixture");
	}
	return cuac::QueryStagedGeneration(
	    BuildPublished(initial_registration, initial_connector, initial_selective_connector, "old"), true,
	    std::unique_ptr<cuac::QueryPublicationLease>(new PackagePublicationLease(probe)));
}

cuac::QueryStagedGeneration
PackageQueryStagingService::StageReload(const std::string &connector,
                                        const std::shared_ptr<const cuac::QueryPublishedGeneration> &active,
                                        cuac::ExecutionControl &control) const {
	probe->reload_stages.fetch_add(1, std::memory_order_relaxed);
	if (control.IsCancellationRequested()) {
		throw cuac::ExecutionCancelled();
	}
	if (!active || connector != active->Registration().Identity().ConnectorId()) {
		throw std::logic_error("Query package test reload received the wrong active generation");
	}
	if (!reload_changed.load(std::memory_order_acquire)) {
		std::lock_guard<std::mutex> guard(candidate_mutex);
		last_candidate = active;
		return cuac::QueryStagedGeneration(active, false);
	}
	return cuac::QueryStagedGeneration(
	    BuildPublished(replacement_registration, replacement_connector, replacement_selective_connector, "new"), true,
	    std::unique_ptr<cuac::QueryPublicationLease>(new PackagePublicationLease(probe)));
}

void PackageQueryStagingService::SetReloadChanged(bool changed) noexcept {
	reload_changed.store(changed, std::memory_order_release);
}

void PackageQueryStagingService::ObserveQueryClose(
    std::weak_ptr<duckdb::cuac_query_internal::CatalogGenerationCoordinator> coordinator) {
	close_observer = std::move(coordinator);
}

std::weak_ptr<const cuac::QueryPublishedGeneration> PackageQueryStagingService::LastCandidate() const {
	std::lock_guard<std::mutex> guard(candidate_mutex);
	return last_candidate;
}

std::shared_ptr<PackageQueryStagingService>
BuildCompatibilityPackageQueryStaging(const std::string &absolute_root,
                                      const std::shared_ptr<PackageQueryProbe> &probe) {
	auto initial = BuildPackageCompatibilityFixture(PackageCompatibilityFixture::BASELINE, "1.2.3", 'a');
	auto replacement = BuildPackageCompatibilityFixture(PackageCompatibilityFixture::APPEND_RELATION, "1.3.0", 'b');
	return std::shared_ptr<PackageQueryStagingService>(
	    new PackageQueryStagingService(initial.QueryRegistration(), initial.Connector(),
	                                   replacement.QueryRegistration(), replacement.Connector(), absolute_root, probe));
}

std::shared_ptr<PackageQueryStagingService>
BuildGithubPackageQueryStaging(const std::string &absolute_repository_root,
                               const std::shared_ptr<PackageQueryProbe> &probe) {
	auto initial = CompileRepositoryGithubRegistrationFixture(absolute_repository_root);
	auto replacement = CompileRepositoryGithubRegistrationFixture(absolute_repository_root);
	// This Query-owned catalog oracle intentionally has no Semantics provider.
	// The lead-owned whole-graph target supplies same-generation planning; this
	// fixture fails if catalog-only assertions cross that unprovisioned port.
	return std::shared_ptr<PackageQueryStagingService>(new PackageQueryStagingService(
	    std::move(initial), std::move(replacement), absolute_repository_root + "/connectors/github", probe));
}

std::shared_ptr<PackageQueryStagingService>
BuildRickAndMortyPackageQueryStaging(const std::string &absolute_repository_root,
                                     const std::shared_ptr<PackageQueryProbe> &probe) {
	auto initial = CompileRepositoryRickAndMortyLocalPackageFixture(absolute_repository_root);
	auto replacement = CompileRepositoryRickAndMortyLocalPackageFixture(absolute_repository_root);
	// ARRAY bind evidence needs the deterministic Semantics provider so DuckDB
	// can compare the published registration schema with its same-generation
	// plan. The DESCRIBE remains Runtime-free.
	return std::shared_ptr<PackageQueryStagingService>(new PackageQueryStagingService(
	    initial.Generation().QueryRegistration(), initial.Generation().Connector(),
	    replacement.Generation().QueryRegistration(), replacement.Generation().Connector(),
	    absolute_repository_root + "/connectors/rickandmorty", probe));
}

std::shared_ptr<duckdb::cuac_query_internal::CatalogGenerationCoordinator>
RegisterPackageQuerySurface(duckdb::DuckDB &database,
                            const std::shared_ptr<const cuac::QueryPackageStagingService> &staging) {
	ConfigureIsolatedCredentialRoot(database);
	duckdb::ExtensionLoader loader(*database.instance, "cuac_package_query_tests");
	duckdb::RegisterCuacSecrets(loader);
	return duckdb::cuac_query_internal::RegisterPackageSurfaceInternal(loader, staging);
}

std::string PackageQueryError(duckdb::Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql);
	Require(result->HasError(), "package query unexpectedly succeeded: " + sql);
	return result->GetError();
}

void RequirePackageQuerySuccess(duckdb::Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql);
	if (result->HasError()) {
		throw std::runtime_error("package query failed: " + sql + ": " + result->GetError());
	}
}

} // namespace cuac_test
