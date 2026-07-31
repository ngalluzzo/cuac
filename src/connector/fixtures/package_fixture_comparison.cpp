#include "cuac/internal/connector/fixtures/package_fixture_comparison.hpp"

#include <algorithm>

namespace cuac {
namespace connector {
namespace internal {
namespace {

bool SameValue(const PackageFixtureValue &left, const PackageFixtureValue &right) {
	return left.is_null == right.is_null && (left.is_null || left.value == right.value);
}

bool SameOrigin(const PackageFixtureOrigin &left, const PackageFixtureOrigin &right) {
	return left.scheme == right.scheme && left.host == right.host && left.port == right.port;
}

bool SameRequest(const PackageFixtureRequest &left, const PackageFixtureRequest &right) {
	if (left.protocol != right.protocol || !SameOrigin(left.origin, right.origin) || left.path != right.path ||
	    left.bearer_authorization != right.bearer_authorization || left.query.size() != right.query.size() ||
	    left.document_digest != right.document_digest || left.variables.size() != right.variables.size() ||
	    left.serialized_body_digest != right.serialized_body_digest) {
		return false;
	}
	for (std::size_t index = 0; index < left.query.size(); index++) {
		if (left.query[index].name != right.query[index].name ||
		    left.query[index].encoded_value != right.query[index].encoded_value) {
			return false;
		}
	}
	for (std::size_t index = 0; index < left.variables.size(); index++) {
		if (left.variables[index].name != right.variables[index].name ||
		    left.variables[index].type != right.variables[index].type ||
		    !SameValue(left.variables[index].value, right.variables[index].value)) {
			return false;
		}
	}
	return true;
}

std::vector<PackageFixtureRequest> ExpectedRequests(const PackageFixtureCase &fixture_case) {
	std::vector<PackageFixtureRequest> requests;
	for (const auto &page : fixture_case.pages) {
		requests.push_back(page.request);
	}
	for (const auto &page : fixture_case.restricted_pages) {
		requests.push_back(page.request);
	}
	return requests;
}

bool SameCell(const PackageFixtureCell &left, const PackageFixtureCell &right) {
	if (left.is_null != right.is_null || left.is_array != right.is_array) {
		return false;
	}
	if (left.is_null) {
		return true;
	}
	if (!left.is_array) {
		return left.value == right.value;
	}
	if (left.elements.size() != right.elements.size()) {
		return false;
	}
	for (std::size_t index = 0; index < left.elements.size(); index++) {
		if (!SameCell(left.elements[index], right.elements[index])) {
			return false;
		}
	}
	return true;
}

bool SameRows(const std::vector<PackageFixtureRow> &left, const std::vector<PackageFixtureRow> &right) {
	if (left.size() != right.size()) {
		return false;
	}
	for (std::size_t row = 0; row < left.size(); row++) {
		if (left[row].cells.size() != right[row].cells.size()) {
			return false;
		}
		for (std::size_t column = 0; column < left[row].cells.size(); column++) {
			const auto &expected = left[row].cells[column];
			const auto &actual = right[row].cells[column];
			if (!SameCell(expected, actual)) {
				return false;
			}
		}
	}
	return true;
}

bool SameFacts(const std::vector<PackageFixtureFact> &left, const std::vector<PackageFixtureFact> &right) {
	if (left.size() != right.size()) {
		return false;
	}
	for (std::size_t index = 0; index < left.size(); index++) {
		if (left[index].name != right[index].name || left[index].value != right[index].value) {
			return false;
		}
	}
	return true;
}

bool SameOutcome(const PackageFixtureExpected &expected, const PackageFixtureExpected &actual) {
	if (expected.kind != actual.kind) {
		return false;
	}
	switch (expected.kind) {
	case PackageFixtureExpectedKind::SUCCESS:
		return expected.remote_accuracy == actual.remote_accuracy && SameRows(expected.rows, actual.rows) &&
		       SameFacts(expected.explain, actual.explain);
	case PackageFixtureExpectedKind::COMPILER_DIAGNOSTIC:
		return expected.diagnostic_code == actual.diagnostic_code;
	case PackageFixtureExpectedKind::RUNTIME_ERROR:
		return expected.runtime_stage == actual.runtime_stage && expected.runtime_field == actual.runtime_field;
	}
	return false;
}

} // namespace

bool FixtureObservationMatches(const PackageFixtureCase &fixture_case, const PackageFixtureObservation &observation,
                               std::string &safe_reason) {
	const auto expected_requests = ExpectedRequests(fixture_case);
	if (expected_requests.size() != observation.requests.size()) {
		safe_reason = "request_count";
		return false;
	}
	for (std::size_t index = 0; index < expected_requests.size(); index++) {
		if (!SameRequest(expected_requests[index], observation.requests[index])) {
			safe_reason = "request_identity";
			return false;
		}
	}
	if (!SameOutcome(fixture_case.expected, observation.actual)) {
		safe_reason = "outcome";
		return false;
	}
	std::vector<std::string> unique = observation.executed_coverage_keys;
	std::sort(unique.begin(), unique.end());
	if (std::adjacent_find(unique.begin(), unique.end()) != unique.end()) {
		safe_reason = "coverage_duplicate";
		return false;
	}
	safe_reason.clear();
	return true;
}

} // namespace internal
} // namespace connector
} // namespace cuac
