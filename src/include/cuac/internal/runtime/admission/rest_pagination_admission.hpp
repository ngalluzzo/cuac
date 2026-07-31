#pragma once

#include "cuac/internal/runtime/admission/http_plan_admission.hpp"

#include <vector>

namespace cuac {
namespace internal {

struct HttpExecutionProfile;

// Validates Link traversal, page bindings, signed BIGINT progression, and
// aggregate budgets after common REST request materialization has succeeded.
bool HasSupportedRestPagination(const ScanPlan &plan, const HttpExecutionProfile &profile,
                                const std::vector<AdmittedQueryParameter> &query);

} // namespace internal
} // namespace cuac
