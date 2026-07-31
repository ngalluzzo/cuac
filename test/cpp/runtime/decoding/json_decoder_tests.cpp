#include "cuac/internal/runtime/decoding/json_decoder.hpp"
#include "support/require.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using cuac_test::Require;

class ManualControl final : public cuac::ExecutionControl {
public:
	ManualControl() : cancelled(false) {
	}

	bool IsCancellationRequested() const noexcept override {
		return cancelled.load(std::memory_order_acquire);
	}

	void Cancel() noexcept {
		cancelled.store(true, std::memory_order_release);
	}

private:
	std::atomic<bool> cancelled;
};

class CountingControl final : public cuac::ExecutionControl {
public:
	explicit CountingControl(std::size_t cancel_at_p = std::numeric_limits<std::size_t>::max())
	    : cancel_at(cancel_at_p), calls(0) {
	}

	bool IsCancellationRequested() const noexcept override {
		return ++calls >= cancel_at;
	}

	std::size_t Calls() const noexcept {
		return calls;
	}

private:
	const std::size_t cancel_at;
	mutable std::size_t calls;
};

cuac::internal::JsonDecodePlan Plan() {
	cuac::internal::JsonDecodePlan plan;
	plan.response_source = cuac::internal::JsonResponseSource::JSON_PATH_MANY;
	plan.records_path = {"items"};
	plan.columns = {{"id", "id", cuac::ValueKind::BIGINT},
	                {"login", "login", cuac::ValueKind::VARCHAR},
	                {"site_admin", "site_admin", cuac::ValueKind::BOOLEAN}};
	plan.max_records = 3;
	plan.max_string_bytes = 256;
	plan.max_json_nesting = 16;
	plan.max_decoded_memory_bytes = 4096;
	plan.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	return plan;
}

cuac::internal::JsonDecodePlan RootPlan() {
	auto plan = Plan();
	plan.response_source = cuac::internal::JsonResponseSource::ROOT_OBJECT;
	plan.records_path.clear();
	plan.max_records = 1;
	return plan;
}

cuac::internal::JsonDecodePlan NestedPlan() {
	auto plan = Plan();
	plan.records_path = {"payload", "records"};
	plan.columns = {{"id", std::vector<std::string>({"identity", "id"}), cuac::ValueKind::BIGINT},
	                {"label", std::vector<std::string>({"attributes", "label"}), cuac::ValueKind::VARCHAR, true},
	                {"active", std::vector<std::string>({"flags", "active"}), cuac::ValueKind::BOOLEAN}};
	return plan;
}

cuac::internal::JsonDecodePlan ArrayPlan() {
	cuac::internal::JsonDecodePlan plan;
	plan.response_source = cuac::internal::JsonResponseSource::JSON_PATH_MANY;
	plan.records_path = {"items"};
	plan.columns = {{"flags", {"flags"}, cuac::OutputValueType::Array(cuac::ValueKind::BOOLEAN, true)},
	                {"ids", {"ids"}, cuac::OutputValueType::Array(cuac::ValueKind::BIGINT, false)},
	                {"names", {"names"}, cuac::OutputValueType::Array(cuac::ValueKind::VARCHAR, false), true},
	                {"scores", {"scores"}, cuac::OutputValueType::Array(cuac::ValueKind::DOUBLE, false)}};
	plan.max_records = 3;
	plan.max_string_bytes = 256;
	plan.max_json_nesting = 16;
	plan.max_decoded_memory_bytes = 65536;
	plan.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	return plan;
}

cuac::internal::JsonDecodePlan SingleColumnPlan(cuac::OutputValueType type) {
	cuac::internal::JsonDecodePlan plan;
	plan.response_source = cuac::internal::JsonResponseSource::JSON_PATH_MANY;
	plan.records_path = {"items"};
	plan.columns = {{"value", {"value"}, type}};
	plan.max_records = 1;
	plan.max_string_bytes = 256;
	plan.max_json_nesting = 16;
	plan.max_decoded_memory_bytes = 65536;
	plan.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	return plan;
}

std::vector<cuac::TypedRow> DecodeRows(const std::string &body, const cuac::internal::JsonDecodePlan &plan,
                                       cuac::ExecutionControl &control) {
	auto page = cuac::internal::DecodeJsonPage(body, plan, control);
	return std::move(page.rows);
}

