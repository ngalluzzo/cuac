#include "runtime/support/package_fixture_observation_internal.hpp"

#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>

namespace cuac_test {
namespace internal {
namespace {

void CheckCancellation(cuac::ExecutionControl &control) {
	if (control.IsCancellationRequested()) {
		throw cuac::ExecutionCancelled();
	}
}

bool IsLinkHeader(const std::string &name) {
	static const char expected[] = "link";
	if (name.size() != sizeof(expected) - 1) {
		return false;
	}
	for (std::size_t index = 0; index < sizeof(expected) - 1; index++) {
		const char byte = name[index];
		const char folded = byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte - 'A' + 'a') : byte;
		if (folded != expected[index]) {
			return false;
		}
	}
	return true;
}

uint64_t AddHeaderBytes(uint64_t total, std::size_t bytes) {
	const auto value = static_cast<uint64_t>(bytes);
	if (value > std::numeric_limits<uint64_t>::max() - total) {
		throw std::invalid_argument("controlled fixture response header bytes overflow");
	}
	return total + value;
}

} // namespace

void ValidateRuntimeFixtureTranscript(const RuntimeFixtureTranscript &transcript) {
	if (transcript.authorization == RuntimeFixtureAuthorizationState::BEARER_MISSING) {
		if (!transcript.pages.empty()) {
			throw std::invalid_argument("bearer-missing fixture transcript cannot contain response pages");
		}
	} else if (transcript.pages.empty()) {
		throw std::invalid_argument("executable fixture transcript requires at least one response page");
	}
}

std::vector<ControlledHttpResponse>
BuildRuntimeFixtureResponses(const RuntimeFixtureTranscript &transcript, cuac::ExecutionControl &control,
                             bool transport_failure, const RuntimeFixtureResponseAccountingOverrides *overrides) {
	if (overrides && overrides->wire_response_bytes.size() != transcript.pages.size()) {
		throw std::invalid_argument("controlled fixture response-accounting override count differs from pages");
	}
	std::vector<ControlledHttpResponse> responses;
	responses.reserve(transcript.pages.size());
	for (const auto &page : transcript.pages) {
		CheckCancellation(control);
		if (transport_failure) {
			responses.push_back(ControlledTransportFailure("controlled package fixture transport failure"));
			break;
		}
		uint64_t header_bytes = 0;
		std::vector<std::string> link_field_values;
		for (const auto &header : page.headers) {
			CheckCancellation(control);
			header_bytes = AddHeaderBytes(header_bytes, header.name.size());
			header_bytes = AddHeaderBytes(header_bytes, 2);
			header_bytes = AddHeaderBytes(header_bytes, header.value.size());
			header_bytes = AddHeaderBytes(header_bytes, 2);
			if (IsLinkHeader(header.name)) {
				link_field_values.push_back(header.value);
			}
		}
		const auto wire_response_bytes = overrides ? overrides->wire_response_bytes[responses.size()] : 0;
		responses.push_back({page.status,
		                     page.body,
		                     std::move(link_field_values),
		                     header_bytes,
		                     false,
		                     "",
		                     wire_response_bytes,
		                     0,
		                     false,
		                     {},
		                     {},
		                     cuac::internal::HttpTransportFailureKind::OTHER,
		                     0});
		CheckCancellation(control);
	}
	return responses;
}

RuntimeFixtureExecutionObservation NewRuntimeFixtureObservation(const cuac::ScanPlan &plan,
                                                                RuntimeFixtureCancellationPoint point) {
	return {false, {},    plan.Snapshot(), false, cuac::ErrorStage::INTERNAL, "", {}, false, 0, false, point,
	        false, false, false,           false};
}

void CaptureRuntimeFixtureRequests(const RuntimeFixtureTranscript &transcript, ControlledHttpRuntime &runtime,
                                   RuntimeFixtureExecutionObservation &result) {
	const auto observed = runtime.Observations();
	result.requests.reserve(observed.size());
	for (const auto &request : observed) {
		result.requests.push_back({request.method, request.scheme, request.host, request.port, request.target,
		                           request.headers, request.body, request.content_type});
	}
	result.request_count = static_cast<uint64_t>(result.requests.size());
	result.transport_observed = !result.requests.empty();

	for (const auto &request : result.requests) {
		std::size_t authorization_headers = 0;
		for (const auto &header : request.headers) {
			if (header.first == "Authorization") {
				authorization_headers++;
				if (header.second != "<redacted>") {
					throw std::logic_error("controlled fixture observation exposed authorization bytes");
				}
			}
		}
		if (transcript.authorization == RuntimeFixtureAuthorizationState::BEARER_PRESENT) {
			if (authorization_headers != 1) {
				throw std::logic_error("controlled bearer fixture did not observe exactly one authorization header");
			}
		} else if (authorization_headers != 0) {
			throw std::logic_error("controlled non-bearer fixture observed authorization authority");
		}
	}
	if (transcript.authorization == RuntimeFixtureAuthorizationState::BEARER_PRESENT &&
	    !runtime.ConsumeBearerExpectation(result.request_count)) {
		throw std::logic_error("controlled fixture bearer bytes differed from Runtime's synthetic capability");
	}
}

} // namespace internal
} // namespace cuac_test
