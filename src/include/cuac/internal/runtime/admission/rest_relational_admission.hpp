#pragma once

#include "cuac/semantics/scan_plan.hpp"

#include <cstdint>
#include <string>

namespace cuac {
namespace internal {

// Call-scoped authority for the sole predicate-derived REST query binding.
// The value copies only the typed equality facts request materialization must
// match; it carries no query name, encoded bytes, proof prose, or plan owner.
struct RestConditionalBindingAuthority {
	RestConditionalBindingAuthority();

	bool enabled;
	std::string source_id;
	PlannedRestScalarKind kind;
	bool boolean_value;
	std::int64_t bigint_value;
	std::string varchar_value;
	double double_value;
};

// Validates only the relational ownership and predicate envelope. On success,
// authority is enabled exactly for a selected package-independent typed
// equality. Request bytes, transport authority, pagination, and provenance are
// deliberately out of scope so changes to those contracts cannot widen this
// decision.
bool TryAdmitRestRelationalEnvelope(const ScanPlan &plan, RestConditionalBindingAuthority &authority);

} // namespace internal
} // namespace cuac
