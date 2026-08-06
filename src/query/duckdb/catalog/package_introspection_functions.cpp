#include "cuac/internal/query/catalog/package_introspection_functions.hpp"

#include "cuac/internal/query/catalog/package_catalog_snapshot.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "cuac/connector/catalog.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace duckdb {
namespace cuac_query_internal {
namespace {

struct IntrospectionRow final {
	std::vector<Value> values;
};

struct IntrospectionBindData final : public TableFunctionData {
	explicit IntrospectionBindData(std::shared_ptr<const PackageCatalogSnapshot> snapshot_p,
	                               std::vector<IntrospectionRow> rows_p)
	    : snapshot(std::move(snapshot_p)), rows(std::move(rows_p)) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<IntrospectionBindData>(snapshot, rows);
	}

	bool Equals(const FunctionData &other_p) const override {
		const auto *other = dynamic_cast<const IntrospectionBindData *>(&other_p);
		return other && snapshot == other->snapshot;
	}

	const std::shared_ptr<const PackageCatalogSnapshot> snapshot;
	const std::vector<IntrospectionRow> rows;
};

struct IntrospectionState final : public GlobalTableFunctionState {
	idx_t row = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

unique_ptr<GlobalTableFunctionState> InitIntrospection(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<IntrospectionState>();
}

void ScanIntrospection(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<IntrospectionBindData>();
	auto &state = input.global_state->Cast<IntrospectionState>();
	idx_t count = 0;
	while (state.row < data.rows.size() && count < STANDARD_VECTOR_SIZE) {
		const auto &row = data.rows[state.row];
		if (row.values.size() != output.ColumnCount()) {
			throw InternalException("package introspection row has unexpected arity");
		}
		for (idx_t column = 0; column < row.values.size(); column++) {
			output.SetValue(column, count, row.values[column]);
		}
		state.row++;
		count++;
	}
	output.SetCardinality(count);
}

const PackageCatalogFunctionInfo &RequireInfo(TableFunctionBindInput &input, PackageCatalogFunctionKind kind) {
	if (!input.info) {
		throw InternalException("package introspection is missing immutable catalog information");
	}
	auto &info = input.info->Cast<PackageCatalogFunctionInfo>();
	if (info.kind != kind || !info.snapshot || info.generation || info.relation) {
		throw InternalException("package introspection has contradictory catalog information");
	}
	return info;
}

std::string RenderDefault(const cuac::CompiledScalarValue &value) {
	if (value.IsNull()) {
		return "NULL";
	}
	switch (value.Type()) {
	case cuac::CompiledScalarType::BOOLEAN:
		return value.Boolean() ? "TRUE" : "FALSE";
	case cuac::CompiledScalarType::BIGINT:
		return std::to_string(value.Bigint());
	case cuac::CompiledScalarType::VARCHAR: {
		std::string result = "'";
		for (const auto character : value.Varchar()) {
			result.push_back(character);
			if (character == '\'') {
				result.push_back('\'');
			}
		}
		result.push_back('\'');
		return result;
	}
	case cuac::CompiledScalarType::DOUBLE: {
		// 17 significant decimal digits round-trips any IEEE-754 double.
		char buffer[64];
		const int written = std::snprintf(buffer, sizeof(buffer), "%.17g", value.Double());
		if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(buffer)) {
			throw std::logic_error("relation input DOUBLE default could not be rendered");
		}
		return std::string(buffer, static_cast<std::size_t>(written));
	}
	case cuac::CompiledScalarType::TIMESTAMPTZ:
		return "TIMESTAMPTZ '" + cuac::CanonicalTimestamptz(value.TimestamptzMicroseconds()) + "'";
	}
	throw std::logic_error("relation input default has an unsupported structural type");
}

unique_ptr<FunctionData> BindLoadedConnectors(ClientContext &, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	const auto &info = RequireInfo(input, PackageCatalogFunctionKind::LOADED_CONNECTORS);
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::UBIGINT};
	names = {"connector", "package_version", "spec_version", "package_digest", "relation_count"};
	std::vector<IntrospectionRow> rows;
	for (const auto &generation : info.snapshot->Generations()) {
		const auto &registration = generation->Registration();
		const auto &identity = registration.Identity();
		rows.push_back(
		    {{Value(identity.ConnectorId()), Value(identity.PackageVersion()), Value(identity.SpecIdentifier()),
		      Value(identity.PackageDigest()), Value::UBIGINT(registration.Relations().size())}});
	}
	return make_uniq<IntrospectionBindData>(info.snapshot, std::move(rows));
}

