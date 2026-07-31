#pragma once

#include "cuac/runtime/execution.hpp"
#include "cuac/internal/runtime/admission/admission_controller.hpp"
#include "cuac/internal/runtime/admission/http_plan_admission.hpp"
#include "cuac/internal/runtime/resilience/http_retry_controller.hpp"
#include "cuac/internal/runtime/transport/http_transport.hpp"

#include <memory>

namespace cuac {
namespace internal {

// Closed Remote Runtime service for a sequential paginated REST profile. Plan
// admission occurs before this construction boundary. Open owns one moved
// authorization capability and returns the ordinary BatchStream team API; no
// pagination type crosses into Query Experience.
std::unique_ptr<BatchStream>
OpenPaginatedRestScan(std::unique_ptr<const AdmittedPaginatedRestRequestProfile> admitted_profile,
                      ScanAuthorization authorization, std::shared_ptr<const HttpTransport> transport,
                      uint64_t max_wall_milliseconds, RateLimitRuntimeContext rate_limit_runtime,
                      AdmissionRuntimeContext admission_runtime, AdmissionController::Permit scan_permit,
                      ExecutionControl &control);

} // namespace internal
} // namespace cuac
