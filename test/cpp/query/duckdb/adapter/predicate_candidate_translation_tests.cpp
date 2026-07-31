#include "cuac/internal/query/adapter/complex_filter_adapter.hpp"

#include "duckdb/common/types/value.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "support/require.hpp"

#include <cstddef>
#include <string>
#include <utility>

namespace cuac_test {
namespace {

using cuac_test::Require;

duckdb::unique_ptr<duckdb::LogicalGet> RepositoryGet() {
	duckdb::vector<duckdb::LogicalType> types = {
	    duckdb::LogicalType::BIGINT,  duckdb::LogicalType::VARCHAR, duckdb::LogicalType::BOOLEAN,
	    duckdb::LogicalType::BOOLEAN, duckdb::LogicalType::BOOLEAN, duckdb::LogicalType::VARCHAR,
	    duckdb::LogicalType::DOUBLE,
	};
	duckdb::vector<duckdb::string> names = {"id", "full_name", "private", "fork", "archived", "visibility", "score"};
	auto result =
	    duckdb::make_uniq<duckdb::LogicalGet>(71, duckdb::TableFunction(), nullptr, std::move(types), std::move(names));
	duckdb::vector<duckdb::ColumnIndex> columns;
	for (duckdb::idx_t index = 0; index < 7; index++) {
		columns.emplace_back(index);
	}
	result->SetColumnIds(std::move(columns));
	return result;
}

duckdb::unique_ptr<duckdb::Expression> Column(duckdb::idx_t local_index, const duckdb::LogicalType &type,
                                              duckdb::idx_t table_index = 71) {
	return duckdb::make_uniq<duckdb::BoundColumnRefExpression>(type, duckdb::ColumnBinding(table_index, local_index));
}

duckdb::unique_ptr<duckdb::Expression> Constant(duckdb::Value value) {
	return duckdb::make_uniq<duckdb::BoundConstantExpression>(std::move(value));
}

duckdb::unique_ptr<duckdb::Expression> Equals(duckdb::unique_ptr<duckdb::Expression> left,
                                              duckdb::unique_ptr<duckdb::Expression> right) {
	return duckdb::make_uniq<duckdb::BoundComparisonExpression>(duckdb::ExpressionType::COMPARE_EQUAL, std::move(left),
	                                                            std::move(right));
}

duckdb::unique_ptr<duckdb::Expression> NotEquals(duckdb::unique_ptr<duckdb::Expression> left,
                                                 duckdb::unique_ptr<duckdb::Expression> right) {
	return duckdb::make_uniq<duckdb::BoundComparisonExpression>(duckdb::ExpressionType::COMPARE_NOTEQUAL,
	                                                            std::move(left), std::move(right));
}

duckdb::unique_ptr<duckdb::Expression> BooleanNode(duckdb::ExpressionType type,
                                                   duckdb::vector<duckdb::unique_ptr<duckdb::Expression>> &&children) {
	auto result = duckdb::make_uniq<duckdb::BoundConjunctionExpression>(type);
	result->children = std::move(children);
	return std::move(result);
}

duckdb::unique_ptr<duckdb::Expression> Negation(duckdb::unique_ptr<duckdb::Expression> child) {
	auto result = duckdb::make_uniq<duckdb::BoundOperatorExpression>(duckdb::ExpressionType::OPERATOR_NOT,
	                                                                 duckdb::LogicalType::BOOLEAN);
	result->children.push_back(std::move(child));
	return std::move(result);
}

duckdb::unique_ptr<duckdb::Expression> VisibilityPrivate(bool reversed = false) {
	auto column = Column(5, duckdb::LogicalType::VARCHAR);
	auto literal = Constant(duckdb::Value("private"));
	return reversed ? Equals(std::move(literal), std::move(column)) : Equals(std::move(column), std::move(literal));
}

duckdb::unique_ptr<duckdb::Expression> Archived(bool value) {
	return Equals(Column(4, duckdb::LogicalType::BOOLEAN), Constant(duckdb::Value::BOOLEAN(value)));
}

duckdb::unique_ptr<duckdb::Expression> Id(std::int64_t value) {
	return Equals(Column(0, duckdb::LogicalType::BIGINT), Constant(duckdb::Value::BIGINT(value)));
}

duckdb::unique_ptr<duckdb::Expression> Score(double value) {
	return Equals(Column(6, duckdb::LogicalType::DOUBLE), Constant(duckdb::Value::DOUBLE(value)));
}

duckdb::cuac_query_internal::ComplexFilterTranslation
Translate(const duckdb::LogicalGet &get, const duckdb::vector<duckdb::unique_ptr<duckdb::Expression>> &filters) {
	return duckdb::cuac_query_internal::TranslateComplexFilters(get, filters);
}

void TestTypedBindingsAndReversedEquality() {
	auto get = RepositoryGet();
	duckdb::vector<duckdb::unique_ptr<duckdb::Expression>> filters;
	filters.push_back(VisibilityPrivate(true));
	auto translated = Translate(*get, filters);
	Require(translated.retained_scope == cuac::RetainedPredicateScope::REQUESTED_PREDICATE,
	        "typed equality did not retain its complete offered scope");
	Require(translated.candidate.Snapshot() ==
	            "comparison[column:5,type:varchar,operator:equals,literal:varchar:hex:70726976617465]",
	        "reversed VARCHAR equality lost its declared binding or typed literal");

	filters.clear();
	filters.push_back(Id(-7));
	translated = Translate(*get, filters);
	Require(translated.candidate.Snapshot() == "comparison[column:0,type:bigint,operator:equals,literal:bigint:-7]",
	        "BIGINT equality lost its exact signed literal identity");
	filters.clear();
	filters.push_back(Archived(false));
	translated = Translate(*get, filters);
	Require(translated.candidate.Snapshot() ==
	            "comparison[column:4,type:boolean,operator:equals,literal:boolean:false]",
	        "BOOLEAN equality lost its exact literal identity");
	filters.clear();
	filters.push_back(Score(3.5));
	translated = Translate(*get, filters);
	Require(translated.candidate.Snapshot() == "comparison[column:6,type:double,operator:equals,literal:double:3.5]",
	        "DOUBLE equality lost its exact literal identity");
}

void TestBooleanStructureAndOpaquePositions() {
	auto get = RepositoryGet();
	duckdb::vector<duckdb::unique_ptr<duckdb::Expression>> or_children;
	or_children.push_back(Archived(false));
	or_children.push_back(Negation(Id(4)));
	duckdb::vector<duckdb::unique_ptr<duckdb::Expression>> and_children;
	and_children.push_back(VisibilityPrivate());
	and_children.push_back(BooleanNode(duckdb::ExpressionType::CONJUNCTION_OR, std::move(or_children)));
	duckdb::vector<duckdb::unique_ptr<duckdb::Expression>> filters;
	filters.push_back(BooleanNode(duckdb::ExpressionType::CONJUNCTION_AND, std::move(and_children)));
	const auto structured = Translate(*get, filters);
	Require(structured.retained_scope == cuac::RetainedPredicateScope::REQUESTED_PREDICATE &&
	            structured.candidate.Snapshot() ==
	                "and[comparison[column:5,type:varchar,operator:equals,literal:varchar:hex:70726976617465],"
	                "or[comparison[column:4,type:boolean,operator:equals,literal:boolean:false],"
	                "not[comparison[column:0,type:bigint,operator:equals,literal:bigint:4]]]]",
	        "AND/OR/NOT structure did not survive typed translation");

	duckdb::vector<duckdb::unique_ptr<duckdb::Expression>> unsupported_or;
	unsupported_or.push_back(VisibilityPrivate());
	unsupported_or.push_back(
	    NotEquals(Column(4, duckdb::LogicalType::BOOLEAN), Constant(duckdb::Value::BOOLEAN(false))));
	filters.clear();
	filters.push_back(BooleanNode(duckdb::ExpressionType::CONJUNCTION_OR, std::move(unsupported_or)));
	const auto opaque = Translate(*get, filters);
	Require(opaque.retained_scope == cuac::RetainedPredicateScope::COMPLETE_DUCKDB_FILTER &&
	            opaque.candidate.Snapshot() ==
	                "or[comparison[column:5,type:varchar,operator:equals,literal:varchar:hex:70726976617465],"
	                "unsupported[position:2]]",
	        "unsupported comparison lost its Boolean position or widened the translated branch");
}

void TestBindingAndNullFailuresStayOpaque() {
	auto get = RepositoryGet();
	duckdb::vector<duckdb::unique_ptr<duckdb::Expression>> filters;
	filters.push_back(Equals(Column(5, duckdb::LogicalType::VARCHAR, 72), Constant(duckdb::Value("private"))));
	auto wrong_table = Translate(*get, filters);
	Require(wrong_table.candidate == cuac::RequestedPredicate::Unsupported(0) &&
	            wrong_table.retained_scope == cuac::RetainedPredicateScope::COMPLETE_DUCKDB_FILTER,
	        "foreign table binding acquired candidate authority");

	filters.clear();
	filters.push_back(
	    Equals(Column(5, duckdb::LogicalType::VARCHAR), Constant(duckdb::Value(duckdb::LogicalType::VARCHAR))));
	auto null_literal = Translate(*get, filters);
	Require(null_literal.candidate == cuac::RequestedPredicate::Unsupported(0),
	        "NULL constant acquired typed comparison authority");
}

void TestSharedStructuralBudgetsFailClosed() {
	auto get = RepositoryGet();
	auto depth_sixteen = VisibilityPrivate();
	for (std::size_t index = 1; index < cuac::MAX_REQUESTED_PREDICATE_DEPTH; index++) {
		depth_sixteen = Negation(std::move(depth_sixteen));
	}
	duckdb::vector<duckdb::unique_ptr<duckdb::Expression>> filters;
	filters.push_back(std::move(depth_sixteen));
	auto admitted = Translate(*get, filters);
	Require(admitted.candidate.Depth() == cuac::MAX_REQUESTED_PREDICATE_DEPTH &&
	            admitted.retained_scope == cuac::RetainedPredicateScope::REQUESTED_PREDICATE,
	        "candidate at the shared depth boundary was not preserved");

	auto depth_seventeen = VisibilityPrivate();
	for (std::size_t index = 0; index < cuac::MAX_REQUESTED_PREDICATE_DEPTH; index++) {
		depth_seventeen = Negation(std::move(depth_seventeen));
	}
	filters.clear();
	filters.push_back(std::move(depth_seventeen));
	auto depth_overflow = Translate(*get, filters);
	Require(depth_overflow.candidate == cuac::RequestedPredicate::Unsupported(0) &&
	            depth_overflow.retained_scope == cuac::RetainedPredicateScope::COMPLETE_DUCKDB_FILTER,
	        "over-depth filter was truncated instead of collapsing completely");

	filters.clear();
	for (std::size_t index = 0; index < cuac::MAX_REQUESTED_PREDICATE_NODES - 1; index++) {
		filters.push_back(Id(static_cast<std::int64_t>(index)));
	}
	auto node_boundary = Translate(*get, filters);
	Require(node_boundary.candidate.NodeCount() == cuac::MAX_REQUESTED_PREDICATE_NODES,
	        "candidate at the shared node boundary was not preserved");
	filters.clear();
	for (std::size_t index = 0; index < cuac::MAX_REQUESTED_PREDICATE_NODES; index++) {
		filters.push_back(Id(static_cast<std::int64_t>(index)));
	}
	auto node_overflow = Translate(*get, filters);
	Require(node_overflow.candidate == cuac::RequestedPredicate::Unsupported(0) &&
	            node_overflow.retained_scope == cuac::RetainedPredicateScope::COMPLETE_DUCKDB_FILTER,
	        "over-node filter selected a partial conjunction");
}

void TestAbsentFilterIsTrueWithoutDuckdbState() {
	auto get = RepositoryGet();
	duckdb::vector<duckdb::unique_ptr<duckdb::Expression>> filters;
	const auto translated = Translate(*get, filters);
	Require(translated.candidate == cuac::RequestedPredicate::Unrestricted() &&
	            translated.retained_scope == cuac::RetainedPredicateScope::UNRESTRICTED,
	        "absent filter did not remain the unrestricted base candidate");
}

} // namespace

void RunPredicateCandidateTranslationTests() {
	TestTypedBindingsAndReversedEquality();
	TestBooleanStructureAndOpaquePositions();
	TestBindingAndNullFailuresStayOpaque();
	TestSharedStructuralBudgetsFailClosed();
	TestAbsentFilterIsTrueWithoutDuckdbState();
}

} // namespace cuac_test