std::vector<cuac::TypedRow> Decode(const std::string &body, ManualControl &control) {
	return DecodeRows(body, Plan(), control);
}

std::vector<cuac::TypedRow> DecodeRoot(const std::string &body, ManualControl &control) {
	return DecodeRows(body, RootPlan(), control);
}

void RequireError(const std::function<void()> &action, cuac::ErrorStage stage, const std::string &field,
                  const std::string &forbidden = "") {
	bool rejected = false;
	try {
		action();
	} catch (const cuac::ExecutionError &error) {
		rejected = true;
		Require(error.Stage() == stage, "decoder error stage drifted");
		Require(error.Field() == field, "decoder error field drifted");
		Require(!error.SafeMessage().empty() && error.SafeMessage().size() <= 128,
		        "decoder error was empty or unbounded");
		if (!forbidden.empty()) {
			Require(error.SafeMessage().find(forbidden) == std::string::npos, "decoder error exposed response content");
		}
	}
	Require(rejected, "expected a structured decoder error");
}

std::string OneRow(const std::string &fields) {
	return std::string("{\"items\":[{") + fields + "}]}";
}

void TestTypedRowsAndCompleteValidation() {
	ManualControl control;
	const auto rows = Decode("{\"ignored\":{\"nested\":[true,null,3.5]},\"items\":["
	                         "{\"id\":11,\"login\":\"duckdb\",\"site_admin\":false,\"ignored\":1},"
	                         "{\"site_admin\":true,\"login\":\"duck\\u0064b-fdw\",\"id\":-22},"
	                         "{\"id\":33,\"login\":\"three\",\"site_admin\":false}]}",
	                         control);
	Require(rows.size() == 3, "decoder did not return all bounded records");
	Require(rows[0].values[0].kind == cuac::ValueKind::BIGINT && rows[0].values[0].bigint_value == 11 &&
	            rows[0].values[1].varchar_value == "duckdb" && !rows[0].values[2].boolean_value,
	        "first schema-aligned row was decoded incorrectly");
	Require(rows[1].values[0].bigint_value == -22 && rows[1].values[1].varchar_value == "cuac" &&
	            rows[1].values[2].boolean_value,
	        "Unicode escape or typed value was decoded incorrectly");
}

void TestStrictSuccessfulRootObject() {
	ManualControl control;
	const auto rows = DecodeRoot(
	    "{\"ignored\":{\"nested\":[true,null]},\"site_admin\":false,\"login\":\"duck\\u0064b\",\"id\":11}", control);
	Require(rows.size() == 1 && rows[0].values.size() == 3 && rows[0].values[0].bigint_value == 11 &&
	            rows[0].values[1].varchar_value == "duckdb" && !rows[0].values[2].boolean_value,
	        "successful root object did not produce exactly one aligned row");

	struct Case {
		std::string body;
		std::string field;
	};
	const std::vector<Case> cases = {
	    {"[]", ""},
	    {"null", ""},
	    {"{}", "id"},
	    {"{\"id\":1,\"login\":\"duckdb\",\"site_admin\":null}", "site_admin"},
	    {"{\"id\":1,\"login\":\"duckdb\",\"login\":\"duplicate\",\"site_admin\":false}", "login"},
	    {"{\"id\":1.0,\"login\":\"duckdb\",\"site_admin\":false}", "id"}};
	for (std::size_t index = 0; index < cases.size(); index++) {
		RequireError([&]() { DecodeRoot(cases[index].body, control); }, cuac::ErrorStage::SCHEMA, cases[index].field);
	}
	RequireError([&]() { DecodeRoot("{\"id\":1", control); }, cuac::ErrorStage::DECODE, "");

	auto bounded = RootPlan();
	bounded.max_string_bytes = 3;
	RequireError([&]() { DecodeRows("{\"id\":1,\"login\":\"four\",\"site_admin\":false}", bounded, control); },
	             cuac::ErrorStage::RESOURCE, "login");
	bounded = RootPlan();
	bounded.max_json_nesting = 2;
	RequireError(
	    [&]() { DecodeRows("{\"id\":1,\"login\":\"ok\",\"site_admin\":false,\"ignored\":[[0]]}", bounded, control); },
	    cuac::ErrorStage::RESOURCE, "json_nesting");

	auto invalid = RootPlan();
	invalid.max_records = 2;
	RequireError([&]() { DecodeRows("{}", invalid, control); }, cuac::ErrorStage::INTERNAL, "");
}

