#pragma once

#include "cuac/runtime/execution.hpp"
#include "cuac/internal/runtime/resilience/rate_limit_policy_admission.hpp"
#include "cuac/internal/runtime/transport/http_transport.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cuac {
namespace internal {

struct HttpExecutionProfile;

struct AdmittedGraphqlColumn {
	std::string name;
	OutputValueType type;
	bool nullable;
	std::vector<std::string> response_path;
};

// Immutable authority for one forward sequential GraphQL traversal. Admission
// copies the exact endpoint, public headers, generated document, variables,
// response paths, typed nullable columns, cursor contract, and page/scan
// budgets from a completely validated plan. Streams retain only this profile;
// provenance names, classification labels, explanations, source bytes, and
// credentials never enter it. Read-only copies are safe across stream-owned
// lifetimes, while cancellation and close remain the stream's responsibility.
class AdmittedGraphqlRequestProfile {
public:
	AdmittedGraphqlRequestProfile(const AdmittedGraphqlRequestProfile &) = default;
	AdmittedGraphqlRequestProfile(AdmittedGraphqlRequestProfile &&) = default;
	AdmittedGraphqlRequestProfile &operator=(const AdmittedGraphqlRequestProfile &) = delete;
	AdmittedGraphqlRequestProfile &operator=(AdmittedGraphqlRequestProfile &&) = delete;

	const std::string &Method() const;
	const std::string &Scheme() const;
	const std::string &Host() const;
	uint16_t Port() const;
	const std::string &Path() const;
	const std::vector<HttpHeader> &Headers() const;
	const std::string &Document() const;
	const std::vector<AdmittedGraphqlColumn> &Columns() const;
	const std::string &PageSizeVariable() const;
	const std::string &CursorVariable() const;
	const std::vector<std::string> &NodesPath() const;
	const std::vector<std::string> &ErrorsPath() const;
	const std::vector<std::string> &PageInfoPath() const;
	const std::vector<std::string> &HasNextPagePath() const;
	const std::vector<std::string> &EndCursorPath() const;
	uint64_t PageSize() const;
	uint64_t MaxPages() const;
	uint64_t MaxRequestBodyBytes() const;
	uint64_t MaxScanBodyBytes() const;
	bool RequiresBearer() const;
	const ResourceBudgets &PageBudgets() const;
	const ScanResourceBudgets &ScanBudgets() const;
	const RetryPlan &RetryPolicy() const;
	const AdmittedRateLimitPolicy &RateLimitPolicy() const;
	const AdmittedResiliencePolicy &ResiliencePolicy() const;

private:
	friend std::unique_ptr<const AdmittedGraphqlRequestProfile> TryAdmitGraphqlPlan(const ScanPlan &,
	                                                                                const HttpExecutionProfile &);
	AdmittedGraphqlRequestProfile(const ScanPlan &plan, bool requires_bearer, RetryPlan retry,
	                              AdmittedRateLimitPolicy rate_limit, AdmittedResiliencePolicy resilience);

	std::string method;
	std::string scheme;
	std::string host;
	uint16_t port;
	std::string path;
	std::vector<HttpHeader> headers;
	std::string document;
	std::vector<AdmittedGraphqlColumn> columns;
	std::string page_size_variable;
	std::string cursor_variable;
	std::vector<std::string> nodes_path;
	std::vector<std::string> errors_path;
	std::vector<std::string> page_info_path;
	std::vector<std::string> has_next_page_path;
	std::vector<std::string> end_cursor_path;
	uint64_t page_size;
	uint64_t max_pages;
	uint64_t max_request_body_bytes;
	uint64_t max_scan_body_bytes;
	bool requires_bearer;
	ResourceBudgets page_budgets;
	ScanResourceBudgets scan_budgets;
	RetryPlan retry;
	AdmittedRateLimitPolicy rate_limit;
	AdmittedResiliencePolicy resilience;
};

std::unique_ptr<const AdmittedGraphqlRequestProfile> TryAdmitGraphqlPlan(const ScanPlan &plan,
                                                                         const HttpExecutionProfile &profile);
// A null result is a complete pre-authorization policy/contract denial.
// Allocation exhaustion is the sole structured error; admission performs no
// credential materialization, request serialization, or transport I/O.

} // namespace internal
} // namespace cuac
