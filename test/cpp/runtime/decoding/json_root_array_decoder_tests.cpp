#include "cuac/internal/runtime/decoding/json_decoder.hpp"
#include "support/require.hpp"

#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>

namespace {

using cuac_test::Require;

class Control final : public cuac::ExecutionControl {
public:
	Control() : cancelled(false) {
	}
	bool IsCancellationRequested() const noexcept override {
		return cancelled;
	}
	bool cancelled;
};

cuac::internal::JsonDecodePlan Plan(uint64_t max_records = 100) {
	cuac::internal::JsonDecodePlan plan;
	plan.response_source = cuac::internal::JsonResponseSource::ROOT_ARRAY;
	plan.columns = {{"id", "id", cuac::ValueKind::BIGINT},
	                {"full_name", "full_name", cuac::ValueKind::VARCHAR},
	                {"private", "private", cuac::ValueKind::BOOLEAN},
	                {"fork", "fork", cuac::ValueKind::BOOLEAN},
	                {"archived", "archived", cuac::ValueKind::BOOLEAN},
	                {"visibility", "visibility", cuac::ValueKind::VARCHAR}};
	plan.max_records = max_records;
	plan.max_string_bytes = 512;
	plan.max_json_nesting = 16;
	plan.max_decoded_memory_bytes = 2 * 1024 * 1024;
	plan.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	return plan;
}

std::vector<cuac::TypedRow> DecodeRows(const std::string &body, const cuac::internal::JsonDecodePlan &plan,
                                       cuac::ExecutionControl &control) {
	auto page = cuac::internal::DecodeJsonPage(body, plan, control);
	return std::move(page.rows);
}

std::string Repository(uint64_t id, const std::string &full_name = "owner/repository") {
	return std::string("{\"id\":") + std::to_string(id) + ",\"full_name\":\"" + full_name +
	       "\",\"private\":false,\"fork\":true,\"archived\":false,\"visibility\":\"public\"}";
}

std::string RepositoryWithVisibility(const std::string &visibility_json) {
	return std::string("{\"id\":1,\"full_name\":\"owner/repository\",\"private\":false,") +
	       "\"fork\":true,\"archived\":false,\"visibility\":" + visibility_json + "}";
}

void RequireError(const std::function<void()> &action, cuac::ErrorStage stage, const std::string &field) {
	bool rejected = false;
	try {
		action();
	} catch (const cuac::ExecutionError &error) {
		rejected = true;
		Require(error.Stage() == stage && error.Field() == field, "root-array failure used the wrong safe diagnostic");
		Require(error.SafeMessage().size() <= 128, "root-array failure diagnostic was unbounded");
	}
	Require(rejected, "root-array counterexample was accepted");
}

void TestSixColumnRootArrayAndMemoryEvidence() {
	Control control;
	auto decoded = cuac::internal::DecodeJsonPage("[" + Repository(11) + "]", Plan(), control);
	Require(decoded.rows.size() == 1 && decoded.rows[0].values.size() == 6 &&
	            decoded.rows[0].values[0].bigint_value == 11 &&
	            decoded.rows[0].values[1].varchar_value == "owner/repository" &&
	            !decoded.rows[0].values[2].boolean_value && decoded.rows[0].values[3].boolean_value &&
	            !decoded.rows[0].values[4].boolean_value && decoded.rows[0].values[5].varchar_value == "public" &&
	            decoded.retained_memory_bytes > 0,
	        "repository root array did not produce the exact typed row and retained-memory evidence");
	Require(DecodeRows("[]", Plan(), control).empty(), "empty root array was not a valid empty page");
}

void TestRecordAndStringBoundaries() {
	Control control;
	std::string exact = "[";
	for (uint64_t index = 0; index < 100; index++) {
		exact += index == 0 ? "" : ",";
		exact += Repository(index + 1, "r");
	}
	exact += "]";
	Require(DecodeRows(exact, Plan(), control).size() == 100, "exact 100-record page was rejected");
	exact.insert(exact.size() - 1, "," + Repository(101, "r"));
	RequireError([&]() { (void)DecodeRows(exact, Plan(), control); }, cuac::ErrorStage::RESOURCE, "");

	auto string_plan = Plan();
	Require(DecodeRows("[" + Repository(1, std::string(512, 'a')) + "]", string_plan, control).size() == 1,
	        "exact 512-byte repository name was rejected");
	RequireError([&]() { (void)DecodeRows("[" + Repository(1, std::string(513, 'b')) + "]", string_plan, control); },
	             cuac::ErrorStage::RESOURCE, "full_name");

	const auto exact_visibility = RepositoryWithVisibility("\"" + std::string(512, 'v') + "\"");
	Require(DecodeRows("[" + exact_visibility + "]", string_plan, control).size() == 1,
	        "exact 512-byte visibility was rejected");
	const auto oversized_visibility = RepositoryWithVisibility("\"" + std::string(513, 'v') + "\"");
	RequireError([&]() { (void)DecodeRows("[" + oversized_visibility + "]", string_plan, control); },
	             cuac::ErrorStage::RESOURCE, "visibility");
}

void TestSchemaAndLifecycleCounterexamples() {
	Control control;
	const std::string cases[] = {
	    "{}",
	    "[null]",
	    "[{\"full_name\":\"r\",\"private\":false,\"fork\":false,\"archived\":false,\"visibility\":\"public\"}]",
	    "[{\"id\":1,\"full_name\":null,\"private\":false,\"fork\":false,\"archived\":false,\"visibility\":\"public\"}]",
	    "[{\"id\":1,\"id\":2,\"full_name\":\"r\",\"private\":false,\"fork\":false,\"archived\":false,\"visibility\":"
	    "\"public\"}]",
	    "[{\"id\":1,\"full_name\":\"r\",\"private\":0,\"fork\":false,\"archived\":false,\"visibility\":\"public\"}]",
	    "[{\"id\":1,\"full_name\":\"r\",\"private\":false,\"fork\":false,\"archived\":false}]",
	    "[" + RepositoryWithVisibility("null") + "]",
	    "[" + RepositoryWithVisibility("1") + "]",
	    "[" + RepositoryWithVisibility("true") + "]"};
	const std::string fields[] = {"",        "",           "id",         "full_name",  "id",
	                              "private", "visibility", "visibility", "visibility", "visibility"};
	for (std::size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		RequireError([&]() { (void)DecodeRows(cases[index], Plan(), control); }, cuac::ErrorStage::SCHEMA,
		             fields[index]);
	}
	RequireError([&]() { (void)DecodeRows("[", Plan(), control); }, cuac::ErrorStage::DECODE, "");
	control.cancelled = true;
	bool cancelled = false;
	try {
		(void)DecodeRows("[]", Plan(), control);
	} catch (const cuac::ExecutionCancelled &) {
		cancelled = true;
	}
	Require(cancelled, "root-array decoder ignored cancellation");
}

} // namespace

int main() {
	try {
		TestSixColumnRootArrayAndMemoryEvidence();
		TestRecordAndStringBoundaries();
		TestSchemaAndLifecycleCounterexamples();
		std::cout << "JSON root-array decoder tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "JSON root-array decoder tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
