#pragma once

#include "cuac/runtime/execution.hpp"
#include "cuac/semantics/scan_plan.hpp"
#include "cuac/internal/runtime/transport/http_transport.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cuac_test {

enum class ControlledHttpMode {
	RESPONSE,
	TRANSPORT_FAILURE,
	BLOCK_UNTIL_CANCEL,
	BLOCK_HOST_AND_RESPOND_ELSEWHERE,
	BEARER_RESPONSE_BARRIER
};

struct ControlledHttpResponse {
	uint32_t status;
	std::string body;
	std::vector<std::string> link_field_values;
	uint64_t header_bytes;
	bool transport_failure;
	std::string dependency_diagnostic;
	// Optional test-only wire accounting. Zero uses body.size(). This lets a
	// closed Runtime fixture exercise admitted byte ceilings through production
	// accounting without retaining multi-megabyte padding.
	uint64_t wire_response_bytes;
	uint64_t decompressed_response_bytes;
	bool retry_after_present;
	std::vector<cuac::internal::HttpObservedHeader> rate_limit_fields;
	std::vector<std::string> date_field_values;
	cuac::internal::HttpTransportFailureKind transport_failure_kind;
	uint32_t transport_response_status;
};

ControlledHttpResponse ControlledResponse(uint32_t status, std::string body,
                                          std::vector<std::string> link_field_values = {});
ControlledHttpResponse ControlledTransportFailure(std::string dependency_diagnostic);
ControlledHttpResponse ControlledTransientTransportFailure(cuac::internal::HttpTransportFailureKind kind);

struct ControlledRequestObservation {
	uint64_t request_count;
	std::string method;
	std::string scheme;
	std::string host;
	uint16_t port;
	std::string target;
	std::vector<std::pair<std::string, std::string>> headers;
	// Exact synthetic request material is observable only through this
	// non-installed fixture service. Production diagnostics never expose it.
	std::string body;
	std::string content_type;
	uint64_t max_request_body_bytes;
	uint64_t max_header_bytes;
	uint64_t max_response_bytes;
	uint64_t max_decompressed_bytes;
	uint64_t max_metadata_bytes;
};

// Runtime-private programmable transport fixture. Focused Runtime tests may
// inspect exact synthetic material through this type; cross-team Query tests
// consume only the dedicated service/controlled_runtime_scenario.hpp and the
// public executor contract. It is never linked into an installed target.
class ControlledHttpRuntime {
public:
	struct State;

	std::shared_ptr<const cuac::ScanExecutor> Executor() const;

	void Respond(uint32_t status, std::string body);
	// Installs a bounded response sequence keyed by request order. Exhausting the
	// sequence fails transport; responses are never replayed or reused.
	void RespondSequence(std::vector<ControlledHttpResponse> responses);
	// Isolated credential-comparison input. Exact values never enter ordinary
	// observations; ConsumeBearerExpectation releases the expected header.
	void ExpectBearer(std::string exact_authorization_header);
	bool ConsumeBearerExpectation(uint64_t expected_request_count);
	void RespondWithBearerBarrier(std::string first_header, std::string first_body, std::string second_header,
	                              std::string second_body);
	void FailWithUnknownTransportDiagnostic(std::string diagnostic);
	void BlockUntilCancelled();
	// Blocks only the exact host while returning one bounded response for every
	// other admitted destination. This lets product tests prove that a saturated
	// bulkhead cannot prevent unrelated work from reaching the same executor.
	void BlockHostAndRespondElsewhere(std::string blocked_host, uint32_t status, std::string body);
	bool WaitForRequestCount(uint64_t count, std::chrono::milliseconds timeout);
	void ReleaseBearerBarrier();
	ControlledRequestObservation Observation() const;
	std::vector<ControlledRequestObservation> Observations() const;

private:
	friend std::shared_ptr<ControlledHttpRuntime> BuildControlledHttpRuntime(uint64_t max_wall_milliseconds,
	                                                                         uint64_t max_decoded_records);
	friend std::shared_ptr<ControlledHttpRuntime> BuildControlledHttpRuntimeForHost(std::string host);
	friend std::shared_ptr<ControlledHttpRuntime> BuildControlledPackageHttpRuntime();
	ControlledHttpRuntime(std::shared_ptr<State> state, std::shared_ptr<const cuac::ScanExecutor> executor);

	std::shared_ptr<State> state;
	std::shared_ptr<const cuac::ScanExecutor> executor;
};

std::shared_ptr<ControlledHttpRuntime>
BuildControlledHttpRuntime(uint64_t max_wall_milliseconds = cuac::MAX_EXECUTION_MILLISECONDS,
                           uint64_t max_decoded_records = cuac::PAGINATION_MAX_DECODED_RECORDS_PER_PAGE);
// Binds the private controlled executor to one exact DNS host. This proves the
// per-generation construction boundary without granting a caller-selected
// host through the installed product composition.
std::shared_ptr<ControlledHttpRuntime> BuildControlledHttpRuntimeForHost(std::string host);

// Uses the same destination-neutral execution ceilings as installed product
// composition while retaining the programmable, non-installed transport.
std::shared_ptr<ControlledHttpRuntime> BuildControlledPackageHttpRuntime();

} // namespace cuac_test
