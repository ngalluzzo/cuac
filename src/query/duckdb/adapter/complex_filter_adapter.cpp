#include "cuac/internal/query/adapter/complex_filter_adapter.hpp"

#include "duckdb/common/types/value.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace duckdb {
namespace cuac_query_internal {
namespace {

struct TranslationState {
	std::size_t node_count;
	std::uint64_t next_position;
	bool exceeded_budget;
};

struct TranslationResult {
	cuac::RequestedPredicate candidate;
	bool complete;
};

struct BoundColumnIdentity {
	std::size_t declared_index;
	cuac::RequestedPredicateValueKind type;
};

bool RequestedType(const LogicalType &type, cuac::RequestedPredicateValueKind &result) {
	if (type == LogicalType::BIGINT) {
		result = cuac::RequestedPredicateValueKind::BIGINT;
		return true;
	}
	if (type == LogicalType::VARCHAR) {
		result = cuac::RequestedPredicateValueKind::VARCHAR;
		return true;
	}
	if (type == LogicalType::BOOLEAN) {
		result = cuac::RequestedPredicateValueKind::BOOLEAN;
		return true;
	}
	if (type == LogicalType::DOUBLE) {
		result = cuac::RequestedPredicateValueKind::DOUBLE;
		return true;
	}
	return false;
}

bool ResolveBoundColumn(const LogicalGet &get, const Expression &expression, BoundColumnIdentity &result) {
	if (expression.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF || get.names.empty() ||
	    get.names.size() != get.returned_types.size()) {
		return false;
	}
	const auto &column = expression.Cast<BoundColumnRefExpression>();
	if (column.depth != 0 || column.binding.table_index != get.table_index) {
		return false;
	}

	const auto &column_ids = get.GetColumnIds();
	idx_t declared_index;
	if (column_ids.empty()) {
		if (column.binding.column_index != 0) {
			return false;
		}
		declared_index = 0;
	} else {
		if (column.binding.column_index >= column_ids.size() ||
		    column_ids[column.binding.column_index].IsVirtualColumn()) {
			return false;
		}
		declared_index = column_ids[column.binding.column_index].GetPrimaryIndex();
	}
	if (declared_index >= get.returned_types.size() || expression.return_type != get.returned_types[declared_index]) {
		return false;
	}
	cuac::RequestedPredicateValueKind requested_type;
	if (!RequestedType(expression.return_type, requested_type)) {
		return false;
	}
	result = {static_cast<std::size_t>(declared_index), requested_type};
	return true;
}

bool RequestedLiteral(const Expression &expression, cuac::RequestedPredicateValueKind expected_type,
                      cuac::RequestedPredicateValue &result) {
	if (expression.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return false;
	}
	const auto &value = expression.Cast<BoundConstantExpression>().value;
	if (value.IsNull() || value.type() != expression.return_type) {
		return false;
	}
	switch (expected_type) {
	case cuac::RequestedPredicateValueKind::BIGINT:
		if (value.type() != LogicalType::BIGINT) {
			return false;
		}
		result = cuac::RequestedPredicateValue::BigInt(value.GetValue<std::int64_t>());
		return true;
	case cuac::RequestedPredicateValueKind::VARCHAR:
		if (value.type() != LogicalType::VARCHAR) {
			return false;
		}
		result = cuac::RequestedPredicateValue::Varchar(StringValue::Get(value));
		return true;
	case cuac::RequestedPredicateValueKind::BOOLEAN:
		if (value.type() != LogicalType::BOOLEAN) {
			return false;
		}
		result = cuac::RequestedPredicateValue::Boolean(value.GetValue<bool>());
		return true;
	case cuac::RequestedPredicateValueKind::DOUBLE:
		if (value.type() != LogicalType::DOUBLE) {
			return false;
		}
		result = cuac::RequestedPredicateValue::Double(value.GetValue<double>());
		return true;
	}
	return false;
}

bool TranslateComparison(const LogicalGet &get, const Expression &expression, cuac::RequestedPredicate &result) {
	if (expression.GetExpressionClass() != ExpressionClass::BOUND_COMPARISON ||
	    expression.type != ExpressionType::COMPARE_EQUAL || expression.return_type != LogicalType::BOOLEAN) {
		return false;
	}
	const auto &comparison = expression.Cast<BoundComparisonExpression>();
	BoundColumnIdentity column;
	cuac::RequestedPredicateValue literal = cuac::RequestedPredicateValue::BigInt(0);
	if (!ResolveBoundColumn(get, *comparison.left, column) ||
	    !RequestedLiteral(*comparison.right, column.type, literal)) {
		if (!ResolveBoundColumn(get, *comparison.right, column) ||
		    !RequestedLiteral(*comparison.left, column.type, literal)) {
			return false;
		}
	}
	result = cuac::RequestedPredicate::Comparison(
	    column.declared_index, column.type, cuac::RequestedPredicateComparisonOperator::EQUALS, std::move(literal));
	return true;
}

TranslationResult Unsupported(std::uint64_t position) {
	return {cuac::RequestedPredicate::Unsupported(position), false};
}

TranslationResult TranslateExpression(const LogicalGet &get, const Expression &expression, std::size_t depth,
                                      TranslationState &state) {
	const auto position = state.next_position++;
	if (depth > cuac::MAX_REQUESTED_PREDICATE_DEPTH || state.node_count >= cuac::MAX_REQUESTED_PREDICATE_NODES) {
		state.exceeded_budget = true;
		return Unsupported(position);
	}
	state.node_count++;

	cuac::RequestedPredicate comparison;
	if (TranslateComparison(get, expression, comparison)) {
		return {std::move(comparison), true};
	}

	if (expression.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION &&
	    (expression.type == ExpressionType::CONJUNCTION_AND || expression.type == ExpressionType::CONJUNCTION_OR)) {
		const auto &conjunction = expression.Cast<BoundConjunctionExpression>();
		if (conjunction.children.size() < 2) {
			return Unsupported(position);
		}
		std::vector<cuac::RequestedPredicate> children;
		children.reserve(conjunction.children.size());
		bool complete = true;
		for (const auto &child : conjunction.children) {
			if (!child) {
				return Unsupported(position);
			}
			auto translated = TranslateExpression(get, *child, depth + 1, state);
			complete = complete && translated.complete;
			children.push_back(std::move(translated.candidate));
			if (state.exceeded_budget) {
				return Unsupported(position);
			}
		}
		if (expression.type == ExpressionType::CONJUNCTION_AND) {
			return {cuac::RequestedPredicate::Conjunction(std::move(children)), complete};
		}
		return {cuac::RequestedPredicate::Disjunction(std::move(children)), complete};
	}

	if (expression.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR &&
	    expression.type == ExpressionType::OPERATOR_NOT) {
		const auto &operation = expression.Cast<BoundOperatorExpression>();
		if (operation.children.size() != 1 || !operation.children.front()) {
			return Unsupported(position);
		}
		auto translated = TranslateExpression(get, *operation.children.front(), depth + 1, state);
		if (state.exceeded_budget) {
			return Unsupported(position);
		}
		return {cuac::RequestedPredicate::Negation(std::move(translated.candidate)), translated.complete};
	}

	return Unsupported(position);
}

} // namespace

ComplexFilterTranslation TranslateComplexFilters(const LogicalGet &get, const vector<unique_ptr<Expression>> &filters) {
	std::size_t present = 0;
	for (const auto &filter : filters) {
		present += filter ? 1 : 0;
	}
	if (present == 0) {
		return {cuac::RequestedPredicate::Unrestricted(), cuac::RetainedPredicateScope::UNRESTRICTED};
	}

	TranslationState state = {present > 1 ? 1U : 0U, 0, false};
	if (state.node_count > cuac::MAX_REQUESTED_PREDICATE_NODES) {
		return {cuac::RequestedPredicate::Unsupported(0), cuac::RetainedPredicateScope::COMPLETE_DUCKDB_FILTER};
	}
	std::vector<cuac::RequestedPredicate> translated_filters;
	translated_filters.reserve(present);
	bool complete = true;
	const auto child_depth = present > 1 ? 2U : 1U;
	for (const auto &filter : filters) {
		if (!filter) {
			continue;
		}
		auto translated = TranslateExpression(get, *filter, child_depth, state);
		complete = complete && translated.complete;
		translated_filters.push_back(std::move(translated.candidate));
		if (state.exceeded_budget) {
			return {cuac::RequestedPredicate::Unsupported(0), cuac::RetainedPredicateScope::COMPLETE_DUCKDB_FILTER};
		}
	}

	cuac::RequestedPredicate candidate = translated_filters.size() == 1
	                                         ? std::move(translated_filters.front())
	                                         : cuac::RequestedPredicate::Conjunction(std::move(translated_filters));
	return {std::move(candidate), complete ? cuac::RetainedPredicateScope::REQUESTED_PREDICATE
	                                       : cuac::RetainedPredicateScope::COMPLETE_DUCKDB_FILTER};
}

} // namespace cuac_query_internal
} // namespace duckdb
