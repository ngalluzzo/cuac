#include "cuac/runtime/authorization.hpp"
#include "runtime/support/controlled_socket_service.hpp"
#include "runtime/support/loopback_curl_runtime.hpp"
#include "support/require.hpp"
#include "runtime/support/runtime_http_test_support.hpp"

#include <csignal>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

namespace {

void RequireSelectedPagination(const cuac::ScanPlan &plan, uint64_t token_suffix, const std::string &label) {
	cuac_test::ControlledSocketService service(cuac_test::ControlledSocketMode::PAGINATED_REPOSITORIES);
	const auto runtime = cuac_test::BuildLoopbackCurlRuntime(service.Port());
	cuac_test::ManualControl control;
	auto token = cuac_test::RuntimeCurlBearerToken(token_suffix);
	const auto authorization_header = "Authorization: Bearer " + token + "\r\n";
	auto stream =
	    runtime->Executor()->OpenWithAuthorization(plan, cuac::ScanAuthorization::Bearer(std::move(token)), control);
	cuac_test::Require(service.ConnectionCount() == 0, label + " curl Open performed socket I/O");

	cuac::TypedBatch batch;
	cuac_test::Require(stream->Next(control, batch) && batch.rows.size() == 1 &&
	                       batch.rows[0].values[0].bigint_value == 1 && service.ConnectionCount() == 1,
	                   label + " curl page 1 did not produce its typed row without prefetch");
	cuac_test::Require(stream->Next(control, batch) && batch.rows.size() == 1 &&
	                       batch.rows[0].values[0].bigint_value == 3 && service.ConnectionCount() == 3,
	                   label + " curl did not cross empty page 2 and produce page 3 in the same pull");
	cuac_test::Require(!stream->Next(control, batch), label + " curl pagination did not exhaust cleanly");
	cuac_test::Require(service.WaitForRequestCount(3, std::chrono::seconds(2)),
	                   label + " controlled socket did not receive all three page requests");
	const auto requests = service.Requests();
	cuac_test::Require(requests.size() == 3, label + " curl pagination request count drifted");
	for (std::size_t index = 0; index < requests.size(); index++) {
		const auto target = "GET /user/repos?per_page=100&page=" + std::to_string(static_cast<uint64_t>(index + 1)) +
		                    "&visibility=private HTTP/1.1\r\n";
		cuac_test::Require(requests[index].find(target) == 0 &&
		                       requests[index].find(authorization_header) != std::string::npos,
		                   label + " curl did not emit the canonical target with one bearer header");
	}
	cuac_test::Require(runtime->Observation().request_count == 3 && runtime->Observation().socket_policy_checks == 3 &&
	                       service.ConnectionCount() == 3,
	                   label + " curl pagination was not one fresh policy-checked request per page");
}

} // namespace

int main() {
	(void)std::signal(SIGPIPE, SIG_IGN);
	try {
		RequireSelectedPagination(cuac_test::BuildPrivateRepositoriesPackageRuntimePlan(), 801, "Superset");
		std::cout << "curl HTTP pagination tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "curl HTTP pagination tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
