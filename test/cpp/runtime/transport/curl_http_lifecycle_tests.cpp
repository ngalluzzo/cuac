#include "cuac/runtime/http_runtime.hpp"
#include "cuac/runtime/authorization.hpp"
#include "cuac/internal/runtime/build/dependency_identity.hpp"
#include "cuac/internal/runtime/transport/curl_http_transport.hpp"
#include "runtime/support/controlled_socket_service.hpp"
#include "runtime/support/loopback_curl_runtime.hpp"
#include "support/require.hpp"
#include "runtime/support/runtime_http_test_support.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

namespace {

using cuac_test::ControlledSocketMode;
using cuac_test::ControlledSocketService;
using cuac_test::ManualControl;
using cuac_test::Require;
using cuac_test::RequireExecutionError;

void TestCheckedProcessInitializationAndIdentity() {
	const auto *owner_before_service_teardown = cuac::internal::AcquireCurlProcessLifetime();
	std::atomic<uint64_t> initialized(0);
	std::atomic<uint64_t> rejected(0);
	std::vector<std::thread> workers;
	for (std::size_t index = 0; index < 8; index++) {
		workers.push_back(std::thread([&]() {
			try {
				const auto service = cuac::InitializeHttpRuntime();
				if (service.executor && service.identity.libcurl_version == CUAC_REQUIRED_LIBCURL_VERSION &&
				    service.identity.ssl_backend == CUAC_REQUIRED_LIBCURL_SSL_BACKEND && service.identity.thread_safe) {
					initialized.fetch_add(1, std::memory_order_relaxed);
				} else {
					rejected.fetch_add(1, std::memory_order_relaxed);
				}
			} catch (...) {
				rejected.fetch_add(1, std::memory_order_relaxed);
			}
		}));
	}
	for (std::size_t index = 0; index < workers.size(); index++) {
		workers[index].join();
	}
	Require(initialized.load(std::memory_order_relaxed) == workers.size() &&
	            rejected.load(std::memory_order_relaxed) == 0,
	        "checked process-global HTTP initialization or identity failed");
	const auto *owner_after_service_teardown = cuac::internal::AcquireCurlProcessLifetime();
	Require(owner_before_service_teardown == owner_after_service_teardown,
	        "accepted process lifetime was replaced during service teardown");
}

void TestPlanDeadlineBoundsBlockedCurlTransfer() {
	ControlledSocketService blocked(ControlledSocketMode::BLOCK);
	const auto runtime = cuac_test::BuildLoopbackCurlRuntime(blocked.Port());
	ManualControl control;
	auto stream = runtime->Executor()->Open(cuac_test::BuildRuntimePlan(), control);
	cuac::TypedBatch batch;
	const auto started = std::chrono::steady_clock::now();
	RequireExecutionError([&]() { stream->Next(control, batch); }, cuac::ErrorStage::RESOURCE);
	const auto elapsed = std::chrono::steady_clock::now() - started;
	Require(elapsed >= std::chrono::seconds(4) && elapsed < std::chrono::seconds(7),
	        "real curl deadline was not bounded by the production plan");
}

void TestStructuredRepositoryOpenAndCloseRemainSideEffectFree() {
	ControlledSocketService service(ControlledSocketMode::PAGINATED_REPOSITORIES);
	const auto runtime = cuac_test::BuildLoopbackCurlRuntime(service.Port());
	ManualControl control;
	cuac::TypedBatch batch;

	auto superset_token = cuac_test::RuntimeCurlBearerToken(72);
	auto superset =
	    runtime->Executor()->OpenWithAuthorization(cuac_test::BuildPrivateRepositoriesPackageRuntimePlan(),
	                                               cuac::ScanAuthorization::Bearer(std::move(superset_token)), control);
	Require(service.ConnectionCount() == 0 && runtime->Observation().request_count == 0,
	        "Superset repository Open materialized authorization or started curl I/O");
	superset->Close();
	superset->Close();
	superset->Cancel();
	Require(!superset->Next(control, batch) && service.ConnectionCount() == 0 &&
	            runtime->Observation().request_count == 0,
	        "closed Superset repository stream resumed or acquired request authority");

	auto ambiguous_token = cuac_test::RuntimeCurlBearerToken(73);
	auto ambiguous = runtime->Executor()->OpenWithAuthorization(
	    cuac_test::BuildAmbiguousPredicateFallbackRuntimePlan(),
	    cuac::ScanAuthorization::Bearer(std::move(ambiguous_token)), control);
	Require(service.ConnectionCount() == 0 && runtime->Observation().request_count == 0,
	        "Ambiguous fallback Open materialized authorization or started curl I/O");
	ambiguous->Cancel();
	ambiguous->Close();
	ambiguous->Close();
	Require(!ambiguous->Next(control, batch) && service.ConnectionCount() == 0 &&
	            runtime->Observation().request_count == 0,
	        "closed Ambiguous fallback stream resumed or acquired request authority");
}

void TestConcurrentCloseAndRecovery() {
	ControlledSocketService blocked(ControlledSocketMode::BLOCK_THEN_AUTHENTICATED_SUCCESS);
	const auto runtime = cuac_test::BuildLoopbackCurlRuntime(blocked.Port());
	ManualControl control;
	auto token = cuac_test::RuntimeCurlBearerToken(70);
	const auto expected_header = "Authorization: Bearer " + token + "\r\n";
	auto stream = runtime->Executor()->OpenWithAuthorization(
	    cuac_test::BuildAuthenticatedRuntimePlan(), cuac::ScanAuthorization::Bearer(std::move(token)), control);
	cuac::TypedBatch batch;
	std::atomic<bool> cancelled(false);
	std::thread worker([&]() {
		try {
			stream->Next(control, batch);
		} catch (const cuac::ExecutionCancelled &) {
			cancelled.store(true, std::memory_order_release);
		}
	});
	Require(blocked.WaitForRequest(std::chrono::seconds(2)), "curl request did not reach the blocking service");
	Require(blocked.Request().find(expected_header) != std::string::npos,
	        "blocking authenticated transfer did not carry its isolated bearer header");
	const auto started = std::chrono::steady_clock::now();
	stream->Close();
	stream->Close();
	stream->Cancel();
	worker.join();
	Require(cancelled.load(std::memory_order_acquire) &&
	            std::chrono::steady_clock::now() - started < std::chrono::seconds(6),
	        "concurrent Close did not contain the real curl transfer within its deadline");
	Require(runtime->Observation().request_count == 1, "cancelled curl request was replayed");
	Require(!stream->Next(control, batch), "closed curl stream resumed after concurrent teardown");

	ManualControl recovered_control;
	auto recovered_token = cuac_test::RuntimeCurlBearerToken(71);
	auto recovered_stream = runtime->Executor()->OpenWithAuthorization(
	    cuac_test::BuildAuthenticatedRuntimePlan(), cuac::ScanAuthorization::Bearer(std::move(recovered_token)),
	    recovered_control);
	Require(recovered_stream->Next(recovered_control, batch) && batch.IsSchemaAligned() && batch.rows.size() == 1,
	        "same curl runtime did not recover after a cancelled transfer");
	Require(blocked.ConnectionCount() == 2 && runtime->Observation().request_count == 2,
	        "shared curl executor did not perform one cancelled and one recovered request");
}

} // namespace

int main() {
	(void)std::signal(SIGPIPE, SIG_IGN);
	try {
		TestCheckedProcessInitializationAndIdentity();
		TestPlanDeadlineBoundsBlockedCurlTransfer();
		TestStructuredRepositoryOpenAndCloseRemainSideEffectFree();
		TestConcurrentCloseAndRecovery();
		std::cout << "curl HTTP lifecycle tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "curl HTTP lifecycle tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
