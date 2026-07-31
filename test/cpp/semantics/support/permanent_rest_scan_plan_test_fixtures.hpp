#pragma once

#include "cuac/semantics/scan_plan.hpp"

namespace cuac_test {

// Deterministic positive package-REST plan built through the production
// Connector -> Query request -> Semantics planner path. The factory performs
// no I/O and returns an anonymous immutable plan with fixed, relation-input,
// page-size, and page-number bindings plus nested structural response paths.
// Runtime consumers use this bounded service instead of constructing plan
// internals or linking Connector-private fixture access.
cuac::ScanPlan BuildValidPermanentRestScanPlanFixture();

} // namespace cuac_test
