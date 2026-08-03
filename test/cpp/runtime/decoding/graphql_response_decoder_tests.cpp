#include "cuac/internal/runtime/decoding/graphql_response_decoder.hpp"
#include "cuac/internal/runtime/admission/graphql_plan_admission.hpp"
#include "cuac/internal/runtime/executor/http_scan_executor.hpp"
#include "semantics/support/repository_graphql_scan_plan_test_fixtures.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

const char *const APPLICATION_ERROR_MESSAGE = "remote protocol response reported application errors";

class NeverCancelled final : public cuac::ExecutionControl {
public:
	bool IsCancellationRequested() const noexcept override {
		return false;
	}
};

void Require(bool condition, const char *message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

std::unique_ptr<const cuac::internal::AdmittedGraphqlRequestProfile> Profile() {
	const auto plan = cuac_test::BuildRepositoryGithubPackageGraphqlPlan(CUAC_SOURCE_ROOT, "decoder_secret");
	const cuac::internal::HttpExecutionProfile host {cuac::PlannedUrlScheme::HTTPS,
	                                                 "api.github.com",
	                                                 443,
	                                                 false,
	                                                 false,
	                                                 false,
	                                                 30000,
	                                                 100,
	                                                 cuac::RETRY_MAX_REQUEST_ATTEMPTS_PER_STEP,
	                                                 cuac::RETRY_MAX_REQUEST_ATTEMPTS_PER_SCAN,
	                                                 cuac::RETRY_MAX_DELAY_MILLISECONDS,
	                                                 cuac::RETRY_MAX_CUMULATIVE_WAITING_MILLISECONDS_PER_SCAN};
	return cuac::internal::TryAdmitGraphqlPlan(plan, host);
}

std::unique_ptr<const cuac::internal::AdmittedGraphqlRequestProfile> ArrayProfile() {
	const auto plan = cuac_test::BuildRepositoryGithubPackageGraphqlArrayPlan(CUAC_SOURCE_ROOT, "decoder_secret");
	const cuac::internal::HttpExecutionProfile host {cuac::PlannedUrlScheme::HTTPS,
	                                                 "api.github.com",
	                                                 443,
	                                                 false,
	                                                 false,
	                                                 false,
	                                                 30000,
	                                                 100,
	                                                 cuac::RETRY_MAX_REQUEST_ATTEMPTS_PER_STEP,
	                                                 cuac::RETRY_MAX_REQUEST_ATTEMPTS_PER_SCAN,
	                                                 cuac::RETRY_MAX_DELAY_MILLISECONDS,
	                                                 cuac::RETRY_MAX_CUMULATIVE_WAITING_MILLISECONDS_PER_SCAN};
	return cuac::internal::TryAdmitGraphqlPlan(plan, host);
}

cuac::internal::GraphqlDecodeLimits Limits() {
	return {100, 512, 16, 2 * 1024 * 1024, std::chrono::steady_clock::now() + std::chrono::seconds(30)};
}

std::string Node(const std::string &id, const std::string &stars = "1", const std::string &language = "null") {
	return "{\"id\":\"" + id + "\",\"nameWithOwner\":\"o/r\",\"owner\":{\"login\":\"o\"}," +
	       "\"stargazerCount\":" + stars + ",\"primaryLanguage\":" + language +
	       ",\"isPrivate\":false,\"isArchived\":false,\"updatedAt\":\"2026-01-01T00:00:00Z\"}";
}

std::string Envelope(const std::string &nodes, bool has_next = false, const std::string &cursor = "null",
                     const std::string &extra = "") {
	return "{" + extra + "\"data\":{\"viewer\":{\"repositories\":{\"nodes\":[" + nodes +
	       "],\"pageInfo\":{\"hasNextPage\":" + (has_next ? "true" : "false") + ",\"endCursor\":" + cursor +
	       "}}}},\"errors\":[]}";
}

void RequireFailure(const std::string &body, cuac::ErrorStage stage, const std::string &field) {
	auto profile = Profile();
	NeverCancelled control;
	try {
		(void)cuac::internal::DecodeGraphqlResponse(body, *profile, Limits(), control);
		throw std::runtime_error("invalid GraphQL response must fail");
	} catch (const cuac::ExecutionError &error) {
		if (error.Stage() != stage || (!field.empty() && error.Field() != field)) {
			throw std::runtime_error("GraphQL response expected field '" + field + "' but received '" + error.Field() +
			                         "'");
		}
	}
}

void TestEightColumnsAndNullableLanguage() {
	auto profile = Profile();
	Require(static_cast<bool>(profile), "decoder test profile must be admitted");
	NeverCancelled control;
	const std::string body =
	    "{\"data\":{\"viewer\":{\"repositories\":{\"nodes\":["
	    "{\"id\":\"R1\",\"nameWithOwner\":\"o/one\",\"owner\":{\"login\":\"o\"},"
	    "\"stargazerCount\":7,\"primaryLanguage\":{\"name\":\"\\uD83D\\uDE00\"},\"isPrivate\":false,"
	    "\"isArchived\":true,\"updatedAt\":\"2026-01-01T00:00:00Z\"},"
	    "{\"id\":\"R2\",\"nameWithOwner\":\"o/two\",\"owner\":{\"login\":\"o\"},"
	    "\"stargazerCount\":0,\"primaryLanguage\":null,\"isPrivate\":true,"
	    "\"isArchived\":false,\"updatedAt\":\"2026-02-01T00:00:00Z\"}],"
	    "\"pageInfo\":{\"hasNextPage\":true,\"endCursor\":\"cursor-2\"}}}},\"errors\":[]}";
	auto decoded = cuac::internal::DecodeGraphqlResponse(body, *profile, Limits(), control);
	Require(decoded.rows.size() == 2 && decoded.rows[0].values.size() == 8 && decoded.has_next &&
	            decoded.end_cursor == "cursor-2",
	        "decoder must produce the admitted schema and cursor metadata");
	Require(decoded.rows[0].values[3].bigint_value == 7 && decoded.rows[0].values[4].valid &&
	            decoded.rows[0].values[4].varchar_value == "\xF0\x9F\x98\x80" &&
	            decoded.rows[0].values[4].varchar_value.size() == 4 && !decoded.rows[1].values[4].valid &&
	            decoded.rows[1].values[4].kind == cuac::ValueKind::VARCHAR &&
	            decoded.rows[0].values[7].kind == cuac::ValueKind::TIMESTAMPTZ &&
	            decoded.rows[0].values[7].timestamptz_microseconds == INT64_C(1767225600000000) &&
	            decoded.rows[1].values[7].timestamptz_microseconds == INT64_C(1769904000000000),
	        "decoder must preserve strict scalars and nullable language");
}

void TestTimestamptzStrictProfile() {
	auto profile = Profile();
	NeverCancelled control;
	const std::vector<std::pair<std::string, std::int64_t>> valid = {
	    {"1970-01-01T00:00:00Z", 0},
	    {"1970-01-01T00:00:00.1Z", 100000},
	    {"1970-01-01T00:00:00.12Z", 120000},
	    {"1970-01-01T00:00:00.123Z", 123000},
	    {"1970-01-01T00:00:00.1234Z", 123400},
	    {"1970-01-01T00:00:00.12345Z", 123450},
	    {"2000-02-29T12:34:56.123456Z", INT64_C(951827696123456)},
	    {"2026-07-01T01:30:00+01:30", INT64_C(1782864000000000)},
	    {"2026-06-30T10:00:00-14:00", INT64_C(1782864000000000)},
	    {"0001-01-01T00:00:00.000000Z", -INT64_C(62135596800000000)},
	    {"9999-12-31T23:59:59.999999Z", INT64_C(253402300799999999)},
	};
	for (const auto &entry : valid) {
		auto node = Node("timestamp");
		const auto begin = node.find("2026-01-01T00:00:00Z");
		Require(begin != std::string::npos, "GraphQL TIMESTAMPTZ valid fixture drifted");
		node.replace(begin, 20, entry.first);
		const auto decoded = cuac::internal::DecodeGraphqlResponse(Envelope(node), *profile, Limits(), control);
		Require(decoded.rows.size() == 1 && decoded.rows[0].values[7].kind == cuac::ValueKind::TIMESTAMPTZ &&
		            decoded.rows[0].values[7].timestamptz_microseconds == entry.second,
		        "GraphQL TIMESTAMPTZ valid vector changed exact UTC microseconds");
	}
	const std::vector<std::string> invalid = {
	    "1970-01-01 00:00:00Z",
	    "1970-01-01t00:00:00z",
	    "1970-01-01T00:00:00",
	    "1970-01-01T00:00:00-00:00",
	    "1970-01-01T00:00:00+14:01",
	    "1970-01-01T00:00:00+15:00",
	    "1970-01-01T00:00:60Z",
	    "2001-02-29T00:00:00Z",
	    "1970-01-01T00:00:00.1234567Z",
	    "1970-01-01T00:00:00.Z",
	    " 1970-01-01T00:00:00Z",
	    "0",
	    "infinity",
	};
	for (const auto &spelling : invalid) {
		auto node = Node("timestamp");
		const auto begin = node.find("2026-01-01T00:00:00Z");
		Require(begin != std::string::npos, "GraphQL TIMESTAMPTZ invalid fixture drifted");
		node.replace(begin, 20, spelling);
		RequireFailure(Envelope(node), cuac::ErrorStage::SCHEMA, "updated_at");
	}
	auto numeric = Node("timestamp");
	const auto numeric_begin = numeric.find("\"2026-01-01T00:00:00Z\"");
	Require(numeric_begin != std::string::npos, "GraphQL numeric-epoch fixture drifted");
	numeric.replace(numeric_begin, 22, "0");
	RequireFailure(Envelope(numeric), cuac::ErrorStage::SCHEMA, "updated_at");
}

void TestErrorsPrecedeDataWithoutLeakingRemoteValues() {
	auto profile = Profile();
	NeverCancelled control;
	const std::string data_canary = "remote-secret-data";
	const std::string error_canary = "remote-secret-error";
	const std::string path_canary = "remote-secret-path";
	const std::string body = "{\"data\":{\"private\":\"" + data_canary + "\"},\"errors\":[{\"message\":\"" +
	                         error_canary + "\",\"path\":[\"" + path_canary + "\"]}]}";
	try {
		(void)cuac::internal::DecodeGraphqlResponse(body, *profile, Limits(), control);
		throw std::runtime_error("nonempty errors must fail");
	} catch (const cuac::ExecutionError &error) {
		Require(error.Stage() == cuac::ErrorStage::REMOTE_PROTOCOL && error.Field() == "errors" &&
		            error.SafeMessage() == APPLICATION_ERROR_MESSAGE &&
		            error.SafeMessage().find(data_canary) == std::string::npos &&
		            error.SafeMessage().find(error_canary) == std::string::npos &&
		            error.SafeMessage().find(path_canary) == std::string::npos,
		        "remote errors must win with the exact redacted protocol failure");
	}
	const std::string long_unknown_key(65, 'k');
	const std::string long_key_body = "{\"" + long_unknown_key + "\":\"ignored\",\"errors\":[{}],\"data\":null}";
	try {
		(void)cuac::internal::DecodeGraphqlResponse(long_key_body, *profile, Limits(), control);
		throw std::runtime_error("errors after an overlong unknown key must fail");
	} catch (const cuac::ExecutionError &error) {
		Require(error.Stage() == cuac::ErrorStage::REMOTE_PROTOCOL && error.Field() == "errors" &&
		            error.SafeMessage() == APPLICATION_ERROR_MESSAGE,
		        "allocation-free unknown-key skipping changed GraphQL errors precedence");
	}
}

void TestMalformedDocumentPrecedesRemoteProtocol() {
	auto profile = Profile();
	NeverCancelled control;
	try {
		(void)cuac::internal::DecodeGraphqlResponse("{\"errors\":[{}],\"later\":[}", *profile, Limits(), control);
		throw std::runtime_error("malformed document must fail");
	} catch (const cuac::ExecutionError &error) {
		Require(error.Stage() == cuac::ErrorStage::DECODE,
		        "complete syntax validation must precede GraphQL envelope semantics");
	}
}

void TestRequiredNestedFieldFailsClosed() {
	auto profile = Profile();
	NeverCancelled control;
	const std::string body = "{\"data\":{\"viewer\":{\"repositories\":{\"nodes\":[{\"id\":\"R\","
	                         "\"nameWithOwner\":\"o/r\",\"owner\":{},\"stargazerCount\":1,\"primaryLanguage\":null,"
	                         "\"isPrivate\":false,\"isArchived\":false,\"updatedAt\":\"2026-01-01T00:00:00Z\"}],"
	                         "\"pageInfo\":{\"hasNextPage\":false,\"endCursor\":null}}}}}";
	try {
		(void)cuac::internal::DecodeGraphqlResponse(body, *profile, Limits(), control);
		throw std::runtime_error("missing owner login must fail");
	} catch (const cuac::ExecutionError &error) {
		Require(error.Stage() == cuac::ErrorStage::SCHEMA && error.Field() == "owner_login",
		        "nested required-field failure must use the safe output name");
	}
}

void TestZeroHundredAndDuplicateOccurrences() {
	auto profile = Profile();
	NeverCancelled control;
	auto zero = cuac::internal::DecodeGraphqlResponse(Envelope(""), *profile, Limits(), control);
	Require(zero.rows.empty() && !zero.has_next, "zero-node GraphQL page must be valid terminal input");

	std::string hundred_nodes;
	for (std::size_t index = 0; index < 100; index++) {
		if (!hundred_nodes.empty()) {
			hundred_nodes += ',';
		}
		hundred_nodes += Node("R" + std::to_string(index));
	}
	auto hundred = cuac::internal::DecodeGraphqlResponse(Envelope(hundred_nodes), *profile, Limits(), control);
	Require(hundred.rows.size() == 100 && hundred.rows.front().values[0].varchar_value == "R0" &&
	            hundred.rows.back().values[0].varchar_value == "R99",
	        "exactly 100 GraphQL nodes must preserve response order");

	auto duplicates =
	    cuac::internal::DecodeGraphqlResponse(Envelope(Node("same") + "," + Node("same")), *profile, Limits(), control);
	Require(duplicates.rows.size() == 2 && duplicates.rows[0].values[0].varchar_value == "same" &&
	            duplicates.rows[1].values[0].varchar_value == "same",
	        "GraphQL decoder must preserve duplicate repository occurrences");
}

void TestIgnoredValuesDoNotConsumeDecodedMemory() {
	auto profile = Profile();
	NeverCancelled control;
	const std::string ignored(2 * 1024 * 1024 + 1, 'x');
	const auto body = Envelope(Node("R"), false, "null", "\"ignored\":\"" + ignored + "\",");
	Require(body.size() < 8 * 1024 * 1024, "ignored-value fixture must remain inside the response ceiling");
	auto decoded = cuac::internal::DecodeGraphqlResponse(body, *profile, Limits(), control);
	Require(decoded.rows.size() == 1 && decoded.rows[0].values[0].varchar_value == "R",
	        "ignored JSON strings must be validated without materialization");
}

void TestMaterializedStringAndMemoryBoundaries() {
	auto profile = Profile();
	NeverCancelled control;
	const std::string exact(512, 'x');
	auto decoded = cuac::internal::DecodeGraphqlResponse(Envelope(Node(exact)), *profile, Limits(), control);
	Require(decoded.rows.size() == 1 && decoded.rows[0].values[0].varchar_value.size() == 512,
	        "string byte ceiling must accept its exact boundary");
	RequireFailure(Envelope(Node(exact + "x")), cuac::ErrorStage::RESOURCE, "id");

	auto exact_memory = Limits();
	exact_memory.max_decoded_memory_bytes = decoded.peak_memory_bytes;
	auto exact_decoded = cuac::internal::DecodeGraphqlResponse(Envelope(Node(exact)), *profile, exact_memory, control);
	Require(exact_decoded.retained_memory_bytes == decoded.retained_memory_bytes,
	        "decoded-memory exact boundary must retain the same complete page");
	Require(decoded.retained_memory_bytes > 0, "decoded page must report retained memory");
	auto one_under = exact_memory;
	one_under.max_decoded_memory_bytes--;
	try {
		(void)cuac::internal::DecodeGraphqlResponse(Envelope(Node(exact)), *profile, one_under, control);
		throw std::runtime_error("one-byte-under decoded-memory budget must fail");
	} catch (const cuac::ExecutionError &error) {
		Require(error.Stage() == cuac::ErrorStage::RESOURCE && error.Field() == "decoded_memory_bytes",
		        "decoded-memory +1 case used the wrong safe diagnostic");
	}
}

void TestWrongNullDuplicateAndEnvelopeShapesFailClosed() {
	RequireFailure(Envelope(Node("R", "\"1\"")), cuac::ErrorStage::SCHEMA, "stars");
	const auto null_id = Node("R");
	auto null_required = null_id;
	const auto id = null_required.find("\"id\":\"R\"");
	Require(id != std::string::npos, "required-null fixture drifted");
	null_required.replace(id, 8, "\"id\":null");
	RequireFailure(Envelope(null_required), cuac::ErrorStage::SCHEMA, "id");

	auto duplicate = Node("R");
	duplicate.insert(1, "\"id\":\"other\",");
	RequireFailure(Envelope(duplicate), cuac::ErrorStage::SCHEMA, "id");
	RequireFailure("{}", cuac::ErrorStage::SCHEMA, "data.viewer.repositories.nodes");
	RequireFailure("{\"data\":null,\"errors\":[]}", cuac::ErrorStage::SCHEMA, "data.viewer.repositories.nodes");
	auto duplicate_data = Envelope(Node("R"));
	const auto errors = duplicate_data.find(",\"errors\"");
	Require(errors != std::string::npos, "duplicate-data fixture drifted");
	duplicate_data.insert(errors + 1, "\"data\":{},");
	RequireFailure(duplicate_data, cuac::ErrorStage::SCHEMA, "data.viewer.repositories.nodes");
	RequireFailure("{\"data\":{},\"errors\":null}", cuac::ErrorStage::SCHEMA, "errors");
}

void TestGraphqlScalarArrays() {
	auto profile = ArrayProfile();
	Require(static_cast<bool>(profile), "GraphQL ARRAY plan must be admitted");
	NeverCancelled control;
	const std::string first =
	    "{\"id\":[\"R1\",\"R1\"],\"nameWithOwner\":\"o/r\",\"owner\":{\"login\":\"o\"},"
	    "\"stargazerCount\":[-9223372036854775808,9223372036854775807],\"primaryLanguage\":null,"
	    "\"isPrivate\":[true,false,true],\"isArchived\":false,\"updatedAt\":\"2026-01-01T00:00:00Z\"}";
	const std::string second = "{\"id\":[],\"nameWithOwner\":\"o/empty\",\"owner\":{\"login\":\"o\"},"
	                           "\"stargazerCount\":[],\"primaryLanguage\":null,\"isPrivate\":[],\"isArchived\":false,"
	                           "\"updatedAt\":\"2026-01-01T00:00:00Z\"}";
	auto decoded = cuac::internal::DecodeGraphqlResponse(Envelope(first + "," + second), *profile, Limits(), control);
	Require(decoded.rows.size() == 2 && decoded.rows[0].values[0].elements.size() == 2 &&
	            decoded.rows[0].values[0].elements[0].varchar_value == "R1" &&
	            decoded.rows[0].values[0].elements[1].varchar_value == "R1" &&
	            decoded.rows[0].values[3].elements.front().bigint_value == std::numeric_limits<int64_t>::min() &&
	            decoded.rows[0].values[3].elements.back().bigint_value == std::numeric_limits<int64_t>::max() &&
	            decoded.rows[0].values[5].elements.size() == 3 && decoded.rows[0].values[5].elements[0].boolean_value &&
	            !decoded.rows[0].values[5].elements[1].boolean_value && decoded.rows[1].values[0].elements.empty() &&
	            decoded.rows[1].values[3].elements.empty() && decoded.rows[1].values[5].elements.empty(),
	        "GraphQL ARRAY decoding changed element kinds, order, duplicates, or empty lists");
	auto exact_memory = Limits();
	exact_memory.max_decoded_memory_bytes = decoded.peak_memory_bytes;
	const auto exact_decoded =
	    cuac::internal::DecodeGraphqlResponse(Envelope(first + "," + second), *profile, exact_memory, control);
	Require(exact_decoded.retained_memory_bytes == decoded.retained_memory_bytes,
	        "GraphQL ARRAY exact co-live decoded-memory peak was rejected");
	auto one_under = exact_memory;
	one_under.max_decoded_memory_bytes--;
	try {
		(void)cuac::internal::DecodeGraphqlResponse(Envelope(first + "," + second), *profile, one_under, control);
		throw std::runtime_error("GraphQL ARRAY one-byte-under co-live decoded-memory peak must fail");
	} catch (const cuac::ExecutionError &error) {
		Require(error.Stage() == cuac::ErrorStage::RESOURCE && error.Field() == "decoded_memory_bytes",
		        "GraphQL ARRAY co-live decoded-memory rejection used the wrong diagnostic");
	}

	const std::vector<std::pair<std::string, std::string>> failures = {
	    {first.substr(0, first.find("[\"R1\",\"R1\"]")) + "\"wrong\"" +
	         first.substr(first.find("[\"R1\",\"R1\"]") + 11),
	     "id"},
	    {"{\"id\":[\"R\",7],\"nameWithOwner\":\"o/r\",\"owner\":{\"login\":\"o\"},"
	     "\"stargazerCount\":[],\"primaryLanguage\":null,\"isPrivate\":[],\"isArchived\":false,"
	     "\"updatedAt\":\"2026-01-01T00:00:00Z\"}",
	     "id"},
	    {"{\"id\":[[\"R\"]],\"nameWithOwner\":\"o/r\",\"owner\":{\"login\":\"o\"},"
	     "\"stargazerCount\":[],\"primaryLanguage\":null,\"isPrivate\":[],\"isArchived\":false,"
	     "\"updatedAt\":\"2026-01-01T00:00:00Z\"}",
	     "id"},
	    {"{\"id\":[null],\"nameWithOwner\":\"o/r\",\"owner\":{\"login\":\"o\"},"
	     "\"stargazerCount\":[],\"primaryLanguage\":null,\"isPrivate\":[],\"isArchived\":false,"
	     "\"updatedAt\":\"2026-01-01T00:00:00Z\"}",
	     "id"}};
	for (const auto &failure : failures) {
		try {
			(void)cuac::internal::DecodeGraphqlResponse(Envelope(failure.first), *profile, Limits(), control);
			throw std::runtime_error("invalid GraphQL ARRAY response must fail");
		} catch (const cuac::ExecutionError &error) {
			Require(error.Stage() == cuac::ErrorStage::SCHEMA && error.Field() == failure.second,
			        "GraphQL ARRAY rejection used the wrong safe field");
		}
	}
}

void TestGraphqlArrayReplacementUsesIndependentPeakOracle() {
	auto scalar_profile = Profile();
	auto array_profile = ArrayProfile();
	Require(static_cast<bool>(scalar_profile) && static_cast<bool>(array_profile),
	        "GraphQL replacement-allocation profiles must be admitted");
	NeverCancelled control;
	const auto baseline =
	    cuac::internal::DecodeGraphqlResponse(Envelope(Node("R")), *scalar_profile, Limits(), control);
	Require(baseline.rows.size() == 1 && baseline.rows[0].values.size() == 8,
	        "GraphQL replacement-allocation baseline changed shape");
	const auto staging_overhead = baseline.peak_memory_bytes - baseline.retained_memory_bytes;

	std::vector<cuac::TypedScalarValue> element_probe;
	std::size_t previous_element_capacity = 0;
	const std::size_t element_count = 65;
	std::string booleans;
	for (std::size_t index = 0; index < element_count; index++) {
		if (element_probe.size() == element_probe.capacity()) {
			previous_element_capacity = element_probe.capacity();
			const auto requested = previous_element_capacity == 0 ? 1 : previous_element_capacity * 2;
			element_probe.reserve(requested);
		}
		element_probe.push_back(cuac::TypedScalarValue::Boolean(index % 2 == 0));
		if (!booleans.empty()) {
			booleans += ',';
		}
		booleans += index % 2 == 0 ? "true" : "false";
	}
	const std::string node = "{\"id\":[\"R\"],\"nameWithOwner\":\"o/r\",\"owner\":{\"login\":\"o\"},"
	                         "\"stargazerCount\":[1],\"primaryLanguage\":null,\"isPrivate\":[" +
	                         booleans + "],\"isArchived\":false,\"updatedAt\":\"2026-01-01T00:00:00Z\"}";
	const auto decoded = cuac::internal::DecodeGraphqlResponse(Envelope(node), *array_profile, Limits(), control);
	Require(decoded.rows[0].values[5].elements.capacity() == element_probe.capacity(),
	        "GraphQL ARRAY capacity oracle drifted from decoder allocation");
	const auto value_storage = static_cast<uint64_t>(decoded.rows[0].values.capacity()) * sizeof(cuac::TypedValue);
	const auto previous_array_bytes = static_cast<uint64_t>(previous_element_capacity) * sizeof(cuac::TypedScalarValue);
	Require(previous_array_bytes > value_storage,
	        "GraphQL ARRAY oracle must make replacement co-liveness the unique peak");
	// updatedAt is a fixed-width TIMESTAMPTZ, so no later retained string
	// allocation is live at the final isPrivate element-buffer replacement.
	Require(previous_array_bytes > value_storage,
	        "GraphQL ARRAY oracle must dominate the typed-row replacement storage");
	const auto expected_peak = decoded.retained_memory_bytes + staging_overhead + previous_array_bytes - value_storage;
	Require(decoded.peak_memory_bytes == expected_peak,
	        "GraphQL ARRAY peak must include current and replacement element buffers");
	auto exact_limits = Limits();
	exact_limits.max_decoded_memory_bytes = expected_peak;
	(void)cuac::internal::DecodeGraphqlResponse(Envelope(node), *array_profile, exact_limits, control);
	auto under_limits = exact_limits;
	under_limits.max_decoded_memory_bytes--;
	try {
		(void)cuac::internal::DecodeGraphqlResponse(Envelope(node), *array_profile, under_limits, control);
		throw std::runtime_error("GraphQL ARRAY one-byte-under independent replacement peak must fail");
	} catch (const cuac::ExecutionError &error) {
		Require(error.Stage() == cuac::ErrorStage::RESOURCE && error.Field() == "decoded_memory_bytes",
		        "GraphQL ARRAY independent replacement rejection used the wrong diagnostic");
	}
}

} // namespace

int main() {
	try {
		TestEightColumnsAndNullableLanguage();
		TestTimestamptzStrictProfile();
		TestErrorsPrecedeDataWithoutLeakingRemoteValues();
		TestMalformedDocumentPrecedesRemoteProtocol();
		TestRequiredNestedFieldFailsClosed();
		TestZeroHundredAndDuplicateOccurrences();
		TestIgnoredValuesDoNotConsumeDecodedMemory();
		TestMaterializedStringAndMemoryBoundaries();
		TestWrongNullDuplicateAndEnvelopeShapesFailClosed();
		TestGraphqlScalarArrays();
		TestGraphqlArrayReplacementUsesIndependentPeakOracle();
		std::cout << "GraphQL response decoder tests passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return EXIT_FAILURE;
	}
}