void TestNestedPermanentRestPaths() {
	ManualControl control;
	const auto rows =
	    DecodeRows("{\"ignored\":0,\"payload\":{\"other\":true,\"records\":["
	               "{\"identity\":{\"id\":42},\"attributes\":{\"label\":\"north\"},\"flags\":{\"active\":true}},"
	               "{\"identity\":{\"id\":43},\"attributes\":{\"label\":null},\"flags\":{\"active\":false}}]}}",
	               NestedPlan(), control);
	Require(rows.size() == 2 && rows[0].values[0].bigint_value == 42 && rows[0].values[1].varchar_value == "north" &&
	            rows[0].values[2].boolean_value && rows[1].values[0].bigint_value == 43 && !rows[1].values[1].valid &&
	            !rows[1].values[2].boolean_value,
	        "nested permanent REST paths did not produce schema-aligned typed rows");

	RequireError(
	    [&]() {
		    (void)DecodeRows("{\"payload\":{\"records\":[{\"identity\":{\"id\":1},\"identity\":{\"id\":2},"
		                     "\"attributes\":{\"label\":\"x\"},\"flags\":{\"active\":true}}]}}",
		                     NestedPlan(), control);
	    },
	    cuac::ErrorStage::SCHEMA, "id");
	RequireError(
	    [&]() {
		    (void)DecodeRows("{\"payload\":{\"records\":[{\"identity\":null,\"attributes\":{\"label\":\"x\"},"
		                     "\"flags\":{\"active\":true}}]}}",
		                     NestedPlan(), control);
	    },
	    cuac::ErrorStage::SCHEMA, "id");
	RequireError([&]() { (void)DecodeRows("{\"payload\":{}}", NestedPlan(), control); }, cuac::ErrorStage::SCHEMA,
	             "records");

	auto invalid = NestedPlan();
	invalid.columns[1].json_path = {"identity"};
	RequireError([&]() { (void)DecodeRows("{}", invalid, control); }, cuac::ErrorStage::INTERNAL, "");
}

void TestMalformedAndInvalidUtf8() {
	ManualControl control;
	std::vector<std::string> malformed = {"", "{", "{\"items\":[],\"ignored\":[1,]}", "{\"items\":[]}}",
	                                      "{\"items\":[] true}"};
	malformed.push_back(std::string("{\"items\":[]}") + std::string(1, '\0'));
	malformed.push_back(std::string("{\"items\":[],\"bad\":\"") + std::string(1, static_cast<char>(0xff)) + "\"}");
	for (std::size_t index = 0; index < malformed.size(); index++) {
		RequireError([&]() { Decode(malformed[index], control); }, cuac::ErrorStage::DECODE, "");
	}
}

void TestRequiredShapeAndStrictTypes() {
	ManualControl control;
	struct Case {
		std::string body;
		std::string field;
	};
	const std::vector<Case> cases = {
	    {"[]", "items"},
	    {"{}", "items"},
	    {"{\"items\":[],\"items\":[]}", "items"},
	    {"{\"items\":null}", "items"},
	    {"{\"items\":[1]}", ""},
	    {OneRow("\"login\":\"duckdb\",\"site_admin\":false"), "id"},
	    {OneRow("\"id\":1,\"site_admin\":false"), "login"},
	    {OneRow("\"id\":1,\"login\":\"duckdb\""), "site_admin"},
	    {OneRow("\"id\":1,\"id\":2,\"login\":\"duckdb\",\"site_admin\":false"), "id"},
	    {OneRow("\"id\":\"1\",\"login\":\"duckdb\",\"site_admin\":false"), "id"},
	    {OneRow("\"id\":1.0,\"login\":\"duckdb\",\"site_admin\":false"), "id"},
	    {OneRow("\"id\":1e0,\"login\":\"duckdb\",\"site_admin\":false"), "id"},
	    {OneRow("\"id\":9223372036854775808,\"login\":\"duckdb\",\"site_admin\":false"), "id"},
	    {OneRow("\"id\":1,\"login\":null,\"site_admin\":false"), "login"},
	    {OneRow("\"id\":1,\"login\":\"duckdb\",\"site_admin\":0"), "site_admin"}};
	for (std::size_t index = 0; index < cases.size(); index++) {
		RequireError([&]() { Decode(cases[index].body, control); }, cuac::ErrorStage::SCHEMA, cases[index].field,
		             "9223372036854775808");
	}
}

