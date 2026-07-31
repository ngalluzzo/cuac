#include "cuac/internal/query/catalog/package_management_functions.hpp"

#include "cuac/internal/query/catalog/catalog_generation_coordinator.hpp"
#include "cuac/internal/query/catalog/package_catalog_snapshot.hpp"
#include "cuac/internal/query/adapter/relation_execution.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/client_context.hpp"
#include "cuac/query/query_generation.hpp"

#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace duckdb {
namespace cuac_query_internal {
namespace {

struct ManagementBindData final : public TableFunctionData {
	ManagementBindData(std::shared_ptr<CatalogGenerationCoordinator> coordinator_p,
	                   std::shared_ptr<const PackageCatalogSnapshot> snapshot_p, PackagePublicationIntent intent_p,
	                   std::string argument_p, std::shared_ptr<const cuac::QueryPublishedGeneration> active_p = nullptr)
	    : coordinator(std::move(coordinator_p)), snapshot(std::move(snapshot_p)), intent(intent_p),
	      argument(std::move(argument_p)), active(std::move(active_p)) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<ManagementBindData>(coordinator, snapshot, intent, argument, active);
	}

	bool Equals(const FunctionData &other_p) const override {
		const auto *other = dynamic_cast<const ManagementBindData *>(&other_p);
		return other && coordinator == other->coordinator && snapshot == other->snapshot && intent == other->intent &&
		       argument == other->argument && active == other->active;
	}

	bool SupportStatementCache() const override {
		// Every execution must stage against and publish from the generation
		// visible to that execution, never a cached prior management bind.
		return false;
	}

	const std::shared_ptr<CatalogGenerationCoordinator> coordinator;
	const std::shared_ptr<const PackageCatalogSnapshot> snapshot;
	const PackagePublicationIntent intent;
	const std::string argument;
	const std::shared_ptr<const cuac::QueryPublishedGeneration> active;
};

struct ManagementState final : public GlobalTableFunctionState {
	bool completed = false;
	bool emitted = false;
	std::string connector;
	std::string package_version;
	std::string spec_version;
	std::string package_digest;
	std::uint64_t relation_count = 0;
	bool changed = false;

	idx_t MaxThreads() const override {
		return 1;
	}
};

const PackageCatalogFunctionInfo &RequireInfo(TableFunctionBindInput &input, PackageCatalogFunctionKind kind) {
	if (!input.info) {
		throw InternalException("package management function is missing immutable catalog information");
	}
	auto &info = input.info->Cast<PackageCatalogFunctionInfo>();
	if (info.kind != kind || !info.snapshot || info.generation || info.relation) {
		throw InternalException("package management function has contradictory catalog information");
	}
	return info;
}

std::string RequiredNamedString(TableFunctionBindInput &input, const std::string &name) {
	const auto entry = input.named_parameters.find(name);
	if (entry == input.named_parameters.end() || entry->second.IsNull()) {
		throw BinderException("[cuac][bind] required named argument %s is missing or NULL", name);
	}
	const auto value = StringValue::Get(entry->second);
	if (value.empty()) {
		throw BinderException("[cuac][bind] required named argument %s must not be empty", name);
	}
	return value;
}

void BindManagementResult(vector<LogicalType> &return_types, vector<string> &names) {
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::UBIGINT, LogicalType::BOOLEAN};
	names = {"connector", "package_version", "spec_version", "package_digest", "relation_count", "changed"};
}

unique_ptr<FunctionData> BindLoad(ClientContext &context, TableFunctionBindInput &input,
                                  vector<LogicalType> &return_types, vector<string> &names) {
	if (!context.transaction.IsAutoCommit()) {
		throw BinderException("[cuac][bind] load and reload require autocommit");
	}
	const auto &info = RequireInfo(input, PackageCatalogFunctionKind::LOAD);
	const auto root = RequiredNamedString(input, "package_root");
	if (root[0] != '/') {
		throw BinderException("[cuac][bind] package_root must be an absolute path");
	}
	info.coordinator->RecordManagementBind(context);
	BindManagementResult(return_types, names);
	return make_uniq<ManagementBindData>(info.coordinator, info.snapshot, PackagePublicationIntent::LOAD, root);
}

unique_ptr<FunctionData> BindReload(ClientContext &context, TableFunctionBindInput &input,
                                    vector<LogicalType> &return_types, vector<string> &names) {
	if (!context.transaction.IsAutoCommit()) {
		throw BinderException("[cuac][bind] load and reload require autocommit");
	}
	const auto &info = RequireInfo(input, PackageCatalogFunctionKind::RELOAD);
	const auto connector = RequiredNamedString(input, "connector");
	auto active = info.snapshot->Find(connector);
	if (!active) {
		throw BinderException("[cuac][bind] connector=%s is not active", connector);
	}
	info.coordinator->RecordManagementBind(context);
	BindManagementResult(return_types, names);
	return make_uniq<ManagementBindData>(info.coordinator, info.snapshot, PackagePublicationIntent::RELOAD, connector,
	                                     std::move(active));
}

