#include "runtime/support/controlled_http_transport.hpp"

#include "runtime/support/package_fixture_checkpoint.hpp"

#include "cuac/internal/runtime/executor/http_scan_executor.hpp"
#include "cuac/internal/runtime/transport/http_transport.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace cuac_test {

struct ControlledHttpRuntime::State {
	State()
	    : mode(ControlledHttpMode::RESPONSE), status(200), next_response(0), expected_bearer_matches(0),
	      unexpected_bearer(false), bearer_barrier_released(true) {
		observation.request_count = 0;
		observation.port = 0;
		observation.max_request_body_bytes = 0;
		observation.max_header_bytes = 0;
		observation.max_response_bytes = 0;
		observation.max_decompressed_bytes = 0;
		observation.max_metadata_bytes = 0;
	}

	mutable std::mutex mutex;
	std::condition_variable condition;
	ControlledHttpMode mode;
	uint32_t status;
	std::string body;
	std::string diagnostic;
	std::string blocked_host;
	std::vector<ControlledHttpResponse> responses;
	std::size_t next_response;
	std::string expected_bearer;
	uint64_t expected_bearer_matches;
	bool unexpected_bearer;
	ControlledRequestObservation observation;
	std::vector<ControlledRequestObservation> observations;
	std::vector<std::pair<std::string, std::string>> bearer_responses;
	bool bearer_barrier_released;
};

namespace {

uint64_t RetainedMetadataBytes(const std::vector<std::string> &values) {
	uint64_t result = static_cast<uint64_t>(values.capacity()) * sizeof(std::string);
	for (const auto &value : values) {
		const auto begin = reinterpret_cast<std::uintptr_t>(&value);
		const auto end = begin + sizeof(value);
		const auto data = reinterpret_cast<std::uintptr_t>(value.data());
		if (data < begin || data >= end) {
			result += static_cast<uint64_t>(value.capacity()) + 1;
		}
	}
	return result;
}

uint64_t RetainedMetadataBytes(const std::vector<cuac::internal::HttpObservedHeader> &fields) {
	uint64_t result = static_cast<uint64_t>(fields.capacity()) * sizeof(cuac::internal::HttpObservedHeader);
	for (const auto &field : fields) {
		result += static_cast<uint64_t>(field.name.capacity()) + 1;
		result += static_cast<uint64_t>(field.value.capacity()) + 1;
	}
	return result;
}

class ControlledTransport final : public cuac::internal::HttpTransport {
public:
	explicit ControlledTransport(std::shared_ptr<ControlledHttpRuntime::State> state_p) : state(std::move(state_p)) {
	}

	cuac::internal::HttpResponse Get(const cuac::internal::HttpRequest &request,
	                                 const cuac::internal::HttpLimits &limits,
	                                 cuac::ExecutionControl &control) const override {
		return Respond(request, limits, control);
	}