void TestExactResourceBoundaries() {
	ManualControl control;
	auto plan = Plan();
	plan.max_string_bytes = 3;
	const auto exact = OneRow("\"id\":1,\"login\":\"abc\",\"site_admin\":false");
	Require(DecodeRows(exact, plan, control).size() == 1, "exact string boundary was rejected");
	const auto long_string = OneRow("\"id\":1,\"login\":\"abcd\",\"site_admin\":false");
	RequireError([&]() { DecodeRows(long_string, plan, control); }, cuac::ErrorStage::RESOURCE, "login");

	const std::string four_rows = "{\"items\":[{\"id\":1,\"login\":\"a\",\"site_admin\":false},"
	                              "{\"id\":2,\"login\":\"b\",\"site_admin\":false},"
	                              "{\"id\":3,\"login\":\"c\",\"site_admin\":false},"
	                              "{\"id\":4,\"login\":\"d\",\"site_admin\":false}]}";
	RequireError([&]() { DecodeRows(four_rows, Plan(), control); }, cuac::ErrorStage::RESOURCE, "items");

	plan = Plan();
	plan.max_json_nesting = 2;
	const std::string exact_nesting = "{\"ignored\":[0],\"items\":[]}";
	Require(DecodeRows(exact_nesting, plan, control).empty(), "exact nesting boundary was rejected");
	const std::string deep = "{\"ignored\":[[0]],\"items\":[]}";
	RequireError([&]() { DecodeRows(deep, plan, control); }, cuac::ErrorStage::RESOURCE, "json_nesting");

	plan = Plan();
	plan.max_decoded_memory_bytes = 1;
	RequireError([&]() { DecodeRows("{\"items\":[]}", plan, control); }, cuac::ErrorStage::RESOURCE,
	             "decoded_memory_bytes");
}

void TestDoubleFormatEquivalence() {
	// RFC 0020: different JSON-source spellings of the same double value must
	// decode to the identical bit pattern.
	cuac::internal::JsonDecodePlan plan;
	plan.response_source = cuac::internal::JsonResponseSource::JSON_PATH_MANY;
	plan.records_path = {"items"};
	plan.columns = {{"value", "value", cuac::ValueKind::DOUBLE}};
	plan.max_records = 1;
	plan.max_string_bytes = 256;
	plan.max_json_nesting = 16;
	plan.max_decoded_memory_bytes = 4096;
	plan.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	ManualControl control;
	const char *spellings[] = {"1.5", "1.50", "1.5e0", "0.00015e4", "15e-1"};
	for (const auto *spelling : spellings) {
		const auto rows = DecodeRows(std::string("{\"items\":[{\"value\":") + spelling + "}]}", plan, control);
		Require(rows.size() == 1 && rows[0].values.size() == 1 && rows[0].values[0].valid &&
		            rows[0].values[0].double_value == 1.5,
		        std::string("DOUBLE spelling \"") + spelling + "\" did not decode to the identical value");
	}
}