unique_ptr<GlobalTableFunctionState> InitManagement(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<ManagementState>();
}

[[noreturn]] void ThrowStagingError(const cuac::QueryStagingError &error) {
	std::ostringstream rendered;
	rendered << "[cuac][" << error.Phase() << "] code=" << error.Code();
	if (!error.File().empty()) {
		rendered << " file=" << error.File();
	}
	if (error.HasLineAndColumn()) {
		rendered << " line=" << error.Line() << " column=" << error.Column();
	}
	if (!error.YamlPath().empty()) {
		rendered << " yaml_path=" << error.YamlPath();
	}
	rendered << ": " << error.SafeDetail();
	throw InvalidInputException("%s", rendered.str());
}

void ValidateStaged(const ManagementBindData &data, const cuac::QueryStagedGeneration &staged) {
	const auto &candidate = staged.Generation();
	const auto &identity = candidate->Registration().Identity();
	if (data.intent == PackagePublicationIntent::LOAD) {
		if (!staged.Changed()) {
			throw std::logic_error("load staging returned an unchanged generation");
		}
		return;
	}
	if (!data.active || identity.ConnectorId() != data.argument) {
		throw std::logic_error("reload staging returned a different connector generation");
	}
	const auto same =
	    candidate->Registration().GenerationHandle().IsSameGeneration(data.active->Registration().GenerationHandle());
	if (staged.Changed() == same) {
		throw std::logic_error("reload staging changed flag contradicts generation identity");
	}
}

void ScanManagement(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<ManagementBindData>();
	auto &state = input.global_state->Cast<ManagementState>();
	if (state.emitted) {
		return;
	}
	try {
		if (!state.completed) {
			DuckdbExecutionControl control(context);
			if (control.IsCancellationRequested()) {
				throw InterruptException();
			}
			auto staged = data.intent == PackagePublicationIntent::LOAD
			                  ? data.coordinator->Staging()->StageLoad(data.argument, control)
			                  : data.coordinator->Staging()->StageReload(data.argument, data.active, control);
			if (control.IsCancellationRequested()) {
				throw InterruptException();
			}
			ValidateStaged(data, staged);
			data.coordinator->Publish(context, data.snapshot, staged, data.intent);
			const auto &registration = staged.Generation()->Registration();
			state.connector = registration.Identity().ConnectorId();
			state.package_version = registration.Identity().PackageVersion();
			state.spec_version = registration.Identity().SpecIdentifier();
			state.package_digest = registration.Identity().PackageDigest();
			state.relation_count = registration.Relations().size();
			state.changed = staged.Changed();
			state.completed = true;
		}
		output.SetCardinality(1);
		output.SetValue(0, 0, Value(state.connector));
		output.SetValue(1, 0, Value(state.package_version));
		output.SetValue(2, 0, Value(state.spec_version));
		output.SetValue(3, 0, Value(state.package_digest));
		output.SetValue(4, 0, Value::UBIGINT(state.relation_count));
		output.SetValue(5, 0, Value::BOOLEAN(state.changed));
		state.emitted = true;
	} catch (const InterruptException &) {
		throw;
	} catch (const cuac::ExecutionCancelled &) {
		throw InterruptException();
	} catch (const cuac::QueryStagingError &error) {
		ThrowStagingError(error);
	} catch (const Exception &) {
		throw;
	} catch (const std::exception &) {
		throw InvalidInputException("[cuac][internal] package management failed safely");
	} catch (...) {
		throw InvalidInputException("[cuac][internal] package management failed safely");
	}
}

TableFunction BuildManagementFunction(const std::string &name, table_function_bind_t bind,
                                      PackageCatalogFunctionKind kind,
                                      const std::shared_ptr<CatalogGenerationCoordinator> &coordinator,
                                      const std::shared_ptr<const PackageCatalogSnapshot> &snapshot) {
	TableFunction function(name, {}, ScanManagement, std::move(bind), InitManagement);
	function.function_info = make_shared_ptr<PackageCatalogFunctionInfo>(coordinator, snapshot, kind);
	return function;
}

} // namespace

TableFunction BuildLoadConnectorFunction(const std::shared_ptr<CatalogGenerationCoordinator> &coordinator,
                                         const std::shared_ptr<const PackageCatalogSnapshot> &snapshot) {
	auto function = BuildManagementFunction("cuac_load_connector", BindLoad, PackageCatalogFunctionKind::LOAD,
	                                        coordinator, snapshot);
	function.named_parameters["package_root"] = LogicalType::VARCHAR;
	return function;
}

TableFunction BuildReloadConnectorFunction(const std::shared_ptr<CatalogGenerationCoordinator> &coordinator,
                                           const std::shared_ptr<const PackageCatalogSnapshot> &snapshot) {
	auto function = BuildManagementFunction("cuac_reload_connector", BindReload, PackageCatalogFunctionKind::RELOAD,
	                                        coordinator, snapshot);
	function.named_parameters["connector"] = LogicalType::VARCHAR;
	return function;
}

} // namespace cuac_query_internal
} // namespace duckdb