	cuac::internal::HttpResponse Post(const cuac::internal::HttpRequest &request,
	                                  const cuac::internal::HttpLimits &limits,
	                                  cuac::ExecutionControl &control) const override {
		return Respond(request, limits, control);
	}

private:
	cuac::internal::HttpResponse Respond(const cuac::internal::HttpRequest &request,
	                                     const cuac::internal::HttpLimits &limits,
	                                     cuac::ExecutionControl &control) const {
		ControlledHttpMode mode;
		uint32_t status;
		std::string body;
		std::string diagnostic;
		std::vector<std::string> link_field_values;
		std::vector<cuac::internal::HttpObservedHeader> rate_limit_fields;
		std::vector<std::string> date_field_values;
		uint64_t header_bytes = 64;
		uint64_t wire_response_bytes = 0;
		uint64_t decompressed_response_bytes = 0;
		bool retry_after_present = false;
		cuac::internal::HttpTransportFailureKind transport_failure_kind =
		    cuac::internal::HttpTransportFailureKind::OTHER;
		uint32_t transport_response_status = 0;
		bool scripted_transport_failure = false;
		bool wait_for_bearer_barrier = false;
		bool block_request = false;
		{
			std::lock_guard<std::mutex> guard(state->mutex);
			state->observation.request_count++;
			state->observation.method = request.method;
			state->observation.scheme = request.scheme;
			state->observation.host = request.host;
			state->observation.port = request.port;
			state->observation.target = request.target;
			state->observation.body = request.body;
			state->observation.content_type = request.content_type;
			state->observation.headers.clear();
			for (std::size_t index = 0; index < request.headers.size(); index++) {
				if (request.headers[index].name == "Authorization" && !state->expected_bearer.empty()) {
					if (request.headers[index].value == state->expected_bearer) {
						state->expected_bearer_matches++;
					} else {
						state->unexpected_bearer = true;
					}
				}
				state->observation.headers.push_back(
				    std::make_pair(request.headers[index].name, request.headers[index].name == "Authorization"
				                                                    ? std::string("<redacted>")
				                                                    : request.headers[index].value));
			}
			state->observation.max_request_body_bytes = limits.max_request_body_bytes;
			state->observation.max_header_bytes = limits.max_header_bytes;
			state->observation.max_response_bytes = limits.max_response_bytes;
			state->observation.max_decompressed_bytes = limits.max_decompressed_bytes;
			state->observation.max_metadata_bytes = limits.max_metadata_bytes;
			state->observations.push_back(state->observation);
			mode = state->mode;
			status = state->status;
			block_request =
			    mode == ControlledHttpMode::BLOCK_UNTIL_CANCEL ||
			    (mode == ControlledHttpMode::BLOCK_HOST_AND_RESPOND_ELSEWHERE && request.host == state->blocked_host);
			if (mode == ControlledHttpMode::BEARER_RESPONSE_BARRIER) {
				wait_for_bearer_barrier = true;
				for (std::size_t response_index = 0; response_index < state->bearer_responses.size();
				     response_index++) {
					for (std::size_t header_index = 0; header_index < request.headers.size(); header_index++) {
						if (request.headers[header_index].name == "Authorization" &&
						    request.headers[header_index].value == state->bearer_responses[response_index].first) {
							body = state->bearer_responses[response_index].second;
						}
					}
				}
			} else if (!state->responses.empty()) {
				if (state->next_response >= state->responses.size()) {
					mode = ControlledHttpMode::TRANSPORT_FAILURE;
					diagnostic = "controlled response sequence exhausted";
				} else {
					const auto &scripted = state->responses[state->next_response++];
					status = scripted.status;
					body = scripted.body;
					link_field_values = scripted.link_field_values;
					header_bytes = scripted.header_bytes;
					scripted_transport_failure = scripted.transport_failure;
					diagnostic = scripted.dependency_diagnostic;
					wire_response_bytes = scripted.wire_response_bytes;
					decompressed_response_bytes = scripted.decompressed_response_bytes;
					retry_after_present = scripted.retry_after_present;
					for (const auto &field : scripted.rate_limit_fields) {
						if (std::find(limits.retained_header_names.begin(), limits.retained_header_names.end(),
						              field.name) != limits.retained_header_names.end()) {
							rate_limit_fields.push_back(field);
						}
					}
					if (limits.retain_date) {
						date_field_values = scripted.date_field_values;
					}
					transport_failure_kind = scripted.transport_failure_kind;
					transport_response_status = scripted.transport_response_status;
				}
			} else {
				body = state->body;
			}
			if (diagnostic.empty()) {
				diagnostic = state->diagnostic;
			}
		}
		state->condition.notify_all();

		if (wait_for_bearer_barrier) {
			if (body.empty()) {
				throw std::runtime_error("controlled bearer response was not configured");
			}
			std::unique_lock<std::mutex> guard(state->mutex);
			while (!state->bearer_barrier_released && !control.IsCancellationRequested() &&
			       std::chrono::steady_clock::now() < limits.deadline) {
				state->condition.wait_for(guard, std::chrono::milliseconds(1));
			}
			if (control.IsCancellationRequested()) {
				throw cuac::ExecutionCancelled();
			}
			if (!state->bearer_barrier_released) {
				throw cuac::ExecutionError(cuac::ErrorStage::RESOURCE, "wall_milliseconds",
				                           "execution exceeded its wall-time budget");
			}
		}

		if (mode == ControlledHttpMode::TRANSPORT_FAILURE || scripted_transport_failure) {
			if (scripted_transport_failure) {
				throw cuac::internal::HttpAttemptFailure(
				    transport_failure_kind,
				    {transport_response_status, header_bytes, wire_response_bytes, decompressed_response_bytes},
				    cuac::ExecutionError(cuac::ErrorStage::TRANSPORT, "", "HTTP request failed"));
			}
			throw std::runtime_error(diagnostic);
		}
		if (block_request) {
			while (!control.IsCancellationRequested() && std::chrono::steady_clock::now() < limits.deadline) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			if (control.IsCancellationRequested()) {
				throw cuac::ExecutionCancelled();
			}
			throw cuac::ExecutionError(cuac::ErrorStage::RESOURCE, "wall_milliseconds",
			                           "execution exceeded its wall-time budget");
		}
		if (wire_response_bytes == 0) {
			wire_response_bytes = static_cast<uint64_t>(body.size());
		}
		if (decompressed_response_bytes == 0) {
			decompressed_response_bytes = static_cast<uint64_t>(body.size());
		}
		if (wire_response_bytes > limits.max_response_bytes) {
			throw cuac::ExecutionError(cuac::ErrorStage::RESOURCE, "response_bytes",
			                           "HTTP response exceeded its byte budget");
		}
		if (static_cast<uint64_t>(body.size()) > limits.max_decompressed_bytes) {
			throw cuac::ExecutionError(cuac::ErrorStage::RESOURCE, "decompressed_bytes",
			                           "HTTP response exceeded its decompressed-byte budget");
		}
		if (limits.max_metadata_bytes == 0) {
			std::vector<std::string>().swap(link_field_values);
		}
		const auto metadata_bytes = RetainedMetadataBytes(link_field_values) +
		                            RetainedMetadataBytes(rate_limit_fields) + RetainedMetadataBytes(date_field_values);
		if (metadata_bytes > limits.max_metadata_bytes) {
			throw cuac::ExecutionError(cuac::ErrorStage::RESOURCE, "decoded_memory_bytes",
			                           "HTTP response metadata exceeded its memory budget");
		}
		NotifyRuntimeFixtureResponseReady(control);
		return {status,
		        header_bytes,
		        wire_response_bytes,
		        decompressed_response_bytes,
		        std::move(body),
		        {std::move(link_field_values), metadata_bytes, retry_after_present, std::move(rate_limit_fields),
		         std::move(date_field_values)}};
	}