void TestFlatScalarArraysAndFailures() {
	ManualControl control;
	auto plan = ArrayPlan();
	const auto body = "{\"items\":["
	                  "{\"flags\":[true,null,false],\"ids\":[-9223372036854775808,7,7,9223372036854775807],"
	                  "\"names\":[\"alpha\",\"\",\"alpha\"],\"scores\":[-0.0,4.9e-324,1.5]},"
	                  "{\"flags\":[],\"ids\":[],\"names\":null,\"scores\":[]}] }";
	const auto page = cuac::internal::DecodeJsonPage(body, plan, control);
	Require(page.rows.size() == 2 && page.rows[0].values.size() == 4 &&
	            page.rows[0].values[0].shape == cuac::ValueShape::ARRAY &&
	            page.rows[0].values[0].elements.size() == 3 && page.rows[0].values[0].elements[0].boolean_value &&
	            !page.rows[0].values[0].elements[1].valid && !page.rows[0].values[0].elements[2].boolean_value &&
	            page.rows[0].values[1].elements.front().bigint_value == std::numeric_limits<int64_t>::min() &&
	            page.rows[0].values[1].elements[1].bigint_value == 7 &&
	            page.rows[0].values[1].elements[2].bigint_value == 7 &&
	            page.rows[0].values[1].elements.back().bigint_value == std::numeric_limits<int64_t>::max() &&
	            page.rows[0].values[2].elements[0].varchar_value == "alpha" &&
	            page.rows[0].values[2].elements[1].varchar_value.empty() &&
	            page.rows[0].values[3].elements[0].double_value == 0.0 &&
	            page.rows[0].values[3].elements[2].double_value == 1.5 && page.rows[1].values[0].elements.empty() &&
	            !page.rows[1].values[2].valid,
	        "REST ARRAY decoding changed order, duplicates, empty arrays, NULL, or scalar boundaries");

	auto exact_memory = plan;
	exact_memory.max_decoded_memory_bytes = page.peak_memory_bytes;
	Require(cuac::internal::DecodeJsonPage(body, exact_memory, control).retained_memory_bytes ==
	            page.retained_memory_bytes,
	        "REST ARRAY decoded-memory peak boundary changed retained accounting");
	auto one_under = exact_memory;
	one_under.max_decoded_memory_bytes--;
	RequireError([&]() { (void)cuac::internal::DecodeJsonPage(body, one_under, control); }, cuac::ErrorStage::RESOURCE,
	             "decoded_memory_bytes");

	const std::vector<std::pair<std::string, std::string>> failures = {
	    {"{\"items\":[{\"flags\":true,\"ids\":[],\"names\":[],\"scores\":[]}]}", "flags"},
	    {"{\"items\":[{\"flags\":[],\"ids\":[1,\"late\"],\"names\":[],\"scores\":[]}]}", "ids"},
	    {"{\"items\":[{\"flags\":[],\"ids\":[[1]],\"names\":[],\"scores\":[]}]}", "ids"},
	    {"{\"items\":[{\"flags\":[],\"ids\":[null],\"names\":[],\"scores\":[]}]}", "ids"},
	    {"{\"items\":[{\"flags\":[],\"ids\":[],\"names\":[],\"scores\":[1e400]}]}", "scores"}};
	for (const auto &failure : failures) {
		RequireError([&]() { (void)DecodeRows(failure.first, plan, control); }, cuac::ErrorStage::SCHEMA,
		             failure.second);
	}
	plan.max_string_bytes = 3;
	Require(
	    DecodeRows("{\"items\":[{\"flags\":[],\"ids\":[],\"names\":[\"abc\"],\"scores\":[]}]}", plan, control).size() ==
	        1,
	    "REST ARRAY string element exact byte boundary was rejected");
	RequireError(
	    [&]() {
		    (void)DecodeRows("{\"items\":[{\"flags\":[],\"ids\":[],\"names\":[\"abcd\"],\"scores\":[]}]}", plan,
		                     control);
	    },
	    cuac::ErrorStage::RESOURCE, "names");

	// The score failure is deliberately later in the same object. Retained
	// VARCHAR child capacity must be charged while names are still decoded;
	// otherwise the parser would traverse an over-budget row and report the
	// later schema error instead.
	auto pending_strings = ArrayPlan();
	pending_strings.max_string_bytes = 128;
	pending_strings.max_decoded_memory_bytes = 5000;
	std::string names;
	for (std::size_t index = 0; index < 32; index++) {
		names += names.empty() ? "\"" : ",\"";
		names += std::string(120, static_cast<char>('a' + (index % 26)));
		names += "\"";
	}
	const auto pending_string_body =
	    "{\"items\":[{\"flags\":[],\"ids\":[],\"names\":[" + names + "],\"scores\":[true]}]}";
	RequireError([&]() { (void)DecodeRows(pending_string_body, pending_strings, control); }, cuac::ErrorStage::RESOURCE,
	             "decoded_memory_bytes");

	std::string elements;
	for (std::size_t index = 0; index < 512; index++) {
		elements += elements.empty() ? "true" : ",true";
	}
	const auto cancellation_body = "{\"items\":[{\"flags\":[" + elements + "],\"ids\":[],\"names\":[],\"scores\":[]}]}";
	CountingControl count_only;
	(void)DecodeRows(cancellation_body, ArrayPlan(), count_only);
	CountingControl cancelled(count_only.Calls() * 3 / 4);
	bool cancelled_during_array = false;
	try {
		(void)DecodeRows(cancellation_body, ArrayPlan(), cancelled);
	} catch (const cuac::ExecutionCancelled &) {
		cancelled_during_array = true;
	}
	Require(cancelled_during_array, "REST ARRAY element traversal did not observe cancellation");
}

