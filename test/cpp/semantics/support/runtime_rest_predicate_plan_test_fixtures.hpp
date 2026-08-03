#pragma once

#include "cuac/semantics/scan_plan.hpp"

namespace cuac_test {

// Closed malformed values for Runtime's REST predicate admission boundary.
// Each variant changes exactly one conditional-binding law from the valid
// planner-produced EXACT fixture. Consumers cannot author arbitrary ScanPlan
// internals through this service.
enum class RuntimeRestPredicatePlanCounterexample {
	CONDITIONAL_SOURCE_ID,
	CONDITIONAL_SCALAR_KIND,
	CONDITIONAL_TYPED_VALUE,
	NONCANONICAL_ENCODED_VALUE,
	DUPLICATE_CONDITIONAL_BINDING,
	COUNT
};

// Closed malformed values for Runtime's permanent REST response-schema
// correlation boundary. Each value starts from a production-planned ARRAY
// relation and changes one agreement law between the protocol operation and
// the protocol-neutral output schema.
enum class RuntimeRestSchemaCounterexample {
	RESULT_NAME,
	RESULT_SHAPE,
	RESULT_ELEMENT_KIND,
	RESULT_ELEMENT_NULLABILITY,
	RESULT_OUTER_NULLABILITY,
	RESULT_PATH,
	RESULT_ARITY,
	RESULT_ORDER,
	OUTPUT_NAME,
	OUTPUT_NAME_ORDER,
	OUTPUT_ARITY,
	OUTPUT_SHAPE,
	COUNT
};

enum class RuntimeRestPathPlanCounterexample {
	RENDERED_PATH,
	SOURCE_ID,
	SCALAR_KIND,
	TYPED_VALUE,
	ENCODED_VALUE,
	ENCODING,
	DUPLICATE_INPUT,
	SEGMENT_ROLE,
	SEGMENT_ORDER,
	SEGMENT_COUNT,
	COUNT
};

// Deterministic package REST plan built through the production Connector ->
// Query request -> Semantics planner path. It carries one BIGINT conditional
// binding, an EXACT occurrence proof, and a DuckDB-owned typed residual.
cuac::ScanPlan BuildRuntimeExactRestPredicatePlanFixture();

// RFC 0020: the same EXACT-occurrence shape as BuildRuntimeExactRestPredicatePlanFixture,
// but over the double_predicates relation's DOUBLE conditional binding, proving
// a DOUBLE predicate actually reaches the remote request rather than only the
// DuckDB residual fallback.
cuac::ScanPlan BuildRuntimeExactDoubleRestPredicatePlanFixture();

// A matching package predicate whose selected operation remains eligible when
// selective-predicate capability is unavailable. The real planner retains the
// typed DuckDB residual and emits no conditional request binding.
cuac::ScanPlan BuildRuntimeResidualOnlyRestPredicatePlanFixture();

// Deliberately malformed immutable value for negative Runtime admission tests.
// Friend access is confined to the implementation and cannot escape as a
// general builder.
cuac::ScanPlan BuildRuntimeRestPredicatePlanCounterexample(RuntimeRestPredicatePlanCounterexample counterexample);

// Deliberately malformed immutable ARRAY plan for negative Runtime admission
// tests. Runtime must reject every variant before request construction or
// transport.
cuac::ScanPlan BuildRuntimeRestSchemaCounterexample(RuntimeRestSchemaCounterexample counterexample);

cuac::ScanPlan BuildRuntimeStructuralPathPlanFixture();
cuac::ScanPlan BuildRuntimeRestPathPlanCounterexample(RuntimeRestPathPlanCounterexample counterexample);

} // namespace cuac_test
