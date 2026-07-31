#pragma once

#include "duckdb/common/common.hpp"
#include "cuac/semantics/relational_predicate.hpp"

namespace duckdb {
class Expression;
class LogicalGet;

namespace cuac_query_internal {

// Query's complete result for one DuckDB complex-filter callback. The
// candidate is a protocol-neutral immutable value; no DuckDB expression is
// retained past the callback. The retained scope describes the filter DuckDB
// continues to own, not permission to execute it outside DuckDB.
struct ComplexFilterTranslation {
	cuac::RequestedPredicate candidate;
	cuac::RetainedPredicateScope retained_scope;
};

// Converts only the structured expression classes exposed by the pinned
// DuckDB integration. Column bindings and exact BIGINT, VARCHAR, and BOOLEAN
// constants are preserved. AND, OR, and NOT positions are retained, while
// unsafe structure becomes an opaque leaf. The shared Semantics limits bound
// all work; exceeding either limit collapses the complete offered filter to a
// single unsupported candidate. This function never mutates `filters`.
ComplexFilterTranslation TranslateComplexFilters(const LogicalGet &get, const vector<unique_ptr<Expression>> &filters);

} // namespace cuac_query_internal
} // namespace duckdb
