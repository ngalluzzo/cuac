#include "runtime/support/controlled_socket_service.hpp"
#include "cuac/internal/runtime/transport/http_chunk_decoder.hpp"
#include "runtime/support/loopback_curl_runtime.hpp"
#include "runtime/support/private_curl_probe.hpp"
#include "support/require.hpp"
#include "runtime/support/runtime_http_test_support.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

namespace {

using cuac_test::ControlledSocketMode;
using cuac_test::ControlledSocketService;
using cuac_test::ManualControl;
using cuac_test::Require;
using cuac_test::RequireExecutionError;

uint64_t CompleteRequestHeaderBlockBytes(const std::string &request) {
	const auto request_line_end = request.find("\r\n");
	const auto block_end = request.find("\r\n\r\n");
	Require(request_line_end != std::string::npos && block_end != std::string::npos &&
	            block_end >= request_line_end + 2,
	        "controlled curl request did not contain a complete header block");
	return static_cast<uint64_t>((block_end + 4) - (request_line_end + 2));
}

uint64_t FixedProjectHeaderBytesWithoutToken() {
	static const uint64_t line_framing = 4;
	return (sizeof("Accept") - 1) + (sizeof("application/vnd.github+json") - 1) + line_framing +
	       (sizeof("User-Agent") - 1) + (sizeof("cuac") - 1) + line_framing + (sizeof("X-GitHub-Api-Version") - 1) +
	       (sizeof("2022-11-28") - 1) + line_framing + (sizeof("Authorization") - 1) + (sizeof("Bearer ") - 1) +
	       line_framing;
}

void CancelAfterBody(void *context) {
	static_cast<ManualControl *>(context)->Cancel();
}

void TestOutboundHeaderBoundaries() {
	const auto limit = cuac::ScanAuthorization::BearerTokenByteLimit();
	Require(limit == 8 * 1024, "real-curl bearer-token boundary drifted");
	{
		ControlledSocketService service(ControlledSocketMode::AUTHENTICATED_SUCCESS);
		const auto runtime = cuac_test::BuildLoopbackCurlRuntime(service.Port());
		ManualControl control;
		auto token = std::string(static_cast<std::size_t>(limit), 'e');
		const auto expected_authorization = "Authorization: Bearer " + token + "\r\n";
		auto stream = runtime->Executor()->OpenWithAuthorization(
		    cuac_test::BuildAuthenticatedRuntimePlan(), cuac::ScanAuthorization::Bearer(std::move(token)), control);
		cuac::TypedBatch batch;
		Require(stream->Next(control, batch), "real curl rejected the exact-limit bearer token");
		Require(service.WaitForRequest(std::chrono::seconds(2)),
		        "exact-limit bearer request did not reach the controlled service");
		const auto request = service.Request();
		Require(request.find(expected_authorization) != std::string::npos,
		        "real curl truncated or replaced the exact-limit bearer value");
		Require(CompleteRequestHeaderBlockBytes(request) <= cuac::HOST_MAX_HEADER_BYTES,
		        "complete real-curl request header block exceeded the 16 KiB transport envelope");
	}
	{
		ControlledSocketService service(ControlledSocketMode::AUTHENTICATED_SUCCESS);
		const auto runtime = cuac_test::BuildLoopbackCurlRuntime(service.Port());
		ManualControl control;
		auto token = std::string(static_cast<std::size_t>(limit + 1), 'o');
		bool rejected = false;
		try {
			(void)runtime->Executor()->OpenWithAuthorization(
			    cuac_test::BuildAuthenticatedRuntimePlan(), cuac::ScanAuthorization::Bearer(std::move(token)), control);
		} catch (const cuac::ExecutionError &error) {
			rejected = true;
			Require(error.Stage() == cuac::ErrorStage::RESOURCE && error.Field() == "header_bytes",
			        "one-byte-over real-curl case used the wrong safe resource diagnostic");
		}
		Require(rejected, "one-byte-over real-curl bearer token was accepted");
		Require(runtime->Observation().request_count == 0 && service.ConnectionCount() == 0,
		        "one-byte-over bearer token reached curl transport or the socket service");
	}
	{
		ControlledSocketService service(ControlledSocketMode::AUTHENTICATED_SUCCESS);
		ManualControl control;
		uint64_t policy_checks = 0;
		const cuac_test::PrivateCurlProbeOptions options {"http://127.0.0.1:" + std::to_string(service.Port()) +
		                                                      "/user",
		                                                  "http",
		                                                  "http",
		                                                  "127.0.0.1",
		                                                  service.Port(),
		                                                  "",
		                                                  "",
		                                                  cuac_test::PrivateCurlSocketPolicy::ALLOW_LOOPBACK_PORT,
		                                                  1000,
		                                                  &policy_checks,
		                                                  nullptr,
		                                                  nullptr};
		const auto fixed_header_bytes = FixedProjectHeaderBytesWithoutToken();
		Require(fixed_header_bytes == 125, "fixed project request-header byte accounting drifted");
		auto oversized_project_header =
		    std::string(static_cast<std::size_t>(cuac::HOST_MAX_HEADER_BYTES - fixed_header_bytes + 1), 'a');
		bool rejected = false;
		try {
			(void)cuac_test::PerformPrivateAuthorizedCurlProbe(options, std::move(oversized_project_header), control);
		} catch (const cuac::ExecutionError &error) {
			rejected = true;
			Require(error.Stage() == cuac::ErrorStage::RESOURCE && error.Field() == "header_bytes",
			        "aggregate request-header overflow used the wrong safe resource diagnostic");
		}
		Require(rejected, "aggregate request-header overflow was accepted");
		Require(policy_checks == 0 && service.ConnectionCount() == 0,
		        "aggregate request-header overflow reached socket policy or transport I/O");
	}
}

void TestDecodeAndWireBudgetFailures() {
	struct FailureCase {
		ControlledSocketMode mode;
		cuac::ErrorStage stage;
		const char *forbidden;
	};
	const FailureCase cases[] = {{ControlledSocketMode::MALFORMED, cuac::ErrorStage::DECODE, "SECRET_MALFORMED"},
	                             {ControlledSocketMode::OVERSIZED_HEADER, cuac::ErrorStage::RESOURCE, ""},
	                             {ControlledSocketMode::OVERSIZED_RESPONSE, cuac::ErrorStage::RESOURCE, ""},
	                             {ControlledSocketMode::OVERSIZED_CHUNK_FRAMING, cuac::ErrorStage::RESOURCE, ""},
	                             {ControlledSocketMode::MALFORMED_CHUNK_EXTENSION, cuac::ErrorStage::TRANSPORT, ""}};
	for (std::size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		ControlledSocketService service(cases[index].mode);
		const auto runtime = cuac_test::BuildLoopbackCurlRuntime(service.Port());
		ManualControl control;
		auto stream = runtime->Executor()->Open(cuac_test::BuildRuntimePlan(), control);
		cuac::TypedBatch batch;
		RequireExecutionError([&]() { stream->Next(control, batch); }, cases[index].stage, cases[index].forbidden);
		RequireExecutionError([&]() { stream->Next(control, batch); }, cases[index].stage, cases[index].forbidden);
		Require(runtime->Observation().request_count == 1 && service.ConnectionCount() == 1,
		        "budget or decode failure performed more than one request");
	}
}

void TestCompressedWireBodyAtExactDecompressedLimit() {
	ControlledSocketService service(ControlledSocketMode::GZIP_EXACT_DECOMPRESSED_LIMIT);
	const auto runtime = cuac_test::BuildLoopbackCurlRuntime(service.Port());
	ManualControl control;
	auto stream = runtime->Executor()->Open(cuac_test::BuildRuntimePlan(), control);
	cuac::TypedBatch batch;
	Require(service.ResponseBodyBytes() < cuac::HOST_MAX_RESPONSE_BYTES,
	        "gzip exact-limit fixture did not stay below the wire-byte ceiling");
	Require(stream->Next(control, batch) && batch.IsSchemaAligned() && batch.rows.size() == 1,
	        "gzip body at the exact decompressed ceiling was rejected");
	Require(batch.rows[0].values[0].bigint_value == 11 && !stream->Next(control, batch),
	        "exact-limit gzip response decoded unexpected rows");
	Require(service.ConnectionCount() == 1, "exact-limit gzip response replayed the request");
}

void TestChunkedBodyIsBoundedBeforeDecoding() {
	ControlledSocketService service(ControlledSocketMode::CHUNKED_SUCCESS);
	const auto runtime = cuac_test::BuildLoopbackCurlRuntime(service.Port());
	ManualControl control;
	auto stream = runtime->Executor()->Open(cuac_test::BuildRuntimePlan(), control);
	cuac::TypedBatch batch;
	Require(stream->Next(control, batch) && batch.rows.size() == 2 && stream->Next(control, batch) &&
	            batch.rows.size() == 1 && !stream->Next(control, batch),
	        "bounded chunked identity body did not decode into the expected batches");
	Require(service.ConnectionCount() == 1, "bounded chunked body replayed the request");
}

void TestChunkDecoderHonorsCallScopedControlAndDeadline() {
	const std::string framed = "2\r\n[]\r\n0\r\n\r\n";
	ManualControl cancelled;
	cancelled.Cancel();
	bool saw_cancel = false;
	try {
		(void)cuac::internal::DecodeHttpChunkedBody(framed, 64, cancelled,
		                                            std::chrono::steady_clock::now() + std::chrono::seconds(1));
	} catch (const cuac::ExecutionCancelled &) {
		saw_cancel = true;
	}
	Require(saw_cancel, "chunk framing decode ignored call-scoped cancellation");

	ManualControl active;
	bool saw_deadline = false;
	try {
		(void)cuac::internal::DecodeHttpChunkedBody(framed, 64, active, std::chrono::steady_clock::now());
	} catch (const cuac::ExecutionError &error) {
		saw_deadline = error.Stage() == cuac::ErrorStage::RESOURCE && error.Field() == "wall_milliseconds";
	}
	Require(saw_deadline, "chunk framing decode ignored the retained page deadline");
}

void TestPartialResponseCancellationCarriesAttemptFacts() {
	ControlledSocketService service(ControlledSocketMode::PARTIAL_RESPONSE_BLOCK);
	ManualControl control;
	uint64_t policy_checks = 0;
	const cuac_test::PrivateCurlProbeOptions options {"http://127.0.0.1:" + std::to_string(service.Port()) +
	                                                      "/search/users?q=duckdb+in%3Alogin&per_page=3",
	                                                  "http",
	                                                  "http",
	                                                  "127.0.0.1",
	                                                  service.Port(),
	                                                  "",
	                                                  "",
	                                                  cuac_test::PrivateCurlSocketPolicy::ALLOW_LOOPBACK_PORT,
	                                                  2000,
	                                                  &policy_checks,
	                                                  CancelAfterBody,
	                                                  &control};
	bool typed_cancelled = false;
	cuac::internal::HttpAttemptFacts facts {0, 0, 0, 0};
	try {
		(void)cuac_test::PerformPrivateCurlProbe(options, control);
	} catch (const cuac::internal::HttpAttemptCancelled &cancelled) {
		typed_cancelled = true;
		facts = cancelled.Facts();
	}
	Require(typed_cancelled && facts.response_status == 200 && facts.header_bytes != 0 && facts.response_bytes != 0 &&
	            facts.decompressed_response_bytes != 0,
	        "partial real-curl cancellation discarded observed attempt facts");
	Require(policy_checks == 1 && service.ConnectionCount() == 1,
	        "partial cancellation changed one-attempt socket policy");
}

void TestChunkDecoderGrammarBoundaries() {
	ManualControl control;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	const std::string valid[] = {"2\r\n[]\r\n0\r\n\r\n",
	                             "2 ; flag; sig = \"a\\\"b\"\r\n[]\r\n0\r\nX-Checksum: ok\r\n\r\n"};
	for (const auto &framed : valid) {
		Require(cuac::internal::DecodeHttpChunkedBody(framed, 64, control, deadline) == "[]",
		        "valid chunk extension or trailer grammar was rejected");
	}
	const std::string malformed[] = {"2   \r\n[]\r\n0\r\n\r\n",      "2;\r\n[]\r\n0\r\n\r\n",
	                                 "2;=x\r\n[]\r\n0\r\n\r\n",      "2;bad name=x\r\n[]\r\n0\r\n\r\n",
	                                 "2;flag   \r\n[]\r\n0\r\n\r\n", "2\r\n[]\r\n0\r\nBad Name: x\r\n\r\n"};
	for (const auto &framed : malformed) {
		bool rejected = false;
		try {
			(void)cuac::internal::DecodeHttpChunkedBody(framed, 64, control, deadline);
		} catch (const cuac::internal::HttpChunkDecodeError &) {
			rejected = true;
		}
		Require(rejected, "malformed chunk extension or trailer grammar was accepted");
	}
}

void TestCompressedWireBodyOverDecompressedLimit() {
	ControlledSocketService service(ControlledSocketMode::GZIP_OVER_DECOMPRESSED_LIMIT);
	const auto runtime = cuac_test::BuildLoopbackCurlRuntime(service.Port());
	ManualControl control;
	auto stream = runtime->Executor()->Open(cuac_test::BuildRuntimePlan(), control);
	cuac::TypedBatch batch;
	Require(service.ResponseBodyBytes() < cuac::HOST_MAX_RESPONSE_BYTES,
	        "gzip over-limit fixture did not stay below the wire-byte ceiling");
	bool rejected = false;
	try {
		stream->Next(control, batch);
	} catch (const cuac::ExecutionError &error) {
		rejected = true;
		Require(error.Stage() == cuac::ErrorStage::RESOURCE && error.Field() == "decompressed_bytes",
		        "gzip +1 counterexample did not exhaust the decompressed-byte ceiling");
	}
	Require(rejected, "gzip +1 counterexample was accepted");
	RequireExecutionError([&]() { stream->Next(control, batch); }, cuac::ErrorStage::RESOURCE);
	Require(runtime->Observation().request_count == 1 && service.ConnectionCount() == 1,
	        "over-limit gzip response performed more than one request");
}

} // namespace

int main() {
	(void)std::signal(SIGPIPE, SIG_IGN);
	try {
		TestOutboundHeaderBoundaries();
		TestDecodeAndWireBudgetFailures();
		TestCompressedWireBodyAtExactDecompressedLimit();
		TestChunkedBodyIsBoundedBeforeDecoding();
		TestChunkDecoderHonorsCallScopedControlAndDeadline();
		TestPartialResponseCancellationCarriesAttemptFacts();
		TestChunkDecoderGrammarBoundaries();
		TestCompressedWireBodyOverDecompressedLimit();
		std::cout << "curl HTTP budget tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "curl HTTP budget tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