	const std::shared_ptr<ControlledHttpRuntime::State> state;
};

} // namespace

ControlledHttpRuntime::ControlledHttpRuntime(std::shared_ptr<State> state_p,
                                             std::shared_ptr<const cuac::ScanExecutor> executor_p)
    : state(std::move(state_p)), executor(std::move(executor_p)) {
}

std::shared_ptr<const cuac::ScanExecutor> ControlledHttpRuntime::Executor() const {
	return executor;
}

void ControlledHttpRuntime::Respond(uint32_t status_p, std::string body_p) {
	std::lock_guard<std::mutex> guard(state->mutex);
	state->mode = ControlledHttpMode::RESPONSE;
	state->status = status_p;
	state->body = std::move(body_p);
	state->diagnostic.clear();
	state->responses.clear();
	state->next_response = 0;
	state->bearer_responses.clear();
	state->bearer_barrier_released = true;
	state->condition.notify_all();
}

void ControlledHttpRuntime::RespondSequence(std::vector<ControlledHttpResponse> responses_p) {
	if (responses_p.empty()) {
		throw std::invalid_argument("controlled response sequence cannot be empty");
	}
	std::lock_guard<std::mutex> guard(state->mutex);
	state->mode = ControlledHttpMode::RESPONSE;
	state->status = 200;
	state->body.clear();
	state->diagnostic.clear();
	state->responses = std::move(responses_p);
	state->next_response = 0;
	state->bearer_responses.clear();
	state->bearer_barrier_released = true;
	state->condition.notify_all();
}

