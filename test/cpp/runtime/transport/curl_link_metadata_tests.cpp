#include "cuac/internal/runtime/transport/curl_response_accumulator.hpp"
#include "cuac/internal/runtime/executor/http_scan_executor.hpp"
#include "cuac/internal/runtime/pagination/link_pagination.hpp"
#include "runtime/support/controlled_socket_service.hpp"
#include "runtime/support/private_curl_probe.hpp"
#include "support/require.hpp"
#include "runtime/support/runtime_http_test_support.hpp"
#include "semantics/support/scan_plan_test_fixtures.hpp"

#include <csignal>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

uint64_t RetainedBytes(const std::vector<std::string> &values) {
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

uint64_t RetainedBytes(const std::vector<cuac::internal::HttpObservedHeader> &fields) {
	uint64_t result = static_cast<uint64_t>(fields.capacity()) * sizeof(cuac::internal::HttpObservedHeader);
	for (const auto &field : fields) {
		const std::string *const values[] = {&field.name, &field.value};
		for (const auto *value : values) {
			const auto begin = reinterpret_cast<std::uintptr_t>(value);
			const auto end = begin + sizeof(*value);
			const auto data = reinterpret_cast<std::uintptr_t>(value->data());
			if (data < begin || data >= end) {
				result += static_cast<uint64_t>(value->capacity()) + 1;
			}
		}
	}
	return result;
}

uint64_t RetainedBytes(const cuac::internal::CurlTransferState &state) {
	return RetainedBytes(state.link_field_values) + RetainedBytes(state.rate_limit_fields) +
	       RetainedBytes(state.date_field_values);
}

bool HasExactBoundedCapacity(const std::string &value) {
	return cuac::internal::HasBoundedHttpStringCapacity(value, static_cast<uint64_t>(value.size()));
}

cuac_test::PrivateCurlProbeOptions Options(uint16_t port, uint64_t *policy_checks) {
	return {"http://127.0.0.1:" + std::to_string(port) + "/search/users?q=duckdb+in%3Alogin&per_page=3",
	        "http",
	        "http",
	        "127.0.0.1",
	        port,
	        "",
	        "",
	        cuac_test::PrivateCurlSocketPolicy::ALLOW_LOOPBACK_PORT,
	        2000,
	        policy_checks,
	        nullptr,
	        nullptr};
}

void TestPhysicalLinkCaptureOrderAndNormalization() {
	cuac_test::ControlledSocketService service(cuac_test::ControlledSocketMode::LINK_SUCCESS);
	cuac_test::ManualControl control;
	uint64_t checks = 0;
	const auto result = cuac_test::PerformPrivateCurlProbe(Options(service.Port(), &checks), control);
	const std::string previous = "<https://api.github.com/user/repos?per_page=100&page=1>; rel=prev";
	const std::string next = "<https://api.github.com/user/repos?per_page=100&page=2>; rel=\"next\"";
	cuac_test::Require(result.response.status == 200 && result.response.metadata.link_field_values.size() == 2 &&
	                       result.response.metadata.link_field_values[0] == previous &&
	                       result.response.metadata.link_field_values[1] == next,
	                   "curl did not capture physical Link values in receipt order with outer OWS removed");
	cuac_test::Require(result.response.metadata.retained_bytes ==
	                       RetainedBytes(result.response.metadata.link_field_values),
	                   "curl Link metadata retained-byte accounting drifted");
	cuac_test::Require(checks == 1 && service.ConnectionCount() == 1, "Link capture changed one-attempt socket policy");
}

void TestMetadataCapacityGrowthIsCharged() {
	cuac_test::ControlledSocketService service(cuac_test::ControlledSocketMode::MANY_LINK_SUCCESS);
	cuac_test::ManualControl control;
	uint64_t checks = 0;
	const auto result = cuac_test::PerformPrivateCurlProbe(Options(service.Port(), &checks), control);
	cuac_test::Require(result.response.metadata.link_field_values.size() == 40 &&
	                       result.response.metadata.link_field_values.capacity() == 40 &&
	                       result.response.metadata.retained_bytes ==
	                           RetainedBytes(result.response.metadata.link_field_values),
	                   "Link vector growth was not exact or its capacity was not charged as retained metadata");
	for (const auto &value : result.response.metadata.link_field_values) {
		cuac_test::Require(HasExactBoundedCapacity(value),
		                   "Link value exceeded its pinned exact-size allocation envelope");
	}
}

void TestInterimMetadataResetAndFailureCleanup() {
	cuac_test::ManualControl control;
	uint64_t checks = 0;
	cuac_test::ControlledSocketService interim(cuac_test::ControlledSocketMode::INTERIM_LINK_SUCCESS);
	const auto result = cuac_test::PerformPrivateCurlProbe(Options(interim.Port(), &checks), control);
	const std::string terminal = "<https://api.github.com/user/repos?per_page=100&page=2>; rel=next";
	cuac_test::Require(result.response.metadata.link_field_values.size() == 1 &&
	                       result.response.metadata.link_field_values[0] == terminal &&
	                       result.response.metadata.link_field_values[0].find("credential-canary") == std::string::npos,
	                   "interim Link metadata survived terminal response-section reset");
	cuac_test::Require(result.response.metadata.link_field_values.capacity() == 1,
	                   "terminal Link fixture did not isolate post-interim retained capacity");

	checks = 0;
	cuac_test::ControlledSocketService failed(cuac_test::ControlledSocketMode::LINK_STATUS);
	const auto failed_result = cuac_test::PerformPrivateCurlProbe(Options(failed.Port(), &checks), control);
	cuac_test::Require(failed_result.response.status == 503 && failed_result.response.body.empty() &&
	                       failed_result.response.metadata.link_field_values.empty() &&
	                       failed_result.response.metadata.link_field_values.capacity() == 0 &&
	                       failed_result.response.metadata.retained_bytes == 0,
	                   "non-success curl response retained body or Link metadata");
}

void TestTrailerCannotGrantContinuationAuthority() {
	cuac_test::ManualControl control;
	uint64_t checks = 0;
	cuac_test::ControlledSocketService trailer(cuac_test::ControlledSocketMode::TRAILER_LINK_SUCCESS);
	const auto result = cuac_test::PerformPrivateCurlProbe(Options(trailer.Port(), &checks), control);
	cuac_test::Require(result.response.status == 200 && result.response.metadata.link_field_values.empty(),
	                   "HTTP trailer Link metadata granted continuation authority");
	cuac_test::Require(checks == 1 && trailer.ConnectionCount() == 1,
	                   "trailer exclusion changed one-attempt socket policy");
}

void FeedHeader(cuac::internal::CurlTransferState &state, const std::string &line) {
	auto mutable_line = line;
	cuac_test::Require(cuac::internal::ReadCurlHeader(&mutable_line[0], 1, mutable_line.size(), &state) ==
	                       mutable_line.size(),
	                   "curl accumulator rejected a bounded regression header");
}

std::size_t FeedHeaderResult(cuac::internal::CurlTransferState &state, const std::string &line) {
	auto mutable_line = line;
	return cuac::internal::ReadCurlHeader(&mutable_line[0], 1, mutable_line.size(), &state);
}

void TestMetadataPreallocationBoundaries() {
	cuac_test::ManualControl control;
	const cuac::internal::CurlTransferProfile profile {"",      "",      nullptr, nullptr, nullptr,
	                                                   nullptr, nullptr, nullptr, nullptr, nullptr};
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	{
		const cuac::internal::HttpLimits limits {0, 4096, 4096, 4096, sizeof(std::string) - 1, deadline, {}, false};
		cuac::internal::CurlTransferState state(control, limits, profile);
		FeedHeader(state, "HTTP/1.1 200 OK\r\n");
		cuac_test::Require(FeedHeaderResult(state, "Link: x\r\n") == 0 && state.metadata_oversized &&
		                       state.link_field_values.empty() && state.link_field_values.capacity() == 0 &&
		                       state.metadata_bytes == 0,
		                   "initial Link metadata allocated before its vector lower bound was admitted");
	}
	{
		const auto metadata_limit = static_cast<uint64_t>(sizeof(cuac::internal::HttpObservedHeader)) + 64;
		const cuac::internal::HttpLimits limits {0, 4096, 4096, 4096, metadata_limit, deadline, {"x-reset"}, false};
		cuac::internal::CurlTransferState state(control, limits, profile);
		FeedHeader(state, "HTTP/1.1 429 Too Many Requests\r\n");
		FeedHeader(state, "X-Reset: 1\r\n");
		cuac_test::Require(state.metadata_bytes == RetainedBytes(state) && state.rate_limit_fields.capacity() == 1,
		                   "folded-metadata fixture did not establish exact initial capacity");
		const std::string oversized_fold = " " + std::string(128, 'x') + "\r\n";
		cuac_test::Require(FeedHeaderResult(state, oversized_fold) == 0 && state.metadata_oversized &&
		                       state.rate_limit_fields.empty() && state.rate_limit_fields.capacity() == 0 &&
		                       state.metadata_bytes == 0,
		                   "folded metadata grew before its replacement allocation was admitted");
	}
	{
		const auto metadata_limit =
		    static_cast<uint64_t>(sizeof(cuac::internal::HttpObservedHeader) + sizeof(std::string) - 1);
		const cuac::internal::HttpLimits limits {0, 4096, 4096, 4096, metadata_limit, deadline, {"link"}, false};
		cuac::internal::CurlTransferState state(control, limits, profile);
		FeedHeader(state, "HTTP/1.1 200 OK\r\n");
		cuac_test::Require(FeedHeaderResult(state, "Link: x\r\n") == 0 && state.metadata_oversized &&
		                       state.rate_limit_fields.capacity() == 0 && state.link_field_values.capacity() == 0 &&
		                       state.metadata_bytes == 0,
		                   "dual-role Link metadata mutated one role before both vector bounds were admitted");
	}
	{
		const auto exact_limit =
		    static_cast<uint64_t>(sizeof(cuac::internal::HttpObservedHeader) + sizeof(std::string));
		const cuac::internal::HttpLimits limits {0, 4096, 4096, 4096, exact_limit, deadline, {"link"}, false};
		cuac::internal::CurlTransferState state(control, limits, profile);
		FeedHeader(state, "HTTP/1.1 200 OK\r\n");
		FeedHeader(state, "Link: x\r\n");
		cuac_test::Require(state.rate_limit_fields.size() == 1 && state.rate_limit_fields.capacity() == 1 &&
		                       state.link_field_values.size() == 1 && state.link_field_values.capacity() == 1 &&
		                       state.metadata_bytes == exact_limit && state.metadata_bytes == RetainedBytes(state),
		                   "dual-role Link metadata did not consume its exact vector-capacity boundary");
	}
	{
		const auto transient_limit = static_cast<uint64_t>(5 * sizeof(std::string));
		const cuac::internal::HttpLimits limits {0, 4096, 4096, 4096, transient_limit, deadline, {}, false};
		cuac::internal::CurlTransferState state(control, limits, profile);
		FeedHeader(state, "HTTP/1.1 200 OK\r\n");
		for (std::size_t index = 0; index < 3; index++) {
			FeedHeader(state, "Link: x\r\n");
			cuac_test::Require(state.link_field_values.size() == index + 1 &&
			                       state.link_field_values.capacity() == index + 1 &&
			                       state.metadata_bytes == RetainedBytes(state),
			                   "Link vector did not retain exact admitted growth capacity");
		}
		cuac_test::Require(FeedHeaderResult(state, "Link: x\r\n") == 0 && state.metadata_oversized &&
		                       state.link_field_values.capacity() == 0 && state.metadata_bytes == 0,
		                   "Link vector replacement ignored its co-live old-plus-new capacity");
	}
	{
		const std::string value(64, 'x');
		const auto metadata_limit =
		    static_cast<uint64_t>(sizeof(std::string)) + cuac::internal::HttpStringAllocationLimit(value.size());
		const cuac::internal::HttpLimits limits {0, 4096, 4096, 4096, metadata_limit, deadline, {}, false};
		cuac::internal::CurlTransferState state(control, limits, profile);
		FeedHeader(state, "HTTP/1.1 200 OK\r\n");
		FeedHeader(state, "Link: " + value + "\r\n");
		cuac_test::Require(state.link_field_values.size() == 1 && HasExactBoundedCapacity(state.link_field_values[0]) &&
		                       state.metadata_bytes == RetainedBytes(state) && state.metadata_bytes <= metadata_limit,
		                   "retained Link string capacity escaped its preflight allocation envelope");
	}
}

void TestTargetedFoldAndProtocolRoleOverlap() {
	cuac_test::ManualControl control;
	const cuac::internal::CurlTransferProfile profile {"",      "",      nullptr, nullptr, nullptr,
	                                                   nullptr, nullptr, nullptr, nullptr, nullptr};
	{
		const cuac::internal::HttpLimits limits {
		    0, 4096, 4096, 4096, 4096, std::chrono::steady_clock::now() + std::chrono::seconds(1), {"x-reset"}, false};
		cuac::internal::CurlTransferState state(control, limits, profile);
		FeedHeader(state, "HTTP/1.1 429 Too Many Requests\r\n");
		FeedHeader(state, "X-Reset: 10\r\n");
		FeedHeader(state, " 0\r\n");
		cuac_test::Require(state.rate_limit_fields.size() == 1 && state.rate_limit_fields[0].name == "x-reset" &&
		                       state.rate_limit_fields[0].value == "10 0" && state.metadata_bytes != 0,
		                   "folded targeted guidance discarded bytes and could become valid early guidance");
	}
	{
		const cuac::internal::HttpLimits limits {0,
		                                         4096,
		                                         4096,
		                                         4096,
		                                         4096,
		                                         std::chrono::steady_clock::now() + std::chrono::seconds(1),
		                                         {"transfer-encoding", "content-encoding", "link"},
		                                         false};
		cuac::internal::CurlTransferState state(control, limits, profile);
		const std::string link = "<https://api.github.com/user/repos?page=2>; rel=next";
		FeedHeader(state, "HTTP/1.1 429 Too Many Requests\r\n");
		FeedHeader(state, "Transfer-Encoding: chunked\r\n");
		FeedHeader(state, "Content-Encoding: identity\r\n");
		FeedHeader(state, "Link: " + link + "\r\n");
		cuac_test::Require(state.transfer_chunked && !state.transfer_encoding_unsupported && !state.content_encoded &&
		                       state.rate_limit_fields.size() == 3 && state.rate_limit_fields[0].value == "chunked" &&
		                       state.rate_limit_fields[1].value == "identity" &&
		                       state.rate_limit_fields[2].value == link &&
		                       state.link_field_values == std::vector<std::string> {link},
		                   "targeted protocol fields were not retained in every authoritative typed role");
	}
	{
		const cuac::internal::HttpLimits limits {
		    0, 4096, 4096, 4096, 4096, std::chrono::steady_clock::now() + std::chrono::seconds(1), {"link"}, false};
		cuac::internal::CurlTransferState state(control, limits, profile);
		const std::string target = "<https://api.github.com/user/repos?per_page=100&page=2>";
		const std::string unfolded = target + " ; rel=next";
		FeedHeader(state, "HTTP/1.1 200 OK\r\n");
		FeedHeader(state, "Link: " + target + "\r\n");
		FeedHeader(state, " ; rel=next\r\n");
		cuac_test::Require(state.rate_limit_fields.size() == 1 && state.rate_limit_fields[0].value == unfolded &&
		                       state.link_field_values == std::vector<std::string> {unfolded},
		                   "folded dual-role Link metadata diverged between rate-limit and pagination copies");

		const cuac::internal::HttpExecutionProfile execution_profile {
		    cuac::PlannedUrlScheme::HTTPS,
		    "api.github.com",
		    443,
		    false,
		    false,
		    false,
		    cuac::MAX_EXECUTION_MILLISECONDS,
		    100,
		    cuac::RETRY_MAX_REQUEST_ATTEMPTS_PER_STEP,
		    cuac::RETRY_MAX_REQUEST_ATTEMPTS_PER_SCAN,
		    cuac::RETRY_MAX_DELAY_MILLISECONDS,
		    cuac::RETRY_MAX_CUMULATIVE_WAITING_MILLISECONDS_PER_SCAN};
		auto admitted = cuac::internal::TryAdmitPaginatedRestPlan(
		    cuac_test::BuildValidAuthenticatedRepositoriesPlanFixture("fixture_secret"), execution_profile);
		cuac_test::Require(admitted != nullptr, "folded Link regression fixture did not pass admission");
		cuac::internal::LinkPaginationState pagination(*admitted);
		const auto transition = pagination.Advance(state.link_field_values);
		cuac_test::Require(transition.has_next && transition.next_page == 2,
		                   "folded dual-role Link metadata silently truncated pagination");
	}
}

} // namespace

int main() {
	(void)std::signal(SIGPIPE, SIG_IGN);
	try {
		TestPhysicalLinkCaptureOrderAndNormalization();
		TestMetadataCapacityGrowthIsCharged();
		TestInterimMetadataResetAndFailureCleanup();
		TestTrailerCannotGrantContinuationAuthority();
		TestMetadataPreallocationBoundaries();
		TestTargetedFoldAndProtocolRoleOverlap();
		std::cout << "curl Link metadata tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "curl Link metadata tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