unique_ptr<FunctionData> BindLoadedRelations(ClientContext &, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	const auto &info = RequireInfo(input, PackageCatalogFunctionKind::LOADED_RELATIONS);
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
	names = {"connector", "relation", "sql_name", "package_version"};
	std::vector<IntrospectionRow> rows;
	for (const auto &generation : info.snapshot->Generations()) {
		const auto &registration = generation->Registration();
		for (const auto &relation : registration.Relations()) {
			rows.push_back({{Value(registration.Identity().ConnectorId()), Value(relation.Name()),
			                 Value(GeneratedRelationName(registration.Identity(), relation)),
			                 Value(registration.Identity().PackageVersion())}});
		}
	}
	std::sort(rows.begin(), rows.end(), [](const IntrospectionRow &left, const IntrospectionRow &right) {
		return std::make_tuple(StringValue::Get(left.values[0]), StringValue::Get(left.values[1])) <
		       std::make_tuple(StringValue::Get(right.values[0]), StringValue::Get(right.values[1]));
	});
	return make_uniq<IntrospectionBindData>(info.snapshot, std::move(rows));
}

unique_ptr<FunctionData> BindRelationArguments(ClientContext &, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<string> &names) {
	const auto &info = RequireInfo(input, PackageCatalogFunctionKind::RELATION_ARGUMENTS);
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::BOOLEAN, LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::VARCHAR};
	names = {"connector", "relation",    "argument",      "duckdb_type",
	         "nullable",  "has_default", "default_value", "argument_origin"};
	std::vector<IntrospectionRow> rows;
	for (const auto &generation : info.snapshot->Generations()) {
		const auto &registration = generation->Registration();
		for (const auto &relation : registration.Relations()) {
			for (const auto &argument : relation.Inputs()) {
				Value rendered_default;
				if (argument.Default().HasDefault()) {
					rendered_default = Value(RenderDefault(argument.Default().Value()));
				}
				rows.push_back(
				    {{Value(registration.Identity().ConnectorId()), Value(relation.Name()), Value(argument.Name()),
				      Value(cuac::CompiledScalarTypeName(argument.Type())), Value::BOOLEAN(argument.Nullable()),
				      Value::BOOLEAN(argument.Default().HasDefault()), rendered_default, Value("relation")}});
			}
			if (relation.Authentication() == cuac::CompiledRegistrationAuthentication::LOGICAL_SECRET_REQUIRED) {
				rows.push_back(
				    {{Value(registration.Identity().ConnectorId()), Value(relation.Name()), Value("secret"),
				      Value("VARCHAR"), Value::BOOLEAN(false), Value::BOOLEAN(false), Value(), Value("query")}});
			}
		}
	}
	std::sort(rows.begin(), rows.end(), [](const IntrospectionRow &left, const IntrospectionRow &right) {
		return std::make_tuple(StringValue::Get(left.values[0]), StringValue::Get(left.values[1]),
		                       StringValue::Get(left.values[2])) < std::make_tuple(StringValue::Get(right.values[0]),
		                                                                           StringValue::Get(right.values[1]),
		                                                                           StringValue::Get(right.values[2]));
	});
	return make_uniq<IntrospectionBindData>(info.snapshot, std::move(rows));
}

TableFunction BuildIntrospectionFunction(const std::string &name, table_function_bind_t bind,
                                         PackageCatalogFunctionKind kind,
                                         const std::shared_ptr<CatalogGenerationCoordinator> &coordinator,
                                         const std::shared_ptr<const PackageCatalogSnapshot> &snapshot) {
	TableFunction function(name, {}, ScanIntrospection, std::move(bind), InitIntrospection);
	function.function_info = make_shared_ptr<PackageCatalogFunctionInfo>(coordinator, snapshot, kind);
	return function;
}

} // namespace

TableFunction BuildLoadedConnectorsFunction(const std::shared_ptr<CatalogGenerationCoordinator> &coordinator,
                                            const std::shared_ptr<const PackageCatalogSnapshot> &snapshot) {
	return BuildIntrospectionFunction("cuac_loaded_connectors", BindLoadedConnectors,
	                                  PackageCatalogFunctionKind::LOADED_CONNECTORS, coordinator, snapshot);
}

TableFunction BuildLoadedRelationsFunction(const std::shared_ptr<CatalogGenerationCoordinator> &coordinator,
                                           const std::shared_ptr<const PackageCatalogSnapshot> &snapshot) {
	return BuildIntrospectionFunction("cuac_loaded_relations", BindLoadedRelations,
	                                  PackageCatalogFunctionKind::LOADED_RELATIONS, coordinator, snapshot);
}

TableFunction BuildRelationArgumentsFunction(const std::shared_ptr<CatalogGenerationCoordinator> &coordinator,
                                             const std::shared_ptr<const PackageCatalogSnapshot> &snapshot) {
	return BuildIntrospectionFunction("cuac_relation_arguments", BindRelationArguments,
	                                  PackageCatalogFunctionKind::RELATION_ARGUMENTS, coordinator, snapshot);
}

} // namespace cuac_query_internal
} // namespace duckdb