void ControlledHttpRuntime::ExpectBearer(std::string exact_authorization_header) {
	std::lock_guard<std::mutex> guard(state->mutex);
	state->expected_bearer = std::move(exact_authorization_header);
	state->expected_bearer_matches = 0;
	state->unexpected_bearer = false;
}

bool ControlledHttpRuntime::ConsumeBearerExpectation(uint64_t expected_request_count) {
	std::lock_guard<std::mutex> guard(state->mutex);
	const bool matched = !state->expected_bearer.empty() && !state->unexpected_bearer &&
	                     state->expected_bearer_matches == expected_request_count;
	state->expected_bearer.clear();
	state->expected_bearer_matches = 0;
	state->unexpected_bearer = false;
	return matched;
}

void ControlledHttpRuntime::RespondWithBearerBarrier(std::string first_header, std::string first_body,
                                                     std::string second_header, std::string second_body) {
	std::lock_guard<std::mutex> guard(state->mutex);
	state->mode = ControlledHttpMode::BEARER_RESPONSE_BARRIER;
	state->status = 200;
	state->body.clear();
	state->diagnostic.clear();
	state->responses.clear();
	state->next_response = 0;
	state->bearer_responses.clear();
	state->bearer_responses.push_back(std::make_pair(std::move(first_header), std::move(first_body)));
	state->bearer_responses.push_back(std::make_pair(std::move(second_header), std::move(second_body)));
	state->bearer_barrier_released = false;
}

void ControlledHttpRuntime::FailWithUnknownTransportDiagnostic(std::string diagnostic_p) {
	std::lock_guard<std::mutex> guard(state->mutex);
	state->mode = ControlledHttpMode::TRANSPORT_FAILURE;
	state->body.clear();
	state->diagnostic = std::move(diagnostic_p);
	state->responses.clear();
	state->next_response = 0;
	state->bearer_responses.clear();
	state->bearer_barrier_released = true;
	state->condition.notify_all();
}

void ControlledHttpRuntime::BlockUntilCancelled() {
	std::lock_guard<std::mutex> guard(state->mutex);
	state->mode = ControlledHttpMode::BLOCK_UNTIL_CANCEL;
	state->body.clear();
	state->diagnostic.clear();
	state->responses.clear();
	state->next_response = 0;
	state->bearer_responses.clear();
	state->bearer_barrier_released = true;
	state->condition.notify_all();
}

void ControlledHttpRuntime::BlockHostAndRespondElsewhere(std::string blocked_host_p, uint32_t status_p,
                                                         std::string body_p) {
	if (blocked_host_p.empty()) {
		throw std::invalid_argument("controlled blocked host cannot be empty");
	}
	std::lock_guard<std::mutex> guard(state->mutex);
	state->mode = ControlledHttpMode::BLOCK_HOST_AND_RESPOND_ELSEWHERE;
	state->status = status_p;
	state->body = std::move(body_p);
	state->blocked_host = std::move(blocked_host_p);
	state->diagnostic.clear();
	state->responses.clear();
	state->next_response = 0;
	state->bearer_responses.clear();
	state->bearer_barrier_released = true;
	state->condition.notify_all();
}

ControlledHttpResponse ControlledResponse(uint32_t status, std::string body,
                                          std::vector<std::string> link_field_values) {
	return {status,
	        std::move(body),
	        std::move(link_field_values),
	        64,
	        false,
	        "",
	        0,
	        0,
	        false,
	        {},
	        {},
	        cuac::internal::HttpTransportFailureKind::OTHER,
	        0};
}

ControlledHttpResponse ControlledTransportFailure(std::string dependency_diagnostic) {
	return {0, "", {},    0,  true, std::move(dependency_diagnostic),
	        0, 0,  false, {}, {},   cuac::internal::HttpTransportFailureKind::OTHER,
	        0};
}

ControlledHttpResponse ControlledTransientTransportFailure(cuac::internal::HttpTransportFailureKind kind) {
	return {0, "", {}, 0, true, "private transient transport canary", 0, 0, false, {}, {}, kind, 0};
}