void TestReplacementAllocationsUseIndependentPeakOracles() {
	ManualControl control;
	const auto scalar_plan = SingleColumnPlan(cuac::OutputValueType::Scalar(cuac::ValueKind::BIGINT));
	const auto baseline = cuac::internal::DecodeJsonPage("{\"items\":[{\"value\":1}]}", scalar_plan, control);
	Require(baseline.rows.size() == 1 && baseline.rows[0].values.size() == 1,
	        "REST replacement-allocation baseline changed shape");
	const auto value_storage = static_cast<uint64_t>(baseline.rows[0].values.capacity()) * sizeof(cuac::TypedValue);
	Require(baseline.peak_memory_bytes >= value_storage,
	        "REST replacement-allocation baseline cannot isolate value storage");

	std::vector<cuac::TypedScalarValue> element_probe;
	std::size_t previous_element_capacity = 0;
	const std::size_t element_count = 65;
	for (std::size_t index = 0; index < element_count; index++) {
		if (element_probe.size() == element_probe.capacity()) {
			previous_element_capacity = element_probe.capacity();
			const auto requested = previous_element_capacity == 0 ? 1 : previous_element_capacity * 2;
			element_probe.reserve(requested);
		}
		element_probe.push_back(cuac::TypedScalarValue::BigInt(static_cast<int64_t>(index)));
	}
	const auto previous_array_bytes = static_cast<uint64_t>(previous_element_capacity) * sizeof(cuac::TypedScalarValue);
	const auto replacement_array_bytes =
	    static_cast<uint64_t>(element_probe.capacity()) * sizeof(cuac::TypedScalarValue);
	Require(previous_array_bytes > value_storage,
	        "REST ARRAY oracle must make replacement co-liveness the unique peak");
	std::string array_body = "{\"items\":[{\"value\":[";
	for (std::size_t index = 0; index < element_count; index++) {
		if (index != 0) {
			array_body += ',';
		}
		array_body += std::to_string(index);
	}
	array_body += "]}]}";
	const auto array_plan = SingleColumnPlan(cuac::OutputValueType::Array(cuac::ValueKind::BIGINT, false));
	const auto array_page = cuac::internal::DecodeJsonPage(array_body, array_plan, control);
	Require(array_page.rows[0].values[0].elements.capacity() == element_probe.capacity(),
	        "REST ARRAY capacity oracle drifted from decoder allocation");
	const auto expected_array_peak =
	    baseline.peak_memory_bytes - value_storage + previous_array_bytes + replacement_array_bytes;
	Require(array_page.peak_memory_bytes == expected_array_peak,
	        "REST ARRAY peak must include current and replacement element buffers");
	auto exact_array_plan = array_plan;
	exact_array_plan.max_decoded_memory_bytes = expected_array_peak;
	(void)cuac::internal::DecodeJsonPage(array_body, exact_array_plan, control);
	auto under_array_plan = exact_array_plan;
	under_array_plan.max_decoded_memory_bytes--;
	RequireError([&]() { (void)cuac::internal::DecodeJsonPage(array_body, under_array_plan, control); },
	             cuac::ErrorStage::RESOURCE, "decoded_memory_bytes");

	std::string string_probe;
	std::size_t previous_string_capacity = 0;
	do {
		previous_string_capacity = string_probe.capacity();
		string_probe.reserve(previous_string_capacity + 1);
		string_probe.resize(previous_string_capacity + 1, 'x');
	} while (previous_string_capacity <= value_storage);
	Require(string_probe.size() <= scalar_plan.max_string_bytes,
	        "REST VARCHAR oracle exceeded the declared string budget");
	const auto string_plan = SingleColumnPlan(cuac::OutputValueType::Scalar(cuac::ValueKind::VARCHAR));
	const auto string_body = "{\"items\":[{\"value\":\"" + string_probe + "\"}]}";
	const auto string_page = cuac::internal::DecodeJsonPage(string_body, string_plan, control);
	Require(string_page.rows[0].values[0].varchar_value.capacity() == string_probe.capacity(),
	        "REST VARCHAR capacity oracle drifted from decoder allocation");
	const auto expected_string_peak = baseline.peak_memory_bytes - value_storage +
	                                  static_cast<uint64_t>(previous_string_capacity) +
	                                  static_cast<uint64_t>(string_probe.capacity());
	Require(string_page.peak_memory_bytes == expected_string_peak,
	        "REST VARCHAR peak must include current and replacement string buffers");
	auto exact_string_plan = string_plan;
	exact_string_plan.max_decoded_memory_bytes = expected_string_peak;
	(void)cuac::internal::DecodeJsonPage(string_body, exact_string_plan, control);
	auto under_string_plan = exact_string_plan;
	under_string_plan.max_decoded_memory_bytes--;
	RequireError([&]() { (void)cuac::internal::DecodeJsonPage(string_body, under_string_plan, control); },
	             cuac::ErrorStage::RESOURCE, "decoded_memory_bytes");
}

