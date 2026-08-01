#include "duckdb/common/exception.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "cuac/internal/query/adapter/scan_plan_explanation.hpp"
#include "query/support/duckdb_adapter_test_support.hpp"
#include "support/require.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace cuac_test {
namespace {

using cuac_test::QueryRuntimeScenario;
using cuac_test::RegisterPackageAdapter;
using cuac_test::Require;

const char REPOSITORY_SCAN[] =
    "cuac_scan(connector := 'github', relation := 'authenticated_repositories', secret := 'offline_secret')";

std::string Explain(duckdb::Connection &connection, const std::string &sql) {
	auto result = connection.Query("EXPLAIN " + sql);
	if (result->HasError()) {
		throw std::runtime_error("EXPLAIN failed: " + result->GetError());
	}
	std::string explanation;
	for (duckdb::idx_t row = 0; row < result->RowCount(); row++) {
		for (duckdb::idx_t column = 0; column < result->ColumnCount(); column++) {
			explanation += result->GetValue(column, row).ToString();
			explanation.push_back('\n');
		}
	}
	return explanation;
}

void RequireExplainedField(const std::string &explanation, const std::string &label, const std::string &value,
                           const std::string &next_label, const std::string &context) {
	const auto label_position = explanation.find(label);
	const auto next_position = explanation.find(next_label, label_position == std::string::npos ? 0 : label_position);
	const auto value_position = explanation.find(value, label_position == std::string::npos ? 0 : label_position);
	Require(label_position != std::string::npos && next_position != std::string::npos &&
	            value_position != std::string::npos && value_position < next_position,
	        context + " did not associate " + label + " with " + value + ":\n" + explanation);
}

void RequireCompleteOwnershipExplanation(const std::string &explanation, const std::string &context) {
	for (const auto &label : {"Filter Action",         "Residual Owner",
	                          "Filter Owner",          "Projection Closure",
	                          "Projection Owner",      "Ordering Owner",
	                          "Limit Owner",           "Offset Owner",
	                          "Remote Ordering",       "Runtime Ordering",
	                          "Remote Limit",          "Runtime Limit",
	                          "Remote Offset",         "Runtime Offset",
	                          "Projection Metadata",   "Generic Filter Execution",
	                          "Candidate Inspection",  "DuckDB Residual Retention",
	                          "Ordering Metadata",     "Limit Metadata",
	                          "Offset Metadata",       "Classification Category",
	                          "Classification Reason", "Classification Detail"}) {
		Require(explanation.find(label) != std::string::npos,
		        context + " omitted typed explanation field " + label + ":\n" + explanation);
	}
	Require(explanation.find("id,full_name,private,fork") != std::string::npos &&
	            explanation.find("archived,visibility") != std::string::npos,
	        context + " omitted the complete projection closure:\n" + explanation);
}

void RequireBaselineExplanation(const std::string &explanation, const std::string &context) {
	RequireCompleteOwnershipExplanation(explanation, context);
	Require(explanation.find("Relation") != std::string::npos &&
	            explanation.find("authenticated_repositories") != std::string::npos &&
	            explanation.find("Residual Owner") != std::string::npos &&
	            explanation.find("duckdb") != std::string::npos,
	        context + " did not explain the complete baseline plan:\n" + explanation);
	RequireExplainedField(explanation, "Remote Predicate", "unrestricted", "Remote Accuracy", context);
	RequireExplainedField(explanation, "Remote Accuracy", "unsupported", "Residual Predicate", context);
	RequireExplainedField(explanation, "Residual Predicate", "unrestricted", "Residual Owner", context);
	RequireExplainedField(explanation, "Classification Category", "unsupported", "Classification Reason", context);
	RequireExplainedField(explanation, "Classification Reason", "no_remote_candidate", "Classification Detail",
	                      context);
}

void RequireStructuredFallbackExplanation(const std::string &explanation, const std::string &context,
                                          const std::string &category = "unsupported", const std::string &reason = "") {
	RequireCompleteOwnershipExplanation(explanation, context);
	RequireExplainedField(explanation, "Remote Predicate", "unrestricted", "Remote Accuracy", context);
	RequireExplainedField(explanation, "Remote Accuracy", "unsupported", "Residual Predicate", context);
	RequireExplainedField(explanation, "Residual Predicate", "complete_duckdb_filter", "Residual Owner", context);
	RequireExplainedField(explanation, "Classification Category", category, "Classification Reason", context);
	Require(explanation.find("authenticated_repositories") != std::string::npos &&
	            explanation.find("Residual Owner") != std::string::npos &&
	            explanation.find("duckdb") != std::string::npos &&
	            explanation.find("Filter Action") != std::string::npos &&
	            explanation.find("retained") != std::string::npos &&
	            explanation.find("Classification Reason") != std::string::npos &&
	            (reason.empty() || explanation.find(reason) != std::string::npos),
	        context + " did not explain the structural fallback reason:\n" + explanation);
}

void RequireSelectiveExplanation(const std::string &explanation, const std::string &context,
                                 const std::string &expected_residual = "typed_equality") {
	RequireCompleteOwnershipExplanation(explanation, context);
	Require(explanation.find("Relation") != std::string::npos &&
	            explanation.find("authenticated_repositories") != std::string::npos &&
	            explanation.find("Residual Owner") != std::string::npos &&
	            explanation.find("duckdb") != std::string::npos,
	        context + " did not explain the complete selected plan:\n" + explanation);
	RequireExplainedField(explanation, "Remote Predicate", "typed_equality", "Remote Accuracy", context);
	RequireExplainedField(explanation, "Remote Accuracy", "superset", "Residual Predicate", context);
	RequireExplainedField(explanation, "Residual Predicate", expected_residual, "Residual Owner", context);
	RequireExplainedField(explanation, "Classification Category", "superset", "Classification Reason", context);
	RequireExplainedField(explanation, "Classification Reason", "selected_superset_mapping", "Classification Detail",
	                      context);
	Require(explanation.find("FILTER") != std::string::npos && explanation.find("visibility") != std::string::npos &&
	            explanation.find("private") != std::string::npos,
	        context + " removed or rewrote DuckDB's original residual filter:\n" + explanation);
}

void RequireNoRuntimeEntry(const QueryLifecycleProbe &probe, const std::string &context) {
	Require(probe.anonymous_open_calls.load(std::memory_order_relaxed) == 0 &&
	            probe.authorization_open_calls.load(std::memory_order_relaxed) == 0 &&
	            probe.streams_opened.load(std::memory_order_relaxed) == 0,
	        context + " entered Runtime during offline planning");
}

void TestExactRecognitionResidualAndFallback() {
	duckdb::DuckDB database(nullptr);
	auto probe = RegisterPackageAdapter(database, QueryRuntimeScenario::SUCCESS);
	duckdb::Connection connection(database);

	const auto baseline = Explain(connection, "SELECT * FROM " + std::string(REPOSITORY_SCAN));
	RequireBaselineExplanation(baseline, "unfiltered scan");

	const auto supported =
	    Explain(connection, "SELECT id FROM " + std::string(REPOSITORY_SCAN) + " WHERE visibility = 'private'");
	RequireSelectiveExplanation(supported, "supported equality");
	const auto reversed =
	    Explain(connection, "SELECT id FROM " + std::string(REPOSITORY_SCAN) + " WHERE 'private' = visibility");
	RequireSelectiveExplanation(reversed, "reversed supported equality");
	const auto erased_same_type_cast = Explain(connection, "SELECT id FROM " + std::string(REPOSITORY_SCAN) +
	                                                           " WHERE visibility = CAST('private' AS VARCHAR)");
	RequireSelectiveExplanation(erased_same_type_cast, "optimizer-erased same-type literal cast");
	const auto conjunction = Explain(connection, "SELECT id FROM " + std::string(REPOSITORY_SCAN) +
	                                                 " WHERE visibility = 'private' AND archived = FALSE");
	RequireSelectiveExplanation(conjunction, "supported conjunction member", "complete_duckdb_filter");
	Require(conjunction.find("Candidate") != std::string::npos && conjunction.find("and[") != std::string::npos &&
	            conjunction.find("literal:boolean:false") != std::string::npos &&
	            conjunction.find("complete visibility predicate") == std::string::npos,
	        "compound explanation lost its complete typed candidate:\n" + conjunction);
	const auto duplicate = Explain(connection, "SELECT id FROM " + std::string(REPOSITORY_SCAN) +
	                                               " WHERE visibility = 'private' AND visibility = 'private'");
	// DuckDB may simplify duplicate expressions before the callback. Query
	// translates only the structure actually offered and leaves that simplified
	// expression in DuckDB; Semantics' direct oracle owns the unsimplified
	// incompatible-candidate case.
	RequireSelectiveExplanation(duplicate, "DuckDB-simplified duplicate equality");

	const std::vector<std::string> unsupported = {
	    "private = TRUE",
	    "visibility = 'public'",
	    "visibility <> 'private'",
	    "visibility IS NULL",
	    "lower(visibility) = 'private'",
	    "CAST(visibility AS BLOB) = CAST('private' AS BLOB)",
	    "visibility = 'private' OR archived = FALSE",
	    "NOT (visibility = 'private')",
	};
	for (const auto &predicate : unsupported) {
		const auto explanation =
		    Explain(connection, "SELECT id FROM " + std::string(REPOSITORY_SCAN) + " WHERE " + predicate);
		RequireStructuredFallbackExplanation(explanation, "unsupported predicate: " + predicate);
	}

	Require(supported.find("offline_secret") == std::string::npos &&
	            supported.find("Authorization") == std::string::npos && supported.find("Bearer ") == std::string::npos,
	        "safe explanation exposed a logical secret or execution authority");
	RequireNoRuntimeEntry(*probe, "recognition, fallback, and explanation matrix");
}

void TestUnknownPredicateExplanationFailsClosed() {
	bool rejected = false;
	try {
		(void)duckdb::cuac_query_internal::PredicateNameForExplanation(static_cast<cuac::PlannedPredicate>(255));
	} catch (const duckdb::InternalException &error) {
		rejected = std::string(error.what()).find("unknown predicate state") != std::string::npos;
	}
	Require(rejected, "Query explanation did not reject an unknown planned-predicate value");
}

void TestDescribePrepareCopyAndBoundParameterReplanningStayOffline() {
	duckdb::DuckDB database(nullptr);
	auto probe = RegisterPackageAdapter(database, QueryRuntimeScenario::SUCCESS);
	duckdb::Connection connection(database);

	auto describe = connection.Query("DESCRIBE SELECT * FROM " + std::string(REPOSITORY_SCAN));
	if (describe->HasError()) {
		throw std::runtime_error("repository DESCRIBE failed: " + describe->GetError());
	}
	const std::vector<std::string> names = {"id", "full_name", "private", "fork", "archived", "visibility"};
	const std::vector<std::string> types = {"BIGINT", "VARCHAR", "BOOLEAN", "BOOLEAN", "BOOLEAN", "VARCHAR"};
	Require(describe->RowCount() == names.size(), "repository DESCRIBE did not expose the additive trailing column");
	for (duckdb::idx_t index = 0; index < names.size(); index++) {
		Require(describe->GetValue(0, index).ToString() == names[index] &&
		            describe->GetValue(1, index).ToString() == types[index],
		        "repository DESCRIBE changed stable column order or type");
	}

	auto constant_prepare = connection.Query("PREPARE visibility_private AS SELECT id FROM " +
	                                         std::string(REPOSITORY_SCAN) + " WHERE visibility = 'private'");
	if (constant_prepare->HasError()) {
		throw std::runtime_error("constant predicate PREPARE failed: " + constant_prepare->GetError());
	}
	auto parameter_prepare = connection.Query("PREPARE visibility_parameter AS SELECT id FROM " +
	                                          std::string(REPOSITORY_SCAN) + " WHERE visibility = $1");
	if (parameter_prepare->HasError()) {
		throw std::runtime_error("parameter predicate PREPARE failed: " + parameter_prepare->GetError());
	}

	const auto first_constant = Explain(connection, "EXECUTE visibility_private");
	const auto second_constant = Explain(connection, "EXECUTE visibility_private");
	RequireSelectiveExplanation(first_constant, "prepared constant predicate");
	Require(first_constant == second_constant, "repeated prepared constant explanation changed selected plan state");

	const auto first_parameter_private = Explain(connection, "EXECUTE visibility_parameter('private')");
	RequireSelectiveExplanation(first_parameter_private, "first private parameter execution");
	const auto parameter_public = Explain(connection, "EXECUTE visibility_parameter('public')");
	RequireStructuredFallbackExplanation(parameter_public, "public parameter execution");
	const auto second_parameter_private = Explain(connection, "EXECUTE visibility_parameter('private')");
	RequireSelectiveExplanation(second_parameter_private, "second private parameter execution");
	Require(first_parameter_private == second_parameter_private,
	        "private parameter executions produced different selected plans around a fallback execution");

	const auto parameter_null = Explain(connection, "EXECUTE visibility_parameter(NULL)");
	Require(parameter_null.find("typed_equality") == std::string::npos &&
	            parameter_null.find("superset") == std::string::npos,
	        "NULL parameter execution acquired selective remote authority:\n" + parameter_null);
	auto unbound = connection.Query("EXPLAIN EXECUTE visibility_parameter");
	Require(unbound->HasError(), "unbound parameter execution unexpectedly planned");
	RequireSelectiveExplanation(Explain(connection, "EXECUTE visibility_parameter('private')"),
	                            "private parameter execution after NULL and unbound values");

	const auto independent_fallback =
	    Explain(connection, "SELECT id FROM " + std::string(REPOSITORY_SCAN) + " WHERE visibility = 'public'");
	RequireStructuredFallbackExplanation(independent_fallback, "independent fallback bind copy");
	RequireSelectiveExplanation(Explain(connection, "EXECUTE visibility_private"),
	                            "constant prepared copy after fallback planning");
	RequireNoRuntimeEntry(*probe, "DESCRIBE, PREPARE, copied EXPLAIN, and bound-parameter replanning");
}

} // namespace

void RunComplexFilterAdapterTests() {
	TestExactRecognitionResidualAndFallback();
	TestUnknownPredicateExplanationFailsClosed();
	TestDescribePrepareCopyAndBoundParameterReplanningStayOffline();
}

} // namespace cuac_test