bool ControlledHttpRuntime::WaitForRequestCount(uint64_t count, std::chrono::milliseconds timeout) {
	std::unique_lock<std::mutex> guard(state->mutex);
	return state->condition.wait_for(guard, timeout, [&]() { return state->observations.size() >= count; });
}

void ControlledHttpRuntime::ReleaseBearerBarrier() {
	{
		std::lock_guard<std::mutex> guard(state->mutex);
		state->bearer_barrier_released = true;
	}
	state->condition.notify_all();
}

ControlledRequestObservation ControlledHttpRuntime::Observation() const {
	std::lock_guard<std::mutex> guard(state->mutex);
	return state->observation;
}

std::vector<ControlledRequestObservation> ControlledHttpRuntime::Observations() const {
	std::lock_guard<std::mutex> guard(state->mutex);
	return state->observations;
}

std::shared_ptr<ControlledHttpRuntime> BuildControlledHttpRuntime(uint64_t max_wall_milliseconds,
                                                                  uint64_t max_decoded_records) {
	auto state = std::make_shared<ControlledHttpRuntime::State>();
	std::unique_ptr<cuac::internal::HttpTransport> transport(new ControlledTransport(state));
	const cuac::internal::HttpExecutionProfile profile {cuac::PlannedUrlScheme::HTTPS,
	                                                    "api.github.com",
	                                                    443,
	                                                    false,
	                                                    false,
	                                                    false,
	                                                    max_wall_milliseconds,
	                                                    max_decoded_records,
	                                                    cuac::RETRY_MAX_REQUEST_ATTEMPTS_PER_STEP,
	                                                    cuac::RETRY_MAX_REQUEST_ATTEMPTS_PER_SCAN,
	                                                    cuac::RETRY_MAX_DELAY_MILLISECONDS,
	                                                    cuac::RETRY_MAX_CUMULATIVE_WAITING_MILLISECONDS_PER_SCAN};
	auto executor = cuac::internal::BuildHttpScanExecutorForProfile(std::move(transport), profile);
	return std::shared_ptr<ControlledHttpRuntime>(new ControlledHttpRuntime(std::move(state), std::move(executor)));
}

std::shared_ptr<ControlledHttpRuntime> BuildControlledHttpRuntimeForHost(std::string host) {
	auto state = std::make_shared<ControlledHttpRuntime::State>();
	std::unique_ptr<cuac::internal::HttpTransport> transport(new ControlledTransport(state));
	const cuac::internal::HttpExecutionProfile profile {cuac::PlannedUrlScheme::HTTPS,
	                                                    std::move(host),
	                                                    443,
	                                                    false,
	                                                    false,
	                                                    false,
	                                                    cuac::MAX_EXECUTION_MILLISECONDS,
	                                                    cuac::PAGINATION_MAX_DECODED_RECORDS_PER_PAGE,
	                                                    cuac::RETRY_MAX_REQUEST_ATTEMPTS_PER_STEP,
	                                                    cuac::RETRY_MAX_REQUEST_ATTEMPTS_PER_SCAN,
	                                                    cuac::RETRY_MAX_DELAY_MILLISECONDS,
	                                                    cuac::RETRY_MAX_CUMULATIVE_WAITING_MILLISECONDS_PER_SCAN};
	auto executor = cuac::internal::BuildHttpScanExecutorForProfile(std::move(transport), profile);
	return std::shared_ptr<ControlledHttpRuntime>(new ControlledHttpRuntime(std::move(state), std::move(executor)));
}

std::shared_ptr<ControlledHttpRuntime> BuildControlledPackageHttpRuntime() {
	auto state = std::make_shared<ControlledHttpRuntime::State>();
	std::unique_ptr<cuac::internal::HttpTransport> transport(new ControlledTransport(state));
	auto executor = cuac::internal::BuildHttpScanExecutor(std::move(transport));
	return std::shared_ptr<ControlledHttpRuntime>(new ControlledHttpRuntime(std::move(state), std::move(executor)));
}

} // namespace cuac_test