void TestContinuationMemoryBoundaries() {
	ManualControl control;
	auto plan = Plan();
	plan.page_continuation_path = {"next"};
	plan.max_string_bytes = 512;
	plan.max_decoded_memory_bytes = 65536;
	const std::string continuation = "https://example.test/items?page=2&padding=" + std::string(400, 'x');
	const auto body =
	    "{\"items\":[{\"id\":1,\"login\":\"one\",\"site_admin\":false}],\"next\":\"" + continuation + "\"}";
	const auto page = cuac::internal::DecodeJsonPage(body, plan, control);
	const auto no_continuation = cuac::internal::DecodeJsonPage(
	    "{\"items\":[{\"id\":1,\"login\":\"one\",\"site_admin\":false}],\"next\":null}", plan, control);
	Require(page.next_url == continuation && page.continuation_memory_bytes > 0 &&
	            page.peak_memory_bytes > no_continuation.peak_memory_bytes,
	        "response_next storage was absent from retained decode-peak accounting");
	auto exact = plan;
	exact.max_decoded_memory_bytes = page.peak_memory_bytes;
	Require(cuac::internal::DecodeJsonPage(body, exact, control).next_url == continuation,
	        "response_next exact decoded-memory peak was rejected");
	auto one_under = exact;
	one_under.max_decoded_memory_bytes--;
	RequireError([&]() { (void)cuac::internal::DecodeJsonPage(body, one_under, control); }, cuac::ErrorStage::RESOURCE,
	             "decoded_memory_bytes");
}

void TestCancellationAndDeadline() {
	ManualControl cancelled;
	cancelled.Cancel();
	bool observed = false;
	try {
		Decode("{\"items\":[]}", cancelled);
	} catch (const cuac::ExecutionCancelled &) {
		observed = true;
	}
	Require(observed, "decoder did not observe pre-parse cancellation");

	ManualControl control;
	auto expired = Plan();
	expired.deadline = std::chrono::steady_clock::now();
	RequireError([&]() { DecodeRows("{\"items\":[]}", expired, control); }, cuac::ErrorStage::RESOURCE,
	             "wall_milliseconds");
}

} // namespace

int main() {
	try {
		TestTypedRowsAndCompleteValidation();
		TestStrictSuccessfulRootObject();
		TestNestedPermanentRestPaths();
		TestMalformedAndInvalidUtf8();
		TestRequiredShapeAndStrictTypes();
		TestExactResourceBoundaries();
		TestDoubleFormatEquivalence();
		TestFlatScalarArraysAndFailures();
		TestReplacementAllocationsUseIndependentPeakOracles();
		TestContinuationMemoryBoundaries();
		TestCancellationAndDeadline();
		std::cout << "JSON decoder tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "JSON decoder tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
